#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

// Companion Mode: X3 as a BLE GATT peripheral showing title/body text pushed
// from a phone app (see docs/companion-display-protocol.md). Bottom LEFT/RIGHT
// page the locally-buffered body text; side UP/DOWN and bottom BACK/CONFIRM
// report PREV/NEXT/PLAY_PAUSE/READ_LATER over BLE. See
// docs/companion-mode-implementation-notes.md for the design rationale. This
// is the device's sole normal-boot activity (see main.cpp) — the firmware is
// companion-only.
class CompanionModeActivity final : public Activity {
  bool connected = false;
  bool haveContent = false;
  bool startFailed = false;  // ensureStarted() refused (heap floor or NimBLE init failure)
  bool readLaterSaved = false;  // toggled by the Status characteristic's READ_LATER_SAVED write
  // Set when a Status write lands, so the next renderPage() flips the E-ink
  // panel with FAST_REFRESH regardless of the normal per-page-turn refresh
  // cadence (pagesUntilFullRefresh) — the icon flip should never trigger a
  // full flashing refresh.
  bool forceFastRefreshNextRender = false;

  std::string title;
  std::string body;
  std::vector<std::string> titleLines;  // title wrapped to at most kMaxTitleLines lines (see wrapTitleToLines())

  int currentPage = 0;
  int totalPages = 0;
  int pagesUntilFullRefresh = 0;
  std::vector<std::vector<std::string>> pages;  // pages[i] = wrapped lines for page i

  int cachedFontId = 0;
  int cachedTitleFontId = 0;
  int viewportWidth = 0;
  int linesPerPage = 0;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;
  int cachedTitleBlockHeight = 0;  // vertical space reserved above the body for the bold title line

  // Set whenever the "Waiting for phone" screen is (re-)entered with no
  // central connected (onEnter(), and again on disconnect); 0 while a central
  // is connected. loop() deep-sleeps once this has been non-zero for longer
  // than kWaitingIdleSleepMs, so an unattended device advertising forever
  // doesn't drain the battery. Named constant so the timeout is easy to tune.
  unsigned long waitingSinceMs = 0;
  static constexpr unsigned long kWaitingIdleSleepMs = 5UL * 60UL * 1000UL;  // 5 minutes

  void computeViewport();
  void updateTitleLayout();
  std::vector<std::string> wrapTitleToLines(const std::string& text) const;
  void paginate();
  void renderWaiting();
  void renderStartFailed();
  void renderPage();
  void renderReadLaterIcon(int x, int y) const;
  void checkWaitingIdleSleep();

 public:
  explicit CompanionModeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("CompanionMode", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
