#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

// Companion Mode: X3 as a BLE GATT peripheral showing title/body text pushed
// from a phone app (see docs/companion-display-protocol.md), with LEFT/RIGHT
// paging locally and CONFIRM/BACK reported back over BLE. See
// docs/companion-mode-implementation-notes.md for the design rationale.
class CompanionModeActivity final : public Activity {
  bool connected = false;
  bool haveContent = false;
  bool startFailed = false;  // ensureStarted() refused (heap floor or NimBLE init failure)

  std::string title;
  std::string body;

  int currentPage = 0;
  int totalPages = 0;
  int pagesUntilFullRefresh = 0;
  std::vector<std::vector<std::string>> pages;  // pages[i] = wrapped lines for page i

  int cachedFontId = 0;
  int viewportWidth = 0;
  int linesPerPage = 0;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  void computeViewport();
  void paginate();
  void renderWaiting();
  void renderStartFailed();
  void renderPage();

 public:
  explicit CompanionModeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("CompanionMode", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
