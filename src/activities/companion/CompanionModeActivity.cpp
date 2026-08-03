#include "CompanionModeActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "CompanionBle.h"
#include "CompanionPeerStore.h"
#include "CompanionTestConsole.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "MappedInputManager.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Companion Mode's fixed fonts — not user-configurable (unlike the reader),
// since the capability characteristic advertises a fixed char-grid size.
constexpr int kCompanionFontId = NOTOSANS_14_FONT_ID;       // body
constexpr int kCompanionTitleFontId = NOTOSANS_16_FONT_ID;  // title: larger + bold

// Tag chip geometry. The row's actual width is measured from the visible
// labels (see measureTagRow()) rather than reserved at a fixed maximum, so an
// app with no tags gives its title the full width.
constexpr int kTagChipPadX = 4;
constexpr int kTagChipPadY = 2;
constexpr int kTagChipGap = 5;

// Breathing room between the tag row and the title text it butts up against.
// Only needed on the row's left (title-facing) side — the right side already
// sits at the screen's own margin, same as the title's left edge.
constexpr int kTagRowLeftMargin = 12;

// Title wraps onto at most this many lines before falling back to
// ellipsis-truncating the last line (see wrapTitleToLines()).
constexpr int kMaxTitleLines = 2;

// Sleep-screen grid geometry. 64x64 tiles with room to breathe; the cap comes
// from companionpeer::kMaxIconTiles.
constexpr int kIconGridColumns = 6;
constexpr int kIconGridGap = 24;

// Same grid, but for the interactive gallery picker (renderGalleryPicker()):
// extra vertical room per row for the per-tile user/app-name label drawn
// below each icon, which the plain decorative grid doesn't need.
constexpr int kGalleryPickerRowGap = kIconGridGap + 20;

// Sleeping-indicator geometry: bottom-left corner, same footprint text mode's
// battery percentage occupies, sized as a small square rather than a bitmap
// (no ready-made small "sleeping" glyph exists in the icon set — see the
// design discussion this came out of).
constexpr int kSleepIndicatorRadius = 16;
constexpr int kSleepIndicatorMargin = 16;

// Solid filled disc, built the same way GfxRenderer's private fillArc<> scans
// (no sqrt: shrink x while it overshoots the radius as dy grows), just
// exposed here as a two-circle primitive for the crescent below rather than
// GfxRenderer's public API, since nothing else in the codebase needs a full
// filled circle.
void fillCircle(GfxRenderer& renderer, int cx, int cy, int radius, bool state) {
  if (radius <= 0) return;
  const int radiusSq = radius * radius;
  int x = radius;
  for (int dy = 0; dy <= radius; ++dy) {
    while (x > 0 && (x * x + dy * dy) > radiusSq) --x;
    if (x < 0) break;
    const int width = 2 * x + 1;
    renderer.fillRect(cx - x, cy + dy, width, 1, state);
    if (dy != 0) renderer.fillRect(cx - x, cy - dy, width, 1, state);
  }
}

// UTF-8-safe: drop one full codepoint (a lead byte plus any continuation
// bytes), matching the boundary-walk CompanionModeActivity::paginate() uses.
void popUtf8Char(std::string& s) {
  if (s.empty()) return;
  s.pop_back();
  while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) {
    s.pop_back();
  }
}

// Task-boundary handoff for everything arriving on the NimBLE host task (see
// CompanionBle.h's callback doc comments). Fixed-size buffers, not heap
// allocation, so the critical section only ever does a memcpy/scalar
// assignment.
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
uint8_t g_pendingTitleBuf[companionble::kMaxFieldLen];
uint16_t g_pendingTitleLen = 0;
volatile bool g_pendingTitleReady = false;
uint8_t g_pendingBodyBuf[companionble::kMaxFieldLen];
uint16_t g_pendingBodyLen = 0;
volatile bool g_pendingBodyReady = false;
uint8_t g_pendingTagId = 0;
uint8_t g_pendingTagState = 0;
volatile bool g_pendingStatusReady = false;

// Tag state arriving as content field 0x07, so it can ride the atomic batch.
uint8_t g_pendingTagStateBuf[1 + 2 * companionble::kMaxTags];
uint8_t g_pendingTagStateLen = 0;
volatile bool g_pendingTagStateReady = false;
// Whether that pending tag state arrived *inside* a title/body batch (clients
// push it last, under the same final flag) or on its own. Only the former is
// bound to the batch's fate; a standalone tag push — CompanionClient.setTag /
// CompanionDeviceService.setReadLaterTag toggling one tag with no content
// change — must keep applying immediately even while some unrelated batch is
// in trouble. So the discriminator is "did this tag state arrive as part of a
// batch that got poisoned", never "is the poison flag set right now".
volatile bool g_pendingTagStateInBatch = false;

// Foreground handover and pairing requests are also host-task events. Paths and
// names are short and fixed-length here so the critical section stays a memcpy.
char g_pendingForegroundKey[companionpeer::kPeerKeyLen] = {0};
char g_pendingForegroundName[companionpeer::kMaxNameLen + 1] = {0};
volatile bool g_pendingForegroundReady = false;
char g_pendingPairingName[companionpeer::kMaxNameLen + 1] = {0};
volatile bool g_pendingPairingReady = false;
char g_pendingImagePath[96] = {0};
char g_pendingImagePeerKey[companionpeer::kPeerKeyLen] = {0};
uint8_t g_pendingImageContentId[companionble::kMaxContentIdLen] = {0};
uint8_t g_pendingImageContentIdLen = 0;
volatile bool g_pendingImageReady = false;

// Set once a field's END arrives with kFinalFieldFlag set. loop() only applies
// gotTitle/gotBody once this is true, so a multi-field push (title, then body,
// then a final-flagged content-id) always lands on screen together instead of
// the title updating first while body is still mid-transfer.
volatile bool g_pendingCommitReady = false;

// millis() timestamp of the first pending field of the current batch — 0 when
// idle. Safety net for kPendingBatchTimeoutMs: if the final-flagged field's END
// never arrives (app crash / disconnect mid-push), pending fields are applied
// anyway rather than leaving the screen stuck on stale content indefinitely.
uint32_t g_pendingBatchStartMs = 0;
constexpr uint32_t kPendingBatchTimeoutMs = 3000;

// Set when any title/body field of the current batch arrived as
// FieldOutcome::Dropped — i.e. a v10 Write-Without-Response CHUNK sequence gap
// cost us that field entirely. A batch that lost a field must fail as a whole:
// committing the rest paints the new body under the *previous* article's title
// (a confirmed real-world symptom), whereas discarding leaves a coherent, if
// stale, page on screen. Cleared whenever the batch is resolved — commit,
// timeout, or disconnect — so the next batch starts clean.
volatile bool g_pendingBatchPoisoned = false;

// Runs on the NimBLE host task — copy into the fixed buffer and set a flag;
// CompanionModeActivity::loop() (main loop task) does the rest.
void onContentField(uint8_t field, const uint8_t* data, size_t len, bool final, companionble::FieldOutcome outcome) {
  const bool isTextField = field == companionble::kFieldTitle || field == companionble::kFieldBody;
  portENTER_CRITICAL(&g_mux);
  // "Idle" has to include the poison flag, or a batch whose only surviving
  // marker is the poison (first field dropped, nothing buffered yet) would look
  // like a fresh batch to the next field and restart the timeout clock.
  const bool wasIdle = !g_pendingTitleReady && !g_pendingBodyReady && !g_pendingBatchPoisoned;
  if (outcome == companionble::FieldOutcome::Dropped) {
    // No data to copy — just poison the batch. The clock still has to start
    // here: if this is the batch's first field and the final flag never
    // arrives, loop()'s timeout is what clears the poison again.
    if (isTextField) g_pendingBatchPoisoned = true;
    if (wasIdle && isTextField) g_pendingBatchStartMs = millis();
    if (final) g_pendingCommitReady = true;
    portEXIT_CRITICAL(&g_mux);
    return;
  }
  if (field == companionble::kFieldTitle) {
    const size_t n = len > sizeof(g_pendingTitleBuf) ? sizeof(g_pendingTitleBuf) : len;
    memcpy(g_pendingTitleBuf, data, n);
    g_pendingTitleLen = static_cast<uint16_t>(n);
    g_pendingTitleReady = true;
  } else if (field == companionble::kFieldTagState) {
    const size_t n = len > sizeof(g_pendingTagStateBuf) ? sizeof(g_pendingTagStateBuf) : len;
    memcpy(g_pendingTagStateBuf, data, n);
    g_pendingTagStateLen = static_cast<uint8_t>(n);
    g_pendingTagStateReady = true;
    // A title/body batch already in flight (or already poisoned) means this
    // tag state belongs to it; an idle handoff means it is a standalone tag
    // push and owes the batch machinery nothing.
    g_pendingTagStateInBatch = !wasIdle;
  } else if (field == companionble::kFieldBody) {
    const size_t n = len > sizeof(g_pendingBodyBuf) ? sizeof(g_pendingBodyBuf) : len;
    memcpy(g_pendingBodyBuf, data, n);
    g_pendingBodyLen = static_cast<uint16_t>(n);
    g_pendingBodyReady = true;
  }
  if (wasIdle && isTextField) {
    g_pendingBatchStartMs = millis();
  }
  if (final) g_pendingCommitReady = true;
  portEXIT_CRITICAL(&g_mux);
}

void onStatus(uint8_t tagId, uint8_t state) {
  portENTER_CRITICAL(&g_mux);
  g_pendingTagId = tagId;
  g_pendingTagState = state;
  g_pendingStatusReady = true;
  portEXIT_CRITICAL(&g_mux);
}

void onPairingRequest(const char* displayName) {
  portENTER_CRITICAL(&g_mux);
  snprintf(g_pendingPairingName, sizeof(g_pendingPairingName), "%s", displayName ? displayName : "");
  g_pendingPairingReady = true;
  portEXIT_CRITICAL(&g_mux);
}

void onForegroundChange(const char* peerKey, const char* displayName) {
  portENTER_CRITICAL(&g_mux);
  snprintf(g_pendingForegroundKey, sizeof(g_pendingForegroundKey), "%s", peerKey ? peerKey : "");
  snprintf(g_pendingForegroundName, sizeof(g_pendingForegroundName), "%s", displayName ? displayName : "");
  g_pendingForegroundReady = true;
  portEXIT_CRITICAL(&g_mux);
}

void onImageStaged(const char* peerKey, const char* path, const uint8_t* contentId, size_t contentIdLen) {
  portENTER_CRITICAL(&g_mux);
  snprintf(g_pendingImagePath, sizeof(g_pendingImagePath), "%s", path ? path : "");
  snprintf(g_pendingImagePeerKey, sizeof(g_pendingImagePeerKey), "%s", peerKey ? peerKey : "");
  const size_t n = contentIdLen > sizeof(g_pendingImageContentId) ? sizeof(g_pendingImageContentId) : contentIdLen;
  if (n > 0) memcpy(g_pendingImageContentId, contentId, n);
  g_pendingImageContentIdLen = static_cast<uint8_t>(n);
  g_pendingImageReady = true;
  portEXIT_CRITICAL(&g_mux);
}

}  // namespace

#ifdef COMPANION_TEST_CONSOLE
// The serial console's screen-name provider is a plain function pointer, so the
// one live activity instance is reachable through this file-scope pointer.
// Companion Mode is the device's sole activity, so there is never a second one.
static CompanionModeActivity* g_screenNameActivity = nullptr;
#endif

void CompanionModeActivity::onEnter() {
  Activity::onEnter();
  connected = false;
  haveContent = false;
  forceFastRefreshNextRender = false;
  currentPage = 0;
  totalPages = 0;
  pages.clear();
  galleryImages.clear();
  galleryIndex = 0;
  clearUiDeclaration();
  cachedFontId = kCompanionFontId;
  computeViewport();

  companionble::setContentFieldCallback(onContentField);
  companionble::setStatusCallback(onStatus);
  companionble::setPairingRequestCallback(onPairingRequest);
  companionble::setForegroundChangeCallback(onForegroundChange);
  companionble::setImageStagedCallback(onImageStaged);
#ifdef COMPANION_TEST_CONSOLE
  // The console reports the screen without knowing what a screen is.
  g_screenNameActivity = this;
  companiontest::setScreenNameProvider(
      []() -> const char* { return g_screenNameActivity ? g_screenNameActivity->screenName() : "none"; });
  companiontest::setTagStateProvider([](companiontest::TagReport* out, uint8_t maxTags) -> uint8_t {
    return g_screenNameActivity ? g_screenNameActivity->reportTags(out, maxTags) : 0;
  });
#endif

  if (!companionble::ensureStarted(renderer, cachedFontId)) {
    LOG_ERR("CMA", "ensureStarted() failed (heap floor or NimBLE init)");
    screen = Screen::StartFailed;
    idleSinceMs = 0;
    requestUpdate();
  } else {
    chooseIdleScreen();
    idleSinceMs = millis();  // start the "nobody is driving the screen" idle timer

    // Paint the idle frame directly instead of requestUpdate()-ing: this is
    // the device's first paint every boot (a deep-sleep wake is a full chip
    // reset, so onEnter() runs fresh every time, cold boot or wake alike),
    // and main.cpp no longer calls goToBoot() for the companion path. By the
    // time this runs, ensureStarted() above has already finished (BLE is
    // live and advertising), so there is no actual "booting" state left to
    // report — this is just the plain idle screen, painted here instead of
    // through goToBoot()'s now-removed upstream splash to avoid flashing
    // that splash for one full refresh only to immediately overwrite it.
    renderer.clearScreen();
    if (screen == Screen::IconGrid) {
      renderIconGrid();
    } else {
      renderWaiting();
    }
  }
}

void CompanionModeActivity::onExit() {
  Activity::onExit();
  companionble::setContentFieldCallback(nullptr);
  companionble::setStatusCallback(nullptr);
  companionble::setPairingRequestCallback(nullptr);
  companionble::setForegroundChangeCallback(nullptr);
  companionble::setImageStagedCallback(nullptr);
#ifdef COMPANION_TEST_CONSOLE
  companiontest::setScreenNameProvider(nullptr);
  companiontest::setTagStateProvider(nullptr);
  g_screenNameActivity = nullptr;
#endif
  companionble::stop();

  portENTER_CRITICAL(&g_mux);
  g_pendingTitleReady = false;
  g_pendingBodyReady = false;
  g_pendingStatusReady = false;
  g_pendingCommitReady = false;
  g_pendingForegroundReady = false;
  g_pendingPairingReady = false;
  g_pendingImageReady = false;
  g_pendingBatchStartMs = 0;
  g_pendingBatchPoisoned = false;
  g_pendingTagStateReady = false;
  g_pendingTagStateInBatch = false;
  portEXIT_CRITICAL(&g_mux);
}

// ---------------------------------------------------------------------------
// Button map
// ---------------------------------------------------------------------------

void CompanionModeActivity::clearUiDeclaration() {
  for (auto& spec : buttons) {
    spec.routing = companionble::ButtonRouting::None;
    spec.label.clear();
  }
  tagRenderStyle = static_cast<uint8_t>(companionble::TagRenderStyle::Bordered);
  // Tags are re-populated from scratch by loadUiDeclaration() right after this
  // call (which captures the pre-clear state to restore by id). Without this
  // reset, every reload — a re-pushed declaration, a fresh ACQUIRE after a
  // reconnect — appended a second copy on top of tagCount instead of replacing
  // it, and setTagState()'s first-match update could end up shadowed by a
  // stale duplicate later in the array. Confirmed on hardware: after two
  // enrollment cycles in the same boot, CTAGS reported `tags count=4` with
  // tag ids 0 and 1 each appearing twice.
  tagCount = 0;
}

// Reads the foreground peer's declared control scheme off the SD card. Called
// on every foreground handover and whenever that peer pushes a new map, so an
// app update changes the buttons without a re-pair and without a firmware mode.
void CompanionModeActivity::loadUiDeclaration() {
  // A declaration can be re-pushed mid-session — most often because the user
  // switched phone language, since labels are localized and can even be
  // server-driven. That must change presentation only: an article the user
  // saved in English is still saved in German. So tag *state* is carried across
  // the reload by id, which is exactly why id and label are separate things on
  // the wire.
  uint8_t previousIds[companionble::kMaxTags];
  uint8_t previousStates[companionble::kMaxTags];
  const uint8_t previousCount = tagCount;
  for (uint8_t i = 0; i < previousCount; ++i) {
    previousIds[i] = tags[i].id;
    previousStates[i] = tags[i].state;
  }

  clearUiDeclaration();
  if (foregroundPeerKey.empty()) return;

  uint8_t raw[companionpeer::kMaxUiDeclarationLen];
  const size_t len =
      companionpeer::readAssetBody(foregroundPeerKey.c_str(), companionpeer::kAssetUiDeclaration, raw, sizeof(raw));
  if (len < 1) return;

  const uint8_t buttonEntries = raw[0];
  size_t offset = 1;
  for (uint8_t i = 0; i < buttonEntries && offset + 3 <= len; ++i) {
    const uint8_t buttonId = raw[offset];
    const uint8_t routing = raw[offset + 1];
    const uint8_t labelLen = raw[offset + 2];
    offset += 3;
    if (offset + labelLen > len) break;

    // POWER is firmware-owned in every app: a wedged app must never be able to
    // make the device un-sleepable.
    if (buttonId < kButtonCount && buttonId != static_cast<uint8_t>(companionble::ButtonId::Power) &&
        routing <= static_cast<uint8_t>(companionble::ButtonRouting::LocalSleep)) {
      buttons[buttonId].routing = static_cast<companionble::ButtonRouting>(routing);
      buttons[buttonId].label.assign(reinterpret_cast<const char*>(raw + offset), labelLen);
    }
    offset += labelLen;
  }

  // Tag section. Optional — an app with no tags may simply end after its
  // buttons. Every tag starts Hidden: the declaration says what exists, not
  // what is currently on, and the app pushes state separately.
  if (offset >= len) return;
  const uint8_t tagEntries = raw[offset++];
  for (uint8_t i = 0; i < tagEntries && offset + 2 <= len; ++i) {
    const uint8_t tagId = raw[offset];
    const uint8_t labelLen = raw[offset + 1];
    offset += 2;
    if (offset + labelLen > len) break;
    if (tagCount < companionble::kMaxTags) {
      TagSpec& tag = tags[tagCount++];
      tag.id = tagId;
      tag.state = static_cast<uint8_t>(companionble::TagState::Hidden);
      size_t copy = labelLen < companionble::kMaxTagLabelLen ? labelLen : companionble::kMaxTagLabelLen;
      // Truncate on a UTF-8 boundary so a clipped label is never invalid.
      while (copy > 0 && (raw[offset + copy] & 0xC0) == 0x80) --copy;
      memcpy(tag.label, raw + offset, copy);
      tag.label[copy] = '\0';
    }
    offset += labelLen;
  }

  // Optional trailing style byte: how this peer's tag row is drawn (see
  // companionble::TagRenderStyle). Absent — an older client, or a declaration
  // that ended after its tags — means Bordered, already set by
  // clearUiDeclaration() above. An out-of-range value is treated the same way
  // rather than trusted verbatim.
  if (offset < len) {
    if (raw[offset] <= static_cast<uint8_t>(companionble::TagRenderStyle::Plain)) {
      tagRenderStyle = raw[offset];
    }
    ++offset;  // advance past it regardless of validity, so a further trailing
               // byte (e.g. the capabilities bitmask read separately by
               // companionpeer::isImageCapable()) is never mistaken for this one.
  }

  // Restore state for every tag that still exists. A tag the new declaration
  // dropped simply goes away; one it added starts hidden.
  for (uint8_t i = 0; i < tagCount; ++i) {
    for (uint8_t j = 0; j < previousCount; ++j) {
      if (tags[i].id == previousIds[j]) {
        tags[i].state = previousStates[j];
        break;
      }
    }
  }
  measureTagRow();
}

// Applies a pushed tag-state field: count, then {tagId, state} pairs. Ids the
// peer never declared are ignored — the declaration is the only place tags come
// into existence.
void CompanionModeActivity::applyTagState(const uint8_t* data, size_t len) {
  if (len < 1) return;
  const uint8_t entries = data[0];
  size_t offset = 1;
  for (uint8_t i = 0; i < entries && offset + 2 <= len; ++i) {
    setTagState(data[offset], data[offset + 1]);
    offset += 2;
  }
  measureTagRow();
}

void CompanionModeActivity::setTagState(uint8_t tagId, uint8_t state) {
  if (state > static_cast<uint8_t>(companionble::TagState::Filled)) return;
  for (uint8_t i = 0; i < tagCount; ++i) {
    if (tags[i].id == tagId) {
      tags[i].state = state;
      return;
    }
  }
}

// Whether a tag currently occupies any space on screen. Hidden never draws;
// in Plain style Outline ("not set") draws nothing either, matching Bordered's
// long-standing Hidden behaviour rather than reserving a box that never shows.
bool CompanionModeActivity::tagIsDrawn(const TagSpec& tag) const {
  const auto state = static_cast<companionble::TagState>(tag.state);
  if (state == companionble::TagState::Hidden) return false;
  if (state == companionble::TagState::Outline &&
      tagRenderStyle == static_cast<uint8_t>(companionble::TagRenderStyle::Plain)) {
    return false;
  }
  return true;
}

// The tag row's width depends on which labels are currently visible, so the
// title's wrap budget is recomputed whenever either changes rather than
// reserving a worst case that would permanently narrow every title.
void CompanionModeActivity::measureTagRow() {
  const bool plain = tagRenderStyle == static_cast<uint8_t>(companionble::TagRenderStyle::Plain);
  int width = 0;
  for (uint8_t i = 0; i < tagCount; ++i) {
    if (!tagIsDrawn(tags[i])) continue;
    const int labelWidth = renderer.getTextWidth(UI_10_FONT_ID, tags[i].label);
    width += plain ? labelWidth + kTagChipGap : labelWidth + 2 * kTagChipPadX + kTagChipGap;
  }
  // The row-to-title gap only applies once, and only when something is
  // actually drawn — an empty row must still give the title the full width.
  if (width > 0) width += kTagRowLeftMargin;
  tagRowWidth = width;
}

companionble::ButtonRouting CompanionModeActivity::routingFor(companionble::ButtonId button) const {
  const size_t index = static_cast<size_t>(button);
  return index < kButtonCount ? buttons[index].routing : companionble::ButtonRouting::None;
}

const char* CompanionModeActivity::labelFor(companionble::ButtonId button) const {
  const size_t index = static_cast<size_t>(button);
  if (index >= kButtonCount) return "";
  // An empty label hides the hint entirely — the convention drawButtonHints()
  // itself checks.
  return buttons[index].routing == companionble::ButtonRouting::None ? "" : buttons[index].label.c_str();
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void CompanionModeActivity::computeViewport() {
  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);

  if (!mappedInput.hasTouch()) {
    // Reserve room for the bottom button-hint bar. Twice the top margin reads as
    // comfortable side whitespace without eating too much line width.
    const auto& metrics = UITheme::getInstance().getMetrics();
    cachedOrientedMarginBottom += metrics.buttonHintsHeight;
    cachedOrientedMarginLeft = cachedOrientedMarginTop * 2;
    cachedOrientedMarginRight = cachedOrientedMarginTop * 2;
  }

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  cachedTitleFontId = kCompanionTitleFontId;

  updateTitleLayout();
}

// Re-wraps `title` into `titleLines` (see kMaxTitleLines) and, since the title
// block's height varies with the wrapped line count, recomputes linesPerPage
// for the body underneath it.
void CompanionModeActivity::updateTitleLayout() {
  titleLines = wrapTitleToLines(title);

  constexpr int kTitleBottomSpacing = 6;  // gap between the title block and the first body line
  const int titleLineHeight = renderer.getLineHeight(cachedTitleFontId);
  cachedTitleBlockHeight = static_cast<int>(titleLines.size()) * titleLineHeight + kTitleBottomSpacing;

  const int viewportHeight =
      renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom - cachedTitleBlockHeight;
  const int lineHeight = renderer.getLineHeight(cachedFontId);

  linesPerPage = lineHeight > 0 ? viewportHeight / lineHeight : 1;
  if (linesPerPage < 1) linesPerPage = 1;
}

std::vector<std::string> CompanionModeActivity::wrapTitleToLines(const std::string& text) const {
  const int maxWidth = viewportWidth - tagRowWidth;
  std::vector<std::string> lines;
  std::string remaining = text;

  while (!remaining.empty()) {
    if (maxWidth <= 0 || renderer.getTextWidth(cachedTitleFontId, remaining.c_str(), EpdFontFamily::BOLD) <= maxWidth) {
      lines.push_back(remaining);
      remaining.clear();
      break;
    }

    const bool lastAllowedLine = static_cast<int>(lines.size()) + 1 >= kMaxTitleLines;
    if (lastAllowedLine) {
      std::string truncated = remaining;
      const std::string ellipsis = "\xE2\x80\xA6";  // U+2026 HORIZONTAL ELLIPSIS
      while (!truncated.empty() &&
             renderer.getTextWidth(cachedTitleFontId, (truncated + ellipsis).c_str(), EpdFontFamily::BOLD) > maxWidth) {
        popUtf8Char(truncated);
      }
      lines.push_back(truncated + ellipsis);
      remaining.clear();
      break;
    }

    size_t breakPos = remaining.length();
    while (breakPos > 0 && renderer.getTextWidth(cachedTitleFontId, remaining.substr(0, breakPos).c_str(),
                                                 EpdFontFamily::BOLD) > maxWidth) {
      size_t spacePos = remaining.rfind(' ', breakPos - 1);
      if (spacePos != std::string::npos && spacePos > 0) {
        breakPos = spacePos;
      } else {
        breakPos--;
        while (breakPos > 0 && (remaining[breakPos] & 0xC0) == 0x80) breakPos--;  // UTF-8 boundary
      }
    }
    if (breakPos == 0) breakPos = 1;

    lines.push_back(remaining.substr(0, breakPos));
    size_t skipChars = breakPos;
    if (breakPos < remaining.length() && remaining[breakPos] == ' ') skipChars++;
    remaining = remaining.substr(skipChars);
  }

  if (lines.empty()) lines.emplace_back();
  return lines;
}

void CompanionModeActivity::paginate() {
  pages.clear();
  currentPage = 0;

  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos < body.size()) {
    size_t lineEnd = body.find('\n', pos);
    if (lineEnd == std::string::npos) lineEnd = body.size();
    std::string line = body.substr(pos, lineEnd - pos);
    pos = lineEnd + 1;

    if (line.empty()) {
      lines.emplace_back();
      continue;
    }

    while (!line.empty()) {
      const int lineWidth = renderer.getTextAdvanceX(cachedFontId, line.c_str(), EpdFontFamily::REGULAR);
      if (lineWidth <= viewportWidth) {
        lines.push_back(line);
        break;
      }

      size_t breakPos = line.length();
      while (breakPos > 0 && renderer.getTextAdvanceX(cachedFontId, line.substr(0, breakPos).c_str(),
                                                      EpdFontFamily::REGULAR) > viewportWidth) {
        size_t spacePos = line.rfind(' ', breakPos - 1);
        if (spacePos != std::string::npos && spacePos > 0) {
          breakPos = spacePos;
        } else {
          breakPos--;
          while (breakPos > 0 && (line[breakPos] & 0xC0) == 0x80) breakPos--;  // UTF-8 boundary
        }
      }
      if (breakPos == 0) breakPos = 1;

      lines.push_back(line.substr(0, breakPos));
      size_t skipChars = breakPos;
      if (breakPos < line.length() && line[breakPos] == ' ') skipChars++;
      line = line.substr(skipChars);
    }
  }
  if (lines.empty()) lines.emplace_back();

  for (size_t i = 0; i < lines.size(); i += static_cast<size_t>(linesPerPage)) {
    const size_t end = std::min(lines.size(), i + static_cast<size_t>(linesPerPage));
    pages.emplace_back(lines.begin() + static_cast<long>(i), lines.begin() + static_cast<long>(end));
  }
  totalPages = static_cast<int>(pages.size());
}

// The screen shown when nobody is driving: the icon grid if any enrolled app
// has an icon, otherwise the plain waiting text.
void CompanionModeActivity::chooseIdleScreen() {
  screen = companionpeer::anyEnrolled() ? Screen::IconGrid : Screen::Waiting;
}

// ---------------------------------------------------------------------------
// Idle / power
// ---------------------------------------------------------------------------

void CompanionModeActivity::checkIdleTimers() {
  if (screen == Screen::Pairing && millis() > pairingDeadlineMs) {
    LOG_INF("CMA", "pairing prompt timed out");
    companionble::resolvePairing(/*accept=*/false, /*timedOut=*/true);
    pairingAppName.clear();
    RenderLock lock;
    chooseIdleScreen();
    requestUpdate();
    return;
  }

  if (screen == Screen::Message && millis() > transientMessageUntilMs) {
    RenderLock lock;
    screen = transientMessageReturnScreen;
    requestUpdate();
    return;
  }

  if (idleSinceMs == 0) return;
  if (millis() - idleSinceMs < kWaitingIdleSleepMs) return;

  // The screen has been held since a disconnect (see the protocol doc's
  // "On-screen behaviour"): the idle timeout is what finally takes it to the
  // sleep grid, rather than blanking the moment the phone goes away.
  if (screen == Screen::Text || screen == Screen::Image) {
    RenderLock lock;
    haveContent = false;
    pages.clear();
    chooseIdleScreen();
    idleSinceMs = millis();
    requestUpdate();
    return;
  }

  // See HalGPIO::isUsbOrDebugConnected()'s doc comment for why USB power skips
  // this too — deep-sleeping drops the USB CDC connection, which is actively
  // unhelpful while plugged in (charging, or connected for serial debugging).
  // Slide the idle clock forward while USB holds sleep off, rather than
  // leaving it stale: otherwise the elapsed idle time keeps accruing
  // unbounded for as long as USB stays connected, and unplugging after (say)
  // 30 idle minutes blows straight through the kWaitingIdleSleepMs guard
  // above and sleeps on the very next loop tick, instead of giving a genuine
  // fresh countdown from the moment USB actually goes away.
  if (gpio.isUsbOrDebugConnected()) {
    idleSinceMs = millis();
    return;
  }

  LOG_INF("CMA", "No app driving the screen for %lu ms, deep-sleeping", kWaitingIdleSleepMs);
  {
    RenderLock lock;
    renderPreSleepScreen();
  }
  powerManager.startDeepSleep(gpio);  // [[noreturn]] — wakes on power button, panel keeps its last image
}

// See Activity::customDeepSleep()'s doc comment. Reached only from main.cpp's
// enterDeepSleep() — i.e. the physical power-button long-press, or the
// general idle-timeout path if it were ever reached here (preventAutoSleep()
// always returns true while this activity is active, so in practice it is
// not). checkIdleTimers() and the app-mapped LocalSleep button already paint
// their own pre-sleep screen and call powerManager.startDeepSleep() directly
// (see above and handleMappedButton()) without going through
// enterDeepSleep()/customDeepSleep() at all.
bool CompanionModeActivity::customDeepSleep() {
  if (screen == Screen::StartFailed) return false;  // nothing sensible to draw; SleepActivity is the better fallback
  RenderLock lock;
  renderPreSleepScreen();
  return true;
}

// ---------------------------------------------------------------------------
// Handover / image
// ---------------------------------------------------------------------------

void CompanionModeActivity::applyForegroundChange() {
  RenderLock lock;
  loadUiDeclaration();

  if (foregroundPeerKey.empty()) {
    // Nobody holds the screen. Content stays up — it is the idle timeout, not
    // the handover, that clears it.
    idleSinceMs = millis();
    if (!haveContent && screen != Screen::Image) chooseIdleScreen();
  } else {
    // A different app took the screen: clear whatever the previous one left,
    // since the device retains no content for a background session. That
    // includes any gallery being browsed locally through the picker — a live
    // app taking the screen always wins over firmware-local browsing.
    idleSinceMs = 0;
    title.clear();
    body.clear();
    pages.clear();
    totalPages = 0;
    currentPage = 0;
    haveContent = false;
    displayedImagePath.clear();
    galleryImages.clear();
    galleryIndex = 0;
    browsingPeerKey.clear();
    galleryPickerBrowsing = false;
    foregroundPushedImageThisSession = false;
    updateTitleLayout();
    screen = Screen::Text;
  }
  requestUpdate();
}

// Decodes a staged raw packed 2bpp image (field 0x04) on the main loop task
// and reports the outcome back to the app. Never runs on the NimBLE host
// task: decoding writes the framebuffer.
//
// Before decoding, the staged file is moved into the pushing peer's bounded
// image gallery (CompanionPeerStore::commitImage) so it survives the next
// push instead of being overwritten by it — see this feature's commit message
// for why that lives in firmware rather than the phone.
void CompanionModeActivity::handlePendingImage(const std::string& stagedPath, const std::string& peerKey,
                                               const uint8_t* contentId, size_t contentIdLen) {
  std::string path = companionpeer::commitImage(peerKey.c_str(), stagedPath, contentId, contentIdLen);
  if (path.empty()) {
    // The gallery move failed (e.g. SD write error); fall back to displaying
    // straight from the scratch file so a storage hiccup doesn't also cost the
    // app the push it just made. It just won't be in the gallery afterward.
    LOG_ERR("CMA", "could not commit staged image for peer %s; displaying from scratch path", peerKey.c_str());
    path = stagedPath;
  }

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(path);
  if (!decoder) {
    LOG_ERR("CMA", "no decoder for staged image %s", path.c_str());
    companionble::notifyRenderStatus(companionble::RenderResult::DecodeFailed, companionble::kFieldImage);
    return;
  }
  ImageDimensions dims{};
  if (!decoder->getDimensions(path, dims)) {
    LOG_ERR("CMA", "staged image %s did not decode", path.c_str());
    companionble::notifyRenderStatus(companionble::RenderResult::DecodeFailed, companionble::kFieldImage);
    return;
  }

  RenderLock lock;
  // Arm the one status this push is owed. Set under the lock, alongside the
  // state that makes render() take the Screen::Image branch, so the render
  // task can never observe one without the other. Unconditional: a previous
  // push that never got its answer (link dropped mid-settle) must not stop
  // this one from being answered — but it does get told it lost the screen
  // first, rather than being silently overwritten.
  supersedePendingRenderStatus();
  renderAwaitingStatus = true;
  renderAwaitingField = companionble::kFieldImage;
  displayedImagePath = path;
  foregroundPushedImageThisSession = true;
  galleryPickerBrowsing = false;  // a live push always wins over picker browsing
  browsingPeerKey.clear();
  refreshGalleryForForeground();
  screen = Screen::Image;
  requestUpdate();
}

// Rebuilds the in-memory gallery list from a peer's stored images and points
// galleryIndex/displayedImagePath at the most recent one. The list itself
// lives on SD (CompanionPeerStore), so this is just re-reading a few dozen
// bytes of JSON, not holding a second copy of anything image-sized. Used both
// for the foreground peer (via refreshGalleryForForeground(), below) and for
// an arbitrary peer selected through the gallery picker or the disconnect
// fallback — browsing a peer's gallery is a local SD read, independent of
// whether that peer has a live BLE session at all.
void CompanionModeActivity::loadGalleryForPeer(const std::string& peerKey) {
  galleryImages.clear();
  galleryIndex = 0;
  if (peerKey.empty()) return;

  companionpeer::ImageEntry entries[companionpeer::kMaxImagesPerPeer];
  const size_t count = companionpeer::listImages(peerKey.c_str(), entries, companionpeer::kMaxImagesPerPeer);
  galleryImages.reserve(count);
  for (size_t i = 0; i < count; ++i) galleryImages.push_back(entries[i].path);
  if (!galleryImages.empty()) {
    galleryIndex = galleryImages.size() - 1;
    displayedImagePath = galleryImages[galleryIndex];
  }
}

void CompanionModeActivity::refreshGalleryForForeground() { loadGalleryForPeer(foregroundPeerKey); }

// Redraws the image at `index` in the current gallery without touching BLE:
// this is firmware-local browsing of already-pushed images, not new content.
void CompanionModeActivity::showGalleryImage(size_t index) {
  if (index >= galleryImages.size()) return;
  RenderLock lock;
  galleryIndex = index;
  displayedImagePath = galleryImages[index];
  requestUpdate();
}

// Button::Up/Down gallery prev/next, active only in Screen::Image and only
// for a button the foreground peer's own map hasn't claimed for something
// that actually applies on this screen. Remote and LocalSleep always defer to
// the app — those are meaningful regardless of what's on screen.
//
// This deliberately does NOT use Left/Right: on real hardware those are the
// bottom front buttons, and every app map seen so far (including scripts/
// push_companion_content.py's default) routes them to LOCAL_PAGE_PREV/NEXT for
// paging buffered text — i.e. what users call "the page-turn buttons".
// Up/Down are the side buttons, physically the pair toward the top of the
// device, and are otherwise unclaimed by a typical text/image app's button
// map, which is exactly why they're free for this. (An app that legitimately
// wants Up/Down for its own purpose, e.g. a camera-control app, still keeps
// them — the None-only guard below never overrides a declared routing.)
// LocalPagePrev/LocalPageNext are still accepted here too: handleMappedButton()
// already scopes them to Screen::Text and no-ops elsewhere, so claiming them
// on Image costs nothing if some future app reuses those routings on Up/Down.
namespace {
bool isGalleryClaimable(companionble::ButtonRouting routing) {
  return routing == companionble::ButtonRouting::None || routing == companionble::ButtonRouting::LocalPagePrev ||
         routing == companionble::ButtonRouting::LocalPageNext;
}
}  // namespace

bool CompanionModeActivity::handleGalleryNav() {
  if (screen != Screen::Image || galleryImages.size() < 2) return false;

  if (isGalleryClaimable(routingFor(companionble::ButtonId::Up)) &&
      buttonWasPressed(MappedInputManager::Button::Up, companionble::ButtonId::Up)) {
    showGalleryImage(galleryIndex == 0 ? galleryImages.size() - 1 : galleryIndex - 1);
    return true;
  }
  if (isGalleryClaimable(routingFor(companionble::ButtonId::Down)) &&
      buttonWasPressed(MappedInputManager::Button::Down, companionble::ButtonId::Down)) {
    showGalleryImage(galleryIndex + 1 >= galleryImages.size() ? 0 : galleryIndex + 1);
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Gallery picker
// ---------------------------------------------------------------------------

// Puts a short line on screen for a few seconds, then reverts to `returnTo`.
// checkIdleTimers() (polled every loop()) is what actually reverts it.
void CompanionModeActivity::showTransientMessage(const std::string& text, Screen returnTo, unsigned long durationMs) {
  RenderLock lock;
  transientMessage = text;
  transientMessageReturnScreen = returnTo;
  transientMessageUntilMs = millis() + durationMs;
  screen = Screen::Message;
  requestUpdate();
}

// Confirm on the idle icon grid. Builds pickerPeerKeys from every enrolled
// peer (companionpeer::listPeers() — ungrouped, unlike the decorative grid's
// listIconTiles(), since two installs of the same app must stay two separate
// tiles here) that declared the image-gallery capability, capped at
// kMaxIconTiles the same as the decorative grid's tile budget. Falls back to
// a transient message rather than entering an empty picker.
void CompanionModeActivity::enterGalleryPicker() {
  char keys[companionpeer::kMaxPeers][companionpeer::kPeerKeyLen];
  const size_t total = companionpeer::listPeers(keys, companionpeer::kMaxPeers);

  pickerPeerKeys.clear();
  for (size_t i = 0; i < total && pickerPeerKeys.size() < companionpeer::kMaxIconTiles; ++i) {
    if (companionpeer::isImageCapable(keys[i])) pickerPeerKeys.emplace_back(keys[i]);
  }

  if (pickerPeerKeys.empty()) {
    showTransientMessage(tr(STR_COMPANION_NO_GALLERY_APPS), Screen::IconGrid);
    return;
  }

  RenderLock lock;
  pickerCursor = 0;
  screen = Screen::GalleryPicker;
  requestUpdate();
}

// Confirm on the picker grid: load the highlighted peer's stored gallery —
// purely a local SD read, no BLE involved regardless of whether that peer is
// currently connected — and show it, or say there's nothing to show yet.
void CompanionModeActivity::selectGalleryPickerPeer() {
  if (pickerCursor >= pickerPeerKeys.size()) return;
  const std::string peerKey = pickerPeerKeys[pickerCursor];
  loadGalleryForPeer(peerKey);

  if (galleryImages.empty()) {
    showTransientMessage(tr(STR_COMPANION_GALLERY_EMPTY), Screen::GalleryPicker);
    return;
  }

  RenderLock lock;
  browsingPeerKey = peerKey;
  galleryPickerBrowsing = true;
  screen = Screen::Image;
  requestUpdate();
}

// Input for the picker itself and for leaving a gallery reached through it.
// Tried before the foreground-button dispatch in loop(), same as
// handleGalleryNav() — these are firmware-owned screens/modes with no
// foreground peer's button map to defer to (most peers in the picker have no
// live session at all).
bool CompanionModeActivity::handlePickerInput() {
  if (screen == Screen::IconGrid) {
    if (buttonWasPressed(MappedInputManager::Button::Confirm, companionble::ButtonId::Confirm)) {
      enterGalleryPicker();
      return true;
    }
    return false;
  }

  if (screen == Screen::GalleryPicker) {
    if (pickerPeerKeys.empty()) return false;  // defensive; enterGalleryPicker() never leaves this empty
    if (buttonWasPressed(MappedInputManager::Button::Up, companionble::ButtonId::Up)) {
      RenderLock lock;
      pickerCursor = pickerCursor == 0 ? pickerPeerKeys.size() - 1 : pickerCursor - 1;
      requestUpdate();
      return true;
    }
    if (buttonWasPressed(MappedInputManager::Button::Down, companionble::ButtonId::Down)) {
      RenderLock lock;
      pickerCursor = pickerCursor + 1 >= pickerPeerKeys.size() ? 0 : pickerCursor + 1;
      requestUpdate();
      return true;
    }
    if (buttonWasPressed(MappedInputManager::Button::Confirm, companionble::ButtonId::Confirm)) {
      selectGalleryPickerPeer();
      return true;
    }
    if (buttonWasPressed(MappedInputManager::Button::Back, companionble::ButtonId::Back)) {
      RenderLock lock;
      screen = Screen::IconGrid;
      requestUpdate();
      return true;
    }
    return false;
  }

  if (screen == Screen::Image && galleryPickerBrowsing) {
    if (buttonWasPressed(MappedInputManager::Button::Back, companionble::ButtonId::Back)) {
      RenderLock lock;
      screen = Screen::GalleryPicker;
      galleryPickerBrowsing = false;
      requestUpdate();
      return true;
    }
    return false;
  }

  return false;
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void CompanionModeActivity::loop() {
  if (screen == Screen::StartFailed) return;  // nothing to poll: BLE never came up

  // Relax the connection interval/slave latency once the link has been idle
  // for a while; tightening back happens automatically on the next write or
  // notify (see CompanionBle.h's tick() doc comment).
  companionble::tick();

  const bool nowConnected = companionble::isConnected();
  if (nowConnected != connected) {
    // RenderLock: screen/pages gate which branch render() takes and renderPage()
    // iterates `pages` directly — mutating either here without the lock races
    // the render task, which can observe a torn `pages` against a stale
    // totalPages/currentPage and index out of bounds. Confirmed via a real
    // device crash before this fix. See EpubReaderActivity.cpp for the same
    // convention.
    RenderLock lock;
    connected = nowConnected;
    if (!connected) {
      // Nobody left to answer. Anything the departing peer was still owed dies
      // with the link; carrying the expectation forward would fire
      // RENDER_STATUS at whichever peer connects next, on a redraw it never
      // asked for.
      renderAwaitingStatus = false;

      // If the peer that just lost the link is image-capable and never
      // pushed anything this session, its own gallery is more useful than an
      // empty "waiting" screen or holding nothing until the idle timeout —
      // jump straight to it. A peer that did push content keeps that content
      // up, same as always (the branch below still applies).
      if (!foregroundPeerKey.empty() && !foregroundPushedImageThisSession &&
          companionpeer::isImageCapable(foregroundPeerKey.c_str())) {
        loadGalleryForPeer(foregroundPeerKey);
        if (!galleryImages.empty()) {
          browsingPeerKey = foregroundPeerKey;
          galleryPickerBrowsing = true;
          screen = Screen::Image;
        } else {
          // Inlined rather than calling showTransientMessage(): that helper
          // takes its own RenderLock, and this block already holds one —
          // nesting would deadlock (see RenderLock.h; the underlying
          // semaphore is not recursive).
          transientMessage = tr(STR_COMPANION_GALLERY_EMPTY);
          transientMessageReturnScreen = Screen::IconGrid;
          transientMessageUntilMs = millis() + 3000;
          screen = Screen::Message;
        }
      }

      // Content is deliberately NOT cleared here. The last thing pushed stays on
      // screen until the idle timeout takes it to the sleep grid.
      idleSinceMs = millis();
      foregroundPeerKey.clear();
      foregroundAppName.clear();
      if (screen == Screen::Pairing) {
        pairingAppName.clear();
        chooseIdleScreen();
      }
      portENTER_CRITICAL(&g_mux);
      g_pendingTitleReady = false;
      g_pendingBodyReady = false;
      g_pendingCommitReady = false;
      g_pendingBatchStartMs = 0;
      g_pendingBatchPoisoned = false;
      // Tag state belonging to the batch the link just killed goes with it —
      // otherwise it would surface on the next loop as if it were a standalone
      // tag push, marking whatever content is still on screen. A genuinely
      // standalone pending tag push is left alone.
      if (g_pendingTagStateInBatch) {
        g_pendingTagStateReady = false;
        g_pendingTagStateInBatch = false;
      }
      portEXIT_CRITICAL(&g_mux);
    }
    // A displayed image is already on the panel and unaffected by the link
    // dropping — redrawing it here just re-decodes and re-flips the same
    // pixels, flickering the panel for nothing. Everything else (text,
    // waiting, pairing) still needs a redraw: the connection badge/prompt
    // they show depends on `connected`.
    if (!(screen == Screen::Image && !connected)) requestUpdate();
  }

  // Drain the host-task handoffs.
  bool gotTitle = false;
  bool gotBody = false;
  bool commit = false;
  bool gotStatus = false;
  bool gotForeground = false;
  bool gotPairing = false;
  bool gotImage = false;
  std::string newTitle;
  std::string newBody;
  uint8_t newTagId = 0;
  uint8_t newTagStateValue = 0;
  bool gotTagState = false;
  bool tagStateWasInBatch = false;
  uint8_t newTagStateBuf[sizeof(g_pendingTagStateBuf)] = {0};
  uint8_t newTagStateLen = 0;
  char newForegroundKey[companionpeer::kPeerKeyLen] = {0};
  char newForegroundName[companionpeer::kMaxNameLen + 1] = {0};
  char newPairingName[companionpeer::kMaxNameLen + 1] = {0};
  char newImagePath[sizeof(g_pendingImagePath)] = {0};
  char newImagePeerKey[sizeof(g_pendingImagePeerKey)] = {0};
  uint8_t newImageContentId[sizeof(g_pendingImageContentId)] = {0};
  uint8_t newImageContentIdLen = 0;

  portENTER_CRITICAL(&g_mux);
  if (g_pendingTitleReady) {
    newTitle.assign(reinterpret_cast<char*>(g_pendingTitleBuf), g_pendingTitleLen);
    gotTitle = true;
  }
  if (g_pendingBodyReady) {
    newBody.assign(reinterpret_cast<char*>(g_pendingBodyBuf), g_pendingBodyLen);
    gotBody = true;
  }
  bool poisoned = false;
  if (g_pendingCommitReady) {
    commit = true;
  } else if ((gotTitle || gotBody || g_pendingBatchPoisoned) && g_pendingBatchStartMs != 0 &&
             millis() - g_pendingBatchStartMs > kPendingBatchTimeoutMs) {
    // Safety net: the final-flagged field's END never arrived in time (e.g. the
    // app crashed or lost the connection mid-push). Apply whatever we have
    // rather than leaving the screen stuck on stale content indefinitely.
    // A poisoned batch resolves here too — it still has to be *cleared*, or the
    // poison would leak into the next batch, it is just discarded rather than
    // applied.
    LOG_ERR("CMA", "content batch commit flag missed after %lu ms, applying pending fields anyway",
            static_cast<unsigned long>(kPendingBatchTimeoutMs));
    commit = true;
  }
  if (commit) {
    poisoned = g_pendingBatchPoisoned;
    g_pendingTitleReady = false;
    g_pendingBodyReady = false;
    g_pendingCommitReady = false;
    g_pendingBatchStartMs = 0;
    g_pendingBatchPoisoned = false;
  }
  if (g_pendingStatusReady) {
    newTagId = g_pendingTagId;
    newTagStateValue = g_pendingTagState;
    g_pendingStatusReady = false;
    gotStatus = true;
  }
  if (g_pendingTagStateReady) {
    memcpy(newTagStateBuf, g_pendingTagStateBuf, g_pendingTagStateLen);
    newTagStateLen = g_pendingTagStateLen;
    g_pendingTagStateReady = false;
    tagStateWasInBatch = g_pendingTagStateInBatch;
    g_pendingTagStateInBatch = false;
    gotTagState = true;
  }
  if (g_pendingForegroundReady) {
    memcpy(newForegroundKey, g_pendingForegroundKey, sizeof(newForegroundKey));
    memcpy(newForegroundName, g_pendingForegroundName, sizeof(newForegroundName));
    g_pendingForegroundReady = false;
    gotForeground = true;
  }
  if (g_pendingPairingReady) {
    memcpy(newPairingName, g_pendingPairingName, sizeof(newPairingName));
    g_pendingPairingReady = false;
    gotPairing = true;
  }
  if (g_pendingImageReady) {
    memcpy(newImagePath, g_pendingImagePath, sizeof(newImagePath));
    memcpy(newImagePeerKey, g_pendingImagePeerKey, sizeof(newImagePeerKey));
    memcpy(newImageContentId, g_pendingImageContentId, sizeof(newImageContentId));
    newImageContentIdLen = g_pendingImageContentIdLen;
    g_pendingImageReady = false;
    gotImage = true;
  }
  portEXIT_CRITICAL(&g_mux);

  if (poisoned) {
    // A field of this batch was lost to a CHUNK sequence gap. Applying the
    // survivors would mix this article's body with the last one's title, so the
    // whole batch goes in the bin and the previous — coherent — page stays on
    // screen. The phone already has its FIELD_SEQ_GAP notification and both
    // consumer apps respond by re-pushing the whole batch, so this is a delay,
    // not a permanent loss.
    LOG_ERR("CMA", "content batch discarded: a field was lost to a CHUNK sequence gap");
    gotTitle = false;
    gotBody = false;
    commit = false;
    // Tag state that rode this batch goes with it. Letting it through would be
    // the worst of the three outcomes: the stale article left on screen would
    // wear the *new* article's tags, i.e. a mark that belongs to content the
    // user cannot see. Clean failure beats that, and beats a half-applied
    // batch. A standalone tag push is untouched — see g_pendingTagStateInBatch.
    if (tagStateWasInBatch) gotTagState = false;

    // A discarded batch never renders, so nothing will reach the Screen::Text
    // branch of render() to answer it the normal way (see notifyRenderStatus()
    // calls below). A client awaiting RENDER_STATUS after this push would
    // otherwise block until its own timeout for a render that is never coming
    // -- answer immediately instead, same opcode, SequenceGap standing in for
    // "discarded" the same way it already does for a single dropped field via
    // FIELD_SEQ_GAP. This bypasses renderAwaitingStatus entirely (nothing was
    // armed for this batch — the arm only happens once a commit is about to
    // render, below) so it cannot race or double-answer a render that does
    // happen to land.
    companionble::notifyRenderStatus(companionble::RenderResult::SequenceGap, companionble::kFieldBody);
  }

  if (gotPairing) {
    RenderLock lock;
    pairingAppName = newPairingName;
    pairingDeadlineMs = millis() + kPairingTimeoutMs;
    screen = Screen::Pairing;
    idleSinceMs = 0;  // a prompt on screen is not idle
    requestUpdate();
  }

  if (gotForeground) {
    foregroundPeerKey = newForegroundKey;
    foregroundAppName = newForegroundName;
    applyForegroundChange();
  }

  if (gotImage) {
    handlePendingImage(newImagePath, newImagePeerKey, newImageContentId, newImageContentIdLen);
  }

  if (commit && (gotTitle || gotBody)) {
    // One lock for both halves so a render can never land between the title and
    // body updates of a single push and see a mismatched pairing.
    RenderLock lock;
    // Arm the one status this push is owed, same mechanism and same caveats as
    // handlePendingImage()'s image arm above: unconditional (a previous push
    // that never got its answer must not block this one), set under the lock
    // alongside the state that makes render() take the Screen::Text branch, so
    // the render task can never observe the flag without the content it names.
    // Consumed exactly once, from render()'s Screen::Text case, once the panel
    // actually shows this batch -- see notifyRenderPushResult(). Any push still
    // awaiting an answer here is told it was superseded before this one arms,
    // rather than being silently overwritten.
    supersedePendingRenderStatus();
    renderAwaitingStatus = true;
    renderAwaitingField = companionble::kFieldBody;
    if (gotTitle) {
      title = newTitle;
      updateTitleLayout();
      if (haveContent && !gotBody) paginate();
    }
    if (gotBody) {
      body = newBody;
      paginate();
      haveContent = true;
    }
    // Text replaces an image, and vice versa. There is no compositing and no
    // mode to enter: the last completed push owns the screen. That includes
    // replacing a gallery being browsed locally through the picker.
    screen = Screen::Text;
    displayedImagePath.clear();
    galleryPickerBrowsing = false;
    browsingPeerKey.clear();
    requestUpdate();
  }

  if (gotTagState) {
    // Arrives as a content field so it can ride the atomic batch: pushed with
    // title/body under one kFinalFieldFlag, content and its tag state land in a
    // single redraw and there is never a frame where new content wears the
    // previous content's tags.
    RenderLock lock;
    applyTagState(newTagStateBuf, newTagStateLen);
    if (!commit) {
      forceFastRefreshNextRender = true;
      if (screen == Screen::Image) tagOnlyRedraw = true;
    }
    requestUpdate();
  }

  if (gotStatus) {
    RenderLock lock;
    setTagState(newTagId, newTagStateValue);
    measureTagRow();
    forceFastRefreshNextRender = true;
    // A tag change while a print is on screen must not re-develop the print:
    // re-decoding and re-settling costs several seconds for a chip that moved.
    // The framebuffer still holds the image (renderAntiAliased restores the BW
    // buffer), so the chips can be redrawn over it directly.
    if (screen == Screen::Image) tagOnlyRedraw = true;
    requestUpdate();
  }

  checkIdleTimers();

  // The pairing prompt is the one screen with a firmware-owned control scheme —
  // the app asking to pair has, by definition, not had a button map accepted
  // yet.
  if (screen == Screen::Pairing) {
    if (buttonWasPressed(MappedInputManager::Button::Confirm, companionble::ButtonId::Confirm)) {
      companionble::resolvePairing(/*accept=*/true);
      RenderLock lock;
      pairingAppName.clear();
      screen = Screen::Text;  // the app will ACQUIRE and push next
      requestUpdate();
    } else if (buttonWasPressed(MappedInputManager::Button::Back, companionble::ButtonId::Back)) {
      companionble::resolvePairing(/*accept=*/false);
      RenderLock lock;
      pairingAppName.clear();
      chooseIdleScreen();
      requestUpdate();
    }
    return;
  }

  // Gallery browsing is tried before the foreground check and works even with
  // nobody connected: pushed images already survive disconnect (see the
  // "nobody holds the screen" branch of applyForegroundChange(), which leaves
  // Screen::Image and galleryImages alone) — requiring a live peer here would
  // make a persisted gallery unbrowsable exactly when there's no app around to
  // re-push anything, which defeats the point of persisting more than one
  // image. routingFor() still reflects the last foreground peer's declared
  // map after it disconnects (buttons[] is only reset by the *next* peer's
  // loadUiDeclaration()), so the "don't steal a claimed button" gate keeps
  // working the same whether or not that peer is still connected.
  handleGalleryNav();

  // Same reasoning as handleGalleryNav() above: the icon grid, the gallery
  // picker, and a gallery reached through it are all firmware-owned screens
  // with no foreground peer's button map to defer to, so this is tried
  // regardless of whether foregroundPeerKey is empty.
  if (handlePickerInput()) return;

  if (foregroundPeerKey.empty()) return;  // no app owns the buttons

  // Route each button through the foreground app's declared map. Nothing here
  // decides what a button means — routingFor() is the app's own answer, read
  // back off the SD card.
  const bool handled = handleMappedButton(MappedInputManager::Button::Left, companionble::ButtonId::Left) ||
                       handleMappedButton(MappedInputManager::Button::Right, companionble::ButtonId::Right) ||
                       handleMappedButton(MappedInputManager::Button::Up, companionble::ButtonId::Up) ||
                       handleMappedButton(MappedInputManager::Button::Down, companionble::ButtonId::Down) ||
                       handleMappedButton(MappedInputManager::Button::Back, companionble::ButtonId::Back) ||
                       handleMappedButton(MappedInputManager::Button::Confirm, companionble::ButtonId::Confirm);
  (void)handled;

  if (holdActive) {
    // Map the tracked button back to its MappedInputManager role to poll
    // isPressed/wasReleased/getHeldTime for it specifically — getHeldTime() is a
    // single global timer (only one physical button can be held at a time on
    // this hardware), so it is always describing holdButton's press.
    MappedInputManager::Button trackedRole = MappedInputManager::Button::Confirm;
    switch (holdButton) {
      case companionble::ButtonId::Up:
        trackedRole = MappedInputManager::Button::Up;
        break;
      case companionble::ButtonId::Down:
        trackedRole = MappedInputManager::Button::Down;
        break;
      case companionble::ButtonId::Left:
        trackedRole = MappedInputManager::Button::Left;
        break;
      case companionble::ButtonId::Right:
        trackedRole = MappedInputManager::Button::Right;
        break;
      case companionble::ButtonId::Back:
        trackedRole = MappedInputManager::Button::Back;
        break;
      case companionble::ButtonId::Confirm:
      case companionble::ButtonId::Power:
        trackedRole = MappedInputManager::Button::Confirm;
        break;
    }

    // Read the held time before wasReleased(): under COMPANION_TEST_CONSOLE,
    // wasReleased() consumes the injected press's releasePending flag, which
    // holdInProgress() (and so getHeldTime()) depends on to report anything
    // but 0. Real hardware has no such coupling, so this ordering is a no-op
    // there.
    const uint16_t heldTicks =
        static_cast<uint16_t>(std::min<unsigned long>(buttonHeldTime(holdButton) / kHoldTickMs, 0xFFFFUL));
    if (buttonWasReleased(trackedRole, holdButton)) {
      companionble::notifyButtonEvent(holdButton, heldTicks, /*isFinal=*/true);
      holdActive = false;
    } else if (buttonIsPressed(trackedRole, holdButton)) {
      if (heldTicks > holdTicksSent) {
        companionble::notifyButtonEvent(holdButton, heldTicks, /*isFinal=*/false);
        holdTicksSent = heldTicks;
      }
    }
  }
}

bool CompanionModeActivity::buttonWasPressed(MappedInputManager::Button role, companionble::ButtonId id) const {
#ifdef COMPANION_TEST_CONSOLE
  if (companiontest::wasPressed(id)) return true;
#else
  (void)id;
#endif
  return mappedInput.wasPressed(role);
}

bool CompanionModeActivity::buttonIsPressed(MappedInputManager::Button role, companionble::ButtonId id) const {
#ifdef COMPANION_TEST_CONSOLE
  if (companiontest::isPressed(id)) return true;
#else
  (void)id;
#endif
  return mappedInput.isPressed(role);
}

bool CompanionModeActivity::buttonWasReleased(MappedInputManager::Button role, companionble::ButtonId id) const {
#ifdef COMPANION_TEST_CONSOLE
  if (companiontest::wasReleased(id)) return true;
#else
  (void)id;
#endif
  return mappedInput.wasReleased(role);
}

unsigned long CompanionModeActivity::buttonHeldTime(companionble::ButtonId id) const {
#ifdef COMPANION_TEST_CONSOLE
  (void)id;
  if (companiontest::holdInProgress()) return companiontest::heldTimeMs();
#else
  (void)id;
#endif
  return mappedInput.getHeldTime();
}

#ifdef COMPANION_TEST_CONSOLE
uint8_t CompanionModeActivity::reportTags(companiontest::TagReport* out, uint8_t maxTags) const {
  const uint8_t count = tagCount < maxTags ? tagCount : maxTags;
  for (uint8_t i = 0; i < count; ++i) {
    out[i].id = tags[i].id;
    out[i].state = tags[i].state;
    snprintf(out[i].label, sizeof(out[i].label), "%s", tags[i].label);
  }
  return count;
}
#endif

const char* CompanionModeActivity::screenName() const {
  switch (screen) {
    case Screen::StartFailed:
      return "start_failed";
    case Screen::Waiting:
      return "waiting";
    case Screen::IconGrid:
      return "icon_grid";
    case Screen::GalleryPicker:
      return "gallery_picker";
    case Screen::Pairing:
      return "pairing";
    case Screen::Text:
      return haveContent ? "text" : "waiting_app";
    case Screen::Image:
      return "image";
    case Screen::Message:
      return "message";
  }
  return "?";
}

// Applies one button press according to the foreground app's map. Returns true
// if the press was consumed.
bool CompanionModeActivity::handleMappedButton(MappedInputManager::Button role, companionble::ButtonId id) {
  if (!buttonWasPressed(role, id)) return false;

  switch (routingFor(id)) {
    case companionble::ButtonRouting::None:
      return false;

    case companionble::ButtonRouting::Remote:
      notifyHeldButton(id);
      return true;

    case companionble::ButtonRouting::LocalPagePrev:
      if (screen == Screen::Text && currentPage > 0) {
        RenderLock lock;
        currentPage--;
        requestUpdate();
      }
      return true;

    case companionble::ButtonRouting::LocalPageNext:
      if (screen == Screen::Text && currentPage < totalPages - 1) {
        RenderLock lock;
        currentPage++;
        requestUpdate();
      }
      return true;

    case companionble::ButtonRouting::LocalSleep:
      LOG_INF("CMA", "app-mapped sleep button");
      {
        RenderLock lock;
        renderPreSleepScreen();
      }
      powerManager.startDeepSleep(gpio);  // [[noreturn]]
      return true;
  }
  return false;
}

// Starts (or restarts) hold-tracking for a just-pressed button and sends the
// initial-down notification (duration 0, isFinal false).
void CompanionModeActivity::notifyHeldButton(companionble::ButtonId button) {
  companionble::notifyButtonEvent(button, /*durationTicks=*/0, /*isFinal=*/false);
  holdActive = true;
  holdButton = button;
  holdTicksSent = 0;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void CompanionModeActivity::render(RenderLock&&) {
  // Tag-only change over a displayed print: the framebuffer still holds the
  // image, so redraw just the chips and flip with a differential refresh
  // instead of re-decoding and re-settling a photo for a moved mark.
  if (tagOnlyRedraw && screen == Screen::Image && !displayedImagePath.empty()) {
    tagOnlyRedraw = false;
    forceFastRefreshNextRender = false;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    renderTags(renderer.getScreenWidth() - cachedOrientedMarginRight, cachedOrientedMarginTop + lineHeight);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
  tagOnlyRedraw = false;

  renderer.clearScreen();
  switch (screen) {
    case Screen::StartFailed:
      renderStartFailed();
      break;
    case Screen::Pairing:
      renderPairingPrompt();
      break;
    case Screen::IconGrid:
      renderIconGrid();
      break;
    case Screen::GalleryPicker:
      renderGalleryPicker();
      break;
    case Screen::Image:
      renderImage();
      break;
    case Screen::Text:
      if (haveContent) {
        renderPage();
      } else {
        renderWaiting();
      }
      // Both branches above end with the actual displayBuffer()/
      // displayWithRefreshCycle() call that puts pixels on the panel, so
      // control reaching here means this redraw is done -- whichever branch
      // ran. Placed at the Screen::Text case rather than inside renderPage()
      // itself so a content push that names a title with no body yet (haveContent
      // still false, so renderWaiting() runs instead) still gets answered
      // instead of leaving renderAwaitingStatus armed until some unrelated
      // future push resolves it. notifyRenderPushResult() no-ops unless a push
      // actually armed the flag, so an ordinary redraw (reconnect, tag change,
      // page turn) with no push in flight is silent, exactly like renderImage().
      notifyRenderPushResult(companionble::RenderResult::Displayed);
      break;
    case Screen::Waiting:
      renderWaiting();
      break;
    case Screen::Message:
      renderTransientMessage();
      break;
  }
}

void CompanionModeActivity::renderWaiting(bool inverted, const char* label) {
  // "Waiting for <app>" once an app holds the screen but has pushed nothing;
  // the generic "Waiting for phone..." before that.
  const int centerY = renderer.getScreenHeight() / 2;
  if (!foregroundAppName.empty()) {
    renderer.drawCenteredText(cachedTitleFontId, centerY - renderer.getLineHeight(cachedTitleFontId),
                              foregroundAppName.c_str(), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(kCompanionFontId, centerY, tr(STR_COMPANION_WAITING_APP), true);
  } else {
    renderer.drawCenteredText(kCompanionFontId, centerY, tr(STR_COMPANION_WAITING), true, EpdFontFamily::BOLD);
  }
  if (label && *label) {
    renderer.drawCenteredText(SMALL_FONT_ID, centerY + renderer.getLineHeight(kCompanionFontId) * 2, label, true);
  }
  if (inverted) renderer.invertScreen();
  renderer.displayBuffer();
}

void CompanionModeActivity::renderStartFailed() {
  renderer.drawCenteredText(kCompanionFontId, renderer.getScreenHeight() / 2, tr(STR_COMPANION_START_FAILED), true,
                            EpdFontFamily::BOLD);
  renderer.displayBuffer();
}

void CompanionModeActivity::renderPairingPrompt() {
  const int centerY = renderer.getScreenHeight() / 2;
  const int lineHeight = renderer.getLineHeight(cachedTitleFontId);
  renderer.drawCenteredText(kCompanionFontId, centerY - lineHeight, tr(STR_COMPANION_PAIR_PROMPT), true);
  renderer.drawCenteredText(cachedTitleFontId, centerY, pairingAppName.c_str(), true, EpdFontFamily::BOLD);

  if (!mappedInput.hasTouch()) {
    const auto labels = mappedInput.mapLabels(tr(STR_COMPANION_PAIR_NO), tr(STR_COMPANION_PAIR_YES), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}

// The decorative sleep grid: one tile per enrolled app, grouped by appId, with
// the connected app's tile marked. Not a launcher — the device cannot start an
// app on the phone, so a selectable grid would promise something it cannot
// deliver.
void CompanionModeActivity::renderIconGrid(bool inverted, const char* label) {
  char keys[companionpeer::kMaxIconTiles][companionpeer::kPeerKeyLen];
  const size_t count = companionpeer::listIconTiles(keys, companionpeer::kMaxIconTiles);
  if (count == 0) {
    renderWaiting(inverted, label);
    return;
  }

  const int tile = companionble::kIconWidthPx;
  const int columns = std::min<int>(kIconGridColumns, static_cast<int>(count));
  const int rows = static_cast<int>((count + columns - 1) / columns);
  const int gridWidth = columns * tile + (columns - 1) * kIconGridGap;
  const int gridHeight = rows * tile + (rows - 1) * kIconGridGap;
  const int originX = (renderer.getScreenWidth() - gridWidth) / 2;
  const int originY = (renderer.getScreenHeight() - gridHeight) / 2;

  // One 512-byte stack buffer, reused for every tile: icons are read from SD one
  // at a time, drawn, and discarded. None are resident.
  uint8_t bitmap[companionble::kIconBytes];
  const int bytesPerRow = companionble::kIconWidthPx / 8;

  for (size_t i = 0; i < count; ++i) {
    const int column = static_cast<int>(i) % columns;
    const int row = static_cast<int>(i) / columns;
    const int x0 = originX + column * (tile + kIconGridGap);
    const int y0 = originY + row * (tile + kIconGridGap);

    if (companionpeer::readAssetBody(keys[i], companionpeer::kAssetIcon, bitmap, sizeof(bitmap)) != sizeof(bitmap)) {
      continue;
    }
    for (int y = 0; y < companionble::kIconHeightPx; ++y) {
      for (int x = 0; x < companionble::kIconWidthPx; ++x) {
        const uint8_t byte = bitmap[y * bytesPerRow + (x / 8)];
        if (byte & (0x80 >> (x % 8))) renderer.drawPixel(x0 + x, y0 + y, true);
      }
    }

    if (!foregroundPeerKey.empty() && foregroundPeerKey == keys[i]) {
      renderer.drawRect(x0 - 4, y0 - 4, tile + 8, tile + 8, true);
    }
  }

  if (label && *label) {
    renderer.drawCenteredText(SMALL_FONT_ID, originY + gridHeight + kIconGridGap, label, true);
  }

  if (inverted) renderer.invertScreen();
  renderer.displayBuffer();
}

// The interactive counterpart to renderIconGrid(): one tile per peer that
// declared the image-gallery capability (pickerPeerKeys, built once in
// enterGalleryPicker() — not grouped by appId, unlike the decorative grid,
// since two installs of the same app have two separate galleries and must
// stay two separate tiles). A thick outline marks the cursor; the existing
// thin "currently connected" marker still applies if that peer also happens
// to hold a live session. A short label under each tile (userName falling
// back to displayName) is what actually tells two installs of the same app
// apart, since their icon and app name alone would be identical.
void CompanionModeActivity::renderGalleryPicker() {
  const size_t count = pickerPeerKeys.size();
  if (count == 0) {
    // Shouldn't happen — enterGalleryPicker() only switches to this screen
    // when pickerPeerKeys is non-empty — but fail safe rather than draw an
    // empty grid.
    chooseIdleScreen();
    renderIconGrid();
    return;
  }

  const int tile = companionble::kIconWidthPx;
  const int columns = std::min<int>(kIconGridColumns, static_cast<int>(count));
  const int rows = static_cast<int>((count + columns - 1) / columns);
  const int gridWidth = columns * tile + (columns - 1) * kIconGridGap;
  const int gridHeight = rows * tile + (rows - 1) * kGalleryPickerRowGap;
  const int originX = (renderer.getScreenWidth() - gridWidth) / 2;
  const int originY = (renderer.getScreenHeight() - gridHeight) / 2;

  // One 512-byte stack buffer, reused for every tile, same as renderIconGrid().
  uint8_t bitmap[companionble::kIconBytes];
  const int bytesPerRow = companionble::kIconWidthPx / 8;

  for (size_t i = 0; i < count; ++i) {
    const int column = static_cast<int>(i) % columns;
    const int row = static_cast<int>(i) / columns;
    const int x0 = originX + column * (tile + kIconGridGap);
    const int y0 = originY + row * (tile + kGalleryPickerRowGap);
    const char* key = pickerPeerKeys[i].c_str();

    if (companionpeer::readAssetBody(key, companionpeer::kAssetIcon, bitmap, sizeof(bitmap)) == sizeof(bitmap)) {
      for (int y = 0; y < companionble::kIconHeightPx; ++y) {
        for (int x = 0; x < companionble::kIconWidthPx; ++x) {
          const uint8_t byte = bitmap[y * bytesPerRow + (x / 8)];
          if (byte & (0x80 >> (x % 8))) renderer.drawPixel(x0 + x, y0 + y, true);
        }
      }
    }

    if (!foregroundPeerKey.empty() && foregroundPeerKey == key) {
      renderer.drawRect(x0 - 4, y0 - 4, tile + 8, tile + 8, true);
    }
    if (i == pickerCursor) {
      renderer.drawRect(x0 - 7, y0 - 7, tile + 14, tile + 14, 3, true);
    }

    std::string label = companionpeer::userName(key);
    if (label.empty()) label = companionpeer::displayName(key);
    if (!label.empty()) {
      // Not pixel-measured against the tile width — just a byte-length cap,
      // truncated on a UTF-8 boundary like every other label in this file, so
      // two adjacent tiles' labels don't run into each other.
      constexpr size_t kMaxLabelBytes = 10;
      if (label.size() > kMaxLabelBytes) {
        size_t cut = kMaxLabelBytes;
        while (cut > 0 && (static_cast<uint8_t>(label[cut]) & 0xC0) == 0x80) --cut;
        label.resize(cut);
      }
      renderer.drawText(SMALL_FONT_ID, x0, y0 + tile + 10, label.c_str(), true);
    }
  }

  renderer.displayBuffer();
}

void CompanionModeActivity::renderTransientMessage() {
  renderer.drawCenteredText(kCompanionFontId, renderer.getScreenHeight() / 2, transientMessage.c_str(), true,
                            EpdFontFamily::BOLD);
  renderer.displayBuffer();
}

void CompanionModeActivity::drawSleepIndicator() {
  const int cx = kSleepIndicatorMargin + kSleepIndicatorRadius;
  const int cy = renderer.getScreenHeight() - kSleepIndicatorMargin - kSleepIndicatorRadius;
  fillCircle(renderer, cx, cy, kSleepIndicatorRadius, /*state=*/true);
  // Carve a waxing crescent out of the disc above: an offset same-size disc,
  // erased, biased up-and-right. Same hand-drawn-primitive approach as the
  // battery gauge (BaseTheme::drawBatteryOutline) rather than a bitmap — no
  // small "sleeping" glyph exists in the icon set at this footprint.
  fillCircle(renderer, cx + kSleepIndicatorRadius / 2, cy - kSleepIndicatorRadius / 3, kSleepIndicatorRadius,
             /*state=*/false);
}

// See the doc comment on the declaration (CompanionModeActivity.h).
//
// The image branch mirrors renderImage()'s own sequence exactly (BW base
// push, then the two grayscale planes, then the composite push) rather than
// just drawing the indicator into whatever's currently in the framebuffer:
// displayGrayBuffer() composites against the BW plane's *current* contents,
// and skipping a fresh base push here (an earlier version of this code did)
// left it compositing against a stale/wrong base — the photo came back
// visibly lighter and the indicator never showed at all (found via real-
// hardware testing, 2026-07-31). renderImage() also only ever bakes overlays
// (tags) into the BW base pass, never into the LSB/MSB decodes — those carry
// only the photo's tone data — so the indicator is drawn once, there, too.
void CompanionModeActivity::renderPreSleepScreen() {
  if (screen == Screen::Image && !displayedImagePath.empty()) {
    ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(displayedImagePath);
    if (decoder) {
      RenderConfig config;
      config.x = 0;
      config.y = 0;
      config.maxWidth = renderer.getScreenWidth();
      config.maxHeight = renderer.getScreenHeight();
      config.useGrayscale = true;
      config.useDithering = false;
      config.performanceMode = false;
      const std::string path = displayedImagePath;

      renderer.clearScreen();
      decoder->decodeToFramebuffer(path, renderer, config);
      drawSleepIndicator();
      renderer.displayBuffer();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      decoder->decodeToFramebuffer(path, renderer, config);
      renderer.copyGrayscaleLsbBuffers();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      decoder->decodeToFramebuffer(path, renderer, config);
      renderer.copyGrayscaleMsbBuffers();

      renderer.displayGrayBuffer();
      renderer.setRenderMode(GfxRenderer::BW);
      return;
    }
    // Decoder vanished since the image was displayed (shouldn't happen —
    // fall through to the grid rather than sleep on a stale/blank panel).
  }
  renderer.clearScreen();
  renderIconGrid(/*inverted=*/true, tr(STR_COMPANION_SLEEPING));
}

// See the header for why this gate exists. Consuming the flag before the
// notify (rather than after) means a status that cannot be delivered — the
// peer disconnected during the multi-second grayscale settle, so
// notifyRenderStatus() finds no foreground session and drops it — still ends
// the expectation. A flag left set there would surface as a phantom
// RENDER_STATUS on the next unrelated redraw.
void CompanionModeActivity::notifyRenderPushResult(companionble::RenderResult result) {
  if (!renderAwaitingStatus) return;
  renderAwaitingStatus = false;
  companionble::notifyRenderStatus(result, renderAwaitingField);
}

// Both arm sites (handlePendingImage() and loop()'s content-commit block) set
// renderAwaitingField unconditionally, and each also sets `screen` — so the
// push that lands second decides which branch render() takes, and the first
// one's render never happens at all. The expectation it armed was previously
// just overwritten, leaving its caller to wait out the full client-side
// timeout for an answer the device already knew would never come.
//
// How narrow is this? Narrower than it first looks, and NOT reproduced on
// hardware. push(awaitRender: false) releases the client's command gate when
// the wire transfer finishes (~0.24s measured) while the render it triggered
// still has ~1.7s to run, so an app can start an image push mid-render. But
// handlePendingImage() takes RenderLock, so it blocks until that render
// completes and the superseded push gets its honest Displayed after all --
// confirmed on hardware 2026-08-03 (text push then an image 0.3s later
// answered Displayed, not Superseded).
//
// What is left is the sliver where the image's staging finishes and
// handlePendingImage() wins RenderLock *before* the render task has taken it
// for the text render queued a moment earlier. Both run from loop(), so this
// needs the render task not to have been scheduled in between -- rare, timing
// dependent, and not something a test can reliably provoke.
//
// Kept anyway: the cost is one no-op call on the common path, and the failure
// it prevents is a caller hanging for its entire timeout on an answer the
// device already knows will never come. Cheap insurance against a real hole,
// not a fix for an observed bug -- do not let this comment imply otherwise.
void CompanionModeActivity::supersedePendingRenderStatus() {
  notifyRenderPushResult(companionble::RenderResult::Superseded);
}

void CompanionModeActivity::renderImage() {
  if (displayedImagePath.empty()) {
    // The image was superseded before it could be drawn (a text push clears
    // displayedImagePath). Drop the expectation rather than carry it into an
    // unrelated future render.
    renderAwaitingStatus = false;
    renderWaiting();
    return;
  }

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(displayedImagePath);
  if (!decoder) {
    renderer.drawCenteredText(kCompanionFontId, renderer.getScreenHeight() / 2, tr(STR_COMPANION_IMAGE_FAILED), true);
    renderer.displayBuffer();
    notifyRenderPushResult(companionble::RenderResult::DecodeFailed);
    displayedImagePath.clear();
    return;
  }

  RenderConfig config;
  config.x = 0;
  config.y = 0;
  config.maxWidth = renderer.getScreenWidth();
  config.maxHeight = renderer.getScreenHeight();
  config.useGrayscale = true;
  // The phone already dithered to this panel's exact 4-level palette. Running
  // the firmware's own dither on top would double-quantize and destroy the
  // pattern the app chose — see the quantization contract in the protocol doc.
  config.useDithering = false;
  config.performanceMode = false;

  const std::string path = displayedImagePath;
  if (!decoder->decodeToFramebuffer(path, renderer, config)) {
    renderer.clearScreen();
    renderer.drawCenteredText(kCompanionFontId, renderer.getScreenHeight() / 2, tr(STR_COMPANION_IMAGE_FAILED), true);
    renderer.displayBuffer();
    notifyRenderPushResult(companionble::RenderResult::DecodeFailed);
    displayedImagePath.clear();
    return;
  }

  // Tags are drawn over the print only when the app has actually switched one
  // on. That is not a firmware policy about when an app's UI is honoured — an
  // app that leaves its tags hidden simply has nothing to draw, and gets a
  // pixel-exact print. An app that wants a tag on a photo asks for it, and
  // accepts the pixels it costs.
  if (tagRowWidth > 0) {
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    renderTags(renderer.getScreenWidth() - cachedOrientedMarginRight, cachedOrientedMarginTop + lineHeight);
  }

  // Display the black/white base before overlaying the grayscale planes. The
  // reader path does the same (see TxtReaderActivity::render): renderAntiAliased
  // only stores/restores the BW buffer and pushes the *gray* planes, so without
  // this the panel would keep whatever was on it and the grayscale overlay would
  // land on the wrong base.
  renderer.displayBuffer();

  // Two-pass grayscale settle. This re-decodes the image twice more. With the
  // raw packed 2bpp decoder (see RawBitmapToFramebufferConverter) the decode
  // itself is now close to free — no PNGdec inflate, just an unpack loop — so
  // the remaining seconds are almost entirely the e-ink panel's own refresh
  // cadence, not decode cost. Kept anyway: a visible "developing" draw is
  // thematically wanted here, not a defect to optimise away, and the settle
  // is what actually resolves the panel to its final grayscale state.
  //
  // Deliberately NOT ReaderUtils::renderAntiAliased(): that helper snapshots
  // the ~52KB framebuffer in RAM (storeBwBuffer()) before the grayscale
  // passes and restores it after, because for text/EPUB content a third
  // re-layout pass to reconstruct the BW plane would be expensive. An image
  // has no such cost -- the decode above already re-reads straight from the
  // SD-staged file every pass -- so the BW plane is just as cheaply
  // reconstructed by decoding a third time in BW mode, and needs no RAM
  // duplicate at all. Found via real-hardware testing (2026-07-29):
  // storeBwBuffer()'s chunked allocation could fail under the heap pressure
  // of an active BLE session (~54KB free against a ~52KB need, thin enough
  // that fragmentation alone could exhaust it), silently skipping grayscale
  // and leaving the BW base on screen with only a log line to explain it.
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  decoder->decodeToFramebuffer(path, renderer, config);
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  decoder->decodeToFramebuffer(path, renderer, config);
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);

  // Reconstruct the BW plane the same way it was originally built (decode,
  // then tags on top) rather than restoring a snapshot of it. RAM-only --
  // deliberately no displayBuffer() call, since the panel already shows the
  // correct grayscale result from displayGrayBuffer() above; this only needs
  // to leave frameBuffer holding the right bytes for later readers (e.g.
  // CMD:SCREENSHOT, partial-refresh diffing).
  //
  // clearScreen()'s default (0xFF, white) here, NOT the 0x00 the two
  // grayscale passes above use -- 0x00 is that accumulation convention, not
  // BW mode's. BW mode's undrawn-pixel value is 0xFF (see writePixel()'s doc
  // comment in DirectPixelWriter.h), matching how the framebuffer started
  // out before the very first (pre-settle) decode of this same image.
  renderer.clearScreen();
  decoder->decodeToFramebuffer(path, renderer, config);
  if (tagRowWidth > 0) {
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    renderTags(renderer.getScreenWidth() - cachedOrientedMarginRight, cachedOrientedMarginTop + lineHeight);
  }

  notifyRenderPushResult(companionble::RenderResult::Displayed);
}

// Draws the foreground app's visible tags as a right-aligned row.
//
// The firmware knows none of these strings — they came out of the peer's UI
// declaration, exactly like button labels, and this only renders them. Hidden
// tags take no space at all, which is what lets an app declare more tags than
// it usually shows. Two independent things vary per peer: which tag is on
// (TagState, per tag) and how the row looks (tagRenderStyle, whole row) — see
// tagIsDrawn() for how Plain also treats an unset (Outline) tag as invisible.
void CompanionModeActivity::renderTags(int rightEdgeX, int centerY) const {
  const int textHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int chipHeight = textHeight + 2 * kTagChipPadY;
  const int y = centerY - chipHeight / 2;
  const bool plain = tagRenderStyle == static_cast<uint8_t>(companionble::TagRenderStyle::Plain);
  int x = rightEdgeX;

  for (int i = static_cast<int>(tagCount) - 1; i >= 0; --i) {
    const TagSpec& tag = tags[i];
    if (!tagIsDrawn(tag)) continue;

    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, tag.label);

    if (plain) {
      // tagIsDrawn() already filtered Outline out in this style, so whatever
      // reaches here is Filled — drawn as plain text, no box, no inversion.
      x -= textWidth;
      if (x < 0) break;  // ran out of room: drop the leftmost chips rather than overlap the title
      renderer.drawText(UI_10_FONT_ID, x, y + kTagChipPadY, tag.label, true);
      x -= kTagChipGap;
      continue;
    }

    const int chipWidth = textWidth + 2 * kTagChipPadX;
    x -= chipWidth;
    if (x < 0) break;  // ran out of room: drop the leftmost chips rather than overlap the title

    const bool filled = tag.state == static_cast<uint8_t>(companionble::TagState::Filled);
    if (filled) {
      renderer.fillRect(x, y, chipWidth, chipHeight, true);
    } else {
      renderer.drawRect(x, y, chipWidth, chipHeight, true);
    }
    // Knocked out of the fill when filled, so a marked tag reads as marked
    // rather than as a black box.
    renderer.drawText(UI_10_FONT_ID, x + kTagChipPadX, y + kTagChipPadY, tag.label, !filled);
    x -= kTagChipGap;
  }
}

void CompanionModeActivity::renderPage() {
  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages > 0 ? totalPages - 1 : 0;

  const int titleLineHeight = renderer.getLineHeight(cachedTitleFontId);
  int titleY = cachedOrientedMarginTop;
  for (const auto& line : titleLines) {
    renderer.drawText(cachedTitleFontId, cachedOrientedMarginLeft, titleY, line.c_str(), true, EpdFontFamily::BOLD);
    titleY += titleLineHeight;
  }

  renderTags(cachedOrientedMarginLeft + viewportWidth, cachedOrientedMarginTop + titleLineHeight / 2);

  const int lineHeight = renderer.getLineHeight(cachedFontId);
  int y = cachedOrientedMarginTop + cachedTitleBlockHeight;
  if (totalPages > 0) {
    for (const auto& line : pages[currentPage]) {
      if (!line.empty()) {
        renderer.drawText(cachedFontId, cachedOrientedMarginLeft, y, line.c_str());
      }
      y += lineHeight;
    }
  }

  if (!mappedInput.hasTouch()) {
    // Battery% (left) / page count (right): each centered in its own
    // edge-to-button band and vertically centered on the button-hint row — both
    // driven by the active theme, since BaseTheme/LyraTheme/RoundedRaffTheme lay
    // that row out completely differently.
    const int rowCenterY = GUI.getButtonHintsRowCenterY(renderer);
    const int statusTextY = rowCenterY - renderer.getLineHeight(UI_10_FONT_ID) / 2;
    const int leftBandWidth = GUI.getButtonHintsSideBandWidth();
    const int rightBandStart = renderer.getScreenWidth() - leftBandWidth;

    char batteryStr[8];
    snprintf(batteryStr, sizeof(batteryStr), "%u%%", powerManager.getBatteryPercentage());
    const int batteryTextWidth = renderer.getTextWidth(UI_10_FONT_ID, batteryStr);
    renderer.drawText(UI_10_FONT_ID, std::max(0, (leftBandWidth - batteryTextWidth) / 2), statusTextY, batteryStr);

    char pageStr[16];
    snprintf(pageStr, sizeof(pageStr), "%d/%d", currentPage + 1, std::max(totalPages, 1));
    const int pageTextWidth = renderer.getTextWidth(UI_10_FONT_ID, pageStr);
    renderer.drawText(UI_10_FONT_ID, rightBandStart + std::max(0, (leftBandWidth - pageTextWidth) / 2), statusTextY,
                      pageStr);

    // Hints come straight from the app's declared labels. A locally-paging
    // button's hint is hidden when that direction is not pageable right now
    // (empty string = hidden, the convention drawButtonHints() checks) — the
    // firmware knows the page count, the app does not.
    const char* backLabel = labelFor(companionble::ButtonId::Back);
    const char* confirmLabel = labelFor(companionble::ButtonId::Confirm);
    const char* leftLabel = labelFor(companionble::ButtonId::Left);
    const char* rightLabel = labelFor(companionble::ButtonId::Right);
    if (routingFor(companionble::ButtonId::Left) == companionble::ButtonRouting::LocalPagePrev && currentPage <= 0) {
      leftLabel = "";
    }
    if (routingFor(companionble::ButtonId::Right) == companionble::ButtonRouting::LocalPageNext &&
        currentPage >= totalPages - 1) {
      rightLabel = "";
    }
    const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, leftLabel, rightLabel);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  if (forceFastRefreshNextRender) {
    // Tag-triggered redraw: always no-flash,
    // independent of the periodic full-refresh cadence below.
    forceFastRefreshNextRender = false;
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
}
