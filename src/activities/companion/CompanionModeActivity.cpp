#include "CompanionModeActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Logging.h>
#include <driver/usb_serial_jtag.h>

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

// Width reserved at the title line's right edge for the app's indicator slots —
// shared by the title-wrap width budget and the slots' own x positions.
constexpr int kIndicatorSlotSize = 12;
constexpr int kIndicatorSlotGap = 6;
constexpr int kIndicatorAreaWidth =
    companionble::kMaxIndicators * (kIndicatorSlotSize + kIndicatorSlotGap) + kIndicatorSlotGap;

// Title wraps onto at most this many lines before falling back to
// ellipsis-truncating the last line (see wrapTitleToLines()).
constexpr int kMaxTitleLines = 2;

// Sleep-screen grid geometry. 64x64 tiles with room to breathe; the cap comes
// from companionpeer::kMaxIconTiles.
constexpr int kIconGridColumns = 6;
constexpr int kIconGridGap = 24;

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
uint8_t g_pendingIndicatorId = 0;
uint8_t g_pendingIndicatorState = 0;
volatile bool g_pendingStatusReady = false;

// Foreground handover and pairing requests are also host-task events. Paths and
// names are short and fixed-length here so the critical section stays a memcpy.
char g_pendingForegroundKey[companionpeer::kPeerKeyLen] = {0};
char g_pendingForegroundName[companionpeer::kMaxNameLen + 1] = {0};
volatile bool g_pendingForegroundReady = false;
char g_pendingPairingName[companionpeer::kMaxNameLen + 1] = {0};
volatile bool g_pendingPairingReady = false;
char g_pendingImagePath[96] = {0};
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

// Runs on the NimBLE host task — copy into the fixed buffer and set a flag;
// CompanionModeActivity::loop() (main loop task) does the rest.
void onContentField(uint8_t field, const uint8_t* data, size_t len, bool final) {
  portENTER_CRITICAL(&g_mux);
  const bool wasIdle = !g_pendingTitleReady && !g_pendingBodyReady;
  if (field == companionble::kFieldTitle) {
    const size_t n = len > sizeof(g_pendingTitleBuf) ? sizeof(g_pendingTitleBuf) : len;
    memcpy(g_pendingTitleBuf, data, n);
    g_pendingTitleLen = static_cast<uint16_t>(n);
    g_pendingTitleReady = true;
  } else if (field == companionble::kFieldBody) {
    const size_t n = len > sizeof(g_pendingBodyBuf) ? sizeof(g_pendingBodyBuf) : len;
    memcpy(g_pendingBodyBuf, data, n);
    g_pendingBodyLen = static_cast<uint16_t>(n);
    g_pendingBodyReady = true;
  }
  if (wasIdle && (field == companionble::kFieldTitle || field == companionble::kFieldBody)) {
    g_pendingBatchStartMs = millis();
  }
  if (final) g_pendingCommitReady = true;
  portEXIT_CRITICAL(&g_mux);
}

void onStatus(uint8_t indicatorId, uint8_t state) {
  portENTER_CRITICAL(&g_mux);
  g_pendingIndicatorId = indicatorId;
  g_pendingIndicatorState = state;
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

void onImageStaged(const char* path) {
  portENTER_CRITICAL(&g_mux);
  snprintf(g_pendingImagePath, sizeof(g_pendingImagePath), "%s", path ? path : "");
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
  memset(indicators, 0, sizeof(indicators));
  forceFastRefreshNextRender = false;
  currentPage = 0;
  totalPages = 0;
  pages.clear();
  clearButtonMap();
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
  companiontest::setScreenNameProvider([]() -> const char* {
    return g_screenNameActivity ? g_screenNameActivity->screenName() : "none";
  });
#endif

  if (!companionble::ensureStarted(renderer, cachedFontId)) {
    LOG_ERR("CMA", "ensureStarted() failed (heap floor or NimBLE init)");
    screen = Screen::StartFailed;
    idleSinceMs = 0;
  } else {
    chooseIdleScreen();
    idleSinceMs = millis();  // start the "nobody is driving the screen" idle timer
  }

  requestUpdate();
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
  portEXIT_CRITICAL(&g_mux);
}

// ---------------------------------------------------------------------------
// Button map
// ---------------------------------------------------------------------------

void CompanionModeActivity::clearButtonMap() {
  for (auto& spec : buttons) {
    spec.routing = companionble::ButtonRouting::None;
    spec.label.clear();
  }
}

// Reads the foreground peer's declared control scheme off the SD card. Called
// on every foreground handover and whenever that peer pushes a new map, so an
// app update changes the buttons without a re-pair and without a firmware mode.
void CompanionModeActivity::loadButtonMap() {
  clearButtonMap();
  if (foregroundPeerKey.empty()) return;

  uint8_t raw[companionpeer::kMaxButtonMapLen];
  const size_t len =
      companionpeer::readAssetBody(foregroundPeerKey.c_str(), companionpeer::kAssetButtonMap, raw, sizeof(raw));
  if (len < 1) return;

  const uint8_t count = raw[0];
  size_t offset = 1;
  for (uint8_t i = 0; i < count && offset + 3 <= len; ++i) {
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
  const int maxWidth = viewportWidth - kIndicatorAreaWidth;
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

  // See main.cpp's general auto-sleep check for why USB power skips this too —
  // deep-sleeping drops the USB CDC connection, which is actively unhelpful
  // while plugged in (charging, or connected for serial debugging).
  if (gpio.isUsbConnected() || usb_serial_jtag_is_connected()) return;

  LOG_INF("CMA", "No app driving the screen for %lu ms, deep-sleeping", kWaitingIdleSleepMs);
  powerManager.startDeepSleep(gpio);  // [[noreturn]] — wakes on power button, panel keeps its last image
}

// ---------------------------------------------------------------------------
// Handover / image
// ---------------------------------------------------------------------------

void CompanionModeActivity::applyForegroundChange() {
  RenderLock lock;
  loadButtonMap();

  if (foregroundPeerKey.empty()) {
    // Nobody holds the screen. Content stays up — it is the idle timeout, not
    // the handover, that clears it.
    idleSinceMs = millis();
    if (!haveContent && screen != Screen::Image) chooseIdleScreen();
  } else {
    // A different app took the screen: clear whatever the previous one left,
    // since the device retains no content for a background session.
    idleSinceMs = 0;
    title.clear();
    body.clear();
    pages.clear();
    totalPages = 0;
    currentPage = 0;
    haveContent = false;
    // A different app owns the screen now: its indicators start clear, since
    // they carry the previous app's meaning, not this one's.
    memset(indicators, 0, sizeof(indicators));
    displayedImagePath.clear();
    updateTitleLayout();
    screen = Screen::Text;
  }
  requestUpdate();
}

// Decodes a staged PNG on the main loop task and reports the outcome back to
// the app. Never runs on the NimBLE host task: decoding writes the framebuffer.
void CompanionModeActivity::handlePendingImage() {
  const std::string path = pendingImagePath;
  pendingImagePath.clear();

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(path);
  if (!decoder) {
    LOG_ERR("CMA", "no decoder for staged image %s", path.c_str());
    companionble::notifyImageStatus(companionble::ImageResult::DecodeFailed);
    return;
  }
  ImageDimensions dims{};
  if (!decoder->getDimensions(path, dims)) {
    LOG_ERR("CMA", "staged image %s did not decode", path.c_str());
    companionble::notifyImageStatus(companionble::ImageResult::DecodeFailed);
    return;
  }

  RenderLock lock;
  displayedImagePath = path;
  screen = Screen::Image;
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void CompanionModeActivity::loop() {
  if (screen == Screen::StartFailed) return;  // nothing to poll: BLE never came up

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
      portEXIT_CRITICAL(&g_mux);
    }
    requestUpdate();
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
  uint8_t newIndicatorId = 0;
  uint8_t newIndicatorState = 0;
  char newForegroundKey[companionpeer::kPeerKeyLen] = {0};
  char newForegroundName[companionpeer::kMaxNameLen + 1] = {0};
  char newPairingName[companionpeer::kMaxNameLen + 1] = {0};
  char newImagePath[sizeof(g_pendingImagePath)] = {0};

  portENTER_CRITICAL(&g_mux);
  if (g_pendingTitleReady) {
    newTitle.assign(reinterpret_cast<char*>(g_pendingTitleBuf), g_pendingTitleLen);
    gotTitle = true;
  }
  if (g_pendingBodyReady) {
    newBody.assign(reinterpret_cast<char*>(g_pendingBodyBuf), g_pendingBodyLen);
    gotBody = true;
  }
  if (g_pendingCommitReady) {
    commit = true;
  } else if ((gotTitle || gotBody) && g_pendingBatchStartMs != 0 &&
             millis() - g_pendingBatchStartMs > kPendingBatchTimeoutMs) {
    // Safety net: the final-flagged field's END never arrived in time (e.g. the
    // app crashed or lost the connection mid-push). Apply whatever we have
    // rather than leaving the screen stuck on stale content indefinitely.
    LOG_ERR("CMA", "content batch commit flag missed after %lu ms, applying pending fields anyway",
            static_cast<unsigned long>(kPendingBatchTimeoutMs));
    commit = true;
  }
  if (commit) {
    g_pendingTitleReady = false;
    g_pendingBodyReady = false;
    g_pendingCommitReady = false;
    g_pendingBatchStartMs = 0;
  }
  if (g_pendingStatusReady) {
    newIndicatorId = g_pendingIndicatorId;
    newIndicatorState = g_pendingIndicatorState;
    g_pendingStatusReady = false;
    gotStatus = true;
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
    g_pendingImageReady = false;
    gotImage = true;
  }
  portEXIT_CRITICAL(&g_mux);

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
    pendingImagePath = newImagePath;
    handlePendingImage();
  }

  if (commit && (gotTitle || gotBody)) {
    // One lock for both halves so a render can never land between the title and
    // body updates of a single push and see a mismatched pairing.
    RenderLock lock;
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
    // mode to enter: the last completed push owns the screen.
    screen = Screen::Text;
    displayedImagePath.clear();
    requestUpdate();
  }

  if (gotStatus && newIndicatorId < companionble::kMaxIndicators) {
    RenderLock lock;
    indicators[newIndicatorId] = newIndicatorState;
    forceFastRefreshNextRender = true;
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

    if (buttonWasReleased(trackedRole, holdButton)) {
      const uint16_t finalTicks =
          static_cast<uint16_t>(std::min<unsigned long>(buttonHeldTime(holdButton) / kHoldTickMs, 0xFFFFUL));
      companionble::notifyButtonEvent(holdButton, finalTicks, /*isFinal=*/true);
      holdActive = false;
    } else if (buttonIsPressed(trackedRole, holdButton)) {
      const uint16_t heldTicks =
          static_cast<uint16_t>(std::min<unsigned long>(buttonHeldTime(holdButton) / kHoldTickMs, 0xFFFFUL));
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

const char* CompanionModeActivity::screenName() const {
  switch (screen) {
    case Screen::StartFailed:
      return "start_failed";
    case Screen::Waiting:
      return "waiting";
    case Screen::IconGrid:
      return "icon_grid";
    case Screen::Pairing:
      return "pairing";
    case Screen::Text:
      return haveContent ? "text" : "waiting_app";
    case Screen::Image:
      return "image";
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
    case Screen::Image:
      renderImage();
      break;
    case Screen::Text:
      if (haveContent) {
        renderPage();
      } else {
        renderWaiting();
      }
      break;
    case Screen::Waiting:
      renderWaiting();
      break;
  }
}

void CompanionModeActivity::renderWaiting() {
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
void CompanionModeActivity::renderIconGrid() {
  char keys[companionpeer::kMaxIconTiles][companionpeer::kPeerKeyLen];
  const size_t count = companionpeer::listIconTiles(keys, companionpeer::kMaxIconTiles);
  if (count == 0) {
    renderWaiting();
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
  renderer.displayBuffer();
}

void CompanionModeActivity::renderImage() {
  if (displayedImagePath.empty()) {
    renderWaiting();
    return;
  }

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(displayedImagePath);
  if (!decoder) {
    renderer.drawCenteredText(kCompanionFontId, renderer.getScreenHeight() / 2, tr(STR_COMPANION_IMAGE_FAILED), true);
    renderer.displayBuffer();
    companionble::notifyImageStatus(companionble::ImageResult::DecodeFailed);
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
  bool decoded = decoder->decodeToFramebuffer(path, renderer, config);
  if (!decoded) {
    renderer.clearScreen();
    renderer.drawCenteredText(kCompanionFontId, renderer.getScreenHeight() / 2, tr(STR_COMPANION_IMAGE_FAILED), true);
    renderer.displayBuffer();
    companionble::notifyImageStatus(companionble::ImageResult::DecodeFailed);
    displayedImagePath.clear();
    return;
  }

  // Two-pass grayscale settle. This re-decodes the image twice more, which is
  // slow — several seconds — and that is fine: a visible "developing" draw is
  // thematically wanted here, not a defect to optimise away.
  ReaderUtils::renderAntiAliased(renderer, [&]() { decoder->decodeToFramebuffer(path, renderer, config); });

  companionble::notifyImageStatus(companionble::ImageResult::Displayed);
}

// Draws the foreground app's indicator slots: an outline or filled mark per
// slot. Deliberately a neutral shape rather than the v5 star — a star reads as
// "favourite", which is exactly the app-level meaning the firmware is not
// allowed to hold. The app decides what each slot means and when it lights.
void CompanionModeActivity::renderIndicators(int rightEdgeX, int centerY) const {
  int x = rightEdgeX - kIndicatorSlotSize;
  const int y = centerY - kIndicatorSlotSize / 2;
  for (int slot = companionble::kMaxIndicators - 1; slot >= 0; --slot) {
    switch (static_cast<companionble::IndicatorState>(indicators[slot])) {
      case companionble::IndicatorState::Filled:
        renderer.fillRect(x, y, kIndicatorSlotSize, kIndicatorSlotSize, true);
        break;
      case companionble::IndicatorState::Outline:
        renderer.drawRect(x, y, kIndicatorSlotSize, kIndicatorSlotSize, true);
        break;
      case companionble::IndicatorState::Hidden:
        break;  // nothing drawn, and the slot still holds its place in the row
    }
    x -= kIndicatorSlotSize + kIndicatorSlotGap;
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

  renderIndicators(cachedOrientedMarginLeft + viewportWidth, cachedOrientedMarginTop + titleLineHeight / 2);

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
    // Status-triggered redraw (an indicator flip): always no-flash,
    // independent of the periodic full-refresh cadence below.
    forceFastRefreshNextRender = false;
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
}
