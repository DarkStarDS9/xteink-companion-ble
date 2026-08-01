#include "CompanionBle.h"

#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "CompanionPeerStore.h"
#include "Memory.h"
#include "Epub/converters/RawBitmapToFramebufferConverter.h"

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
// Adaptive connection interval / slave latency
// ---------------------------------------------------------------------------
//
// Two link-layer profiles, requested via NimBLEServer::updateConnParams (a
// peripheral can only ever *request* new parameters -- the central, iOS here,
// grants or ignores it). This is transport tuning, not a protocol change: no
// wire field, byte layout, or characteristic is affected, so it carries no
// version bump.
//
// Units match the BLE spec directly (updateConnParams forwards them straight
// into ble_gap_upd_params): interval in 1.25 ms units, latency as a skipped-
// event count, supervision timeout in 10 ms units.
//
// Bounds are iOS's accessory-design-guidelines envelope, since the real
// central here is a phone (CompanionKit/Snap2Ink/SpokenFeeds), and a request
// outside it is simply ignored, leaving iOS's own defaults in place:
//   - peripheral latency <= 30 intervals
//   - supervision timeout 6-18 s
//   - interval >= 15 ms, in 15 ms multiples
//   - maxInterval * (latency+1) <= 6 s
//   - timeout(ms) > maxInterval(ms) * (latency+1) * 3
//
// "Busy": tight interval, no latency skipping -- lowest round-trip time while
// a button press or a content/image chunk sequence is actively in flight.
// 15 ms is iOS's own floor (interval>=15ms/15ms-multiple); latency 0 means
// maxInterval*(latency+1)=15ms and the timeout floor (6s) clears
// 15ms*1*3=45ms by a wide margin. Each image chunk write is a full
// request/ack round trip bound by this interval, so this was previously 30ms
// (measured ~2.9 KB/s); halving it roughly doubles chunk throughput.
//
// Safe only because image chunk storage no longer does blocking SD I/O on
// this callback's task -- see the chunk queue in CompanionImageWriter
// (image writes moved to a dedicated task so a slow SdFat/SPI write or the
// end-of-image flush()/close() can never stall the BLE host task long enough
// to miss a scheduled radio event at this tighter interval).
constexpr uint16_t kConnIntervalBusyUnits = 12;  // 15 ms (12 * 1.25 ms)
constexpr uint16_t kConnLatencyBusy = 0;
constexpr uint16_t kConnTimeoutBusyUnits = 600;  // 6 s (10 ms units) -- iOS's floor

// "Idle": relaxed interval + latency skip once nothing has moved for a
// while -- the "slave latency" lever from the platform research this change
// is based on, which alone (independent of modem/light-sleep) measurably cuts
// average current by letting the peripheral skip waking for idle connection
// events. 150 ms * (4+1) = 750 ms, comfortably under the 6 s cap; the 6 s
// timeout is unchanged from the busy profile so relaxing/tightening never
// crosses a supervision-timeout boundary.
constexpr uint16_t kConnIntervalIdleUnits = 120;  // 150 ms (120 * 1.25 ms)
constexpr uint16_t kConnLatencyIdle = 4;
constexpr uint16_t kConnTimeoutIdleUnits = 600;  // 6 s

// How long the link must go without a content/status/session write or an
// outgoing button notify before it relaxes to the idle profile. Matches
// HalPowerManager::IDLE_POWER_SAVING_MS's idea of "idle" (not its value
// directly -- that constant lives in a different module -- but the same
// shape: a short, fixed quiet period before backing off).
constexpr uint32_t kConnIdleRelaxMs = 3000;

uint32_t g_lastBleActivityMs = 0;
// Tracks which profile was last requested, so tick() and the activity
// helpers below don't spam updateConnParams() every call once already in the
// right state. Starts false (idle) so onConnect()'s noteBleActivity() call
// always fires an explicit busy request on a fresh connection -- the v6
// handshake happens right after connect, and should get the tightest
// round-trip available rather than whatever the central defaulted to.
bool g_connParamsBusy = false;

void requestConnParams(bool busy) {
  if (!g_server || g_server->getConnectedCount() == 0) return;
  if (g_connParamsBusy == busy) return;
  const uint16_t interval = busy ? kConnIntervalBusyUnits : kConnIntervalIdleUnits;
  const uint16_t latency = busy ? kConnLatencyBusy : kConnLatencyIdle;
  const uint16_t timeout = busy ? kConnTimeoutBusyUnits : kConnTimeoutIdleUnits;
  const auto peer = g_server->getPeerInfo(0);
  g_server->updateConnParams(peer.getConnHandle(), interval, interval, latency, timeout);
  g_connParamsBusy = busy;
  LOG_DBG("CBLE", "requested %s conn params (interval=%u latency=%u timeout=%u)", busy ? "busy" : "idle",
          static_cast<unsigned>(interval), static_cast<unsigned>(latency), static_cast<unsigned>(timeout));
}

// Called from every characteristic write and outgoing notify -- i.e. anything
// that means a phone app is actively driving the link right now. Cheap: a
// timestamp store and, only on the idle->busy edge, one GAP parameter-update
// request.
void noteBleActivity() {
  g_lastBleActivityMs = millis();
  requestConnParams(/*busy=*/true);
}

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
    companionpeer::assetTag(peerKey, assetId == kFieldIcon ? companionpeer::kAssetIcon : companionpeer::kAssetUiDeclaration,
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
bool g_activeImageOverflow = false;
// Set once handing image work to the writer task fails (queue full/missing,
// see enqueueImageWork()) -- once true, further CHUNKs for this transfer are
// dropped without retrying (a retry loop here would just re-introduce the
// blocking-host-task problem this task exists to avoid), and END reports
// StorageFailed immediately rather than waiting on a message the writer task
// may process very late, if ever.
bool g_activeImageFailed = false;
// Wall-clock span of the image CHUNK sequence only -- set at START, read at
// END -- so BLE transfer time can be told apart from decode/settle time
// without needing a host-side script to measure it. Deliberately computed
// here on the host task rather than in the writer task, so a slow flush/close
// or a backlog of still-queued CHUNKs never inflates this number.
uint32_t g_imageTransferStartMs = 0;

// ---------------------------------------------------------------------------
// Image write-behind task
// ---------------------------------------------------------------------------
//
// ContentCharCallbacks::onWrite() runs on the NimBLE host task. ESP32-C3 is
// single-core, so any blocking call made inline there -- SD/SPI I/O very much
// included, since it is neither fast nor bounded -- can make the host task
// miss its own scheduled BLE radio events. At the 30ms connection interval
// this link used to run at there was enough slack for that to go unnoticed;
// at the 15ms interval now in use (see kConnIntervalBusyUnits above) there
// usually isn't, and an occasionally-slow write -- or the reliably-slower
// end-of-image flush/close -- caused real disconnects on real hardware.
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

// Worst-case CHUNK payload: the negotiated MTU (185, see ensureStarted())
// minus 3 bytes of ATT protocol overhead minus the 2-byte opcode+sessionId
// header ContentCharCallbacks::onWrite's kOpChunk case strips before this
// payload is measured.
constexpr size_t kMaxImageChunkPayload = 185 - 3 - 2;

enum class ImageWorkType : uint8_t { Open, Chunk, End, Abort };

// Fixed-size and POD (no heap pointers) -- safe and cheap to copy by value
// through a FreeRTOS queue. Only `type` plus the fields that type actually
// uses are meaningful; the rest are simply unused for a given message.
struct ImageWorkMsg {
  ImageWorkType type = ImageWorkType::Abort;
  uint16_t len = 0;                                // Chunk: valid bytes in data[]
  uint8_t data[kMaxImageChunkPayload] = {0};        // Chunk: payload
  char peerKey[companionpeer::kPeerKeyLen] = {0};   // Open: whose staging file to open
  uint8_t contentId[kMaxContentIdLen] = {0};        // End: session's content-id, copied here
  uint8_t contentIdLen = 0;                         // (host task) before enqueueing so a later
                                                     // session-table reset can't race it
};

// Sized to smooth over the writer task falling behind briefly (a slow SD
// write, or a GC/wear-leveling pause on the card) without the host task
// needing to wait at all. 24 * sizeof(ImageWorkMsg) is a few KB, affordable
// against the ~34KB of free heap this feature typically runs with (see
// ensureStarted()'s heap-cost comment).
//
// It does NOT cover a sustained gap between incoming and drain rate -- a
// fast sender can outpace the SD card for an entire transfer, not just a
// brief stall, and no queue depth fixes that (the image is ~100KB; buffering
// the whole thing in RAM instead of streaming to SD is exactly what the
// heap-cost comment above rules out). That case is handled by
// enqueueImageWork() blocking the host task briefly when the queue is full
// (safe -- see its comment), which throttles the incoming rate down to
// whatever the writer can actually sustain instead of failing the transfer.
constexpr UBaseType_t kImageWriteQueueLen = 24;

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
        break;
      }

      case ImageWorkType::Chunk: {
        if (!g_activeImageFile.isOpen()) break;  // already failed/aborted -- drop silently
        if (g_activeImageFile.write(msg.data, msg.len) != msg.len) {
          LOG_ERR("CBLE", "SD write failed staging image");
          writerDiscardStagedImage();
        }
        break;
      }

      case ImageWorkType::End: {
        if (!g_activeImageFile.isOpen()) {
          notifyImageStatus(ImageResult::StorageFailed);
        } else {
          g_activeImageFile.flush();
          g_activeImageFile.close();
          // Decoding touches the framebuffer, so it happens on the main loop
          // task, not here. The activity answers with notifyImageStatus().
          // onImageStaged() (CompanionModeActivity.cpp) only copies
          // fixed-size buffers under its own critical section, so calling it
          // from this task rather than the host task is safe.
          if (g_imageStagedCb) {
            g_imageStagedCb(g_activeImagePeerKey, g_activeImagePath.c_str(), msg.contentId, msg.contentIdLen);
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
  g_activeImageOverflow = false;
  g_activeImageFailed = false;
  g_activeBuf.reset();
}

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
    default:
      return kMaxFieldLen;
  }
}

bool isKnownField(uint8_t field) {
  return field == kFieldTitle || field == kFieldBody || field == kFieldContentId || field == kFieldImage ||
         field == kFieldUiDeclaration || field == kFieldIcon || field == kFieldTagState;
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
  g_capabilityValue[offset++] = 8;  // protocol version
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
  // opcode(1) helloTag(2) appId(16) installId(16) tokenLen(1) token[..]
  // nameLen(1) name[..] userNameLen(1) userName[..]
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
  if (len < 37u + tokenLen + nameLen + 1u) {
    notifyHelloDenied(helloTag, kDeniedMalformed);
    return;
  }
  const uint8_t* name = data + 37 + tokenLen;
  const uint8_t userNameLen = data[37 + tokenLen + nameLen];
  if (len < 38u + tokenLen + nameLen + userNameLen) {
    notifyHelloDenied(helloTag, kDeniedMalformed);
    return;
  }
  const uint8_t* userNameBytes = data + 38 + tokenLen + nameLen;

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
        // A peer with no button map cannot take the screen. This is what makes
        // "an app with undefined buttons" structurally impossible rather than a
        // case the rendering code has to handle.
        if (!companionpeer::hasUiDeclaration(session->peerKey)) {
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

void finishAsset(uint8_t field, const Session& session, uint8_t sessionId) {
  const companionpeer::AssetStoreResult result =
      companionpeer::storeAsset(session.peerKey, field == kFieldIcon ? companionpeer::kAssetIcon
                                                                     : companionpeer::kAssetUiDeclaration,
                                g_activeBuf.get(), g_activeWritten, kIconBytes);
  notifyAssetAck(sessionId, field, result, session.peerKey);
  if (result != companionpeer::AssetStoreResult::Stored) {
    LOG_ERR("CBLE", "asset 0x%02x rejected (%u)", field, static_cast<unsigned>(static_cast<uint8_t>(result)));
  } else if (field == kFieldUiDeclaration && g_foreground == sessionId && g_foregroundCb) {
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

        if (field == kFieldImage) {
          // Oversize images are rejected at END rather than here so the app gets
          // one clear IMAGE_STATUS either way; the bytes are simply not stored.
          g_activeImageOverflow = totalLen > cap;
          g_activeTotalLen = totalLen;
          g_imageTransferStartMs = millis();
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
          if (g_activeImageOverflow || g_activeImageFailed) return;
          if (payloadLen > kMaxImageChunkPayload) {
            // Cannot happen at the negotiated MTU (185, see ensureStarted()) --
            // guard anyway so a future MTU change fails loudly instead of
            // overflowing ImageWorkMsg::data.
            LOG_ERR("CBLE", "image CHUNK payload %u exceeds max %u", static_cast<unsigned>(payloadLen),
                    static_cast<unsigned>(kMaxImageChunkPayload));
            g_activeImageFailed = true;
            enqueueImageAbort();
            return;
          }
          ImageWorkMsg msg;
          msg.type = ImageWorkType::Chunk;
          msg.len = static_cast<uint16_t>(payloadLen);
          memcpy(msg.data, payload, payloadLen);
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
            const uint32_t transferMs = millis() - g_imageTransferStartMs;
            if (transferMs > 0) {
              LOG_DBG("CBLE", "image transfer: %u bytes in %u ms (%u B/s)",
                      static_cast<unsigned>(g_activeWritten), static_cast<unsigned>(transferMs),
                      static_cast<unsigned>(g_activeWritten * 1000UL / transferMs));
            }
            if (g_activeImageOverflow) {
              notifyImageStatus(ImageResult::RejectedSize);
            } else if (g_activeImageFailed) {
              // The writer task already knows (or will shortly, via the
              // Abort enqueued when the failure happened) that this transfer
              // is dead -- report it here rather than waiting on an End
              // message it may process very late, if ever.
              notifyImageStatus(ImageResult::StorageFailed);
            } else {
              // flush()/close() (and, on an open/write failure the writer
              // task hit earlier, StorageFailed) now happen on the writer
              // task once it drains any CHUNKs still ahead of this message in
              // the queue -- see "Image write-behind task" above.
              ImageWorkMsg msg;
              msg.type = ImageWorkType::End;
              memcpy(msg.contentId, session->contentId, sizeof(msg.contentId));
              msg.contentIdLen = session->contentIdLen;
              if (!enqueueImageWork(msg, "end")) {
                notifyImageStatus(ImageResult::StorageFailed);
                enqueueImageAbort();
              }
            }
            break;
          }

          case kFieldUiDeclaration:
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
    noteBleActivity();
    // tagId is whatever the app declared; the firmware neither allocates nor
    // validates ids — an id the peer never declared is simply ignored upstream.
    if (g_statusCb) g_statusCb(data[1], data[2]);
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    LOG_DBG("CBLE", "central connected");
    // Request the tight profile right away: the v6 HELLO handshake happens
    // immediately after connect, before anything else marks the link busy.
    noteBleActivity();
    // 2M PHY halves on-air time per packet versus the 1M PHY default. Purely
    // a request -- the central (iOS) grants or ignores it, same as
    // updateConnParams above -- and iOS decides silently, so onPhyUpdate()
    // below is the only way to know what actually landed.
    if (server) {
      server->updatePhy(connInfo.getConnHandle(), BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK, 0);
    }
  }
  void onConnParamsUpdate(NimBLEConnInfo& connInfo) override {
    // Confirms what the central actually granted -- requestConnParams() above
    // only logs what was asked for; a peripheral request can be silently
    // ignored, leaving the previous interval in place.
    LOG_DBG("CBLE", "conn params granted: interval=%.2fms latency=%u timeout=%ums",
            connInfo.getConnInterval() * 1.25f, static_cast<unsigned>(connInfo.getConnLatency()),
            static_cast<unsigned>(connInfo.getConnTimeout() * 10));
  }
  void onPhyUpdate(NimBLEConnInfo& /*connInfo*/, uint8_t txPhy, uint8_t rxPhy) override {
    auto phyName = [](uint8_t phy) {
      switch (phy) {
        case BLE_GAP_LE_PHY_1M: return "1M";
        case BLE_GAP_LE_PHY_2M: return "2M";
        case BLE_GAP_LE_PHY_CODED: return "CODED";
        default: return "?";
      }
    };
    LOG_DBG("CBLE", "PHY update: tx=%s rx=%s", phyName(txPhy), phyName(rxPhy));
  }
  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& /*connInfo*/, int /*reason*/) override {
    LOG_DBG("CBLE", "central disconnected");
    // The next connect gets a fresh onConnect() -> noteBleActivity() edge;
    // reset to idle so a stale "already busy" doesn't suppress that request.
    g_connParamsBusy = false;
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
  if (!isConnected() || !g_connParamsBusy) return;
  if (millis() - g_lastBleActivityMs < kConnIdleRelaxMs) return;
  requestConnParams(/*busy=*/false);
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
