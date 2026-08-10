#include "CompanionBle.h"

#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

#include "CompanionConnPolicy.h"
#include "CompanionPeerStore.h"
#include "CompanionUiDeclaration.h"
#include "Epub/converters/RawBitmapToFramebufferConverter.h"
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
// v9-v10: IMAGE_STATUS, a 3-byte {opcode, sessionId, result} answer to an
// image push only. v11 widens this to RENDER_STATUS -- a 4th `field` byte so
// the same opcode can also answer a text push (see notifyRenderStatus()'s doc
// comment in CompanionBle.h) -- without a second opcode, per this feature's
// commit message. The opcode value is unchanged; only clients that parse a
// fixed 3-byte payload need to know about the new byte, which is exactly why
// this is a version bump.
constexpr uint8_t kSessRenderStatus = 0x88;
// v9: progress marker for an in-flight image push sent over Write Without
// Response, so the phone can detect a stalled/diverged transfer well before
// the final RENDER_STATUS -- see kOpChunk's image branch below.
constexpr uint8_t kSessImageChunkAck = 0x89;
// v10: the title/body CHUNK sequence number (see kOpChunk's seq-checked
// branch below) skipped ahead of what was expected -- a packet was lost or
// reordered under Write Without Response. Unlike the image field there is no
// mid-transfer ack (title/body pushes are a handful of chunks, not hundreds),
// so this is the only signal the phone ever gets that a text push landed
// corrupt; the device drops the field rather than displaying garbage.
constexpr uint8_t kSessFieldSeqGap = 0x8A;

// How often (in chunks) to send kSessImageChunkAck during an image push. Not
// flow control -- iOS's own canSendWriteWithoutResponse/
// peripheralIsReadyToSendWriteWithoutResponse already throttles the sender at
// the OS level -- this is purely so the phone learns the device is still
// alive and in sync well before the ~25s transfer's final RENDER_STATUS,
// letting it abort early instead of always waiting for the whole push to
// finish before finding out it was corrupt.
constexpr uint16_t kImageChunkAckInterval = 32;

constexpr uint8_t kDeniedUserRejected = 0x00;
constexpr uint8_t kDeniedTimeout = 0x01;
constexpr uint8_t kDeniedNoSlots = 0x02;
constexpr uint8_t kDeniedMalformed = 0x03;
constexpr uint8_t kDeniedStorage = 0x04;
constexpr uint8_t kDeniedBusy = 0x05;
// v12: the client's own declared protocolVersion (HELLO's first field after
// helloTag) does not match kProtocolVersion. Distinct from kDeniedMalformed on
// purpose -- a length-valid HELLO from a genuinely different protocol version
// is not corrupt, it is just talking to the wrong device generation, and a
// client is owed that distinction the same way RejectedNoShape (see
// docs/companion-declared-shape-design.md section 3) used to try to infer it
// after the fact from a v11-shaped UI declaration. This replaces that
// inference: a client's version is now a fact the device is told, not a shape
// it has to guess from a buffer that is missing a byte.
constexpr uint8_t kDeniedProtocolMismatch = 0x06;

constexpr uint8_t kAcquireDeniedNoButtonMap = 0x00;
constexpr uint8_t kAcquireDeniedUnknownSession = 0x01;

constexpr uint32_t kTeardownDisconnectWaitMs = 600;

// Name of the staged image inside the pushing peer's data/ directory. Per-peer
// rather than one global scratch file, so two apps staging at once cannot
// collide — the answer to the image sketch's "where does the staging file go".
// ".raw" because field 0x04 is now raw packed 2bpp, not PNG — see
// RawBitmapToFramebufferConverter and docs/companion-display-protocol.md.
constexpr const char* kStagedImageName = "incoming.raw";

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
//
// NOT removed by platformio.ini's new CONFIG_BT_CTRL_MODEM_SLEEP block (see
// that file's comment). Modem sleep is a BT-controller radio-power state
// (the controller stops the radio between BLE events, still clocked off the
// main XTAL) and needs no esp_pm lock coordination and no CPU-frequency
// change to work -- CONFIG_PM_ENABLE is deliberately NOT enabled here (see
// platformio.ini: it doesn't link against this project's prebuilt
// libfreertos.a). So nothing about this rollout touches the CPU-frequency
// mechanism this lock exists to hold steady; the hang this guards against is
// exactly as reachable as before, and this lock is unambiguously still
// required. If a future change does bring in esp_pm/tickless idle, re-read
// this comment before assuming that supersedes it -- see the git history for
// the fuller reasoning that was here when this rollout still attempted that.
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
// Connection interval / peripheral latency
// ---------------------------------------------------------------------------
//
// The profile decision itself -- Session vs Idle, the Idle holdoff, grant
// matching (a grant anywhere inside the requested min/max range counts as
// compliance), deferred-request retry after kConnParamsGraceMs -- lives in
// CompanionConnPolicy; see that header for the full envelope reasoning
// (Apple accessory-design-guidelines checks, the bursty-traffic latency
// measurement that reverted Session off latency 10, the dropped-request
// history that split desired/current/requested into three variables). This
// file just wires it to NimBLE: forward the ParamRequest from step()/
// onConnect() to NimBLEServer::updateConnParams(), and feed onGrant() from
// NimBLEServerCallbacks::onConnParamsUpdate().
//
// This is transport tuning, not a protocol change: no wire field, byte
// layout, or characteristic is affected, so it carries no version bump.
CompanionConnPolicy g_connPolicy;

const char* connProfileName(CompanionConnPolicy::ConnProfile profile) {
  switch (profile) {
    case CompanionConnPolicy::ConnProfile::Session:
      return "session";
    case CompanionConnPolicy::ConnProfile::Idle:
      return "idle";
  }
  return "?";
}

// millis() of the last write/notify "activity" edge. No longer drives any
// profile decision -- kept because five call sites already mark this edge and
// a future feature may want "time since last BLE write". Nothing currently
// reads it; the only reader was the old Busy/Near/Deep ladder's idle-relax
// timer in tick(), which a much earlier change deleted along with the ladder
// itself.
uint32_t g_lastBleActivityMs = 0;

// DIAGNOSTIC (2026-08-03 link-robustness investigation): millis() at the last
// onConnect, so every link-layer log line can carry "t=+Nms into this
// connection". This is what turns "it disconnects every ~43 seconds" from a
// stopwatch observation into a measured number correlated with the profile
// churn and the granted parameters. Remove with the rest of this
// investigation's instrumentation.
uint32_t g_connectMs = 0;
uint32_t connUptimeMs() { return g_connectMs == 0 ? 0 : millis() - g_connectMs; }

// Serialization state for the connect-time procedure chain: conn params,
// then PHY, then Data Length Extension, one LLCP procedure at a time (Core
// Spec Vol 6 Part B §5.3) -- see onConnect()/onConnParamsUpdate()/
// onPhyUpdate() below, and ca5e518a for what firing these concurrently used
// to cost. Reset at the top of every onConnect(); read only within that same
// connection's lifetime.
bool g_phyRequestedThisConn = false;
bool g_dleRequestedThisConn = false;

// Issues g_connPolicy's pending request, if any, against the live link.
// Safe and cheap to call repeatedly -- tick() does exactly that, which is
// what turns a deferred request (one CompanionConnPolicy::step() held back
// because another was still in flight) into a retried one.
void applyConnPolicyRequest(const std::optional<CompanionConnPolicy::ParamRequest>& request) {
  if (!request || !g_server || g_server->getConnectedCount() == 0) return;
  const auto peer = g_server->getPeerInfo(0);
  g_server->updateConnParams(peer.getConnHandle(), request->intervalUnits, request->intervalMaxUnits,
                              request->latencyUnits, request->timeoutUnits);
  LOG_DBG("CBLE", "t=+%lums requested %s conn params (interval=%u-%u latency=%u timeout=%u)",
          static_cast<unsigned long>(connUptimeMs()), connProfileName(request->profile),
          static_cast<unsigned>(request->intervalUnits), static_cast<unsigned>(request->intervalMaxUnits),
          static_cast<unsigned>(request->latencyUnits), static_cast<unsigned>(request->timeoutUnits));
}

// DIAGNOSTIC: the last parameters the central actually granted, independent
// of whether they matched a request -- CompanionConnPolicy::divergenceDue()'s
// log line wants "what the link is actually at", and by the time that fires
// the NimBLEConnInfo that reported it is long gone.
uint16_t g_lastGrantedIntervalUnits = 0;
uint16_t g_lastGrantedLatency = 0;
uint16_t g_lastGrantedTimeoutUnits = 0;

// Called from every characteristic write and outgoing notify -- i.e. anything
// that means a phone app is actively driving the link right now. Used to
// force the link to the Session profile on this edge; no longer does, now
// that the profile is chosen purely by session state (see
// CompanionConnPolicy) and never renegotiated on a per-write/per-notify
// basis. Left as a timestamp store only -- see g_lastBleActivityMs's own
// comment for why it is kept despite having no current reader.
void noteBleActivity() { g_lastBleActivityMs = millis(); }

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
  // This peer's declared content shape (companionble::ContentShape), cached
  // from SD at ACQUIRE and refreshed when the peer re-pushes its declaration.
  // The cache is the whole point: see docs/companion-declared-shape-design.md
  // §5 -- consulting SD per push would put an open on the BLE host task for
  // every field of every push.
  //
  // A raw byte rather than ContentShape because 0 is not a legal shape and has
  // to mean "not known yet": allocateSession() zero-initialises, and an enum
  // with no zero enumerator would make that state unnameable.
  uint8_t declaredShape = 0;
};

Session g_sessions[kMaxSessions];
uint8_t g_foreground = kNoSession;

// Feeds g_connPolicy the one thing it needs from session state: whether some
// app currently holds the screen. Level-triggered (see
// CompanionConnPolicy::setForegroundActive()'s doc comment) so tick() can
// simply call this every time rather than needing an edge-triggered hook at
// every g_foreground writer (setForeground(), dropSession(), onConnect(),
// onDisconnect()). Given how rarely g_foreground itself changes, the policy
// ends up issuing at most a couple of real updateConnParams() calls per
// connection, never one per article.
void syncConnPolicyForegroundState() { g_connPolicy.setForegroundActive(g_foreground != kNoSession, millis()); }

// Pending on-screen pairing prompt. Exactly one at a time — a second prompt
// stacked behind the first would leave the user confirming an app they can no
// longer see the name of.
struct PendingPairing {
  bool active = false;
  uint16_t helloTag = 0;
  uint8_t appId[companionpeer::kIdLen] = {0};
  uint8_t installId[companionpeer::kIdLen] = {0};
  char name[companionpeer::kMaxNameLen + 1] = {0};
  char userName[companionpeer::kMaxNameLen + 1] = {0};
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

// notify(data, len) builds and queues its own buffer immediately
// (ble_gattc_notify_custom), unlike the no-arg notify(), which defers to
// ble_gatts_chr_updated() and reads whatever setValue() last wrote when the
// host task eventually drains it — back-to-back calls would race and the
// earlier payload could be overwritten before it is ever sent.
void notifySession(const uint8_t* data, size_t len) {
  if (!g_sessionChar) return;
  g_sessionChar->notify(data, len);
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

  payload[offset++] = kFieldUiDeclaration;
  companionpeer::assetTag(peerKey, companionpeer::kAssetUiDeclaration, payload + offset);
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
    companionpeer::assetTag(
        peerKey, assetId == kFieldIcon ? companionpeer::kAssetIcon : companionpeer::kAssetUiDeclaration, payload + 4);
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
//
// The actual SD write/flush/close for the image field do NOT happen inline
// here, though -- see "Image write-behind task" below. This block only holds
// bookkeeping that's cheap enough to touch straight from the BLE host task.
uint8_t g_activeField = 0;
uint8_t g_activeSession = kNoSession;
uint32_t g_activeTotalLen = 0;
uint32_t g_activeWritten = 0;
bool g_activeFinal = false;
std::unique_ptr<uint8_t[]> g_activeBuf;
// Set at START when a field whose reassembly is bounded by a RAM cap (the
// image field's SD-backed cap, and, since v12's part 3, the list-document
// field's heap cap) arrives with more bytes declared than that cap allows.
// Shared between the two rather than a second parallel flag -- both need the
// exact same lifecycle: refuse the bytes outright rather than silently
// truncating them (title/body's own cap enforcement below does truncate,
// which is fine for them but would be a silent-data-loss bug for a document
// whose truncation could land on a structural boundary and parse as a
// shorter-but-valid document missing items), latch here because START
// carries no pushId, and answer RejectedSize once END's pushId exists. Only
// one field is ever mid-reassembly at a time (g_activeField), so one flag
// unambiguously describes whichever one that is.
bool g_activeOverflow = false;
// v12: set at START when the field being pushed is not one the foreground
// peer's declared content shape permits (see fieldMatchesShape() below). The
// transfer is then consumed and thrown away -- no buffer is allocated, CHUNKs
// no-op -- and END answers RejectedShape. Latched rather than answered on the
// spot for the same reason g_activeOverflow is: START carries no pushId,
// it arrives only on END.
bool g_activeShapeRejected = false;
// Set once handing image work to the writer task fails (queue full/missing,
// see enqueueImageWork()) -- once true, further CHUNKs for this transfer are
// dropped without retrying (a retry loop here would just re-introduce the
// blocking-host-task problem this task exists to avoid), and END reports
// StorageFailed immediately rather than waiting on a message the writer task
// may process very late, if ever.
bool g_activeImageFailed = false;
// v9+: image CHUNKs carry a 2-byte sequence number (see kOpChunk below) so a
// dropped/reordered chunk under Write Without Response is detected instead of
// silently corrupting the reassembled payload. v10 extends this to the
// title/body fields (also pushed over Write Without Response as of v10) --
// shared rather than duplicated per field because only one field is ever
// being reassembled at a time (see g_activeField). g_activeSeq is the next
// expected value, reset to 0 at START (via resetReassembly()); g_activeSeqGap
// latches once a mismatch is seen. Image reports this distinctly from
// StorageFailed via RENDER_STATUS; title/body report it via
// kSessFieldSeqGap and drop the field instead of displaying garbage (and, if
// the field was part of an atomic batch, also via RENDER_STATUS/SequenceGap
// once the whole batch is discarded -- see CompanionModeActivity.cpp).
uint16_t g_activeSeq = 0;
bool g_activeSeqGap = false;
// Wall-clock span of the image CHUNK sequence only -- set at START, read at
// END -- so BLE transfer time can be told apart from decode/settle time
// without needing a host-side script to measure it. Deliberately computed
// here on the host task rather than in the writer task, so a slow flush/close
// or a backlog of still-queued CHUNKs never inflates this number.
uint32_t g_imageTransferStartMs = 0;
// Same idea for the text fields, which had no timing at all until now: every
// instrument above is gated on kFieldImage, so an image push could be
// attributed down to the individual SD write while a title/body push was a
// black box between "phone says it sent" and "panel shows it". That is the
// half the news-companion workload actually lives in.
uint32_t g_textTransferStartMs = 0;

// Temporary instrumentation: per-CHUNK timing taken directly in
// ContentCharCallbacks::onWrite() (host task), to tell apart "peripheral is
// slow to return from onWrite" (our bug -- e.g. blocking on a full write
// queue) from "the link only delivers one write per N connection events"
// (central scheduling, nothing this task can fix). gap = wall-clock between
// one CHUNK's onWrite() entry and the next's, dominated by the ATT
// write-with-response round trip if onWrite itself is fast. busy = time
// onWrite() itself takes to return for a successful CHUNK -- normally just
// enqueueImageWork()'s memcpy onto the queue, occasionally its brief
// full-queue wait (see kImageEnqueueTimeoutTicks). Reset at START, dumped as
// a histogram at END alongside the writer-task counters above.
// TODO remove once the real bottleneck is identified.
uint32_t g_chunkGapLastEntryMs = 0;
uint32_t g_chunkGapMinMs = UINT32_MAX;
uint32_t g_chunkGapMaxMs = 0;
uint32_t g_chunkGapSumMs = 0;
uint32_t g_chunkGapCount = 0;
uint32_t g_chunkBusyMaxMs = 0;
uint32_t g_chunkBusySumMs = 0;
uint32_t g_chunkBusyCount = 0;
// Bucket upper bounds in ms; last bucket catches everything >= 100ms.
constexpr uint32_t kChunkGapBucketBoundsMs[7] = {5, 10, 15, 20, 30, 50, 100};
uint32_t g_chunkGapHistogram[8] = {0};

void resetChunkTiming() {
  g_chunkGapLastEntryMs = 0;
  g_chunkGapMinMs = UINT32_MAX;
  g_chunkGapMaxMs = 0;
  g_chunkGapSumMs = 0;
  g_chunkGapCount = 0;
  g_chunkBusyMaxMs = 0;
  g_chunkBusySumMs = 0;
  g_chunkBusyCount = 0;
  memset(g_chunkGapHistogram, 0, sizeof(g_chunkGapHistogram));
}

void recordChunkGap(uint32_t entryMs) {
  if (g_chunkGapLastEntryMs != 0) {
    const uint32_t gapMs = entryMs - g_chunkGapLastEntryMs;
    g_chunkGapSumMs += gapMs;
    ++g_chunkGapCount;
    if (gapMs < g_chunkGapMinMs) g_chunkGapMinMs = gapMs;
    if (gapMs > g_chunkGapMaxMs) g_chunkGapMaxMs = gapMs;
    size_t bucket = 7;
    for (size_t i = 0; i < 7; ++i) {
      if (gapMs < kChunkGapBucketBoundsMs[i]) {
        bucket = i;
        break;
      }
    }
    ++g_chunkGapHistogram[bucket];
  }
  g_chunkGapLastEntryMs = entryMs;
}

void recordChunkBusy(uint32_t entryMs) {
  const uint32_t busyMs = millis() - entryMs;
  g_chunkBusySumMs += busyMs;
  ++g_chunkBusyCount;
  if (busyMs > g_chunkBusyMaxMs) g_chunkBusyMaxMs = busyMs;
}

void logChunkTiming() {
  if (g_chunkGapCount > 0) {
    LOG_DBG("CBLE",
            "chunk gap: min=%ums max=%ums avg=%ums (n=%u) buckets"
            " <5/<10/<15/<20/<30/<50/<100/>=100ms = %u/%u/%u/%u/%u/%u/%u/%u",
            static_cast<unsigned>(g_chunkGapMinMs), static_cast<unsigned>(g_chunkGapMaxMs),
            static_cast<unsigned>(g_chunkGapSumMs / g_chunkGapCount), static_cast<unsigned>(g_chunkGapCount),
            static_cast<unsigned>(g_chunkGapHistogram[0]), static_cast<unsigned>(g_chunkGapHistogram[1]),
            static_cast<unsigned>(g_chunkGapHistogram[2]), static_cast<unsigned>(g_chunkGapHistogram[3]),
            static_cast<unsigned>(g_chunkGapHistogram[4]), static_cast<unsigned>(g_chunkGapHistogram[5]),
            static_cast<unsigned>(g_chunkGapHistogram[6]), static_cast<unsigned>(g_chunkGapHistogram[7]));
  }
  if (g_chunkBusyCount > 0) {
    LOG_DBG("CBLE", "chunk onWrite busy: max=%ums avg=%ums (n=%u)", static_cast<unsigned>(g_chunkBusyMaxMs),
            static_cast<unsigned>(g_chunkBusySumMs / g_chunkBusyCount), static_cast<unsigned>(g_chunkBusyCount));
  }
}

// ---------------------------------------------------------------------------
// Image write-behind task
// ---------------------------------------------------------------------------
//
// ContentCharCallbacks::onWrite() runs on the NimBLE host task. ESP32-C3 is
// single-core, so any blocking call made inline there -- SD/SPI I/O very much
// included, since it is neither fast nor bounded -- can make the host task
// miss its own scheduled BLE radio events. At the ~150ms connection interval
// this link used to run at there was enough slack for that to go unnoticed;
// at the 15-30ms interval both profiles now use (see the ConnProfile
// constants above) there usually isn't, and an occasionally-slow write -- or
// the reliably-slower end-of-image flush/close -- caused real disconnects on
// real hardware.
//
// So none of the image staging file's open/write/flush/close calls happen on
// the host task any more. onWrite() only ever copies a small fixed-size
// message onto a queue (occasionally waiting briefly for room -- see
// enqueueImageWork()'s kImageEnqueueTimeoutTicks -- but never doing SD/SPI
// I/O itself) and this dedicated task drains that queue and does the actual
// SD I/O.
//
// g_activeImageFile/g_activeImagePath/g_activeImagePeerKey are, from here on,
// touched by the writer task ONLY -- never by the host task -- which is what
// makes this safe without wrapping the HalFile itself in a mutex. (HalStorage
// already serializes SD/SPI access against other SD users elsewhere in the
// firmware, e.g. EPUB/cover reads; that says nothing about two tasks sharing
// one open HalFile, which single ownership avoids entirely.)
HalFile g_activeImageFile;
std::string g_activeImagePath;
char g_activeImagePeerKey[companionpeer::kPeerKeyLen] = {0};

// Worst-case CHUNK payload: NimBLE's own compiled ATT MTU ceiling
// (BLE_ATT_MTU_MAX == 527, nimble/nimble/host/include/host/ble_att.h) --
// deliberately NOT the 185 NimBLEDevice::setMTU() requests in ensureStarted().
// That call only sets NimBLE's own *initiating* preference; a central that
// initiates the MTU exchange itself (which CoreBluetooth does) negotiates
// min(both sides' actual capability), not capped at what we asked for.
// Confirmed on real hardware: an iPhone negotiated an ATT MTU around 515,
// nearly 3x the 185 this buffer used to assume, and every CHUNK was
// rejected outright ("payload N exceeds max 180") the instant a real
// high-MTU central pushed an image -- hard-failing every transfer with
// storageFailed after streaming the whole thing for nothing.
// 3 bytes of ATT protocol overhead, 2-byte opcode+sessionId header stripped
// in onWrite's kOpChunk case, subtracted from NimBLE's true ceiling.
constexpr size_t kMaxImageChunkPayload = 527 - 3 - 2;

enum class ImageWorkType : uint8_t { Open, Chunk, End, Abort };

// Fixed-size and POD (no heap pointers) -- safe and cheap to copy by value
// through a FreeRTOS queue. Only `type` plus the fields that type actually
// uses are meaningful; the rest are simply unused for a given message.
struct ImageWorkMsg {
  ImageWorkType type = ImageWorkType::Abort;
  uint16_t len = 0;                                // Chunk: valid bytes in data[]
  uint8_t data[kMaxImageChunkPayload] = {0};       // Chunk: payload
  char peerKey[companionpeer::kPeerKeyLen] = {0};  // Open: whose staging file to open
  uint8_t contentId[kMaxContentIdLen] = {0};       // End: session's content-id, copied here
  uint8_t contentIdLen = 0;                        // (host task) before enqueueing so a later
                                                   // session-table reset can't race it
  uint8_t pushId = 0;                              // End: this transfer's END byte 2, carried
                                                   // through to the eventual RENDER_STATUS --
                                                   // see notifyRenderStatus()'s doc comment.
};

// Sized to smooth over the writer task falling behind briefly (a slow SD
// write, or a GC/wear-leveling pause on the card) without the host task
// needing to wait at all. Kept at 10 (rather than the 24 this started at)
// now that kMaxImageChunkPayload reflects NimBLE's real ~522-byte ceiling
// instead of an assumed 180 -- 10 * sizeof(ImageWorkMsg) lands close to the
// original queue's total footprint (a few KB), affordable against the
// ~34KB of free heap this feature typically runs with (see ensureStarted()'s
// heap-cost comment).
//
// It does NOT cover a sustained gap between incoming and drain rate -- a
// fast sender can outpace the SD card for an entire transfer, not just a
// brief stall, and no queue depth fixes that (the image is ~100KB; buffering
// the whole thing in RAM instead of streaming to SD is exactly what the
// heap-cost comment above rules out). That case is handled by
// enqueueImageWork() blocking the host task briefly when the queue is full
// (safe -- see its comment), which throttles the incoming rate down to
// whatever the writer can actually sustain instead of failing the transfer.
constexpr UBaseType_t kImageWriteQueueLen = 10;

QueueHandle_t g_imageWriteQueue = nullptr;
TaskHandle_t g_imageWriteTaskHandle = nullptr;

// Writer-task-only: same job the old discardStagedImage() did, just run from
// the task that now exclusively owns g_activeImageFile/g_activeImagePath.
void writerDiscardStagedImage() {
  if (g_activeImageFile.isOpen()) g_activeImageFile.close();
  if (!g_activeImagePath.empty()) {
    // A partial image is not decodable and would sit on the card until the
    // next push overwrote it. Drop it rather than leave a trap for the
    // decoder.
    Storage.remove(g_activeImagePath.c_str());
    g_activeImagePath.clear();
  }
}

// Temporary instrumentation: attributes wall-clock time in the writer loop to
// either the SD write() call itself or everything else (queue wait,
// scheduling latency), to find out where a slow transfer is actually
// spending its time. TODO remove once the real bottleneck is identified.
uint32_t g_writerChunkCount = 0;
uint32_t g_writerWriteBusyMs = 0;
uint32_t g_writerFirstChunkMs = 0;

void imageWriteTaskLoop(void* /*param*/) {
  for (;;) {
    ImageWorkMsg msg;
    if (xQueueReceive(g_imageWriteQueue, &msg, portMAX_DELAY) != pdTRUE) continue;

    switch (msg.type) {
      case ImageWorkType::Open: {
        // Belt-and-suspenders: a prior transfer's Abort should always have
        // closed this already, but if that Abort itself was ever dropped
        // (e.g. it lost the race against a full queue), don't leak/overwrite
        // a still-open handle out from under the SD layer.
        if (g_activeImageFile.isOpen()) writerDiscardStagedImage();
        memcpy(g_activeImagePeerKey, msg.peerKey, sizeof(g_activeImagePeerKey));
        g_activeImagePath = companionpeer::dataFilePath(g_activeImagePeerKey, kStagedImageName);
        if (!Storage.openFileForWrite("CBLE", g_activeImagePath, g_activeImageFile)) {
          LOG_ERR("CBLE", "could not open %s for image staging", g_activeImagePath.c_str());
          g_activeImagePath.clear();
        }
        g_writerChunkCount = 0;
        g_writerWriteBusyMs = 0;
        g_writerFirstChunkMs = 0;
        break;
      }

      case ImageWorkType::Chunk: {
        if (!g_activeImageFile.isOpen()) break;  // already failed/aborted -- drop silently
        if (g_writerFirstChunkMs == 0) g_writerFirstChunkMs = millis();
        const uint32_t writeStartMs = millis();
        const bool ok = g_activeImageFile.write(msg.data, msg.len) == msg.len;
        g_writerWriteBusyMs += millis() - writeStartMs;
        ++g_writerChunkCount;
        if (!ok) {
          LOG_ERR("CBLE", "SD write failed staging image");
          writerDiscardStagedImage();
        }
        break;
      }

      case ImageWorkType::End: {
        if (g_writerChunkCount > 0) {
          const uint32_t spanMs = millis() - g_writerFirstChunkMs;
          LOG_DBG("CBLE", "writer: %u chunks, %u ms write()-busy, %u ms span (%u%% busy)",
                  static_cast<unsigned>(g_writerChunkCount), static_cast<unsigned>(g_writerWriteBusyMs),
                  static_cast<unsigned>(spanMs),
                  static_cast<unsigned>(spanMs > 0 ? (g_writerWriteBusyMs * 100UL / spanMs) : 0));
        }
        if (!g_activeImageFile.isOpen()) {
          notifyRenderStatus(RenderResult::StorageFailed, msg.pushId);
        } else {
          const uint32_t flushStartMs = millis();
          g_activeImageFile.flush();
          g_activeImageFile.close();
          LOG_DBG("CBLE", "writer: flush()+close() took %u ms", static_cast<unsigned>(millis() - flushStartMs));
          // Decoding touches the framebuffer, so it happens on the main loop
          // task, not here. The activity answers with notifyRenderStatus().
          // onImageStaged() (CompanionModeActivity.cpp) only copies
          // fixed-size buffers under its own critical section, so calling it
          // from this task rather than the host task is safe.
          if (g_imageStagedCb) {
            g_imageStagedCb(g_activeImagePeerKey, g_activeImagePath.c_str(), msg.contentId, msg.contentIdLen,
                            msg.pushId);
          }
          g_activeImagePath.clear();  // ownership passes to the activity
        }
        break;
      }

      case ImageWorkType::Abort:
        writerDiscardStagedImage();
        break;
    }
  }
}

void ensureImageWriteTaskStarted() {
  if (g_imageWriteTaskHandle) return;
  g_imageWriteQueue = xQueueCreate(kImageWriteQueueLen, sizeof(ImageWorkMsg));
  if (!g_imageWriteQueue) {
    LOG_ERR("CBLE", "failed to create image write queue");
    return;
  }
  // Priority 2, matching InputManager::beginAsync()'s default for a small
  // dedicated I/O task: above the main render/loop task's default priority
  // (ActivityManager's render task runs at 1) so queued SD work is drained
  // promptly, but well below the NimBLE host/controller tasks so this task
  // never competes with actual radio servicing.
  const BaseType_t created = xTaskCreate(&imageWriteTaskLoop, "cble_imgw", 4096, nullptr, 2, &g_imageWriteTaskHandle);
  if (created != pdPASS) {
    LOG_ERR("CBLE", "failed to create image write task");
    vQueueDelete(g_imageWriteQueue);
    g_imageWriteQueue = nullptr;
    g_imageWriteTaskHandle = nullptr;
  }
}

// A real sender (e.g. iOS, which paces writes far tighter than the queue
// depth assumed) can sustain a higher incoming rate than the SD card can
// absorb -- not just the occasional slow write/GC pause this queue was sized
// for, but a persistent gap between incoming and drain rate for the whole
// transfer. Without backpressure that gap fills the queue in well under a
// second and fails the transfer outright.
//
// So this blocks, briefly, when full -- which is safe here in a way the old
// inline SD call was not: xQueueSend() blocking suspends the calling task on
// a semaphore wait (zero CPU, fully preemptible), letting the NimBLE
// host/controller tasks run and service the radio on schedule regardless of
// how long the wait takes. That is categorically different from blocking
// inside an uninterruptible SPI transaction, which is what actually caused
// missed radio events before. The bound below only guards against a
// genuinely stuck writer task (e.g. a wedged SD card) rather than normal
// backlog draining.
constexpr TickType_t kImageEnqueueTimeoutTicks = pdMS_TO_TICKS(4000);

bool enqueueImageWork(const ImageWorkMsg& msg, const char* context) {
  if (!g_imageWriteQueue || xQueueSend(g_imageWriteQueue, &msg, kImageEnqueueTimeoutTicks) != pdTRUE) {
    LOG_ERR("CBLE", "image write queue full/unavailable (%s)", context);
    return false;
  }
  return true;
}

// Tears down whatever the writer task has in flight for the current transfer.
// Always safe to call (a no-op if nothing is open/pending) -- used both for a
// real mid-transfer abort (disconnect, a new session taking the screen, a
// START overriding an unfinished transfer) and, harmlessly, once more after
// every clean END, mirroring the old discardStagedImage()-is-a-no-op-after-a-
// clean-finish idiom this replaces.
void enqueueImageAbort() {
  if (!g_imageWriteQueue) return;
  ImageWorkMsg msg;
  msg.type = ImageWorkType::Abort;
  enqueueImageWork(msg, "abort");
}

void resetReassembly() {
  if (g_activeField == kFieldImage) enqueueImageAbort();
  g_activeField = 0;
  g_activeSession = kNoSession;
  g_activeTotalLen = 0;
  g_activeWritten = 0;
  g_activeFinal = false;
  g_activeOverflow = false;
  g_activeShapeRejected = false;
  g_activeImageFailed = false;
  g_activeSeq = 0;
  g_activeSeqGap = false;
  g_activeBuf.reset();
}

// v10: which fields go out over Write Without Response and therefore need
// CHUNK-level sequence-gap detection -- title/body joined the image field in
// v10 (see docs/companion-display-protocol.md "Text fields"). Every other
// field stays on ordinary Write, which already guarantees delivery order.
bool fieldUsesSeqChunk(uint8_t field) { return field == kFieldImage || field == kFieldTitle || field == kFieldBody; }

uint32_t fieldCap(uint8_t field) {
  switch (field) {
    case kFieldContentId:
      return kMaxContentIdLen;
    case kFieldImage:
      return kMaxImageFieldLen;
    case kFieldUiDeclaration:
      return companionpeer::kMaxUiDeclarationLen;
    case kFieldIcon:
      return 4 + kIconBytes;
    case kFieldTagState:
      // 1 count byte + kMaxTags x { tagId, state }.
      return 1 + 2 * kMaxTags;
    case kFieldListDoc:
      return kMaxListDocLen;
    default:
      return kMaxFieldLen;
  }
}

bool isKnownField(uint8_t field) {
  return field == kFieldTitle || field == kFieldBody || field == kFieldContentId || field == kFieldImage ||
         field == kFieldUiDeclaration || field == kFieldIcon || field == kFieldTagState || field == kFieldListDoc;
}

// v12's permitted-field table is companionui::fieldMatchesShape(), one file
// over: it is a pure lookup with no NimBLE dependency, and it lives beside the
// parser that produces the shape in the first place so a host gtest can reach
// it. This translation unit cannot be host-built at all, which is the same
// argument that moved parseBody() out of CompanionPeerStore.cpp.

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
  g_capabilityValue[offset++] = kProtocolVersion;
  g_capabilityValue[offset++] = static_cast<uint8_t>(screenWidthChars > 255 ? 255 : screenWidthChars);
  g_capabilityValue[offset++] = static_cast<uint8_t>(screenHeightChars > 255 ? 255 : screenHeightChars);
  g_capabilityValue[offset++] = static_cast<uint8_t>(kMaxFieldLen & 0xFF);
  g_capabilityValue[offset++] = static_cast<uint8_t>((kMaxFieldLen >> 8) & 0xFF);
  // bit0 image, bit1 button map, bit2 icons, bit3 sessions,
  // bit4 declared content shape (v12)
  g_capabilityValue[offset++] = 0x1F;
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

  // The field 0x04 image decoder has no header of its own (see
  // RawBitmapToFramebufferConverter) — its expected file size is derived
  // from these same screenWidthPx/screenHeightPx values, so it must learn
  // them from the one place they're computed rather than guessing.
  RawBitmapToFramebufferConverter::setScreenDimensions(screenWidthPx, screenHeightPx);
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
  // opcode(1) helloTag(2) protocolVersion(1) appId(16) installId(16) tokenLen(1)
  // token[..] nameLen(1) name[..] userNameLen(1) userName[..]
  if (len < 1 + 2 + 1) {
    notifyHelloDenied(0, kDeniedMalformed);
    return;
  }
  const uint16_t helloTag = static_cast<uint16_t>(data[1] | (data[2] << 8));

  // v12: checked before anything past it is trusted. This field's wire offset
  // is fixed for good -- every future protocol version keeps it right here,
  // straight after helloTag -- specifically so a mismatch can always be caught
  // this early, before assuming the rest of the payload is laid out the way
  // this build expects. Replaces inferring a stale client after the fact from
  // a v11-shaped UI declaration (the old RejectedNoShape path); see
  // kDeniedProtocolMismatch's comment.
  const uint8_t protocolVersion = data[3];
  if (protocolVersion != kProtocolVersion) {
    notifyHelloDenied(helloTag, kDeniedProtocolMismatch);
    return;
  }

  if (len < 1 + 2 + 1 + 32 + 1) {
    notifyHelloDenied(helloTag, kDeniedMalformed);
    return;
  }
  const uint8_t* appId = data + 4;
  const uint8_t* installId = data + 20;
  const uint8_t tokenLen = data[36];
  if (len < 37u + tokenLen + 1u) {
    notifyHelloDenied(helloTag, kDeniedMalformed);
    return;
  }
  const uint8_t* token = data + 37;
  const uint8_t nameLen = data[37 + tokenLen];
  if (len < 38u + tokenLen + nameLen + 1u) {
    notifyHelloDenied(helloTag, kDeniedMalformed);
    return;
  }
  const uint8_t* name = data + 38 + tokenLen;
  const uint8_t userNameLen = data[38 + tokenLen + nameLen];
  if (len < 39u + tokenLen + nameLen + userNameLen) {
    notifyHelloDenied(helloTag, kDeniedMalformed);
    return;
  }
  const uint8_t* userNameBytes = data + 39 + tokenLen + nameLen;

  char displayName[companionpeer::kMaxNameLen + 1] = {0};
  const size_t nameCopy = nameLen > companionpeer::kMaxNameLen ? companionpeer::kMaxNameLen : nameLen;
  memcpy(displayName, name, nameCopy);

  char userName[companionpeer::kMaxNameLen + 1] = {0};
  const size_t userNameCopy = userNameLen > companionpeer::kMaxNameLen ? companionpeer::kMaxNameLen : userNameLen;
  memcpy(userName, userNameBytes, userNameCopy);

  char peerKey[companionpeer::kPeerKeyLen];
  companionpeer::makePeerKey(appId, installId, peerKey);

  if (companionpeer::isEnrolled(appId, installId) && companionpeer::tokenMatches(peerKey, token, tokenLen)) {
    // The "connects automatically" relationship: known peer, valid token, no
    // prompt. Refresh the stored display/user name in case the app was
    // renamed or this connect is from a different one of the user's devices.
    if (!companionpeer::ensurePeer(appId, installId, displayName, userName, peerKey)) {
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
  snprintf(g_pendingPairing.userName, sizeof(g_pendingPairing.userName), "%s", userName);

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
    noteBleActivity();

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
        // A peer with no usable declaration cannot take the screen. This is
        // what makes "an app with undefined buttons" -- and, since v12, "an app
        // whose content shape nobody knows" -- structurally impossible rather
        // than a case the rendering code has to handle.
        //
        // THIS READ IS THE ONLY SD TOUCH FOR THE SHAPE. It is cached on the
        // session here and consulted from RAM on every subsequent push; see
        // docs/companion-declared-shape-design.md §5 for why per-push reads are
        // the one implementation that must not be written, however natural it
        // looks: an SD open on the NimBLE host task for every field of every
        // push is exactly what the image writer task exists to avoid.
        //
        // Deliberately a full parse rather than the bare Storage.exists() this
        // used to be. It tightens the gate -- a stored-but-corrupt declaration
        // now denies instead of admitting a peer with a garbage button map --
        // and needs no new denial reason: §3's structural rule is that a peer
        // which has not declared itself cannot reach the screen, and an
        // unparseable declaration has declared nothing.
        companionble::ContentShape shape;
        if (!companionpeer::readDeclaredShape(session->peerKey, &shape)) {
          const uint8_t denied[3] = {kSessAcquireDenied, sessionId, kAcquireDeniedNoButtonMap};
          notifySession(denied, sizeof(denied));
          return;
        }
        session->declaredShape = static_cast<uint8_t>(shape);
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

const char* phyName(uint8_t phy) {
  switch (phy) {
    case BLE_GAP_LE_PHY_1M:
      return "1M";
    case BLE_GAP_LE_PHY_2M:
      return "2M";
    case BLE_GAP_LE_PHY_CODED:
      return "CODED";
    default:
      return "?";
  }
}

// Last conn params/PHY this session saw, from ServerCallbacks::
// onConnParamsUpdate()/onPhyUpdate() below -- negotiation is bimodal on this
// hardware (15ms/2M vs 30ms/1M with a much shorter supervision timeout), so
// echoing what was actually granted next to the per-image throughput/
// histogram log lets a run be attributed to link scheduling without
// cross-referencing timestamps against the connect-time log line.
float g_lastConnIntervalMs = 0;
uint16_t g_lastConnLatency = 0;
uint16_t g_lastConnTimeoutMs = 0;
const char* g_lastPhyTx = "?";
const char* g_lastPhyRx = "?";

void logLastConnParams() {
  LOG_DBG("CBLE", "conn params at end of transfer: interval=%.2fms latency=%u timeout=%ums phy=%s/%s",
          g_lastConnIntervalMs, static_cast<unsigned>(g_lastConnLatency), static_cast<unsigned>(g_lastConnTimeoutMs),
          g_lastPhyTx, g_lastPhyRx);
}

// ---------------------------------------------------------------------------
// Content characteristic
// ---------------------------------------------------------------------------

// Only enqueues the open request -- the actual SD open happens on the writer
// task (see "Image write-behind task" above). Runs once per transfer, so it's
// the least critical of the three SD calls to move off the host task, but the
// open can still stall on a busy card exactly like write()/flush()/close()
// can, so it moves too for the same reason.
void beginImageStaging(const Session& session) {
  ImageWorkMsg msg;
  msg.type = ImageWorkType::Open;
  memcpy(msg.peerKey, session.peerKey, sizeof(msg.peerKey));
  if (!enqueueImageWork(msg, "open")) g_activeImageFailed = true;
}

// `session` is non-const because a stored UI declaration re-push updates the
// cached content shape on it -- see the Stored branch below.
void finishAsset(uint8_t field, Session& session, uint8_t sessionId) {
  const companionpeer::AssetStoreResult result = companionpeer::storeAsset(
      session.peerKey, field == kFieldIcon ? companionpeer::kAssetIcon : companionpeer::kAssetUiDeclaration,
      g_activeBuf.get(), g_activeWritten, kIconBytes);
  notifyAssetAck(sessionId, field, result, session.peerKey);
  if (result != companionpeer::AssetStoreResult::Stored) {
    LOG_ERR("CBLE", "asset 0x%02x rejected (%u)", field, static_cast<unsigned>(static_cast<uint8_t>(result)));
  } else if (field == kFieldUiDeclaration && g_foreground == sessionId) {
    // The foreground app just changed its control scheme; re-read it now rather
    // than waiting for the next connect.
    //
    // The cached shape is refreshed from the same read, and must be: re-pushing
    // the declaration while foreground is the *only* sanctioned way to change
    // content shape mid-session (docs/companion-declared-shape-design.md §4),
    // so leaving the cache stale here would enforce the old shape against an
    // app that has just legitimately become a different one. This is the second
    // and last SD touch for the shape; the push path never reads.
    companionble::ContentShape shape;
    if (companionpeer::readDeclaredShape(session.peerKey, &shape)) session.declaredShape = static_cast<uint8_t>(shape);
    if (g_foregroundCb) {
      const std::string name = companionpeer::displayName(session.peerKey);
      g_foregroundCb(session.peerKey, name.c_str());
    }
  }
}

class ContentCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& /*connInfo*/) override {
    const NimBLEAttValue& value = characteristic->getValue();
    const uint8_t* data = value.data();
    const size_t len = value.size();
    if (len == 0) return;
    noteBleActivity();

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
        Session* session = sessionById(sessionId);
        if (!session) return;

        // Assets are per-peer state, not screen content, so they are accepted
        // from any live session — a background app may refresh its icon or its
        // labels without taking the screen.
        //
        // This is also load-bearing rather than a nicety: ACQUIRE is refused
        // until a UI declaration is stored, so requiring foreground to push one
        // deadlocks enrollment outright. A peer could never push the
        // declaration that would let it become foreground. Found the first time
        // the end-to-end harness ran against real hardware.
        //
        // Everything that touches the screen still requires the foreground,
        // which is the case the sessionId check exists for: without it a stray
        // write from a backgrounded app on the same phone lands in whatever
        // transfer is in flight.
        const bool isAsset = field == kFieldUiDeclaration || field == kFieldIcon;
        if (!isAsset && sessionId != g_foreground) return;

        uint32_t totalLen;
        memcpy(&totalLen, data + 3, sizeof(totalLen));  // data may be unaligned
        const uint32_t cap = fieldCap(field);

        resetReassembly();
        g_activeField = field;
        g_activeSession = sessionId;
        g_activeFinal = (fieldByte & kFinalFieldFlag) != 0;
        g_activeWritten = 0;

        // v12: does this content field match what the peer declared it pushes?
        // Latched here -- after resetReassembly() has cleared the flag, before
        // any buffer is allocated and before the image branch's early return --
        // and answered at END, where the pushId finally arrives. Assets are
        // never checked; see fieldMatchesShape() on why doing so deadlocks.
        //
        // The shape comes from the Session cache, never from SD: this is the
        // hot path §5 of docs/companion-declared-shape-design.md exists to keep
        // clean.
        if (!isAsset) {
          if (session->declaredShape == 0) {
            // Unreachable by construction -- a content push requires the
            // foreground, and becoming foreground requires ACQUIRE, which
            // caches the shape. If it ever happens it is a cache bug, and
            // failing open beats bricking every push from a legitimate peer.
            LOG_ERR("CBLE", "session %u foreground with no cached shape", static_cast<unsigned>(sessionId));
          } else if (!companionui::fieldMatchesShape(field, session->declaredShape)) {
            LOG_ERR("CBLE", "field 0x%02x rejected: peer declared shape 0x%02x", field,
                    static_cast<unsigned>(session->declaredShape));
            g_activeShapeRejected = true;
            return;
          }
        }

        if (field == kFieldImage) {
          // Oversize images are rejected at END rather than here so the app gets
          // one clear RENDER_STATUS either way; the bytes are simply not stored.
          g_activeOverflow = totalLen > cap;
          g_activeTotalLen = totalLen;
          g_imageTransferStartMs = millis();
          resetChunkTiming();
          if (!g_activeOverflow) beginImageStaging(*session);
          return;
        }

        if (field == kFieldTitle || field == kFieldBody) g_textTransferStartMs = millis();

        if (field == kFieldListDoc && totalLen > cap) {
          // v12 part 3: refuse over-cap bytes outright rather than the
          // silent truncation below -- unlike title/body, a list document
          // truncated at an arbitrary byte can land exactly on a structural
          // boundary and parse as a shorter-but-well-formed document, which
          // would silently store a shopping list missing items with no error
          // the app or the user ever sees. No buffer allocated; CHUNKs for
          // this transfer are consumed and thrown away by the `!g_activeBuf`
          // guard below, and END answers RejectedSize once pushId exists.
          g_activeOverflow = true;
          g_activeTotalLen = totalLen;
          break;
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
          // Gap is recorded on every CHUNK entry, even ones that bail out
          // below, so an overflow/failed transfer's tail doesn't silently
          // vanish from the histogram -- the gap itself is purely a function
          // of when the write arrived, not what this task did with it.
          const uint32_t chunkEntryMs = millis();
          recordChunkGap(chunkEntryMs);
          if (g_activeOverflow || g_activeShapeRejected || g_activeImageFailed || g_activeSeqGap) return;
          // v9: image CHUNKs carry a 2-byte little-endian sequence number
          // right after sessionId -- byte 0 opcode, byte 1 sessionId, bytes
          // 2-3 seq, bytes 4..N payload. Every other field's CHUNK is
          // unchanged (payload starts at byte 2). This exists because the
          // image field is the one pushed over Write Without Response (see
          // docs/companion-display-protocol.md "Image field"), which drops
          // the link-layer's own delivery guarantee -- without a sequence
          // number a lost chunk would shift every byte after it and
          // silently corrupt the raw 2bpp payload instead of failing loudly.
          if (len < 4) {
            LOG_ERR("CBLE", "image CHUNK too short for seq header (%u bytes)", static_cast<unsigned>(len));
            g_activeSeqGap = true;
            enqueueImageAbort();
            return;
          }
          uint16_t seq;
          memcpy(&seq, data + 2, sizeof(seq));  // data may be unaligned
          if (seq != g_activeSeq) {
            LOG_ERR("CBLE", "image CHUNK sequence gap: expected %u got %u", static_cast<unsigned>(g_activeSeq),
                    static_cast<unsigned>(seq));
            g_activeSeqGap = true;
            enqueueImageAbort();
            return;
          }
          const uint8_t* imgPayload = data + 4;
          const size_t imgPayloadLen = len - 4;
          if (imgPayloadLen > kMaxImageChunkPayload) {
            // Cannot happen at the negotiated MTU (185, see ensureStarted()) --
            // guard anyway so a future MTU change fails loudly instead of
            // overflowing ImageWorkMsg::data.
            LOG_ERR("CBLE", "image CHUNK payload %u exceeds max %u", static_cast<unsigned>(imgPayloadLen),
                    static_cast<unsigned>(kMaxImageChunkPayload));
            g_activeImageFailed = true;
            enqueueImageAbort();
            return;
          }
          ImageWorkMsg msg;
          msg.type = ImageWorkType::Chunk;
          msg.len = static_cast<uint16_t>(imgPayloadLen);
          memcpy(msg.data, imgPayload, imgPayloadLen);
          if (!enqueueImageWork(msg, "chunk")) {
            // Writer task is falling behind (or never started) -- fail this
            // transfer the same way a real SD write failure would: stop
            // accepting further CHUNKs and let END report StorageFailed.
            // Retrying the enqueue here would just turn into the blocking
            // callback this task exists to avoid.
            g_activeImageFailed = true;
            enqueueImageAbort();
            return;
          }
          // Optimistic: counts bytes handed to the writer task, not bytes
          // actually persisted to SD yet. Only the throughput log at END
          // reads this for the image field (see g_imageTransferStartMs's
          // comment) -- nothing else does, so a few bytes of skew in the rare
          // failure case is harmless.
          g_activeWritten += imgPayloadLen;
          ++g_activeSeq;
          if (g_activeSeq % kImageChunkAckInterval == 0) {
            const uint16_t ackedSeq = g_activeSeq - 1;
            const uint8_t ack[4] = {kSessImageChunkAck, g_activeSession, static_cast<uint8_t>(ackedSeq & 0xFF),
                                    static_cast<uint8_t>((ackedSeq >> 8) & 0xFF)};
            notifySession(ack, sizeof(ack));
          }
          recordChunkBusy(chunkEntryMs);
          return;
        }

        // v12: a shape-rejected transfer is consumed and thrown away -- no
        // buffer was allocated at START, and the answer is owed at END, so
        // every CHUNK in between is a no-op. Stated explicitly rather than
        // left to the !g_activeBuf guards below, which would reach the same
        // place for the wrong reason.
        if (g_activeShapeRejected) return;

        if (fieldUsesSeqChunk(g_activeField)) {
          // v10: title/body CHUNKs carry the same 2-byte little-endian
          // sequence number as the image field (see the kFieldImage branch
          // above) -- title/body moved to Write Without Response in v10 too
          // (docs/companion-display-protocol.md "Text fields"), for the same
          // reason: a ~120ms write-with-response round trip per CHUNK, paid
          // regardless of connection interval, dominated a full-page text
          // push. Unlike the image field there is no mid-transfer ack -- a
          // text push is a handful of chunks, not hundreds -- so a gap is
          // simply latched here and the whole field is dropped at END rather
          // than risk displaying a spliced, corrupted page.
          if (g_activeSeqGap) return;
          if (len < 4) {
            LOG_ERR("CBLE", "field 0x%02x CHUNK too short for seq header (%u bytes)", g_activeField,
                    static_cast<unsigned>(len));
            g_activeSeqGap = true;
            return;
          }
          uint16_t seq;
          memcpy(&seq, data + 2, sizeof(seq));  // data may be unaligned
          if (seq != g_activeSeq) {
            LOG_ERR("CBLE", "field 0x%02x CHUNK sequence gap: expected %u got %u", g_activeField,
                    static_cast<unsigned>(g_activeSeq), static_cast<unsigned>(seq));
            g_activeSeqGap = true;
            return;
          }
          ++g_activeSeq;
          if (!g_activeBuf) return;
          const uint8_t* fieldPayload = data + 4;
          const size_t fieldPayloadLen = len - 4;
          const size_t remaining = g_activeTotalLen - g_activeWritten;
          const size_t toCopy = fieldPayloadLen < remaining ? fieldPayloadLen : remaining;
          if (toCopy > 0) {
            memcpy(g_activeBuf.get() + g_activeWritten, fieldPayload, toCopy);
            g_activeWritten += toCopy;
          }
          break;
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
        // v11: END grew a required third byte, pushId -- see
        // docs/companion-display-protocol.md's END framing and
        // notifyRenderStatus()'s doc comment in CompanionBle.h. A 2-byte END
        // was the whole wire format through v10; now it is simply malformed,
        // rejected the same way START's own length guard rejects a too-short
        // packet above, rather than tolerated as an optional trailing byte --
        // these are our own client and tooling, so there is no outside caller
        // to stay compatible with, and carrying an "END might be 2 or 3
        // bytes" branch forever would be a permanent tax for a distinction
        // that stopped existing the same day it was introduced.
        if (len < 3) {
          LOG_ERR("CBLE", "END packet too short (%u bytes)", static_cast<unsigned>(len));
          return;
        }
        if (g_activeField == 0 || data[1] != g_activeSession) return;
        Session* session = sessionById(g_activeSession);
        if (!session) {
          resetReassembly();
          return;
        }
        const uint8_t field = g_activeField;
        const uint8_t sessionId = g_activeSession;
        // Client-chosen, echoed verbatim in the eventual RENDER_STATUS -- the
        // device never interprets it. Meaningful only when this field is the
        // final one of its batch (see ContentFieldCallback's doc comment);
        // pulled out here, once, so every branch below can just use it.
        const uint8_t pushId = data[2];

        // v12: this transfer was refused at START because the field is not one
        // the peer's declared content shape can render. Answered here, where
        // the pushId finally exists, and before the per-field switch so none of
        // it runs on bytes that were deliberately never buffered.
        if (g_activeShapeRejected) {
          // Only the final-flagged field answers. The device owes exactly one
          // RENDER_STATUS per push and only the final field's pushId is
          // retained (docs/companion-display-protocol.md:731-742, :904), so a
          // title+body batch to an IMAGE peer must fire one status, not two.
          if (g_activeFinal) notifyRenderStatus(RenderResult::RejectedShape, pushId);
          resetReassembly();
          break;
        }

        switch (field) {
          case kFieldImage: {
            const uint32_t transferMs = millis() - g_imageTransferStartMs;
            if (transferMs > 0) {
              LOG_DBG("CBLE", "image transfer: %u bytes in %u ms (%u B/s)", static_cast<unsigned>(g_activeWritten),
                      static_cast<unsigned>(transferMs), static_cast<unsigned>(g_activeWritten * 1000UL / transferMs));
            }
            logChunkTiming();
            logLastConnParams();
            if (g_activeOverflow) {
              notifyRenderStatus(RenderResult::RejectedSize, pushId);
            } else if (g_activeSeqGap) {
              // Distinct from StorageFailed: the SD path never ran into
              // trouble here, a CHUNK's sequence number skipped ahead of
              // what was expected -- the signature of a dropped Write
              // Without Response packet, not a card write failure.
              notifyRenderStatus(RenderResult::SequenceGap, pushId);
            } else if (g_activeImageFailed) {
              // The writer task already knows (or will shortly, via the
              // Abort enqueued when the failure happened) that this transfer
              // is dead -- report it here rather than waiting on an End
              // message it may process very late, if ever.
              notifyRenderStatus(RenderResult::StorageFailed, pushId);
            } else {
              // flush()/close() (and, on an open/write failure the writer
              // task hit earlier, StorageFailed) now happen on the writer
              // task once it drains any CHUNKs still ahead of this message in
              // the queue -- see "Image write-behind task" above. pushId rides
              // along on the message so the eventual RENDER_STATUS -- fired
              // from that task once decode/settle finishes, well after this
              // handler returns -- can still echo it.
              ImageWorkMsg msg;
              msg.type = ImageWorkType::End;
              memcpy(msg.contentId, session->contentId, sizeof(msg.contentId));
              msg.contentIdLen = session->contentIdLen;
              msg.pushId = pushId;
              if (!enqueueImageWork(msg, "end")) {
                notifyRenderStatus(RenderResult::StorageFailed, pushId);
                enqueueImageAbort();
              }
            }
            break;
          }

          case kFieldUiDeclaration:
          case kFieldIcon:
            if (g_activeBuf) finishAsset(field, *session, sessionId);
            break;

          case kFieldListDoc: {
            // v12 part 3: a list document is content, answered via
            // RENDER_STATUS like title/body/image -- NOT an asset (ASSET_ACK
            // is for kFieldUiDeclaration/kFieldIcon only). Ingested straight
            // to SD here, on this task, rather than via ContentFieldCallback
            // (which crosses to the main loop and must stay cheap -- see its
            // doc comment in CompanionBle.h): one SD write per whole-document
            // push is the same infrequent-write class as finishAsset()'s own
            // storeAsset() call just above, not the per-CHUNK hot path the
            // image writer task exists to avoid.
            if (g_activeOverflow) {
              notifyRenderStatus(RenderResult::RejectedSize, pushId);
              break;
            }
            if (!g_activeBuf) {
              // OOM at START already fully reset via resetReassembly() there
              // (g_activeField -> 0), so this case would not even be reached
              // -- guarded anyway rather than dereferencing a null buffer.
              notifyRenderStatus(RenderResult::StorageFailed, pushId);
              break;
            }
            switch (companionpeer::storeListDocument(session->peerKey, g_activeBuf.get(), g_activeWritten)) {
              case companionpeer::ListStoreResult::Stored:
                notifyRenderStatus(RenderResult::Displayed, pushId);
                break;
              case companionpeer::ListStoreResult::RejectedFormat:
                notifyRenderStatus(RenderResult::DecodeFailed, pushId);
                break;
              case companionpeer::ListStoreResult::RejectedStorage:
                notifyRenderStatus(RenderResult::StorageFailed, pushId);
                break;
              // Under kMaxListDocLen bytes but over kMaxListItems items --
              // a size refusal just like the byte-cap g_activeOverflow
              // branch above, not a format/decode problem. See
              // ListStoreResult's comment in CompanionPeerStore.h.
              case companionpeer::ListStoreResult::RejectedSize:
                notifyRenderStatus(RenderResult::RejectedSize, pushId);
                break;
            }
            break;
          }

          case kFieldContentId:
            session->contentIdLen = static_cast<uint8_t>(g_activeWritten);
            memcpy(session->contentId, g_activeBuf.get(), session->contentIdLen);
            if (g_contentCb)
              g_contentCb(field, g_activeBuf.get(), g_activeWritten, g_activeFinal, FieldOutcome::Complete, pushId);
            break;

          case kFieldTitle:
          case kFieldBody:
            // v10: pushed over Write Without Response -- see the seq-checked
            // CHUNK branch above. A gap here means part of this field never
            // arrived or arrived out of order; the partially-filled buffer is
            // not a valid page, so the field is dropped rather than handed to
            // the renderer. The phone finds out via kSessFieldSeqGap instead
            // of silence, unlike a pre-v10 write failure (which could not
            // happen at all under Write With Response).
            //
            // The drop is *also* reported upward as FieldOutcome::Dropped.
            // Staying silent toward the activity was a confirmed real-world
            // bug: a multi-field batch (title, body, then a final-flagged
            // field) whose title was lost still committed on the final flag,
            // so the body updated while the title kept the previous article's
            // text -- a fresh story under a stale headline. The activity
            // poisons the batch on this and discards it instead. The phone's
            // recovery is unchanged: re-push the whole batch on
            // kSessFieldSeqGap.
            // Wire time for this field, plus the parameters it actually ran
            // under. Logged before the seq-gap bail-out so a dropped field is
            // still timed -- a slow transfer and a lossy one look identical
            // from the phone, and telling them apart is the point.
            {
              const uint32_t transferMs = millis() - g_textTransferStartMs;
              LOG_DBG("CBLE", "text field 0x%02x: %u bytes in %u ms%s", field, static_cast<unsigned>(g_activeWritten),
                      static_cast<unsigned>(transferMs), g_activeSeqGap ? " (SEQ GAP)" : "");
              logLastConnParams();
            }
            if (g_activeSeqGap) {
              LOG_ERR("CBLE", "field 0x%02x dropped: CHUNK sequence gap under Write Without Response", field);
              const uint8_t payload[3] = {kSessFieldSeqGap, sessionId, field};
              notifySession(payload, sizeof(payload));
              if (g_contentCb) g_contentCb(field, nullptr, 0, g_activeFinal, FieldOutcome::Dropped, pushId);
              break;
            }
            if (g_contentCb)
              g_contentCb(field, g_activeBuf.get(), g_activeWritten, g_activeFinal, FieldOutcome::Complete, pushId);
            break;

          default:
            if (g_contentCb)
              g_contentCb(field, g_activeBuf.get(), g_activeWritten, g_activeFinal, FieldOutcome::Complete, pushId);
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
    noteBleActivity();
    // tagId is whatever the app declared; the firmware neither allocates nor
    // validates ids — an id the peer never declared is simply ignored upstream.
    if (g_statusCb) g_statusCb(data[1], data[2]);
  }
};

// DIAGNOSTIC (2026-08-03): names the HCI reason behind a disconnect. NimBLE
// hands onDisconnect() a host-stack error, not a raw HCI code -- controller
// reasons arrive offset by BLE_HS_ERR_HCI_BASE (0x200, ble_hs.h:171), so the
// low byte is the actual HCI value only for codes in that range.
//
// The distinction is the whole point of this instrumentation: 0x08 blames the
// idle connection profile's 6 s supervision timeout against a 750 ms effective
// wake cadence; 0x13/0x16 mean iOS or the app deliberately dropped the link
// (an app-lifecycle timer, not the radio); 0x22/0x28 point at the busy<->idle
// parameter-update churn colliding with itself.
const char* disconnectReasonName(int reason) {
  if (reason >= BLE_HS_ERR_HCI_BASE && reason < BLE_HS_ERR_HCI_BASE + 0x100) {
    switch (reason - BLE_HS_ERR_HCI_BASE) {
      case 0x08:
        return "HCI 0x08 supervision timeout";
      case 0x13:
        return "HCI 0x13 remote user terminated";
      case 0x14:
        return "HCI 0x14 remote terminated, low resources";
      case 0x15:
        return "HCI 0x15 remote terminated, powering off";
      case 0x16:
        return "HCI 0x16 local host terminated";
      case 0x1F:
        return "HCI 0x1f unspecified error";
      case 0x22:
        return "HCI 0x22 LMP/LL response timeout";
      case 0x28:
        return "HCI 0x28 instant passed";
      case 0x3B:
        return "HCI 0x3b unacceptable conn interval";
      case 0x3E:
        return "HCI 0x3e connection failed to establish";
      default:
        return "HCI (other)";
    }
  }
  return "host-stack error (not an HCI reason)";
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* /*server*/, NimBLEConnInfo& /*connInfo*/) override {
    g_connectMs = millis();
    LOG_DBG("CBLE", "central connected");
    // Negotiate the connection params once, right here, rather than waiting
    // for tick()'s next call to notice -- "negotiate once per connection,
    // then never again" (see the comment above CompanionConnPolicy's
    // inclusion). g_connPolicy.onConnect() forces Session immediately and
    // starts the Idle holdoff clock -- see its doc comment for why that must
    // happen here rather than through the ordinary session-state path.
    //
    // This is deliberately the ONLY procedure started here. PHY and Data
    // Length Extension used to fire from this same block, back-to-back with
    // this one -- three LLCP procedures racing each other and the central's
    // own setup exchange, which produced every 40 s HCI 0x22 (TPRT)
    // disconnect this link ever had (ca5e518a). Core Spec Vol 6 Part B §5.3
    // allows only one LLCP procedure pending on a connection at a time.
    // onConnParamsUpdate() below starts PHY once this procedure is confirmed
    // settled, and onPhyUpdate() starts DLE once PHY is.
    noteBleActivity();
    g_phyRequestedThisConn = false;
    g_dleRequestedThisConn = false;
    applyConnPolicyRequest(g_connPolicy.onConnect(millis()));
  }
  void onConnParamsUpdate(NimBLEConnInfo& connInfo) override {
    // Confirms what the central actually granted -- the request side only
    // logs what was asked for; a peripheral request can be silently ignored,
    // leaving the previous interval in place.
    g_lastConnIntervalMs = connInfo.getConnInterval() * 1.25f;
    g_lastConnLatency = connInfo.getConnLatency();
    g_lastConnTimeoutMs = connInfo.getConnTimeout() * 10;
    LOG_DBG("CBLE", "t=+%lums conn params granted: interval=%.2fms latency=%u timeout=%ums",
            static_cast<unsigned long>(connUptimeMs()), g_lastConnIntervalMs, static_cast<unsigned>(g_lastConnLatency),
            static_cast<unsigned>(g_lastConnTimeoutMs));
    // DIAGNOSTIC: does the link actually end up where we asked? A request the
    // central never honours leaves the firmware streaming Write-Without-Response
    // chunks into a link it wrongly believes is on the 15 ms busy profile.
    //
    // g_connPolicy.onGrant() only RECORDS the match here rather than logging
    // it. The first update event of a connection carries the parameters the
    // *central* chose when it established the link, which naturally differ
    // from a request we sent microseconds earlier and that nothing has
    // answered yet. Measured on hardware 2026-08-03 against a
    // bleak/CoreBluetooth central: request at t=+1ms for 15ms/0/6s, first
    // update at t=+300ms reporting the central's own 30ms/0/720ms, then the
    // next request granted exactly. Logging on every update turned that
    // normal opening handshake into an ERR line on every single connection --
    // loud enough that the e2e harness had to allowlist the string, which is
    // precisely how a diagnostic stops being read.
    //
    // tick() does the actual reporting once the request has had time to land
    // (g_connPolicy.divergenceDue()).
    g_connPolicy.onGrant(connInfo.getConnInterval(), connInfo.getConnLatency(), connInfo.getConnTimeout(), millis());
    g_lastGrantedIntervalUnits = connInfo.getConnInterval();
    g_lastGrantedLatency = connInfo.getConnLatency();
    g_lastGrantedTimeoutUnits = connInfo.getConnTimeout();
    // Serialized chain, step 2: start PHY only once conn params are
    // confirmed settled -- g_connPolicy.inFlight() is false only after a
    // *matching* grant lands (onGrant()'s doc comment above), not on just any
    // conn-param event. The central's own opening announcement fires this
    // callback too (see the DIAGNOSTIC comment above) but doesn't match and
    // leaves the request outstanding, so it correctly does not trip this.
    // g_phyRequestedThisConn guards against a later Session<->Idle
    // renegotiation mid-connection re-firing PHY.
    if (!g_phyRequestedThisConn && !g_connPolicy.inFlight() && g_server) {
      g_phyRequestedThisConn = true;
      g_server->updatePhy(connInfo.getConnHandle(), BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK, 0);
      LOG_DBG("CBLE", "t=+%lums requested 2M PHY", static_cast<unsigned long>(connUptimeMs()));
    }
  }
  void onPhyUpdate(NimBLEConnInfo& connInfo, uint8_t txPhy, uint8_t rxPhy) override {
    g_lastPhyTx = phyName(txPhy);
    g_lastPhyRx = phyName(rxPhy);
    LOG_DBG("CBLE", "PHY update: tx=%s rx=%s", g_lastPhyTx, g_lastPhyRx);
    // Serialized chain, step 3: Data Length Extension, started only once
    // PHY's own procedure has completed -- this callback firing at all is
    // that signal. Single-shot, no retry on refusal: neither the Core Spec
    // nor Apple's/Nordic's public guidance mandates a backoff-and-retry for a
    // rejected LL Control Procedure, and NimBLE-Arduino's GAP dispatcher
    // (NimBLEServer.cpp's gapEventHandler) never forwards
    // BLE_GAP_EVENT_DATA_LEN_CHG, so there is no way to confirm DLE actually
    // landed even if we wanted to retry on failure -- and nothing is
    // requested after it in this chain, so that blind spot doesn't matter
    // here. This used to fire from onConnect(), racing conn params and PHY;
    // see ca5e518a for what that cost (every connection dying at 40 s, HCI
    // 0x22/TPRT) and this class's own comment for the full chain reasoning.
    if (!g_dleRequestedThisConn && g_server) {
      g_dleRequestedThisConn = true;
      g_server->setDataLen(connInfo.getConnHandle(), 251);
      LOG_DBG("CBLE", "t=+%lums requested DLE (251 octets)", static_cast<unsigned long>(connUptimeMs()));
    }
  }
  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& /*connInfo*/, int reason) override {
    // DIAGNOSTIC: the reason code was previously discarded, which left every
    // disconnect indistinguishable -- radio, iOS policy, and our own parameter
    // churn all looked the same. The uptime alongside it is what a periodic
    // drop shows up in. Also reports whether a transfer was in flight, since a
    // drop mid-field is a different story from a drop on an idle link.
    LOG_ERR("CBLE", "central disconnected after %lums: reason=0x%04x (%s), field-in-flight=0x%02x",
            static_cast<unsigned long>(connUptimeMs()), static_cast<unsigned>(reason), disconnectReasonName(reason),
            g_activeField);
    g_connectMs = 0;
    // The next connect gets a fresh onConnect() edge; see
    // CompanionConnPolicy::onDisconnect()'s doc comment.
    g_connPolicy.onDisconnect();
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
    if (g_begun && server) {
      const bool restarted = server->startAdvertising();
      LOG_DBG("CBLE", "advertising restart after disconnect: start()=%d isAdvertising()=%d", restarted ? 1 : 0,
              NimBLEDevice::getAdvertising()->isAdvertising() ? 1 : 0);
    }
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

  // Created once and kept alive across stop()/ensureStarted() cycles, same as
  // e.g. the session table -- cheap to leave idle, and avoids having to
  // synchronize a clean task/queue teardown against an in-flight transfer.
  ensureImageWriteTaskStarted();

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
  LOG_INF("CBLE", "advertising as \"%s\"", g_deviceName);

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

void tick() {
  if (!isConnected()) return;

  // The whole job: make sure the profile matching current session state has
  // been requested. This is both the initial request on a state change (a
  // session claiming/losing the foreground) and the retry point for one that
  // was deferred behind another procedure or went unanswered -- cheap and
  // idempotent, since CompanionConnPolicy::step() no-ops once the link is
  // already at (or already pursuing) the desired profile. There is no timer
  // here any more: no periodic idle-relax step, and no per-write or
  // per-notify tightening -- see CompanionConnPolicy.h for why the interval
  // no longer needs to move at all.
  syncConnPolicyForegroundState();
  applyConnPolicyRequest(g_connPolicy.step(millis()));

  // DIAGNOSTIC: report a parameter request the central never honoured, once per
  // request, and only after it has had kConnParamsGraceMs to land. Checking here
  // rather than in onConnParamsUpdate() is the whole point: the opening update
  // of a connection reports the central's own chosen parameters, not a reply to
  // us, and flagging that produced an ERR line on every single connection.
  if (g_connPolicy.divergenceDue(millis())) {
    LOG_ERR("CBLE",
            "conn params NOT honoured after %lums: asked interval=%u-%u latency=%u timeout=%u, link is interval=%u "
            "latency=%u timeout=%u (units: 1.25ms / events / 10ms)",
            static_cast<unsigned long>(CompanionConnPolicy::kConnParamsGraceMs),
            static_cast<unsigned>(g_connPolicy.requestedIntervalUnits()),
            static_cast<unsigned>(g_connPolicy.requestedIntervalMaxUnits()),
            static_cast<unsigned>(g_connPolicy.requestedLatencyUnits()),
            static_cast<unsigned>(g_connPolicy.requestedTimeoutUnits()),
            static_cast<unsigned>(g_lastGrantedIntervalUnits), static_cast<unsigned>(g_lastGrantedLatency),
            static_cast<unsigned>(g_lastGrantedTimeoutUnits));
  }
}

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
  noteBleActivity();

  // Header byte: bit7 isFinal, bits6-4 event type, bits3-0 button id.
  const uint8_t header = (isFinal ? 0x80 : 0x00) | ((static_cast<uint8_t>(ButtonEventType::ButtonPress) & 0x07) << 4) |
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

void notifyRenderStatus(RenderResult result, uint8_t pushId) {
  // pushId 0 is the client's explicit "I don't want an answer" -- see this
  // function's doc comment in CompanionBle.h. Guarding it here, rather than
  // trusting every call site to check first, means a future call site can
  // never regress into notifying on a push nobody asked to hear about.
  if (pushId == 0) return;
  if (g_foreground == kNoSession) return;
  const uint8_t payload[4] = {kSessRenderStatus, g_foreground, static_cast<uint8_t>(result), pushId};
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
  if (!companionpeer::ensurePeer(pending.appId, pending.installId, pending.name, pending.userName, peerKey)) {
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
