#pragma once

#include <string>
#include <vector>

#include "CompanionBle.h"
#include "CompanionPeerStore.h"
#include "CompanionTestConsole.h"
#include "activities/Activity.h"

// Companion Mode: X3 as a BLE GATT peripheral, rendering whatever the
// foreground phone app pushes — title/body text or a full-screen image — and
// reporting the buttons that app declared as remote (see
// docs/companion-display-protocol.md). This is the device's sole normal-boot
// activity (see main.cpp); the firmware is companion-only.
//
// The v6 shape: several apps can be connected at once over one BLE link, but
// only one holds the screen. What each button does is not a firmware decision —
// it comes from the foreground peer's button map, which the app declared at
// enrollment and which is reloaded on every foreground handover. Switching apps
// on the phone is therefore not a mode change here: same activity, same
// protocol, different data.
class CompanionModeActivity final : public Activity {
  // What is on screen. An explicit state rather than a pile of booleans: the
  // screens are mutually exclusive and render() must dispatch on exactly one.
  enum class Screen : uint8_t {
    StartFailed,    // BLE never came up (heap floor or NimBLE init failure)
    Waiting,        // no app holds the screen, and no enrolled app has an icon
    IconGrid,       // idle with enrolled apps: the decorative sleep grid
    GalleryPicker,  // interactive: choose which image-capable peer's gallery to browse
    Pairing,        // "Pair with <app>?" prompt, awaiting CONFIRM/BACK
    Text,           // title/body from the foreground app
    Image,          // a pushed photo, full screen
    Message,        // a transient status line, auto-reverting after a few seconds
  };

  Screen screen = Screen::Waiting;

  // What one physical button does in the foreground app. Mirrors one entry of
  // the pushed button map; NONE for every button the app did not declare.
  struct ButtonSpec {
    companionble::ButtonRouting routing = companionble::ButtonRouting::None;
    std::string label;
  };
  static constexpr size_t kButtonCount = 7;  // one per companionble::ButtonId
  ButtonSpec buttons[kButtonCount];

  bool connected = false;
  bool haveContent = false;

  // The foreground app's declared tags: short labelled chips it switches on and
  // off. The firmware declares none of these — id, label and meaning all come
  // from the peer's UI declaration, and the device only draws them. Not reset by
  // a content push: when a tag should clear is app meaning too, so an app that
  // wants them to change together pushes tag state inside the same atomic batch.
  //
  // MEMORY: kMaxTags (6) x (kMaxTagLabelLen + 1 + 2) = 90 bytes, resident only
  // for the peer that currently owns the screen. Every other peer's declaration
  // stays on SD.
  struct TagSpec {
    uint8_t id = 0;
    uint8_t state = 0;  // companionble::TagState
    char label[companionble::kMaxTagLabelLen + 1] = {0};
  };
  TagSpec tags[companionble::kMaxTags];
  uint8_t tagCount = 0;
  int tagRowWidth = 0;  // measured once per change, so the title wrap can reserve it
  // How the whole row is drawn (companionble::TagRenderStyle); one choice per
  // peer, sent as a trailing byte on the UI declaration. Defaults to Bordered.
  uint8_t tagRenderStyle = 0;

  // Set when a Status write lands, so the next renderPage() flips the E-ink
  // panel with FAST_REFRESH regardless of the normal per-page-turn refresh
  // cadence (pagesUntilFullRefresh) — an indicator flip should never trigger a
  // full flashing refresh.
  bool forceFastRefreshNextRender = false;

  // Set when only tag state changed while a print is on screen. render() then
  // draws the chips over the retained framebuffer instead of re-decoding and
  // re-settling the image, which would cost seconds for a mark that moved.
  bool tagOnlyRedraw = false;

  std::string foregroundPeerKey;
  std::string foregroundAppName;

  std::string title;
  std::string body;
  std::vector<std::string> titleLines;  // title wrapped to at most kMaxTitleLines lines

  std::string displayedImagePath;

  // True only between an image push committing and its IMAGE_STATUS going out.
  // IMAGE_STATUS is defined by the protocol as the *response* to a push, but
  // renderImage() is also reached by every ordinary redraw of an image already
  // on screen (a central connecting, a foreground change, a gallery page turn).
  // Without this gate those redraws broadcast IMAGE_STATUS(DISPLAYED) to
  // whoever holds the foreground, so a client that connects while an older
  // image is up sees its own pushImage() resolve after a couple of packets and
  // stops transmitting — reproduced on hardware 6/6 against the iOS app.
  // Set under RenderLock in handlePendingImage(), consumed exactly once by
  // notifyImagePushResult(), and cleared on disconnect so an abandoned push
  // cannot leak a status onto an unrelated later redraw.
  bool imagePushAwaitingStatus = false;

  // Firmware-local browsing of the foreground peer's previously pushed images
  // (CompanionPeerStore's bounded per-peer gallery, kMaxImagesPerPeer entries,
  // oldest first). Refreshed whenever a new image is committed
  // (refreshGalleryForForeground()) and cleared on foreground handover — an
  // app's gallery does not follow it off screen. No protocol involvement: the
  // phone is never told navigation happened, same as text pagination's
  // currentPage/totalPages, which are also purely on-device state (see
  // docs/companion-display-protocol.md and this feature's commit message for
  // why that's the right call here too).
  std::vector<std::string> galleryImages;
  size_t galleryIndex = 0;

  // On-device picker over enrolled peers that declared the image-gallery
  // capability (companionble::kUiCapabilityImageGallery) — not every enrolled
  // app, since most don't push photos at all. Selecting one loads its stored
  // gallery locally; no BLE interaction is involved; see
  // docs/companion-display-protocol.md's "On-screen behaviour".
  std::vector<std::string> pickerPeerKeys;
  size_t pickerCursor = 0;

  // Which peer's gallery is on screen via the picker or the disconnect
  // fallback below, distinct from foregroundPeerKey (a live BLE session —
  // most peers in the picker have none). Only meaningful while
  // galleryPickerBrowsing is true.
  std::string browsingPeerKey;
  bool galleryPickerBrowsing = false;

  // Reset whenever foregroundPeerKey changes to a new peer; set once that
  // peer's image push actually commits. Used only to decide whether a
  // disconnect should fall through to that peer's gallery instead of the
  // normal hold-last-content behaviour — see loop()'s disconnect handling.
  bool foregroundPushedImageThisSession = false;

  std::string transientMessage;
  unsigned long transientMessageUntilMs = 0;
  Screen transientMessageReturnScreen = Screen::IconGrid;

  std::string pairingAppName;
  unsigned long pairingDeadlineMs = 0;
  // Matches the client-side handshake timeout in CompanionKit, which waits 35 s
  // so it never gives up while the user is still reaching for the button.
  static constexpr unsigned long kPairingTimeoutMs = 30UL * 1000UL;

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

  // Set whenever no app holds the screen; 0 while one does. loop() deep-sleeps
  // once this has been non-zero for longer than kWaitingIdleSleepMs, so an
  // unattended device advertising forever doesn't drain the battery. This is
  // also what takes a held-after-disconnect screen to the icon grid: the
  // content stays up until this timer expires (see the protocol doc's
  // "On-screen behaviour").
  unsigned long idleSinceMs = 0;
  static constexpr unsigned long kWaitingIdleSleepMs = 5UL * 60UL * 1000UL;  // 5 minutes

  // Hold-tracking for whichever button is currently down and routed REMOTE.
  // The hardware only ever has one physical button held at a time (mirrors
  // InputManager::getHeldTime()'s own single global press-start timestamp),
  // so one in-flight sequence is all that needs tracking.
  bool holdActive = false;
  companionble::ButtonId holdButton = companionble::ButtonId::Back;
  uint16_t holdTicksSent = 0;
  static constexpr unsigned long kHoldTickMs = 100;

  // Input seams. Each is `MappedInputManager` ORed with the serial test
  // console's injected state, so an automated test drives the *real* routing,
  // hold-tick and notify paths rather than a parallel one. Without the console
  // compiled in these are one-line forwards.
  bool buttonWasPressed(MappedInputManager::Button role, companionble::ButtonId id) const;
  bool buttonIsPressed(MappedInputManager::Button role, companionble::ButtonId id) const;
  bool buttonWasReleased(MappedInputManager::Button role, companionble::ButtonId id) const;
  unsigned long buttonHeldTime(companionble::ButtonId id) const;
  const char* screenName() const;
#ifdef COMPANION_TEST_CONSOLE
 public:
  // Live tag state for the serial console's CTAGS. The Status characteristic is
  // write-without-response, so this is the only way a test can confirm a tag
  // write actually landed.
  uint8_t reportTags(companiontest::TagReport* out, uint8_t maxTags) const;

 private:
#endif

  void notifyHeldButton(companionble::ButtonId button);
  void loadUiDeclaration();
  void clearUiDeclaration();
  void applyTagState(const uint8_t* data, size_t len);
  void setTagState(uint8_t tagId, uint8_t state);
  void measureTagRow();
  bool tagIsDrawn(const TagSpec& tag) const;
  companionble::ButtonRouting routingFor(companionble::ButtonId button) const;
  const char* labelFor(companionble::ButtonId button) const;
  bool handleMappedButton(MappedInputManager::Button role, companionble::ButtonId id);
  void applyForegroundChange();
  void handlePendingImage(const std::string& stagedPath, const std::string& peerKey, const uint8_t* contentId,
                          size_t contentIdLen);
  void refreshGalleryForForeground();
  void loadGalleryForPeer(const std::string& peerKey);
  bool handleGalleryNav();
  void showGalleryImage(size_t index);
  void showTransientMessage(const std::string& text, Screen returnTo, unsigned long durationMs = 3000);
  void enterGalleryPicker();
  void selectGalleryPickerPeer();
  bool handlePickerInput();
  void computeViewport();
  void updateTitleLayout();
  std::vector<std::string> wrapTitleToLines(const std::string& text) const;
  void paginate();
  void chooseIdleScreen();
  // inverted flips the whole frame (renderer.invertScreen()) before flipping
  // the panel; label, if non-null/non-empty, is drawn as a status word below
  // the grid (or below the waiting text, for renderWaiting()). Used both for
  // the plain idle screen (inverted=false, label=nullptr) and for the
  // sleep/boot variants — see renderPreSleepScreen() and onEnter().
  void renderWaiting(bool inverted = false, const char* label = nullptr);
  void renderIconGrid(bool inverted = false, const char* label = nullptr);
  void renderGalleryPicker();
  void renderTransientMessage();
  void renderPairingPrompt();
  void renderStartFailed();
  void renderPage();
  void renderImage();
  // Sends IMAGE_STATUS only if a pushed image is still awaiting its answer,
  // and consumes that expectation. Every notify reached from renderImage()
  // must go through here; the pre-render rejections in handlePendingImage()
  // and CompanionBle's START-time checks are unconditionally solicited and
  // call companionble::notifyImageStatus() directly.
  void notifyImagePushResult(companionble::ImageResult result);
  void renderTags(int rightEdgeX, int centerY) const;
  // Draws a small sleeping indicator (bottom-left, same corner text mode's
  // battery percentage occupies) over the currently-displayed image, without
  // touching the rest of the framebuffer.
  void drawSleepIndicator();
  // Paints whatever should be on screen the instant before deep sleep, for
  // every trigger that reaches this activity (idle timeout, app-mapped sleep
  // button, and the physical power button via customDeepSleep()) — so all
  // three agree. Screen::Image keeps the photo and adds drawSleepIndicator();
  // every other screen (including Text — see the protocol doc) falls back to
  // the inverted grid. Does not itself sleep: callers decide how (direct
  // powerManager.startDeepSleep(), or main.cpp's enterDeepSleep() finishing
  // its teardown sequence after customDeepSleep() returns true).
  void renderPreSleepScreen();
  void checkIdleTimers();

 public:
  explicit CompanionModeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("CompanionMode", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // A live BLE session has no physical-input signal for main.cpp's general
  // auto-sleep timer to see (see docs/companion-multi-app-design.md: the whole
  // point of companion mode is phone-driven, button-free operation) — without
  // this, a battery-powered device with an app connected but no button presses
  // hits the inactivity timeout and deep-sleeps mid-session, dropping the BLE
  // link. Beyond that, this activity fully owns its own idle-to-sleep timing
  // (checkIdleTimers()'s 5-minute countdown) regardless of connection state,
  // so it always returns true — letting main.cpp's generic timer run here too
  // would race checkIdleTimers() and could sleep mid-countdown with the wrong
  // (generic) sleep screen.
  bool preventAutoSleep() override { return true; }

  // See Activity::customDeepSleep()'s doc comment. Renders the appropriate
  // pre-sleep screen for whatever's currently up (renderPreSleepScreen()) and
  // returns true, unless nothing sensible can be drawn (StartFailed), in
  // which case the generic SleepActivity is a better fallback than a blank
  // panel.
  bool customDeepSleep() override;
};
