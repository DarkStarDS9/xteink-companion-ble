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

// Width reserved at the title line's right edge for the read-later star icon
// — shared by the title-wrap width budget and the icon's own x position.
constexpr int kReadLaterIconAreaWidth = 24;

// Title wraps onto at most this many lines before falling back to
// ellipsis-truncating the last line (see wrapTitleToLines()).
constexpr int kMaxTitleLines = 2;

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

// Set once a field's END arrives with kFinalFieldFlag set (see CompanionBle.h). loop() only
// applies gotTitle/gotBody to on-screen state once this is true, so a multi-field push (title,
// then body, then a final-flagged content-id) always lands on screen together instead of the
// title updating first while body is still mid-transfer.
volatile bool g_pendingCommitReady = false;

// millis() timestamp of the first pending (title or body) field of the current batch — 0 when
// idle. Safety net for kPendingBatchTimeoutMs below: if the final-flagged field's END never
// arrives (app crash / disconnect mid-push), pending fields are applied anyway rather than
// leaving the screen stuck on stale content indefinitely.
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
  g_pendingCommitReady = false;
  g_pendingBatchStartMs = 0;
  portEXIT_CRITICAL(&g_mux);
}

void CompanionModeActivity::computeViewport() {
  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginBottom += UITheme::getInstance().getStatusBarHeight();

  if (!mappedInput.hasTouch()) {
    // Reserve room for the bottom button-hint bar. The side margins below
    // (sideButtonHintsWidth) are kept reserved for layout/hardware-parity
    // reasons even though renderPage() no longer draws side hint text there
    // (see the removed GUI.drawSideButtonHints() call) — not reclaimed for
    // the title-wrap width budget by design.
    const auto& metrics = UITheme::getInstance().getMetrics();
    cachedOrientedMarginBottom += metrics.buttonHintsHeight;
    cachedOrientedMarginLeft += metrics.sideButtonHintsWidth;
    cachedOrientedMarginRight += metrics.sideButtonHintsWidth;
  }

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  cachedTitleFontId = kCompanionTitleFontId;

  updateTitleLayout();
}

// Re-wraps `title` into `titleLines` (see kMaxTitleLines) and, since the
// title block's height varies with the wrapped line count, recomputes
// linesPerPage for the body underneath it. Called from computeViewport()
// (title == "" on first call, i.e. one line reserved) and again whenever a
// new title arrives in loop() — the body must be re-paginated afterward if
// it's already loaded, since linesPerPage may have changed.
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

// Wraps `text` onto at most kMaxTitleLines lines, breaking at the last space
// that fits (falling back to a UTF-8-safe hard break), mirroring paginate()'s
// body-wrap loop but measured with the bold title font. If text still
// doesn't fit after kMaxTitleLines lines, the last line is ellipsis-truncated
// (the same popUtf8Char-based approach the single-line elide used before).
std::vector<std::string> CompanionModeActivity::wrapTitleToLines(const std::string& text) const {
  const int maxWidth = viewportWidth - kReadLaterIconAreaWidth;
  std::vector<std::string> lines;
  std::string remaining = text;

  while (!remaining.empty()) {
    if (maxWidth <= 0 ||
        renderer.getTextWidth(cachedTitleFontId, remaining.c_str(), EpdFontFamily::BOLD) <= maxWidth) {
      lines.push_back(remaining);
      remaining.clear();
      break;
    }

    const bool lastAllowedLine = static_cast<int>(lines.size()) + 1 >= kMaxTitleLines;
    if (lastAllowedLine) {
      std::string truncated = remaining;
      const std::string ellipsis = "\xE2\x80\xA6";  // U+2026 HORIZONTAL ELLIPSIS
      while (!truncated.empty() && renderer.getTextWidth(cachedTitleFontId, (truncated + ellipsis).c_str(),
                                                          EpdFontFamily::BOLD) > maxWidth) {
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
    // RenderLock: connected/haveContent/pages gate which branch render() takes
    // (renderWaiting() vs renderPage()) and renderPage() iterates `pages`
    // directly — mutating it here without the lock races the render task,
    // which can observe a torn `pages` (mid-clear()) against a stale
    // `totalPages`/`currentPage` and index out of bounds. Confirmed via a
    // real device crash (EXC inside renderPage()'s pages[currentPage] loop)
    // before this fix. See EpubReaderActivity.cpp for the same convention.
    RenderLock lock;
    connected = nowConnected;
    if (!connected) {
      haveContent = false;
      pages.clear();
      waitingSinceMs = millis();  // re-arm the idle-sleep timer for the waiting screen
      // Discard any in-flight, not-yet-committed batch — a disconnect mid-push means the
      // final-flagged field's END may never arrive, and the buffered bytes belong to a
      // session that's now gone (mirrors CompanionBle.cpp's resetReassembly() on disconnect).
      portENTER_CRITICAL(&g_mux);
      g_pendingTitleReady = false;
      g_pendingBodyReady = false;
      g_pendingCommitReady = false;
      g_pendingBatchStartMs = 0;
      portEXIT_CRITICAL(&g_mux);
    } else {
      waitingSinceMs = 0;
    }
    requestUpdate();
  }

  checkWaitingIdleSleep();

  bool gotTitle = false;
  bool gotBody = false;
  bool commit = false;
  bool gotStatus = false;
  std::string newTitle;
  std::string newBody;
  uint8_t newStatus = 0;
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
    // Safety net: the final-flagged field's END never arrived in time (e.g. the app
    // crashed or lost the connection mid-push). Apply whatever we have rather than
    // leaving the screen stuck on stale content indefinitely.
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
    newStatus = g_pendingStatusValue;
    g_pendingStatusReady = false;
    gotStatus = true;
  }
  portEXIT_CRITICAL(&g_mux);

  if (commit && (gotTitle || gotBody)) {
    // One lock for both halves (rather than two separate locks) so a render
    // can never land between the title and body updates of a single push and
    // see a mismatched pairing (e.g. new title still showing the old page
    // count). See the connect/disconnect branch above for why this needs a
    // RenderLock at all. Gating on `commit` (rather than applying gotTitle/
    // gotBody the moment each arrives) is what makes title+body land on
    // screen together instead of the headline updating first while body is
    // still mid-transfer — see CompanionBle.h's kFinalFieldFlag doc comment.
    RenderLock lock;
    if (gotTitle) {
      title = newTitle;
      updateTitleLayout();
      // If the body's already loaded and isn't about to be re-paginated below
      // by gotBody, re-paginate now — linesPerPage may have changed with the
      // title's wrapped line count.
      if (haveContent && !gotBody) paginate();
    }
    if (gotBody) {
      body = newBody;
      paginate();
      haveContent = true;
      readLaterSaved = false;  // new article: reset any previous save-state indicator
    }
    requestUpdate();
  }
  if (gotStatus && newStatus == static_cast<uint8_t>(companionble::StatusEvent::ReadLaterSaved)) {
    RenderLock lock;
    readLaterSaved = true;
    forceFastRefreshNextRender = true;
    requestUpdate();
  }

  if (!haveContent || !connected) return;

  if (mappedInput.wasPressed(MappedInputManager::Button::Left) && currentPage > 0) {
    RenderLock lock;
    currentPage--;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Right) && currentPage < totalPages - 1) {
    RenderLock lock;
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

  // Title: bold, larger than the body, wraps onto up to kMaxTitleLines lines
  // (see wrapTitleToLines(), computed in updateTitleLayout() whenever the
  // title changes), leaving room for the read-later icon at the first
  // line's right edge.
  const int titleLineHeight = renderer.getLineHeight(cachedTitleFontId);
  int titleY = cachedOrientedMarginTop;
  for (const auto& line : titleLines) {
    renderer.drawText(cachedTitleFontId, cachedOrientedMarginLeft, titleY, line.c_str(), true, EpdFontFamily::BOLD);
    titleY += titleLineHeight;
  }

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
    // PageBack/PageForward hints only appear when that direction is actually
    // pageable right now (empty string = hidden, the convention drawButtonHints()
    // itself checks — see e.g. FileBrowserActivity's confirmLabel/dirUp/dirDown).
    // No side (UP/DOWN) hints are drawn — those buttons still work and still
    // notify PREV/NEXT over BLE (see loop()), just without an on-screen hint.
    const char* pageBack = currentPage > 0 ? kPageBackHint : "";
    const char* pageForward = currentPage < totalPages - 1 ? kPageForwardHint : "";
    const auto labels = mappedInput.mapLabels(kPlayPauseHint, kReadLaterHint, pageBack, pageForward);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
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
