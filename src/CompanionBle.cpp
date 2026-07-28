#include "CompanionBle.h"

#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <NimBLEDevice.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "CompanionPeerStore.h"
#include "Memory.h"

namespace companionble {

namespace {

constexpr const char* kServiceUuid = "7c9c0000-3e4a-4b1a-9c1e-6d8a1f2b0001";
constexpr const char* kContentCharUuid = "7c9c0001-3e4a-4b1a-9c1e-6d8a1f2b0001";
constexpr const char* kButtonCharUuid = "7c9c0002-3e4a-4b1a-9c1e-6d8a1f2b0001";
constexpr const char* kCapabilityCharUuid = "7c9c0003-3e4a-4b1a-9c1e-6d8a1f2b0001";
constexpr const char* kStatusCharUuid = "7c9c0004-3e4a-4b1a-9c1e-6d8a1f2b0001";
constexpr const char* kSessionCharUuid = "7c9c0005-3e4a-4b1a-9c1e-6d8a1f2b0001";

constexpr uint8_t kOpStart = 0x01;
constexpr uint8_t kOpChunk = 0x02;
constexpr uint8_t kOpEnd = 0x03;

// Session characteristic opcodes, phone -> device.
constexpr uint8_t kSessHello = 0x01;
constexpr uint8_t kSessBye = 0x02;
constexpr uint8_t kSessAcquire = 0x03;
constexpr uint8_t kSessRelease = 0x04;

// Session characteristic opcodes, device -> phone.
constexpr uint8_t kSessHelloOk = 0x81;
constexpr uint8_t kSessHelloPending = 0x82;
constexpr uint8_t kSessHelloDenied = 0x83;
constexpr uint8_t kSessForeground = 0x84;
constexpr uint8_t kSessBackground = 0x85;
constexpr uint8_t kSessAcquireDenied = 0x86;
constexpr uint8_t kSessAssetAck = 0x87;
constexpr uint8_t kSessImageStatus = 0x88;

constexpr uint8_t kDeniedUserRejected = 0x00;
constexpr uint8_t kDeniedTimeout = 0x01;
constexpr uint8_t kDeniedNoSlots = 0x02;
constexpr uint8_t kDeniedMalformed = 0x03;
constexpr uint8_t kDeniedStorage = 0x04;
constexpr uint8_t kDeniedBusy = 0x05;

constexpr uint8_t kAcquireDeniedNoButtonMap = 0x00;
constexpr uint8_t kAcquireDeniedUnknownSession = 0x01;

constexpr uint32_t kTeardownDisconnectWaitMs = 600;

// Name of the staged image inside the pushing peer's data/ directory. Per-peer
// rather than one global scratch file, so two apps staging at once cannot
// collide — the answer to the image sketch's "where does the staging file go".
constexpr const char* kStagedImageName = "incoming.png";

// Full advertised name buffer: kDeviceNamePrefix + " " + 4 hex chars (2 bytes
// of the eFuse MAC tail) + NUL. Built once in ensureStarted() from
// ESP.getEfuseMac() so two devices running this firmware advertise distinct
// names (see kDeviceNamePrefix's doc comment in CompanionBle.h).
char g_deviceName[48] = {0};

void buildDeviceName() {
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(g_deviceName, sizeof(g_deviceName), "%s %04X", kDeviceNamePrefix, static_cast<unsigned>(mac & 0xFFFF));
}

volatile bool g_startInProgress = false;
bool g_begun = false;

// Held for the entire BLE session (ensureStarted() success through stop()),
// not just around NimBLEDevice::init(). HalPowerManager's idle power-saving
// logic drops the CPU to its low-power clock after a period of no activity —
// exactly what happens while sitting on "Waiting for phone" with no further
// screen redraws — and NimBLE's connection-establishment processing on the
// host task hangs at that low clock, the same WDT-hang class BleInput.cpp
// documents for init/deinit specifically. A scoped Lock around only the init
// call (this file's first version) let the CPU drop back to low-power well
// before a real central's connection attempt arrived, so the link never
// completed. Confirmed by reproducing on real hardware: advertising ran and
// was independently discoverable (byte-verified UUID), but no central ever
// completed a connection until this lock was made session-scoped.
std::unique_ptr<HalPowerManager::Lock> g_powerLock;

NimBLEServer* g_server = nullptr;
NimBLECharacteristic* g_contentChar = nullptr;
NimBLECharacteristic* g_buttonChar = nullptr;
NimBLECharacteristic* g_capabilityChar = nullptr;
NimBLECharacteristic* g_statusChar = nullptr;
NimBLECharacteristic* g_sessionChar = nullptr;

ContentFieldCallback g_contentCb = nullptr;
StatusCallback g_statusCb = nullptr;
PairingRequestCallback g_pairingCb = nullptr;
ForegroundChangeCallback g_foregroundCb = nullptr;
ImageStagedCallback g_imageStagedCb = nullptr;

// ---------------------------------------------------------------------------
// Session table
// ---------------------------------------------------------------------------

// One entry per live app on the current link. ~14 bytes each, four of them:
// the whole session layer costs well under the 1 KB net-new steady-state RAM
// budget in docs/companion-multi-app-design.md §11, because everything a
// session *knows* (token, button map, icon) stays on SD.
struct Session {
  bool active = false;
  char peerKey[companionpeer::kPeerKeyLen] = {0};
  // Last content-id this session pushed, echoed on its button events. Only the
  // foreground session's is ever read, but it is stored per session so a
  // background app that comes back does not inherit somebody else's token.
  uint8_t contentId[kMaxContentIdLen] = {0};
  uint8_t contentIdLen = 0;
};

Session g_sessions[kMaxSessions];
uint8_t g_foreground = kNoSession;

// Pending on-screen pairing prompt. Exactly one at a time — a second prompt
// stacked behind the first would leave the user confirming an app they can no
// longer see the name of.
struct PendingPairing {
  bool active = false;
  uint16_t helloTag = 0;
  uint8_t appId[companionpeer::kIdLen] = {0};
  uint8_t installId[companionpeer::kIdLen] = {0};
  char name[companionpeer::kMaxNameLen + 1] = {0};
};
PendingPairing g_pendingPairing;

// Defined with the reassembly state below; needed here because a foreground
// handover has to drop whatever the outgoing session had in flight.
void resetReassembly();

Session* sessionById(uint8_t id) {
  if (id == kNoSession || id > kMaxSessions) return nullptr;
  Session& session = g_sessions[id - 1];
  return session.active ? &session : nullptr;
}

uint8_t allocateSession(const char* peerKey) {
  for (uint8_t i = 0; i < kMaxSessions; ++i) {
    if (g_sessions[i].active) continue;
    g_sessions[i] = Session();
    g_sessions[i].active = true;
    snprintf(g_sessions[i].peerKey, sizeof(g_sessions[i].peerKey), "%s", peerKey);
    return static_cast<uint8_t>(i + 1);
  }
  return kNoSession;
}

void notifySession(const uint8_t* data, size_t len) {
  if (!g_sessionChar) return;
  g_sessionChar->setValue(data, len);
  g_sessionChar->notify();
}

void notifyHelloDenied(uint16_t helloTag, uint8_t reason) {
  const uint8_t payload[4] = {kSessHelloDenied, static_cast<uint8_t>(helloTag & 0xFF),
                              static_cast<uint8_t>((helloTag >> 8) & 0xFF), reason};
  notifySession(payload, sizeof(payload));
}

// HELLO_OK: sessionId, the peer's token, then the asset digest block. The tags
// are read straight off the stored assets and reported verbatim — the device
// performs no comparison and computes no hash. Which asset is stale is the
// app's conclusion, not the firmware's (docs/companion-display-protocol.md,
// "Asset digests").
void notifyHelloOk(uint16_t helloTag, uint8_t sessionId, const char* peerKey, const uint8_t token[16]) {
  uint8_t payload[4 + 16 + 1 + 2 * 5];
  size_t offset = 0;
  payload[offset++] = kSessHelloOk;
  payload[offset++] = static_cast<uint8_t>(helloTag & 0xFF);
  payload[offset++] = static_cast<uint8_t>((helloTag >> 8) & 0xFF);
  payload[offset++] = sessionId;
  memcpy(payload + offset, token, 16);
  offset += 16;
  payload[offset++] = 2;  // assetCount

  payload[offset++] = kFieldButtonMap;
  companionpeer::assetTag(peerKey, companionpeer::kAssetButtonMap, payload + offset);
  offset += 4;

  payload[offset++] = kFieldIcon;
  companionpeer::assetTag(peerKey, companionpeer::kAssetIcon, payload + offset);
  offset += 4;

  notifySession(payload, offset);
}

void notifyForeground(uint8_t sessionId) {
  const uint8_t payload[2] = {kSessForeground, sessionId};
  notifySession(payload, sizeof(payload));
}

void notifyBackground(uint8_t sessionId, BackgroundReason reason) {
  const uint8_t payload[3] = {kSessBackground, sessionId, static_cast<uint8_t>(reason)};
  notifySession(payload, sizeof(payload));
}

void notifyAssetAck(uint8_t sessionId, uint8_t assetId, companionpeer::AssetStoreResult result, const char* peerKey) {
  uint8_t payload[8] = {kSessAssetAck, sessionId, assetId, static_cast<uint8_t>(result), 0, 0, 0, 0};
  if (result == companionpeer::AssetStoreResult::Stored) {
    companionpeer::assetTag(peerKey, assetId == kFieldIcon ? companionpeer::kAssetIcon : companionpeer::kAssetButtonMap,
                            payload + 4);
  }
  notifySession(payload, sizeof(payload));
}

// Hands the screen to `sessionId`, backgrounding whoever held it. Policy is
// last-requester-wins, unconditionally: the user just brought that app to the
// foreground on their phone, which is knowledge the firmware does not have and
// has no business overriding.
void setForeground(uint8_t sessionId) {
  if (g_foreground == sessionId) return;
  if (sessionById(g_foreground) != nullptr) notifyBackground(g_foreground, BackgroundReason::Preempted);
  // A transfer in flight belonged to the session that just lost the screen. Its
  // remaining CHUNKs will be dropped by the sessionId check anyway, so keeping
  // the partial field would only leave the buffer (or a half-written staged
  // image) lying around until the next START.
  resetReassembly();
  g_foreground = sessionId;

  const Session* session = sessionById(sessionId);
  if (g_foregroundCb) {
    const std::string name = session ? companionpeer::displayName(session->peerKey) : std::string();
    g_foregroundCb(session ? session->peerKey : "", name.c_str());
  }
  if (session) notifyForeground(sessionId);
}

void dropSession(uint8_t sessionId, BackgroundReason reason) {
  Session* session = sessionById(sessionId);
  if (!session) return;
  if (g_foreground == sessionId) {
    notifyBackground(sessionId, reason);
    g_foreground = kNoSession;
    if (g_foregroundCb) g_foregroundCb("", "");
  }
  session->active = false;
}

// ---------------------------------------------------------------------------
// Content reassembly
// ---------------------------------------------------------------------------

// Text fields (title/body/content-id) reassemble into a heap buffer bounded by
// kMaxFieldLen. The image field does NOT: its CHUNKs are appended straight to
// an SD file as they arrive, so the only image-sized memory user at any point
// is the PNG decoder's own per-row scratch, which already runs today for EPUB
// cover art under this same heap budget. Buffering an image in RAM would be a
// third large allocation stacked on NimBLE's ~63 KB and the 48 KB framebuffer.
uint8_t g_activeField = 0;
uint8_t g_activeSession = kNoSession;
uint32_t g_activeTotalLen = 0;
uint32_t g_activeWritten = 0;
bool g_activeFinal = false;
std::unique_ptr<uint8_t[]> g_activeBuf;
HalFile g_activeImageFile;
std::string g_activeImagePath;
bool g_activeImageOverflow = false;

void discardStagedImage() {
  if (g_activeImageFile.isOpen()) g_activeImageFile.close();
  if (!g_activeImagePath.empty()) {
    // A partial PNG is not decodable and would sit on the card until the next
    // push overwrote it. Drop it rather than leave a trap for the decoder.
    Storage.remove(g_activeImagePath.c_str());
    g_activeImagePath.clear();
  }
}

void resetReassembly() {
  if (g_activeField == kFieldImage) discardStagedImage();
  g_activeField = 0;
  g_activeSession = kNoSession;
  g_activeTotalLen = 0;
  g_activeWritten = 0;
  g_activeFinal = false;
  g_activeImageOverflow = false;
  g_activeBuf.reset();
}

uint32_t fieldCap(uint8_t field) {
  switch (field) {
    case kFieldContentId:
      return kMaxContentIdLen;
    case kFieldImage:
      return kMaxImageFieldLen;
    case kFieldButtonMap:
      return companionpeer::kMaxButtonMapLen;
    case kFieldIcon:
      return 4 + kIconBytes;
    default:
      return kMaxFieldLen;
  }
}

bool isKnownField(uint8_t field) {
  return field == kFieldTitle || field == kFieldBody || field == kFieldContentId || field == kFieldImage ||
         field == kFieldButtonMap || field == kFieldIcon;
}

// ---------------------------------------------------------------------------
// Capability characteristic
// ---------------------------------------------------------------------------

// Static for the lifetime of a session — computed once in ensureStarted() from
// the live renderer/font, never hardcoded. 23 bytes in v6; see
// docs/companion-display-protocol.md for the field-by-field layout.
uint8_t g_capabilityValue[23] = {0};

void computeCapabilityValue(const GfxRenderer& renderer, int fontId) {
  const int lineHeight = renderer.getLineHeight(fontId);
  const int advanceWidth = renderer.getTextAdvanceX(fontId, "M", EpdFontFamily::REGULAR);
  const int screenWidthPx = renderer.getScreenWidth();
  const int screenHeightPx = renderer.getScreenHeight();
  const int screenWidthChars = advanceWidth > 0 ? screenWidthPx / advanceWidth : 0;
  const int screenHeightChars = lineHeight > 0 ? screenHeightPx / lineHeight : 0;
  const uint64_t mac = ESP.getEfuseMac();

  size_t offset = 0;
  g_capabilityValue[offset++] = 6;  // protocol version
  g_capabilityValue[offset++] = static_cast<uint8_t>(screenWidthChars > 255 ? 255 : screenWidthChars);
  g_capabilityValue[offset++] = static_cast<uint8_t>(screenHeightChars > 255 ? 255 : screenHeightChars);
  g_capabilityValue[offset++] = static_cast<uint8_t>(kMaxFieldLen & 0xFF);
  g_capabilityValue[offset++] = static_cast<uint8_t>((kMaxFieldLen >> 8) & 0xFF);
  g_capabilityValue[offset++] = 0x0F;  // bit0 image, bit1 button map, bit2 icons, bit3 sessions
  g_capabilityValue[offset++] = static_cast<uint8_t>(kMaxImageFieldLen & 0xFF);
  g_capabilityValue[offset++] = static_cast<uint8_t>((kMaxImageFieldLen >> 8) & 0xFF);
  g_capabilityValue[offset++] = static_cast<uint8_t>((kMaxImageFieldLen >> 16) & 0xFF);
  g_capabilityValue[offset++] = static_cast<uint8_t>((kMaxImageFieldLen >> 24) & 0xFF);
  g_capabilityValue[offset++] = kMaxSessions;
  g_capabilityValue[offset++] = kIconWidthPx;
  g_capabilityValue[offset++] = kIconHeightPx;
  // deviceId: the 4-byte eFuse MAC tail, most-significant byte first, so the
  // hex a client prints matches the tail in the advertised name.
  g_capabilityValue[offset++] = static_cast<uint8_t>((mac >> 24) & 0xFF);
  g_capabilityValue[offset++] = static_cast<uint8_t>((mac >> 16) & 0xFF);
  g_capabilityValue[offset++] = static_cast<uint8_t>((mac >> 8) & 0xFF);
  g_capabilityValue[offset++] = static_cast<uint8_t>(mac & 0xFF);
  g_capabilityValue[offset++] = static_cast<uint8_t>(screenWidthPx & 0xFF);
  g_capabilityValue[offset++] = static_cast<uint8_t>((screenWidthPx >> 8) & 0xFF);
  g_capabilityValue[offset++] = static_cast<uint8_t>(screenHeightPx & 0xFF);
  g_capabilityValue[offset++] = static_cast<uint8_t>((screenHeightPx >> 8) & 0xFF);
  g_capabilityValue[offset++] = static_cast<uint8_t>(kMaxContentIdLen);
  g_capabilityValue[offset++] = 4;  // grey levels the panel renders
}

// ---------------------------------------------------------------------------
// Session characteristic
// ---------------------------------------------------------------------------

// Completes a handshake for a peer the device is willing to talk to: allocates
// the session and answers HELLO_OK. Shared by the auto-accept path (known peer,
// valid token) and resolvePairing()'s accept path.
void admitPeer(uint16_t helloTag, const char* peerKey, const uint8_t token[16]) {
  const uint8_t sessionId = allocateSession(peerKey);
  if (sessionId == kNoSession) {
    notifyHelloDenied(helloTag, kDeniedNoSlots);
    return;
  }
  companionpeer::touch(peerKey);
  notifyHelloOk(helloTag, sessionId, peerKey, token);
}

void handleHello(const uint8_t* data, size_t len) {
  // opcode(1) helloTag(2) appId(16) installId(16) tokenLen(1) token[..] nameLen(1) name[..]
  if (len < 1 + 2 + 32 + 1) {
    notifyHelloDenied(0, kDeniedMalformed);
    return;
  }
  const uint16_t helloTag = static_cast<uint16_t>(data[1] | (data[2] << 8));
  const uint8_t* appId = data + 3;
  const uint8_t* installId = data + 19;
  const uint8_t tokenLen = data[35];
  if (len < 36u + tokenLen + 1u) {
    notifyHelloDenied(helloTag, kDeniedMalformed);
    return;
  }
  const uint8_t* token = data + 36;
  const uint8_t nameLen = data[36 + tokenLen];
  if (len < 37u + tokenLen + nameLen) {
    notifyHelloDenied(helloTag, kDeniedMalformed);
    return;
  }
  const uint8_t* name = data + 37 + tokenLen;

  char displayName[companionpeer::kMaxNameLen + 1] = {0};
  const size_t nameCopy = nameLen > companionpeer::kMaxNameLen ? companionpeer::kMaxNameLen : nameLen;
  memcpy(displayName, name, nameCopy);

  char peerKey[companionpeer::kPeerKeyLen];
  companionpeer::makePeerKey(appId, installId, peerKey);

  if (companionpeer::isEnrolled(appId, installId) && companionpeer::tokenMatches(peerKey, token, tokenLen)) {
    // The "connects automatically" relationship: known peer, valid token, no
    // prompt. Refresh the stored display name in case the app was renamed.
    if (!companionpeer::ensurePeer(appId, installId, displayName, peerKey)) {
      notifyHelloDenied(helloTag, kDeniedStorage);
      return;
    }
    uint8_t storedToken[companionpeer::kTokenLen];
    memcpy(storedToken, token, companionpeer::kTokenLen);
    admitPeer(helloTag, peerKey, storedToken);
    return;
  }

  if (g_pendingPairing.active) {
    notifyHelloDenied(helloTag, kDeniedBusy);
    return;
  }

  g_pendingPairing.active = true;
  g_pendingPairing.helloTag = helloTag;
  memcpy(g_pendingPairing.appId, appId, companionpeer::kIdLen);
  memcpy(g_pendingPairing.installId, installId, companionpeer::kIdLen);
  snprintf(g_pendingPairing.name, sizeof(g_pendingPairing.name), "%s", displayName);

  const uint8_t pending[3] = {kSessHelloPending, static_cast<uint8_t>(helloTag & 0xFF),
                              static_cast<uint8_t>((helloTag >> 8) & 0xFF)};
  notifySession(pending, sizeof(pending));
  if (g_pairingCb) g_pairingCb(g_pendingPairing.name);
}

class SessionCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& /*connInfo*/) override {
    const NimBLEAttValue& value = characteristic->getValue();
    const uint8_t* data = value.data();
    const size_t len = value.size();
    if (len == 0) return;

    switch (data[0]) {
      case kSessHello:
        handleHello(data, len);
        break;

      case kSessBye:
        if (len >= 2) dropSession(data[1], BackgroundReason::Released);
        break;

      case kSessAcquire: {
        if (len < 2) return;
        const uint8_t sessionId = data[1];
        Session* session = sessionById(sessionId);
        if (!session) {
          const uint8_t denied[3] = {kSessAcquireDenied, sessionId, kAcquireDeniedUnknownSession};
          notifySession(denied, sizeof(denied));
          return;
        }
        // A peer with no button map cannot take the screen. This is what makes
        // "an app with undefined buttons" structurally impossible rather than a
        // case the rendering code has to handle.
        if (!companionpeer::hasButtonMap(session->peerKey)) {
          const uint8_t denied[3] = {kSessAcquireDenied, sessionId, kAcquireDeniedNoButtonMap};
          notifySession(denied, sizeof(denied));
          return;
        }
        setForeground(sessionId);
        break;
      }

      case kSessRelease: {
        if (len < 2) return;
        const uint8_t sessionId = data[1];
        if (g_foreground != sessionId || !sessionById(sessionId)) return;
        notifyBackground(sessionId, BackgroundReason::Released);
        g_foreground = kNoSession;
        if (g_foregroundCb) g_foregroundCb("", "");
        break;
      }

      default:
        LOG_ERR("CBLE", "unknown session opcode 0x%02x", data[0]);
        break;
    }
  }
};

// ---------------------------------------------------------------------------
// Content characteristic
// ---------------------------------------------------------------------------

void beginImageStaging(const Session& session) {
  g_activeImagePath = companionpeer::dataFilePath(session.peerKey, kStagedImageName);
  if (!Storage.openFileForWrite("CBLE", g_activeImagePath, g_activeImageFile)) {
    LOG_ERR("CBLE", "could not open %s for image staging", g_activeImagePath.c_str());
    g_activeImagePath.clear();
  }
}

void finishAsset(uint8_t field, const Session& session, uint8_t sessionId) {
  const companionpeer::AssetStoreResult result =
      companionpeer::storeAsset(session.peerKey, field == kFieldIcon ? companionpeer::kAssetIcon
                                                                     : companionpeer::kAssetButtonMap,
                                g_activeBuf.get(), g_activeWritten, kIconBytes);
  notifyAssetAck(sessionId, field, result, session.peerKey);
  if (result != companionpeer::AssetStoreResult::Stored) {
    LOG_ERR("CBLE", "asset 0x%02x rejected (%u)", field, static_cast<unsigned>(static_cast<uint8_t>(result)));
  } else if (field == kFieldButtonMap && g_foreground == sessionId && g_foregroundCb) {
    // The foreground app just changed its control scheme; re-read it now rather
    // than waiting for the next connect.
    const std::string name = companionpeer::displayName(session.peerKey);
    g_foregroundCb(session.peerKey, name.c_str());
  }
}

class ContentCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& /*connInfo*/) override {
    const NimBLEAttValue& value = characteristic->getValue();
    const uint8_t* data = value.data();
    const size_t len = value.size();
    if (len == 0) return;

    switch (data[0]) {
      case kOpStart: {
        if (len < 7) {
          LOG_ERR("CBLE", "START packet too short (%u bytes)", static_cast<unsigned>(len));
          return;
        }
        const uint8_t fieldByte = data[1];
        const uint8_t field = fieldByte & kFieldMask;
        const uint8_t sessionId = data[2];
        if (!isKnownField(field)) {
          LOG_ERR("CBLE", "START packet unknown field 0x%02x", field);
          return;
        }
        // Frames from a background (or unknown) session are dropped. Without
        // this, a stray write from a backgrounded app on the same phone lands in
        // whatever transfer is in flight.
        Session* session = sessionById(sessionId);
        if (!session || sessionId != g_foreground) return;

        uint32_t totalLen;
        memcpy(&totalLen, data + 3, sizeof(totalLen));  // data may be unaligned
        const uint32_t cap = fieldCap(field);

        resetReassembly();
        g_activeField = field;
        g_activeSession = sessionId;
        g_activeFinal = (fieldByte & kFinalFieldFlag) != 0;
        g_activeWritten = 0;

        if (field == kFieldImage) {
          // Oversize images are rejected at END rather than here so the app gets
          // one clear IMAGE_STATUS either way; the bytes are simply not stored.
          g_activeImageOverflow = totalLen > cap;
          g_activeTotalLen = totalLen;
          if (!g_activeImageOverflow) beginImageStaging(*session);
          return;
        }

        g_activeTotalLen = totalLen > cap ? cap : totalLen;
        g_activeBuf = makeUniqueNoThrow<uint8_t[]>(g_activeTotalLen == 0 ? 1 : g_activeTotalLen);
        if (!g_activeBuf) {
          LOG_ERR("CBLE", "OOM allocating %u-byte field buffer", static_cast<unsigned>(g_activeTotalLen));
          resetReassembly();
        }
        break;
      }

      case kOpChunk: {
        if (g_activeField == 0 || len < 2 || data[1] != g_activeSession) return;
        const uint8_t* payload = data + 2;
        const size_t payloadLen = len - 2;

        if (g_activeField == kFieldImage) {
          if (g_activeImageOverflow || !g_activeImageFile.isOpen()) return;
          if (g_activeImageFile.write(payload, payloadLen) != payloadLen) {
            LOG_ERR("CBLE", "SD write failed staging image");
            discardStagedImage();
            return;
          }
          g_activeWritten += payloadLen;
          return;
        }

        if (!g_activeBuf) return;
        const size_t remaining = g_activeTotalLen - g_activeWritten;
        const size_t toCopy = payloadLen < remaining ? payloadLen : remaining;
        if (toCopy > 0) {
          memcpy(g_activeBuf.get() + g_activeWritten, payload, toCopy);
          g_activeWritten += toCopy;
        }
        break;
      }

      case kOpEnd: {
        if (g_activeField == 0 || len < 2 || data[1] != g_activeSession) return;
        Session* session = sessionById(g_activeSession);
        if (!session) {
          resetReassembly();
          return;
        }
        const uint8_t field = g_activeField;
        const uint8_t sessionId = g_activeSession;

        switch (field) {
          case kFieldImage: {
            if (g_activeImageOverflow) {
              notifyImageStatus(ImageResult::RejectedSize);
            } else if (!g_activeImageFile.isOpen()) {
              notifyImageStatus(ImageResult::StorageFailed);
            } else {
              g_activeImageFile.flush();
              g_activeImageFile.close();
              // Decoding touches the framebuffer, so it happens on the main loop
              // task, not here. The activity answers with notifyImageStatus().
              if (g_imageStagedCb) g_imageStagedCb(g_activeImagePath.c_str());
              g_activeImagePath.clear();  // ownership passes to the activity
            }
            break;
          }

          case kFieldButtonMap:
          case kFieldIcon:
            if (g_activeBuf) finishAsset(field, *session, sessionId);
            break;

          case kFieldContentId:
            session->contentIdLen = static_cast<uint8_t>(g_activeWritten);
            memcpy(session->contentId, g_activeBuf.get(), session->contentIdLen);
            if (g_contentCb) g_contentCb(field, g_activeBuf.get(), g_activeWritten, g_activeFinal);
            break;

          default:
            if (g_contentCb) g_contentCb(field, g_activeBuf.get(), g_activeWritten, g_activeFinal);
            break;
        }
        resetReassembly();
        break;
      }

      default:
        LOG_ERR("CBLE", "unknown content opcode 0x%02x", data[0]);
        break;
    }
  }
};

class StatusCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& /*connInfo*/) override {
    const NimBLEAttValue& value = characteristic->getValue();
    if (value.size() < 3) return;
    const uint8_t* data = value.data();
    if (data[0] != g_foreground) return;  // not the app that owns the screen
    if (data[1] >= kMaxIndicators) return;
    if (g_statusCb) g_statusCb(data[1], data[2]);
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* /*server*/, NimBLEConnInfo& /*connInfo*/) override {
    LOG_DBG("CBLE", "central connected");
  }
  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& /*connInfo*/, int /*reason*/) override {
    LOG_DBG("CBLE", "central disconnected");
    resetReassembly();
    // Sessions do not survive the link. The token does — that is what makes the
    // next connect silent.
    for (uint8_t i = 0; i < kMaxSessions; ++i) g_sessions[i] = Session();
    g_foreground = kNoSession;
    g_pendingPairing.active = false;
    // Deliberately does NOT tell the activity to clear the screen: the last
    // content stays up until the idle timeout, so a photo stays a photo after
    // the phone walks away (docs/companion-display-protocol.md, "On-screen
    // behaviour").
    if (g_begun && server) server->startAdvertising();
  }
};

ContentCharCallbacks g_contentCharCallbacks;
StatusCharCallbacks g_statusCharCallbacks;
SessionCharCallbacks g_sessionCharCallbacks;
ServerCallbacks g_serverCallbacks;

}  // namespace

bool ensureStarted(const GfxRenderer& renderer, int fontId) {
  if (g_begun) return true;

  g_startInProgress = true;

  if (ESP.getFreeHeap() < kStartMinFreeHeap) {
    LOG_ERR("CBLE", "ensureStarted: free heap %u < floor %u, not starting", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(kStartMinFreeHeap));
    g_startInProgress = false;
    return false;
  }

  // Held until stop() — see the g_powerLock declaration comment for why this
  // can't be scoped to just this function.
  g_powerLock = makeUniqueNoThrow<HalPowerManager::Lock>();
  if (!g_powerLock) {
    LOG_ERR("CBLE", "ensureStarted: OOM allocating power lock");
    g_startInProgress = false;
    return false;
  }

  // Self-heal a partial teardown, same as bleinput::ensureStarted()/BleKeyboardHost::begin().
  if (NimBLEDevice::isInitialized()) {
    NimBLEDevice::deinit(true);
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  const uint32_t heapBeforeInit = ESP.getFreeHeap();

  buildDeviceName();

  if (!NimBLEDevice::init(g_deviceName)) {
    LOG_ERR("CBLE", "ensureStarted: NimBLEDevice::init() failed");
    g_powerLock.reset();
    g_startInProgress = false;
    return false;
  }

  // Content chunking wants fewer round trips than a 3-6 byte HID report does;
  // request a larger MTU than the HID-host role's minimal 23 (runtime call,
  // no custom_sdkconfig needed — see platformio.ini's note on why we don't
  // force a from-source Arduino core rebuild for this). v6 also needs it for
  // the handshake: HELLO is up to 76 bytes and is never chunked.
  NimBLEDevice::setMTU(185);

  computeCapabilityValue(renderer, fontId);

  g_server = NimBLEDevice::createServer();
  if (!g_server) {
    LOG_ERR("CBLE", "ensureStarted: createServer() returned null");
    NimBLEDevice::deinit(true);
    g_powerLock.reset();
    g_startInProgress = false;
    return false;
  }
  g_server->setCallbacks(&g_serverCallbacks, false);

  NimBLEService* service = g_server->createService(kServiceUuid);

  g_contentChar = service->createCharacteristic(kContentCharUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  g_contentChar->setCallbacks(&g_contentCharCallbacks);

  g_buttonChar = service->createCharacteristic(kButtonCharUuid, NIMBLE_PROPERTY::NOTIFY);

  g_capabilityChar = service->createCharacteristic(kCapabilityCharUuid, NIMBLE_PROPERTY::READ);
  g_capabilityChar->setValue(g_capabilityValue, sizeof(g_capabilityValue));

  g_statusChar = service->createCharacteristic(kStatusCharUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  g_statusChar->setCallbacks(&g_statusCharCallbacks);

  g_sessionChar = service->createCharacteristic(kSessionCharUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
  g_sessionChar->setCallbacks(&g_sessionCharCallbacks);

  g_server->start();  // starts all of the server's services (NimBLEService::start() is a no-op now)

  // A 128-bit service UUID (18 bytes) + flags (3 bytes) + the device name
  // (kDeviceNamePrefix + " " + 4 hex digits, e.g. "CrossPoint Companion A1B2",
  // 26 bytes) overflows the 31-byte legacy advertising PDU (47 bytes total) —
  // build the primary advertisement with just the service UUID (what iOS's
  // scanForPeripherals(withServices:) filters on) and put the name in the
  // separate scan-response packet instead.
  NimBLEAdvertisementData advData;
  advData.addServiceUUID(kServiceUuid);

  NimBLEAdvertisementData scanResponseData;
  scanResponseData.setName(g_deviceName);

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setAdvertisementData(advData);
  advertising->setScanResponseData(scanResponseData);
  const bool advStarted = advertising->start();

  {
    const std::vector<uint8_t>& payload = advData.getPayload();
    char hex[3 * 31 + 1] = {0};
    size_t off = 0;
    for (size_t i = 0; i < payload.size() && off + 3 < sizeof(hex); ++i) {
      off += snprintf(hex + off, sizeof(hex) - off, "%02x ", payload[i]);
    }
    LOG_DBG("CBLE", "advertising: start()=%d isAdvertising()=%d advPayloadLen=%u adv=%s", advStarted ? 1 : 0,
            advertising->isAdvertising() ? 1 : 0, static_cast<unsigned>(payload.size()), hex);
  }

  g_begun = true;
  g_startInProgress = false;

  const uint32_t heapAfterStart = ESP.getFreeHeap();
  LOG_DBG("CBLE", "ensureStarted: heap before init=%u after server start=%u (cost=%u)",
          static_cast<unsigned>(heapBeforeInit), static_cast<unsigned>(heapAfterStart),
          static_cast<unsigned>(heapBeforeInit - heapAfterStart));
  return true;
}

bool startInProgress() { return g_startInProgress; }

void stop() {
  if (!g_begun) return;
  g_begun = false;

  // g_powerLock (held since ensureStarted() succeeded) already keeps the CPU
  // at normal frequency through the deinit calls below; released at the end
  // of this function once teardown is complete.

  if (g_server && g_server->getConnectedCount() > 0) {
    // getConnectedCount()==0 has no direct "disconnect all" without a handle; iterate
    // peer info to close any live link before deinit, mirroring BleKeyboardHost::end()'s
    // "close the link before deinit" discipline.
    const auto peers = g_server->getPeerInfo(0);
    g_server->disconnect(peers.getConnHandle());
    const uint32_t waitStart = millis();
    while (g_server->getConnectedCount() > 0 && millis() - waitStart < kTeardownDisconnectWaitMs) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }

  g_contentChar = nullptr;
  g_buttonChar = nullptr;
  g_capabilityChar = nullptr;
  g_statusChar = nullptr;
  g_sessionChar = nullptr;
  g_server = nullptr;
  resetReassembly();
  for (uint8_t i = 0; i < kMaxSessions; ++i) g_sessions[i] = Session();
  g_foreground = kNoSession;
  g_pendingPairing.active = false;

  NimBLEDevice::deinit(true);
  if (NimBLEDevice::isInitialized()) {
    vTaskDelay(pdMS_TO_TICKS(50));
    NimBLEDevice::deinit(true);
  }

  g_powerLock.reset();
}

bool isConnected() { return g_begun && g_server && g_server->getConnectedCount() > 0; }

const uint8_t* capabilityValue(size_t& lengthOut) {
  lengthOut = sizeof(g_capabilityValue);
  return g_capabilityValue;
}

uint8_t foregroundSessionId() { return g_foreground; }

uint8_t activeSessionCount() {
  uint8_t count = 0;
  for (uint8_t i = 0; i < kMaxSessions; ++i) {
    if (g_sessions[i].active) ++count;
  }
  return count;
}

const char* foregroundPeerKey() {
  const Session* session = sessionById(g_foreground);
  return session ? session->peerKey : "";
}

bool notifyButtonEvent(ButtonId button, uint16_t durationTicks, bool isFinal) {
  const Session* session = sessionById(g_foreground);
  if (!isConnected() || !g_buttonChar || !session) return false;

  // Header byte: bit7 isFinal, bits6-4 event type, bits3-0 button id.
  const uint8_t header = (isFinal ? 0x80 : 0x00) |
                          ((static_cast<uint8_t>(ButtonEventType::ButtonPress) & 0x07) << 4) |
                          (static_cast<uint8_t>(button) & 0x0F);
  uint8_t payload[4 + kMaxContentIdLen];
  payload[0] = g_foreground;
  payload[1] = header;
  payload[2] = static_cast<uint8_t>(durationTicks & 0xFF);
  payload[3] = static_cast<uint8_t>((durationTicks >> 8) & 0xFF);
  if (session->contentIdLen > 0) memcpy(payload + 4, session->contentId, session->contentIdLen);

  g_buttonChar->setValue(payload, 4 + session->contentIdLen);
  return g_buttonChar->notify();
}

void notifyImageStatus(ImageResult result) {
  if (g_foreground == kNoSession) return;
  const uint8_t payload[3] = {kSessImageStatus, g_foreground, static_cast<uint8_t>(result)};
  notifySession(payload, sizeof(payload));
}

void resolvePairing(bool accept, bool timedOut) {
  if (!g_pendingPairing.active) return;
  const PendingPairing pending = g_pendingPairing;
  g_pendingPairing.active = false;

  if (!accept) {
    notifyHelloDenied(pending.helloTag, timedOut ? kDeniedTimeout : kDeniedUserRejected);
    return;
  }

  char peerKey[companionpeer::kPeerKeyLen];
  if (!companionpeer::ensurePeer(pending.appId, pending.installId, pending.name, peerKey)) {
    notifyHelloDenied(pending.helloTag, kDeniedStorage);
    return;
  }
  uint8_t token[companionpeer::kTokenLen];
  if (!companionpeer::issueToken(peerKey, token)) {
    notifyHelloDenied(pending.helloTag, kDeniedStorage);
    return;
  }
  admitPeer(pending.helloTag, peerKey, token);
}

void setContentFieldCallback(ContentFieldCallback cb) { g_contentCb = cb; }
void setStatusCallback(StatusCallback cb) { g_statusCb = cb; }
void setPairingRequestCallback(PairingRequestCallback cb) { g_pairingCb = cb; }
void setForegroundChangeCallback(ForegroundChangeCallback cb) { g_foregroundCb = cb; }
void setImageStagedCallback(ImageStagedCallback cb) { g_imageStagedCb = cb; }

}  // namespace companionble
