#include "CompanionModeActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "CompanionBle.h"
#include "MappedInputManager.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Companion Mode's fixed font — not user-configurable (unlike the reader),
// since the capability characteristic advertises a fixed char-grid size.
constexpr int kCompanionFontId = NOTOSANS_14_FONT_ID;

// Task-boundary handoff for content arriving on the NimBLE host task (see
// CompanionBle.h's ContentFieldCallback doc comment). Fixed-size buffers, not
// heap allocation, so the critical section only ever does a memcpy.
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
uint8_t g_pendingTitleBuf[companionble::kMaxFieldLen];
uint16_t g_pendingTitleLen = 0;
volatile bool g_pendingTitleReady = false;
uint8_t g_pendingBodyBuf[companionble::kMaxFieldLen];
uint16_t g_pendingBodyLen = 0;
volatile bool g_pendingBodyReady = false;

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

}  // namespace

void CompanionModeActivity::onEnter() {
  Activity::onEnter();
  connected = false;
  haveContent = false;
  currentPage = 0;
  totalPages = 0;
  pages.clear();
  cachedFontId = kCompanionFontId;
  computeViewport();

  companionble::setContentFieldCallback(onContentField);
  startFailed = !companionble::ensureStarted(renderer, cachedFontId);
  if (startFailed) {
    LOG_ERR("CMA", "ensureStarted() failed (heap floor or NimBLE init)");
  }

  requestUpdate();
}

void CompanionModeActivity::onExit() {
  Activity::onExit();
  companionble::setContentFieldCallback(nullptr);
  companionble::stop();

  portENTER_CRITICAL(&g_mux);
  g_pendingTitleReady = false;
  g_pendingBodyReady = false;
  portEXIT_CRITICAL(&g_mux);
}

void CompanionModeActivity::computeViewport() {
  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginBottom += UITheme::getInstance().getStatusBarHeight();

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
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

void CompanionModeActivity::loop() {
  if (startFailed) return;  // nothing to poll: BLE never came up

  const bool nowConnected = companionble::isConnected();
  if (nowConnected != connected) {
    connected = nowConnected;
    if (!connected) {
      haveContent = false;
      pages.clear();
    }
    requestUpdate();
  }

  bool gotTitle = false;
  bool gotBody = false;
  std::string newTitle;
  std::string newBody;
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
  portEXIT_CRITICAL(&g_mux);

  if (gotTitle) title = newTitle;
  if (gotBody) {
    body = newBody;
    paginate();
    haveContent = true;
    requestUpdate();
  }

  if (!haveContent || !connected) return;

  const auto [prev, next, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  (void)fromTilt;
  if (prev && currentPage > 0) {
    currentPage--;
    requestUpdate();
  } else if (next && currentPage < totalPages - 1) {
    currentPage++;
    requestUpdate();
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    companionble::notifyButtonEvent(companionble::ButtonEvent::Confirm);
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    companionble::notifyButtonEvent(companionble::ButtonEvent::Back);
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

void CompanionModeActivity::renderPage() {
  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages > 0 ? totalPages - 1 : 0;

  const int lineHeight = renderer.getLineHeight(cachedFontId);
  int y = cachedOrientedMarginTop;
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

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
}
