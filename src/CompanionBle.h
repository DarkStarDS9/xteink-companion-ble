#pragma once

// Companion Display Protocol v6 — NimBLE GATT *peripheral* glue.
//
// Unlike BleInput.h/.cpp (BLE HID *host*, X3 as central connecting to a
// page-turner remote), this is the opposite BLE role: X3 as GATT *peripheral*,
// a phone app as central. The FreeInk SDK has no peripheral/GATT-server
// abstraction (only BleKeyboardHost's host role, see docs/ble-keyboard-host.md
// upstream) — this talks to NimBLE-Arduino's server APIs (NimBLEServer,
// NimBLEService, NimBLECharacteristic) directly.
//
// See docs/companion-display-protocol.md for the wire format this implements.
// That document is authoritative; this file is a transcription of it.
//
// The v6 shape in one paragraph: a phone app is a *session*, established by a
// HELLO handshake on the Session characteristic and identified by a one-byte
// sessionId that rides on every frame in both directions. Exactly one session
// is foreground and may drive the screen. This exists because a BLE peripheral
// gets one link per phone, not per app — two apps on one phone share a single
// connection, so app identity has to be declared in-band. Per-peer state
// (token, button map, icon, staged image) lives on the SD card; see
// CompanionPeerStore.h.

#include <cstddef>
#include <cstdint>

class GfxRenderer;

namespace companionble {

// Advertised local name prefix while in Companion Mode. The full advertised
// name appends a short stable per-device suffix (see buildDeviceName() in
// CompanionBle.cpp) so two readers running this same generic firmware don't
// show up as two identical rows in a phone's BLE picker. Kept app-agnostic —
// this protocol/firmware doesn't assume a specific companion app.
inline constexpr const char* kDeviceNamePrefix = "CrossPoint Companion";

// Measured on real X3 hardware (2026-07-22, ESP32-C3, stock NimBLE footprint,
// no custom_sdkconfig trim — see platformio.ini's note on why): ensureStarted()
// logs ESP.getFreeHeap() immediately before NimBLEDevice::init() and right
// after g_server->start() (CBLE tag), isolating NimBLE's own cost from
// navigation/activity overhead. Measured delta: 99220 -> 34620 bytes, a
// 64600-byte (~63 KB) cost — confirmed stable afterward (steady low-heap
// reading held, no crash, no drift over the session). Floor set with ~16 KB
// margin above that measured cost.
inline constexpr size_t kStartMinFreeHeap = 80 * 1024;

// Max bytes buffered per text field (title/body), independent of negotiated
// MTU — this bounds the reassembly buffer, not a single CHUNK packet.
// Advertised to clients via the capability characteristic.
inline constexpr uint16_t kMaxFieldLen = 4096;

// Max bytes accepted for an image push. Unlike the text fields this is NOT a
// RAM buffer: image CHUNKs are appended straight to a file in the pushing
// peer's data/ directory as they arrive, so this bounds SD usage and transfer
// time rather than heap. That is the whole reason it can be this large on a
// part with no room for a third big allocation — see the memory discussion in
// docs/companion-image-protocol-sketch.md, and why v6 widened the START
// length field from uint16 to uint32.
//
// Field 0x04 is raw packed 2bpp (see RawBitmapToFramebufferConverter and
// docs/companion-display-protocol.md), not PNG — a fixed, exact size for a
// given panel with no compression blowup risk. This measured X4 panel is
// 528x792: bytesPerRow = ceil(528/4) = 132, so a full push is exactly
// 132 * 792 = 104544 bytes. This cap is set with ~24KB (~24%) of headroom
// above that so a slightly different panel/orientation still fits comfortably
// without this constant needing to track the exact figure — the real gate is
// RawBitmapToFramebufferConverter's own runtime size check against the live
// renderer's screen dimensions, which rejects anything that isn't an exact
// match regardless of this cap.
inline constexpr uint32_t kMaxImageFieldLen = 128 * 1024;

// Sleep-screen icon dimensions, 1 bit per pixel. Advertised in the capability
// characteristic; an icon push of any other length is rejected rather than
// stored, since a wrong-sized bitmap cannot be drawn meaningfully.
inline constexpr uint8_t kIconWidthPx = 64;
inline constexpr uint8_t kIconHeightPx = 64;
inline constexpr size_t kIconBytes = (static_cast<size_t>(kIconWidthPx) * kIconHeightPx) / 8;

// Max bytes retained for the content-id field (kFieldContentId) — deliberately
// much smaller than kMaxFieldLen. This is an opaque, client-defined
// correlation token (device never interprets it), appended verbatim to every
// button-event notification so a client can detect "the button was pressed for
// a since-replaced article" and no-op instead of acting on stale state. Kept
// small on purpose: the button-event notify payload is bounded by (negotiated
// MTU - 3 bytes ATT overhead) minus the 4-byte session+header+duration prefix,
// and other clients on this app-agnostic protocol may negotiate a much smaller
// MTU than this firmware's own 185 (BLE's guaranteed floor is MTU 23) — see
// docs/companion-display-protocol.md for the full budget math.
inline constexpr size_t kMaxContentIdLen = 32;

// Concurrent sessions on one link. Four apps on one phone driving one screen is
// already generous; the cap exists because the session table is fixed-size, and
// only the foreground session has a reassembly buffer, so this number does not
// multiply the large allocations.
inline constexpr uint8_t kMaxSessions = 4;

// sessionId 0 is never assigned and always means "no session".
inline constexpr uint8_t kNoSession = 0;

// Raw physical button identity — mirrors HalGPIO::BTN_*/InputManager::BTN_*
// exactly (do not renumber independently of those). The firmware assigns no
// meaning to a button beyond its physical identity; what each one does is
// declared by the foreground peer's button map (see ButtonRouting).
enum class ButtonId : uint8_t {
  Back = 0,
  Confirm = 1,
  Left = 2,
  Right = 3,
  Up = 4,
  Down = 5,
  Power = 6,
};

// What the device does when a button fires, as declared by the foreground
// peer's button map. A closed set on purpose: this is the entire list of things
// the firmware can do by itself, and it stays closed so "dumb firmware, smart
// phone" cannot erode one opcode at a time.
enum class ButtonRouting : uint8_t {
  None = 0x00,
  Remote = 0x01,
  LocalPagePrev = 0x02,
  LocalPageNext = 0x03,
  LocalSleep = 0x04,
};

// What kind of content a peer pushes, declared once per peer in its UI
// declaration (kFieldUiDeclaration) rather than inferred from whichever field
// happened to arrive last. See docs/companion-declared-shape-design.md: the
// firmware has to know a peer's shape *while disconnected* (the idle icon grid
// opens a peer's stored document with no session in existence), which an
// ACQUIRE-time byte could never answer, and a shape that holds local user
// state must not be clobbered by a stray title/body push from the same app.
//
// A single value, not a bitmask: a peer declaring "text or list" would
// reintroduce exactly the state-clobbering and button-claiming ambiguity the
// declaration exists to remove.
//
// 0x00 and anything above 0x03 are INVALID, not reserved — an unknown shape is
// refused rather than tolerated, because the whole point of declaring is that
// the firmware always knows what a peer will push. Tolerating an unknown value
// would mean falling back to "whatever arrives", i.e. the reactive model this
// replaces.
enum class ContentShape : uint8_t {
  Text = 0x01,   // title/body/content-id/tag-state
  Image = 0x02,  // image
  List = 0x03,   // todo-list document
};

// Button-event characteristic event types (bits 6-4 of the header byte). Only
// one exists today; the field is reserved so a future non-press event could
// share this characteristic without a wire-incompatible change.
enum class ButtonEventType : uint8_t {
  ButtonPress = 0x01,
};

// Tags: short labelled marks an app declares and then switches on and off.
//
// The firmware defines NO tags. There is no built-in "saved", no fixed slots,
// no reserved ids and no enum of permitted values — an app declares which tags
// exist and what each is called in its UI declaration (kFieldUiDeclaration),
// exactly as it declares button labels, and the device stores the strings and
// draws them. Tag ids are per-peer, so two apps both using id 0 never collide.
//
// Identity and presentation are separate on the wire, and that separation is
// load-bearing: a tag's id is what state refers to, and a label is only ever
// drawn. App labels are localized — they change when the user switches phone
// language, and can change from a server update with no app release — so a
// design where state referenced label text would decide that switching to
// German replaced every tag with a different one, and an article saved in
// English would come back unsaved in German.
//
// This replaces v5's READ_LATER_SAVED, which baked one app's vocabulary into
// firmware, and the numbered indicator slots that briefly replaced it, which
// still made the firmware own the set.
inline constexpr uint8_t kMaxTags = 6;

// Longest tag label stored and drawn, in bytes. Matches the display-name cap,
// and 24 bytes is what a localized label actually needs — German is the long
// case among the languages consumer apps here ship, and "Später lesen" is 13
// UTF-8 bytes on its own. A client that sends more is truncated on a UTF-8
// boundary. Sized against the RAM budget: kMaxTags * (kMaxTagLabelLen + 3) is
// the entire per-peer cost, resident only for the foreground peer.
inline constexpr size_t kMaxTagLabelLen = 24;

// How a declared tag is currently drawn. Visual, not semantic — what "on"
// means is the app's business.
enum class TagState : uint8_t {
  Hidden = 0x00,   // declared but not drawn at all
  Outline = 0x01,  // drawn, unfilled
  Filled = 0x02,   // drawn, filled
};

// How a peer's whole tag row is drawn. Orthogonal to TagState: state says
// which tag is on, this says what "on"/"off" look like. Declared once per UI
// declaration rather than per tag — apps pick one look for their tag row, not
// a mix. An older client that never sends this byte gets Bordered, today's
// only behaviour, so the field is additive rather than a breaking change.
enum class TagRenderStyle : uint8_t {
  Bordered = 0x00,  // outline box when Outline, filled+inverted box when Filled (v6 default)
  Plain = 0x01,     // Outline draws nothing; Filled draws the label with no box (v5-style)
};

// Second (and, today, last) optional trailing byte of the UI declaration,
// right after the tag render style byte — a bitmask of what this peer's app
// can do beyond title/body. Absent means no bits set, same "ran out of
// buffer" convention as the style byte. A peer only appears in the on-device
// gallery picker (docs/companion-display-protocol.md, "On-screen behaviour")
// if it has declared kUiCapabilityImageGallery — the grid is not a launcher,
// it browses a peer's own already-pushed, locally-stored photos, but only for
// apps that say they push photos at all.
inline constexpr uint8_t kUiCapabilityImageGallery = 0x01;

// Content characteristic field identifiers, matching docs/companion-display-protocol.md.
inline constexpr uint8_t kFieldTitle = 0x01;
inline constexpr uint8_t kFieldBody = 0x02;
inline constexpr uint8_t kFieldContentId = 0x03;
// Raw packed 2bpp full-screen image, no header — see
// RawBitmapToFramebufferConverter and docs/companion-display-protocol.md.
inline constexpr uint8_t kFieldImage = 0x04;
// The peer's UI declaration: button labels/routing AND tag labels, in one
// versioned asset. One declaration rather than two because both halves are
// near-static strings the firmware only renders, and folding them halves the
// digest bookkeeping for every client.
inline constexpr uint8_t kFieldUiDeclaration = 0x05;
inline constexpr uint8_t kFieldIcon = 0x06;
// Tag *state* — which of the peer's declared tags are currently in which
// state. Carries no labels and declares nothing. Pushed through the same
// framing as title/body so it can ride the kFinalFieldFlag batch, which is what
// makes content and its tag state commit in a single redraw.
inline constexpr uint8_t kFieldTagState = 0x07;
// Next free: 0x08.

// The top bit of a START packet's field byte marks "this is the last field of
// an atomic content push". The device buffers each field's END as before, but
// only commits (applies + redraws) everything gathered so far once it processes
// an END whose START carried this bit — see the "Atomic multi-field pushes"
// section of docs/companion-display-protocol.md. Kept out of the low 7 bits
// used for field identity so `data[1] & kFieldMask` recovers the field id
// regardless of the flag.
inline constexpr uint8_t kFinalFieldFlag = 0x80;
inline constexpr uint8_t kFieldMask = 0x7F;

// Outcome of a push, reported to the pushing app as RENDER_STATUS (opcode
// 0x88). Whether a push actually reached the panel is the one thing about a
// push the phone cannot work out for itself -- and, since protocol v11, that
// includes text: a title/body push returns on the wire in ~0.24s (a handful
// of CHUNKs), but the panel's own multi-pass grayscale settle takes a further
// ~2.2s measured on hardware, during which the phone has no way to tell
// whether the previous page is still showing.
//
// Named ImageResult through v10, when RENDER_STATUS only ever answered an
// image push; renamed RenderResult in the v11 generalization. The cases
// themselves needed no change -- SequenceGap and StorageFailed are just as
// meaningful for a dropped/discarded text batch as for an image.
enum class RenderResult : uint8_t {
  Displayed = 0x00,
  DecodeFailed = 0x01,
  RejectedSize = 0x02,
  StorageFailed = 0x03,
  // v9: the image CHUNK sequence number (see CompanionBle.cpp's kOpChunk)
  // skipped ahead of what was expected -- a packet was lost or reordered
  // under Write Without Response. Distinct from StorageFailed so the app can
  // tell "the link dropped a packet" (retry the whole push) from "the SD
  // card failed" (a device-local problem retrying won't fix).
  //
  // v11: also used when a title/body/tag batch is discarded whole because one
  // of its fields hit exactly this condition -- see
  // CompanionModeActivity.cpp's g_pendingBatchPoisoned and
  // docs/companion-display-protocol.md's "Atomic multi-field pushes".
  SequenceGap = 0x04,
  // v11: a second push arrived before this one reached the panel and took the
  // screen instead, so the superseded push will never render. A content push
  // sets Screen::Text and an image push sets Screen::Image, so whichever lands
  // second decides what render() actually draws -- the first one's render
  // simply never happens.
  //
  // Reported rather than silently dropped because the device owes exactly one
  // answer per push. Without it, a caller awaiting the superseded push (see
  // CompanionClient's push(..., awaitRender:)) sits out its entire timeout
  // waiting for a render the device already knows will never occur. Distinct
  // from every other case here in that nothing actually went wrong: the push
  // was merely overtaken, and a caller may well not want to retry it.
  Superseded = 0x05,
};

// Why a session lost the screen, reported as BACKGROUND.
enum class BackgroundReason : uint8_t {
  Preempted = 0x00,
  Released = 0x01,
  LinkLost = 0x02,
};

// Start advertising the Companion Display Protocol GATT service (idempotent).
// Follow BleInput::ensureStarted()'s pattern: wrap NimBLE init in
// HalPowerManager::Lock (NimBLE controller init hangs at the 10 MHz low-power
// clock — see BleInput.cpp's comment on this), and gate on
// ESP.getFreeHeap() >= kStartMinFreeHeap same as bleinput::ensureStarted() does,
// logging + returning false rather than starting under budget.
//
// `renderer` and `fontId` are used once, synchronously, to compute the static
// capability characteristic (screen dimensions in characters and in pixels at
// the Companion Mode font, per root CLAUDE.md § Orientation-Aware Logic — never
// hardcode screen dimensions). No reference to `renderer` is retained.
bool ensureStarted(const GfxRenderer& renderer, int fontId);
bool startInProgress();

// Full NimBLE teardown, mirroring bleinput::stop() — must return the stack's
// RAM to the heap, not just drop the link.
void stop();

bool isConnected();

// Ensures the connection interval/peripheral-latency profile matching current
// session state (Session while some app holds the screen, Idle otherwise —
// see CompanionBle.cpp's ConnProfile) has been requested. Cheap and idempotent
// — an actual GAP request only goes out on a real state change or a retry of
// one the central hasn't answered yet — call it once per main loop iteration,
// e.g. from CompanionModeActivity::loop(). Unlike the interval-ladder this
// replaced, there is no periodic relax/tighten timer: the interval is
// negotiated once per connection and never renegotiated: only peripheral
// latency differs between the two profiles, and that costs nothing on the
// outgoing (button-notify) path — see the comment above ConnProfile.
void tick();

// The capability characteristic's bytes, for diagnostics and for a test host
// that wants to assert on them without a BLE read. Never mutated after
// ensureStarted().
const uint8_t* capabilityValue(size_t& lengthOut);

// sessionId of the session that currently owns the screen, or kNoSession.
uint8_t foregroundSessionId();

// How many sessions are live on the current link.
uint8_t activeSessionCount();

// peerKey of the session that currently owns the screen, or an empty string.
// Valid until the next foreground change; copy it if you need to keep it.
const char* foregroundPeerKey();

// Notify a button-press event to the foreground session, if any. No-op if no
// session holds the screen. Call this from CompanionModeActivity::loop() — the
// caller owns all hold-tracking (first-press vs. repeat-while-held vs.
// release): this function just serializes whatever it's given.
//
// `durationTicks` is elapsed hold time in 100ms units since the initial press
// (0 for the initial-down event); `isFinal` marks the release event. A client
// MUST NOT rely on `isFinal` alone to detect release (a disconnect mid-hold
// means it may never arrive).
//
// The payload (v6) is `[sessionId] + [header byte] + [duration, uint16 LE] +
// [last-pushed content-id bytes]`. The header packs isFinal (bit 7), event type
// (bits 6-4) and the button id (bits 3-0). The leading sessionId is what lets a
// backgrounded app on the same phone ignore an event that is not its own —
// every app sharing the link receives every notification.
bool notifyButtonEvent(ButtonId button, uint16_t durationTicks, bool isFinal);

// Reports the outcome of a push to the app that made it, as RENDER_STATUS:
// {kSessRenderStatus, sessionId, result, pushId}. `pushId` is simply echoed
// back from whatever the pushing client sent on the final-flagged field's END
// -- the device never interprets it, so the same mechanism answers an image
// push, a text batch, or any future push type with no protocol growth. This
// replaced an earlier `field` byte (kFieldImage / kFieldBody, the latter
// standing in for "the whole title/body/content-id/tag batch" since there was
// no single field id for a batch) on the same day v11 landed, before any
// consumer app had adopted it -- see the "v11" note below and the commit that
// made this change for why redefining it was free.
//
// pushId 0 means "the client did not ask for an answer" and MUST NOT produce
// a notification -- callers that armed nothing (pushId == 0) simply never
// call this, and this function itself no-ops if handed one anyway, as a
// belt-and-suspenders guard against a future call site forgetting the check.
//
// Called from the main loop once the pushed content has actually been
// decoded/laid out and flipped to the panel (or failed, or -- v11 --
// discarded whole; see RenderResult::SequenceGap).
//
// v10 and earlier clients only ever received this 3 bytes long (no trailing
// byte at all), and only for an image push -- text pushes were silent. A v10
// client fed a 4-byte notification triggered by a text push would misparse
// it, which is exactly why generalizing this past images was a protocol
// version bump (10 -> 11) in the first place. See
// docs/companion-display-protocol.md's RENDER_STATUS section.
void notifyRenderStatus(RenderResult result, uint8_t pushId);

// Answers a pending pairing prompt. Called from the main loop when the user
// presses CONFIRM/BACK, or when the prompt times out. Writing the peer record
// and issuing the token happen here, on the caller's task.
//
// `timedOut` distinguishes the two rejection paths on the wire: an app that was
// actively refused should stop asking, while one that timed out because nobody
// was looking at the device should feel free to retry.
void resolvePairing(bool accept, bool timedOut = false);

// Callback for a completed content field, fully reassembled from START/CHUNK/
// END frames. Invoked from the NimBLE host task's write callback — this
// direction DOES cross a task boundary (NimBLE host task -> main loop task), so
// implementations must only do cheap, thread-safe work here (copy into a
// lock-guarded buffer and set a flag loop() polls), per the ISR/task
// shared-state rules in root CLAUDE.md. `field` is kFieldTitle/kFieldBody
// (kFinalFieldFlag already stripped); content-id, image and the two assets are
// handled inside CompanionBle.cpp and do not reach here except via the
// dedicated callbacks below. `final` mirrors kFinalFieldFlag from this field's
// START packet.
//
// `outcome` says whether `data` is actually a field. Dropped is reported so a
// receiver batching fields for an atomic commit can tell "this field did not
// change" apart from "this field was lost" — committing a batch that lost one
// field puts a fresh body under a stale headline, which is worse than not
// committing at all. On Dropped, `data` is nullptr and `len` is 0; `field` and
// `final` still describe the field that was lost.
//
// `pushId` is this field's END byte 2, verbatim -- but it only MEANS anything
// when `final` is true. A batch is several fields (title, then body, then
// maybe content-id/tag-state) each with their own END and their own pushId on
// the wire, yet the batch is answered exactly once, so only the id riding the
// final-flagged field's END identifies it; the receiver is expected to ignore
// this value on every non-final call and latch it on the final one. Kept as a
// plain byte here rather than resolved to "the batch's pushId" inside
// CompanionBle.cpp because only the receiver (CompanionModeActivity.cpp) knows
// when a batch actually commits vs. gets discarded/timed-out -- see
// g_pendingBatchPushId there.
enum class FieldOutcome : uint8_t {
  Complete,  // fully reassembled; `data`/`len` are the field
  Dropped,   // reassembly failed (v10 CHUNK sequence gap); no data
};
using ContentFieldCallback = void (*)(uint8_t field, const uint8_t* data, size_t len, bool final,
                                      FieldOutcome outcome, uint8_t pushId);
void setContentFieldCallback(ContentFieldCallback cb);

// Callback for a Status characteristic write from the foreground session: set
// declared tag `tagId` to `state`, without re-pushing content. Same
// task-boundary rules as ContentFieldCallback.
//
// Tags are NOT reset by a subsequent content push — when a tag should clear is
// app meaning, and the app is the one that knows. An app that wants content and
// tag state to change together pushes kFieldTagState inside the same atomic
// batch instead, which is the whole reason that field exists.
using StatusCallback = void (*)(uint8_t tagId, uint8_t state);
void setStatusCallback(StatusCallback cb);

// An unknown peer (or one with a bad token) wants to pair. The activity shows a
// prompt and answers with resolvePairing(). `displayName` is the app's own
// user-visible name, valid only for the duration of the call.
using PairingRequestCallback = void (*)(const char* displayName);
void setPairingRequestCallback(PairingRequestCallback cb);

// The foreground session changed — a different app took the screen, or the last
// one let go (`peerKey` empty). The activity reloads that peer's button map and
// clears the screen.
using ForegroundChangeCallback = void (*)(const char* peerKey, const char* displayName);
void setForegroundChangeCallback(ForegroundChangeCallback cb);

// A complete image has been staged to SD at `path` (the pushing peer's
// data/incoming.raw scratch file) and is ready to decode. `peerKey` is the
// pushing (always foreground — image is a screen-owning field, not an asset)
// peer, and `contentId`/`contentIdLen` are that peer's last-pushed content-id,
// for the activity to hand to CompanionPeerStore::commitImage() so the
// gallery's sidecar index can record it. The activity decodes on the main loop
// (never on the host task — decoding touches the framebuffer) and then calls
// notifyRenderStatus() with `pushId` (this image's END byte 2, echoed
// verbatim -- an image push is always a single field, so unlike
// ContentFieldCallback's pushId there is no "only when final" caveat here).
using ImageStagedCallback = void (*)(const char* peerKey, const char* path, const uint8_t* contentId,
                                     size_t contentIdLen, uint8_t pushId);
void setImageStagedCallback(ImageStagedCallback cb);

}  // namespace companionble
