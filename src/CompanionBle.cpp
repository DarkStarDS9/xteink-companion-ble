#include "CompanionBle.h"

#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <NimBLEDevice.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "Memory.h"

namespace companionble {

namespace {

constexpr const char* kServiceUuid = "7c9c0000-3e4a-4b1a-9c1e-6d8a1f2b0001";
constexpr const char* kContentCharUuid = "7c9c0001-3e4a-4b1a-9c1e-6d8a1f2b0001";
constexpr const char* kButtonCharUuid = "7c9c0002-3e4a-4b1a-9c1e-6d8a1f2b0001";
constexpr const char* kCapabilityCharUuid = "7c9c0003-3e4a-4b1a-9c1e-6d8a1f2b0001";
constexpr const char* kStatusCharUuid = "7c9c0004-3e4a-4b1a-9c1e-6d8a1f2b0001";

constexpr uint8_t kOpStart = 0x01;
constexpr uint8_t kOpChunk = 0x02;
constexpr uint8_t kOpEnd = 0x03;

constexpr uint32_t kTeardownDisconnectWaitMs = 600;

// Full advertised name buffer: kDeviceNamePrefix + " " + 4 hex chars (2 bytes
// of the eFuse MAC tail) + NUL. Built once in ensureStarted() from
// ESP.getEfuseMac() so two devices running this firmware advertise distinct
// names (see kDeviceNamePrefix's doc comment in CompanionBle.h).
char g_deviceName[48] = {0};

void buildDeviceName() {
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(g_deviceName, sizeof(g_deviceName), "%s %04X", kDeviceNamePrefix,
           static_cast<unsigned>(mac & 0xFFFF));
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

ContentFieldCallback g_contentCb = nullptr;
StatusCallback g_statusCb = nullptr;

// Last-received content-id blob (see kFieldContentId's doc comment) — not
// routed through g_contentCb, just remembered here so notifyButtonEvent() can
// echo it. Cleared on disconnect (a stray notify before the next reconnect's
// re-push should carry an empty id, not a stale one from a previous session).
uint8_t g_lastContentId[kMaxContentIdLen] = {0};
uint8_t g_lastContentIdLen = 0;

// Reassembly state for the field currently being received (title xor body —
// the protocol doc guarantees "each field is internally ordered" but does not
// interleave title/body chunks). 0 == no field in progress.
uint8_t g_activeField = 0;
uint16_t g_activeTotalLen = 0;
uint16_t g_activeWritten = 0;
bool g_activeFinal = false;  // this field's START carried kFinalFieldFlag
std::unique_ptr<uint8_t[]> g_activeBuf;

void resetReassembly() {
  g_activeField = 0;
  g_activeTotalLen = 0;
  g_activeWritten = 0;
  g_activeFinal = false;
  g_activeBuf.reset();
}

// Capability characteristic value is static for the lifetime of a session —
// computed once in ensureStarted() from the live renderer/font, never hardcoded.
uint8_t g_capabilityValue[5] = {0};

void computeCapabilityValue(const GfxRenderer& renderer, int fontId) {
  const int lineHeight = renderer.getLineHeight(fontId);
  const int advanceWidth = renderer.getTextAdvanceX(fontId, "M", EpdFontFamily::REGULAR);
  const int screenWidthChars = advanceWidth > 0 ? renderer.getScreenWidth() / advanceWidth : 0;
  const int screenHeightChars = lineHeight > 0 ? renderer.getScreenHeight() / lineHeight : 0;

  g_capabilityValue[0] = 4;  // protocol version — v4 adds kFinalFieldFlag (atomic multi-field pushes)
  g_capabilityValue[1] = static_cast<uint8_t>(screenWidthChars > 255 ? 255 : screenWidthChars);
  g_capabilityValue[2] = static_cast<uint8_t>(screenHeightChars > 255 ? 255 : screenHeightChars);
  g_capabilityValue[3] = static_cast<uint8_t>(kMaxFieldLen & 0xFF);
  g_capabilityValue[4] = static_cast<uint8_t>((kMaxFieldLen >> 8) & 0xFF);
}

class ContentCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& /*connInfo*/) override {
    const NimBLEAttValue& value = characteristic->getValue();
    const uint8_t* data = value.data();
    const size_t len = value.size();
    if (len == 0) return;

    const uint8_t opcode = data[0];
    switch (opcode) {
      case kOpStart: {
        if (len < 4) {
          LOG_ERR("CBLE", "START packet too short (%u bytes)", static_cast<unsigned>(len));
          return;
        }
        const uint8_t fieldByte = data[1];
        const bool final = (fieldByte & kFinalFieldFlag) != 0;
        const uint8_t field = fieldByte & kFieldMask;
        if (field != kFieldTitle && field != kFieldBody && field != kFieldContentId) {
          LOG_ERR("CBLE", "START packet unknown field 0x%02x", field);
          return;
        }
        uint16_t totalLen;
        memcpy(&totalLen, data + 2, sizeof(totalLen));  // data may be unaligned
        const uint16_t fieldCap = field == kFieldContentId ? static_cast<uint16_t>(kMaxContentIdLen) : kMaxFieldLen;
        const uint16_t bufLen = totalLen > fieldCap ? fieldCap : totalLen;

        // A new START discards any partial field of the same type (per the
        // protocol doc: "a partial START without a matching END ... is
        // discarded if a new START for the same field arrives").
        resetReassembly();
        g_activeBuf = makeUniqueNoThrow<uint8_t[]>(bufLen == 0 ? 1 : bufLen);
        if (!g_activeBuf) {
          LOG_ERR("CBLE", "OOM allocating %u-byte field buffer", static_cast<unsigned>(bufLen));
          return;
        }
        g_activeField = field;
        g_activeTotalLen = bufLen;
        g_activeWritten = 0;
        g_activeFinal = final;
        break;
      }
      case kOpChunk: {
        if (g_activeField == 0 || !g_activeBuf) return;  // no START in progress: ignore stray chunk
        const uint8_t* payload = data + 1;
        const size_t payloadLen = len - 1;
        const size_t remaining = g_activeTotalLen - g_activeWritten;
        const size_t toCopy = payloadLen < remaining ? payloadLen : remaining;
        if (toCopy > 0) {
          uint8_t* dst = g_activeBuf.get();
          memcpy(dst + g_activeWritten, payload, toCopy);
          g_activeWritten += toCopy;
        }
        break;
      }
      case kOpEnd: {
        if (g_activeField == 0 || !g_activeBuf) return;  // no START in progress: ignore stray end
        if (g_activeField == kFieldContentId) {
          // The id itself is opaque and only needed internally, to echo back
          // from notifyButtonEvent() — but the completed-field callback still
          // fires below (with field == kFieldContentId, no title/body data)
          // so a final-flagged content-id push can commit a pending batch.
          memcpy(g_lastContentId, g_activeBuf.get(), g_activeWritten);
          g_lastContentIdLen = static_cast<uint8_t>(g_activeWritten);
        }
        if (g_contentCb) {
          g_contentCb(g_activeField, g_activeBuf.get(), g_activeWritten, g_activeFinal);
        }
        resetReassembly();
        break;
      }
      default:
        LOG_ERR("CBLE", "unknown content opcode 0x%02x", opcode);
        break;
    }
  }
};

class StatusCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& /*connInfo*/) override {
    const NimBLEAttValue& value = characteristic->getValue();
    if (value.size() == 0) return;
    if (g_statusCb) g_statusCb(value.data()[0]);
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* /*server*/, NimBLEConnInfo& /*connInfo*/) override {
    LOG_DBG("CBLE", "central connected");
  }
  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& /*connInfo*/, int /*reason*/) override {
    LOG_DBG("CBLE", "central disconnected");
    resetReassembly();
    g_lastContentIdLen = 0;  // see g_lastContentId's doc comment
    // Only one central at a time (see docs/companion-display-protocol.md) — resume
    // advertising so a reconnect (or a fresh phone) can pair without a full restart.
    if (g_begun && server) server->startAdvertising();
  }
};

ContentCharCallbacks g_contentCharCallbacks;
StatusCharCallbacks g_statusCharCallbacks;
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
  // force a from-source Arduino core rebuild for this).
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

  g_contentChar = service->createCharacteristic(
      kContentCharUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  g_contentChar->setCallbacks(&g_contentCharCallbacks);

  g_buttonChar = service->createCharacteristic(kButtonCharUuid, NIMBLE_PROPERTY::NOTIFY);

  g_capabilityChar = service->createCharacteristic(kCapabilityCharUuid, NIMBLE_PROPERTY::READ);
  g_capabilityChar->setValue(g_capabilityValue, sizeof(g_capabilityValue));

  g_statusChar = service->createCharacteristic(kStatusCharUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  g_statusChar->setCallbacks(&g_statusCharCallbacks);

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
    LOG_DBG("CBLE", "advertising: start()=%d isAdvertising()=%d advPayloadLen=%u adv=%s",
            advStarted ? 1 : 0, advertising->isAdvertising() ? 1 : 0, static_cast<unsigned>(payload.size()), hex);
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
  g_server = nullptr;
  resetReassembly();

  NimBLEDevice::deinit(true);
  if (NimBLEDevice::isInitialized()) {
    vTaskDelay(pdMS_TO_TICKS(50));
    NimBLEDevice::deinit(true);
  }

  g_powerLock.reset();
}

bool isConnected() { return g_begun && g_server && g_server->getConnectedCount() > 0; }

bool notifyButtonEvent(ButtonEvent event) {
  if (!isConnected() || !g_buttonChar) return false;
  uint8_t payload[1 + kMaxContentIdLen];
  payload[0] = static_cast<uint8_t>(event);
  if (g_lastContentIdLen > 0) {
    memcpy(payload + 1, g_lastContentId, g_lastContentIdLen);
  }
  g_buttonChar->setValue(payload, 1 + g_lastContentIdLen);
  return g_buttonChar->notify();
}

void setContentFieldCallback(ContentFieldCallback cb) { g_contentCb = cb; }

void setStatusCallback(StatusCallback cb) { g_statusCb = cb; }

}  // namespace companionble
