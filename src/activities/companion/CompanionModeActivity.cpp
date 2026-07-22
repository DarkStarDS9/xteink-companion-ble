#include "CompanionModeActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "CompanionBle.h"
#include "MappedInputManager.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Companion Mode's fixed fonts — not user-configurable (unlike the reader),
// since the capability characteristic advertises a fixed char-grid size.
constexpr int kCompanionFontId = NOTOSANS_14_FONT_ID;       // body
constexpr int kCompanionTitleFontId = NOTOSANS_16_FONT_ID;  // title: larger + bold

// Which physical side button is UP is a hardware detail shared across the
// X3/X4 binary that can't be derived from code (see root CLAUDE.md's
// "Established hardware facts") — swap this single constant after on-device
// testing if UP/DOWN turn out backwards from the intended PREV/NEXT feel.
constexpr bool kSideUpMeansPrev = true;

// Bottom-row hint labels. Plain ASCII (guaranteed present in the built-in
// font's Basic Latin range) standing in for icons: the companion font's
// glyph set (see notosans_14_regular's EpdUnicodeInterval table) has no
// star/play/arrow glyphs (U+25xx/U+26xx), so these are drawn as short
// symbolic text rather than as icon codepoints.
constexpr const char* kPlayPauseHint = "> ||";
constexpr const char* kReadLaterHint = "*";
constexpr const char* kPageBackHint = "<";
constexpr const char* kPageForwardHint = ">";
// Side hints use double chevrons so they read as distinct from the single-
// chevron LEFT/RIGHT local-paging hints above.
constexpr const char* kPrevArticleHint = "<<";
constexpr const char* kNextArticleHint = ">>";

// UTF-8-safe: drop one full codepoint (a lead byte plus any continuation
// bytes), matching the boundary-walk CompanionModeActivity::paginate() uses.
void popUtf8Char(std::string& s) {
  if (s.empty()) return;
  s.pop_back();
  while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) {
    s.pop_back();
  }
}

// Task-boundary handoff for content/status arriving on the NimBLE host task
// (see CompanionBle.h's ContentFieldCallback/StatusCallback doc comments).
// Fixed-size buffers, not heap allocation, so the critical section only ever
// does a memcpy/scalar assignment.
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
uint8_t g_pendingTitleBuf[companionble::kMaxFieldLen];
uint16_t g_pendingTitleLen = 0;
volatile bool g_pendingTitleReady = false;
uint8_t g_pendingBodyBuf[companionble::kMaxFieldLen];
uint16_t g_pendingBodyLen = 0;
volatile bool g_pendingBodyReady = false;
uint8_t g_pendingStatusValue = 0;
volatile bool g_pendingStatusReady = false;

// Runs on the NimBLE host task — copy into the fixed buffer and set a flag;
// CompanionModeActivity::loop() (main loop task) does the rest.
void onContentField(uint8_t field, const uint8_t* data, size_t len) {
  portENTER_CRITICAL(&g_mux);
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
  portEXIT_CRITICAL(&g_mux);
}

// Runs on the NimBLE host task — same handoff pattern as onContentField().
void onStatus(uint8_t status) {
  portENTER_CRITICAL(&g_mux);
  g_pendingStatusValue = status;
  g_pendingStatusReady = true;
  portEXIT_CRITICAL(&g_mux);
}

}  // namespace

void CompanionModeActivity::onEnter() {
  Activity::onEnter();
  connected = false;
  haveContent = false;
  readLaterSaved = false;
  forceFastRefreshNextRender = false;
  currentPage = 0;
  totalPages = 0;
  pages.clear();
  cachedFontId = kCompanionFontId;
  computeViewport();

  companionble::setContentFieldCallback(onContentField);
  companionble::setStatusCallback(onStatus);
  startFailed = !companionble::ensureStarted(renderer, cachedFontId);
  if (startFailed) {
    LOG_ERR("CMA", "ensureStarted() failed (heap floor or NimBLE init)");
    waitingSinceMs = 0;
  } else {
    waitingSinceMs = millis();  // start the "no phone connected" idle-sleep timer
  }

  requestUpdate();
}

void CompanionModeActivity::onExit() {
  Activity::onExit();
  companionble::setContentFieldCallback(nullptr);
  companionble::setStatusCallback(nullptr);
  companionble::stop();

  portENTER_CRITICAL(&g_mux);
  g_pendingTitleReady = false;
  g_pendingBodyReady = false;
  g_pendingStatusReady = false;
  portEXIT_CRITICAL(&g_mux);
}

void CompanionModeActivity::computeViewport() {
  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginBottom += UITheme::getInstance().getStatusBarHeight();

  if (!mappedInput.hasTouch()) {
    // Reserve room for the bottom button-hint bar and the two side hint
    // strips drawn in renderPage() (GUI.drawButtonHints()/drawSideButtonHints()
    // early-return on touch devices, so no reservation is needed there).
    const auto& metrics = UITheme::getInstance().getMetrics();
    cachedOrientedMarginBottom += metrics.buttonHintsHeight;
    cachedOrientedMarginLeft += metrics.sideButtonHintsWidth;
    cachedOrientedMarginRight += metrics.sideButtonHintsWidth;
  }

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;

  cachedTitleFontId = kCompanionTitleFontId;
  constexpr int kTitleBottomSpacing = 6;  // gap between the bold title line and the first body line
  cachedTitleBlockHeight = renderer.getLineHeight(cachedTitleFontId) + kTitleBottomSpacing;

  const int viewportHeight =
      renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom - cachedTitleBlockHeight;
  const int lineHeight = renderer.getLineHeight(cachedFontId);

  linesPerPage = lineHeight > 0 ? viewportHeight / lineHeight : 1;
  if (linesPerPage < 1) linesPerPage = 1;
}

void CompanionModeActivity::paginate() {
  pages.clear();
  currentPage = 0;

  // Wrap the body into lines using the same measure-and-break loop
  // TxtReaderActivity uses (renderer.getTextAdvanceX + break at the last
  // space, or a UTF-8-safe character boundary if there's no space to break
  // at), then group linesPerPage lines per page. Content is a small in-memory
  // buffer (bounded by kMaxFieldLen), not a file, so no offset bookkeeping.
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

void CompanionModeActivity::checkWaitingIdleSleep() {
  if (connected || waitingSinceMs == 0) return;
  if (millis() - waitingSinceMs < kWaitingIdleSleepMs) return;

  LOG_INF("CMA", "No phone connected for %lu ms on the waiting screen, deep-sleeping", kWaitingIdleSleepMs);
  powerManager.startDeepSleep(gpio);  // [[noreturn]] — wakes on power button, panel keeps its last image
}

void CompanionModeActivity::loop() {
  if (startFailed) return;  // nothing to poll: BLE never came up

  const bool nowConnected = companionble::isConnected();
  if (nowConnected != connected) {
    connected = nowConnected;
    if (!connected) {
      haveContent = false;
      pages.clear();
      waitingSinceMs = millis();  // re-arm the idle-sleep timer for the waiting screen
    } else {
      waitingSinceMs = 0;
    }
    requestUpdate();
  }

  checkWaitingIdleSleep();

  bool gotTitle = false;
  bool gotBody = false;
  bool gotStatus = false;
  std::string newTitle;
  std::string newBody;
  uint8_t newStatus = 0;
  portENTER_CRITICAL(&g_mux);
  if (g_pendingTitleReady) {
    newTitle.assign(reinterpret_cast<char*>(g_pendingTitleBuf), g_pendingTitleLen);
    g_pendingTitleReady = false;
    gotTitle = true;
  }
  if (g_pendingBodyReady) {
    newBody.assign(reinterpret_cast<char*>(g_pendingBodyBuf), g_pendingBodyLen);
    g_pendingBodyReady = false;
    gotBody = true;
  }
  if (g_pendingStatusReady) {
    newStatus = g_pendingStatusValue;
    g_pendingStatusReady = false;
    gotStatus = true;
  }
  portEXIT_CRITICAL(&g_mux);

  if (gotTitle) title = newTitle;
  if (gotBody) {
    body = newBody;
    paginate();
    haveContent = true;
    readLaterSaved = false;  // new article: reset any previous save-state indicator
    requestUpdate();
  }
  if (gotStatus && newStatus == static_cast<uint8_t>(companionble::StatusEvent::ReadLaterSaved)) {
    readLaterSaved = true;
    forceFastRefreshNextRender = true;
    requestUpdate();
  }

  if (!haveContent || !connected) return;

  if (mappedInput.wasPressed(MappedInputManager::Button::Left) && currentPage > 0) {
    currentPage--;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Right) && currentPage < totalPages - 1) {
    currentPage++;
    requestUpdate();
  }

  // Side UP/DOWN report PREV/NEXT over BLE (see kSideUpMeansPrev); bottom
  // BACK/CONFIRM report PLAY_PAUSE/READ_LATER. LEFT/RIGHT above page the
  // locally-buffered body and never produce a BLE event.
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    companionble::notifyButtonEvent(kSideUpMeansPrev ? companionble::ButtonEvent::Prev
                                                      : companionble::ButtonEvent::Next);
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    companionble::notifyButtonEvent(kSideUpMeansPrev ? companionble::ButtonEvent::Next
                                                      : companionble::ButtonEvent::Prev);
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    companionble::notifyButtonEvent(companionble::ButtonEvent::PlayPause);
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    companionble::notifyButtonEvent(companionble::ButtonEvent::ReadLater);
  }
}

void CompanionModeActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (startFailed) {
    renderStartFailed();
  } else if (!haveContent || !connected) {
    renderWaiting();
  } else {
    renderPage();
  }
}

void CompanionModeActivity::renderWaiting() {
  renderer.drawCenteredText(kCompanionFontId, renderer.getScreenHeight() / 2, tr(STR_COMPANION_WAITING), true,
                            EpdFontFamily::BOLD);
  renderer.displayBuffer();
}

void CompanionModeActivity::renderStartFailed() {
  renderer.drawCenteredText(kCompanionFontId, renderer.getScreenHeight() / 2, tr(STR_COMPANION_START_FAILED), true,
                            EpdFontFamily::BOLD);
  renderer.displayBuffer();
}

void CompanionModeActivity::renderReadLaterIcon(int x, int y) const {
  // Small hand-drawn 5-point star (the built-in font has no U+2605/U+2606
  // star glyphs — see notosans_14_regular's EpdUnicodeInterval table). Filled
  // when readLaterSaved, outline otherwise. Points computed at render time
  // (cheap relative to the E-ink refresh this is always followed by); no
  // trig-table caching needed for ~10 calls/render.
  constexpr int kPoints = 10;
  constexpr float kOuterR = 8.0f;
  constexpr float kInnerR = 3.2f;
  constexpr float kStepRad = 0.6283185307f;   // 2*PI/10 = 36 degrees
  constexpr float kStartRad = -1.5707963268f;  // -90 degrees: first point straight up

  int xs[kPoints];
  int ys[kPoints];
  for (int i = 0; i < kPoints; ++i) {
    const float angle = kStartRad + static_cast<float>(i) * kStepRad;
    const float r = (i % 2 == 0) ? kOuterR : kInnerR;
    xs[i] = x + static_cast<int>(std::lround(r * std::cos(angle)));
    ys[i] = y + static_cast<int>(std::lround(r * std::sin(angle)));
  }

  if (readLaterSaved) {
    renderer.fillPolygon(xs, ys, kPoints, true);
  } else {
    for (int i = 0; i < kPoints; ++i) {
      const int next = (i + 1) % kPoints;
      renderer.drawLine(xs[i], ys[i], xs[next], ys[next], true);
    }
  }
}

void CompanionModeActivity::renderPage() {
  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages > 0 ? totalPages - 1 : 0;

  // Title: bold, larger than the body, single line. Right-elided if it
  // overflows, leaving room for the read-later icon at the line's right edge.
  constexpr int kReadLaterIconAreaWidth = 24;
  const int titleMaxWidth = viewportWidth - kReadLaterIconAreaWidth;
  std::string displayTitle = title;
  if (titleMaxWidth > 0 &&
      renderer.getTextWidth(cachedTitleFontId, displayTitle.c_str(), EpdFontFamily::BOLD) > titleMaxWidth) {
    const std::string ellipsis = "\xE2\x80\xA6";  // U+2026 HORIZONTAL ELLIPSIS
    while (!displayTitle.empty() &&
           renderer.getTextWidth(cachedTitleFontId, (displayTitle + ellipsis).c_str(), EpdFontFamily::BOLD) >
               titleMaxWidth) {
      popUtf8Char(displayTitle);
    }
    displayTitle += ellipsis;
  }
  renderer.drawText(cachedTitleFontId, cachedOrientedMarginLeft, cachedOrientedMarginTop, displayTitle.c_str(), true,
                    EpdFontFamily::BOLD);

  const int titleLineHeight = renderer.getLineHeight(cachedTitleFontId);
  renderReadLaterIcon(cachedOrientedMarginLeft + viewportWidth - kReadLaterIconAreaWidth / 2,
                      cachedOrientedMarginTop + titleLineHeight / 2);

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

  GUI.drawStatusBar(renderer, totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0, currentPage + 1,
                    std::max(totalPages, 1), title);

  if (!mappedInput.hasTouch()) {
    const auto labels = mappedInput.mapLabels(kPlayPauseHint, kReadLaterHint, kPageBackHint, kPageForwardHint);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    const char* upLabel = kSideUpMeansPrev ? kPrevArticleHint : kNextArticleHint;
    const char* downLabel = kSideUpMeansPrev ? kNextArticleHint : kPrevArticleHint;
    GUI.drawSideButtonHints(renderer, upLabel, downLabel);
  }

  if (forceFastRefreshNextRender) {
    // Status-triggered redraw (read-later icon flip): always no-flash,
    // independent of the periodic full-refresh cadence below.
    forceFastRefreshNextRender = false;
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
}
