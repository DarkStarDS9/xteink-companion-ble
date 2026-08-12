#pragma once

#include <memory>
#include <string>
#include <vector>

#include "CompanionBle.h"
#include "CompanionPeerStore.h"
#include "CompanionTestConsole.h"
#include "CompanionTodoDiff.h"
#include "CompanionTodoDocument.h"
#include "CompanionTodoNav.h"
#include "activities/Activity.h"

// One row of the current ToDo List's visible window -- either a group header
// (drawn as a label, never selectable) or an item (checkbox + text,
// selectable). File-scope rather than nested in CompanionModeActivity so the
// .cpp's row-window-walking companiontodo::Visitor (private to that file)
// can build a vector of these without needing class access. See
// CompanionModeActivity::listVisibleRows's doc comment for the full picture.
struct CompanionListRow {
  bool isHeader = false;
  // What the checkbox draws: the document's value with any local check-off
  // deviation already applied (companiontodo::Diff::effectiveChecked()), so
  // rendering never has to know the diff exists.
  bool checked = false;
  // What the *document* says, undeviated. Kept alongside `checked` because
  // companiontodo::Diff::applyToggle() is defined against the document value,
  // not the on-screen one -- feeding it the effective value would invert the
  // deviation logic (an entry would be created exactly when it should be
  // removed) while still looking right for a single press.
  bool documentChecked = false;
  // The phone's opaque id for this item, echoed back in the check-off diff.
  // Meaningless for a header row.
  uint16_t itemId = 0;
  // -1 for a header row; this item's 0-based position among its list's
  // items (ignoring group boundaries) otherwise -- what
  // CompanionModeActivity::listNav's cursor() indexes, so rendering compares
  // the two to place the cursor marker.
  int itemFlatIndex = -1;
  std::string text;
};

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
    List,           // a ToDo List document (kFieldListDoc) -- see docs/companion-todo-list-design.md
    Message,        // a transient status line, auto-reverting after a few seconds
  };

  Screen screen = Screen::Waiting;

  // What one physical button does in the foreground app. Mirrors one entry of
  // the pushed button map; NONE for every button the app did not declare.
  struct ButtonSpec {
    companionble::ButtonRouting routing = companionble::ButtonRouting::None;
    // High-nibble flags from the declaration's byte 0 -- kButtonFlagAlso-
    // Notify / kButtonFlagLocalOnlyOffline (reserved bits may also be set
    // here; companionbuttons::decide() ignores anything it doesn't
    // recognise). See CompanionBle.h.
    uint8_t flags = 0;
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

  // True only between a push (image OR, since v11, a title/body/tag content
  // batch) committing and its RENDER_STATUS going out. RENDER_STATUS is
  // defined by the protocol as the *response* to a push, but both renderImage()
  // and renderPage() are also reached by ordinary redraws that were never asked
  // for (a central connecting, a foreground change, a gallery/page turn, a
  // tag-only redraw). Without this gate those redraws broadcast
  // RENDER_STATUS(DISPLAYED) to whoever holds the foreground, so a client that
  // connects while older content is up sees its own push resolve after a
  // couple of packets and stops transmitting — reproduced on hardware 6/6
  // against the iOS app for the image case (see commit 2cdbb3a7), and the same
  // mechanism generalizes to text rather than growing a second flag.
  //
  // One shared bool rather than one per pushable field: only one push (image
  // or content) is ever rendering at a time — the two screens are mutually
  // exclusive (see loop()'s "text replaces an image, and vice versa") — so
  // there is never a moment where two renders could both be legitimately
  // awaiting an answer. `renderAwaitingPushId` records which push this is, so
  // the eventual RENDER_STATUS names the right one.
  //
  // Set under RenderLock in handlePendingImage() (image) or loop()'s content
  // commit block (text), consumed exactly once by notifyRenderPushResult(),
  // and cleared on disconnect so an abandoned push cannot leak a status onto
  // an unrelated later redraw. Only ever set true when the push's pushId is
  // non-zero — see both arm sites' comments — so a client that did not ask
  // for an answer (pushId 0) never causes RENDER_STATUS traffic at all.
  bool renderAwaitingStatus = false;
  // The pushId this armed expectation must echo — see notifyRenderStatus()'s
  // doc comment in CompanionBle.h. Replaced the v11-launch-day `field` byte
  // (kFieldImage/kFieldBody) with the pushing client's own chosen id: exact
  // correlation, and a future push type costs nothing here.
  uint8_t renderAwaitingPushId = 0;

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

  // ---------------------------------------------------------------------
  // ToDo List (Screen::List) -- docs/companion-todo-list-design.md §5, §8.
  // ---------------------------------------------------------------------

  // Rebuilt by reloadListView() from a single companiontodo::parseDocument()
  // walk of listDocBuf (below) every time the view changes (nav press, new
  // document, foreground/peer change) -- never per render(), per that
  // function's own cost note. Bounded by listNav.visibleCapacity() (a
  // handful of rows, sized to the viewport) -- never the whole document. See
  // CompanionTodoNav.h for the pure cursor/paging/list-switching state
  // machine this is rendered against; that class is host-unit-tested
  // (test/companion_todo_nav/), this vector and the walk that fills it are
  // not (no host seam -- see this feature's commit message).
  std::vector<CompanionListRow> listVisibleRows;
  companiontodo::Nav listNav;
  std::string listDocTitle;    // the current list's title
  std::string listPeerKey;     // whose document is loaded -- see enterListDocument()
  bool listDocLoaded = false;  // false: peer has no (parseable) document at all
  // The peer's lists.bin (companionpeer::readListDocument()), held in RAM for
  // the whole time Screen::List is up over it, sized exactly to the stored
  // document (<= companionble::kMaxListDocLen, 16 KB) via makeUniqueNoThrow.
  // reloadListView() walks THIS with companiontodo::parseDocument() on every
  // nav press rather than re-opening SD each time -- the previous design read
  // and re-parsed from SD on every single cursor keypress, a real
  // heap-fragmentation risk on this no-PSRAM part over a long browse. Loaded
  // by enterListDocument() and by the gotListDoc handler when a fresh
  // document lands for the peer already on screen; freed by
  // freeListDocBuf(), called from every path that leaves Screen::List --
  // audited in this feature's commit message. A <=16 KB buffer must never
  // outlive the screen it was read for.
  std::unique_ptr<uint8_t[]> listDocBuf;
  size_t listDocBufLen = 0;
  // The peer's check-off deviations (list_state.bin), materialised for exactly
  // as long as listDocBuf is -- allocated and freed by the same two functions,
  // so its 1096 bytes (CompanionTodoDiff.h's MEMORY note) cost nothing while
  // no list is on screen. That pairing is the whole reason it is a
  // unique_ptr and not a plain member: 1096 permanently-resident bytes
  // alongside NimBLE is not a trade this part can make.
  //
  // NULL IS A SUPPORTED STATE, not just an error one: makeUniqueNoThrow can
  // fail on a part this tight, and when it does the list stays browsable
  // read-only (Phase A behaviour) rather than the screen refusing to open.
  // Every read of this pointer must therefore be guarded.
  std::unique_ptr<companiontodo::Diff> listDiff;
  // Where Back returns to -- GalleryPicker for the picker entry point (no
  // live session), Text for a live LIST peer that pushed a document while
  // foreground (applyForegroundChange() landed here directly).
  Screen listReturnScreen = Screen::IconGrid;

  // What one physical button does on Screen::List, declared by listPeerKey's
  // own button map -- loaded fresh by loadListButtonRouting() every time
  // enterListDocument() opens a document, live or via the offline picker.
  // Deliberately separate from `buttons` above: listPeerKey is not always
  // foregroundPeerKey (the picker can browse a LIST peer with no live
  // session at all), so List's routing cannot simply reuse whatever the
  // foreground peer last declared. See handleListNav().
  ButtonSpec listButtons[kButtonCount];

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
  int cachedTitleBlockHeight = 0;   // vertical space reserved above the body for the bold title line
  int listRowHeight = 0;            // pixel height of one ToDo List row, computed alongside the above
  uint8_t listVisibleCapacity = 1;  // how many item rows fit below the list title -- feeds listNav

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
  // Parses the button-map section of a UI declaration body (offset already
  // past the 2-byte header) into `out`, advancing `offset` past it so a
  // caller that also wants the trailing tag section can continue from there.
  // Shared by loadUiDeclaration() (foreground peer -> `buttons`) and
  // loadListButtonRouting() (a browsed LIST peer -> `listButtons`), since the
  // wire layout and validation are identical -- only which peer and which
  // array differ.
  void parseButtonEntries(const uint8_t* raw, size_t len, size_t& offset, uint8_t buttonEntries,
                          ButtonSpec (&out)[kButtonCount]);
  // Reads `peerKey`'s persisted UI declaration and fills `listButtons` with
  // its button map -- no default, so a peer that declares none of the
  // LocalList* routings leaves Screen::List's buttons dead. Called by
  // enterListDocument() for whichever peer's document is being opened, live
  // or via the offline picker; independent of `buttons`/foregroundPeerKey
  // because the two can name different peers (see listButtons' doc comment).
  void loadListButtonRouting(const std::string& peerKey);
  void applyTagState(const uint8_t* data, size_t len);
  void setTagState(uint8_t tagId, uint8_t state);
  void measureTagRow();
  bool tagIsDrawn(const TagSpec& tag) const;
  companionble::ButtonRouting routingFor(companionble::ButtonId button) const;
  uint8_t flagsFor(companionble::ButtonId button) const;
  const char* labelFor(companionble::ButtonId button) const;
  companionble::ButtonRouting listRoutingFor(companionble::ButtonId button) const;
  uint8_t listFlagsFor(companionble::ButtonId button) const;
  const char* listLabelFor(companionble::ButtonId button) const;
  bool handleMappedButton(MappedInputManager::Button role, companionble::ButtonId id);
  void applyForegroundChange();
  void handlePendingImage(const std::string& stagedPath, const std::string& peerKey, const uint8_t* contentId,
                          size_t contentIdLen, uint8_t pushId);
  void refreshGalleryForForeground();
  void loadGalleryForPeer(const std::string& peerKey);
  bool handleGalleryNav();
  void showGalleryImage(size_t index);
  void showTransientMessage(const std::string& text, Screen returnTo, unsigned long durationMs = 3000);
  void enterGalleryPicker();
  void selectGalleryPickerPeer();
  bool handlePickerInput();
  // Enters Screen::List over `peerKey`'s stored document -- the one entry
  // point both a live LIST peer's applyForegroundChange() and the offline
  // icon-grid picker (enterListPicker()/selectListPickerPeer(), see the
  // second commit of this feature) go through. `returnTo` is where Back
  // takes the user (see listReturnScreen's doc comment). Shows a transient
  // "nothing yet" message and returns false, without switching screens, if
  // the peer has no (parseable) document -- fine to call speculatively.
  bool enterListDocument(const std::string& peerKey, Screen returnTo);
  // Reads listPeerKey's stored document (companionpeer::listDocumentSize() +
  // readListDocument()) into listDocBuf, replacing whatever was held before.
  // This is the only place that touches SD for the list document; everything
  // else (reloadListView() and its Visitor walks) reads listDocBuf in RAM.
  // Called from enterListDocument() and from the gotListDoc handler when a
  // fresh document lands for the peer already on screen. Leaves listDocBuf
  // null (and listDocLoaded false, via the reloadListView() call every
  // caller makes right after) if the peer has no document or the read
  // failed -- fine to call speculatively, same as enterListDocument().
  void loadListDocBuf();
  // Materialises listDiff for listPeerKey, against the document already in
  // listDocBuf. Called only from loadListDocBuf(), and only once the document
  // is in hand -- the two must be loaded together or the revision check below
  // has nothing to check against. Leaves listDiff null (read-only browsing)
  // if the 1096 bytes cannot be had.
  void loadListDiff();
  // Frees listDocBuf and listDiff. Called from every path that can leave Screen::List --
  // Back, a foreground handover to a different peer, a live push that takes
  // the screen out from under a locally-browsed (picker) document, and
  // Activity::onExit() -- see this feature's commit message for the full
  // audit. A <=16 KB buffer must not survive the screen it was read for.
  void freeListDocBuf();
  // Re-walks listDocBuf (companiontodo::parseDocument(), pure in-RAM parsing
  // -- no SD, no allocation) to refresh listNav's counts and listVisibleRows
  // for whatever list/window listNav is currently pointed at. Called once per
  // view change -- never from render() itself. `recountTotals` controls how
  // many passes that costs: Up/Down inside one list can never change that
  // list's own item count or the document's list count, so
  // handleListNav()'s Up/Down branches pass false and this does one
  // windowed pass; switching lists, a fresh document landing, and first
  // entry all pass true (the default) because a list's item count -- or the
  // whole document -- may have changed since listNav last knew it, costing
  // up to two extra passes to re-derive that safely (see the .cpp comment).
  //
  // See this feature's commit message for why the extracted seam stops at
  // CompanionTodoNav's pure cursor arithmetic rather than reaching this
  // function too: the row-window walk depends on the on-device renderer's
  // string/text handling and isn't cheaply host-testable.
  void reloadListView(bool recountTotals = true);
  // Up/Down/Left/Right/Confirm/Back on Screen::List. Unlike
  // handleGalleryNav(), claims every one of those buttons unconditionally --
  // see this function's .cpp comment for why that's safe here specifically.
  bool handleListNav();
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
  void renderList();
  // Sends RENDER_STATUS only if a push (image or content) is still awaiting
  // its answer, and consumes that expectation. Every notify reached from
  // renderImage() or the Screen::Text branch of render() must go through here;
  // the pre-render rejections in handlePendingImage() and CompanionBle's
  // START-time checks are unconditionally solicited and call
  // companionble::notifyRenderStatus() directly.
  void notifyRenderPushResult(companionble::RenderResult result);
  // Answers a still-armed render expectation with RenderResult::Superseded and
  // disarms it, so a new push can arm its own. Call immediately before arming,
  // from every site that arms — both arm sites overwrite renderAwaitingPushId
  // unconditionally, and whichever push lands second also sets `screen`, so the
  // first one's render never runs and its caller would otherwise wait out its
  // whole timeout for an answer that was never coming. No-op when nothing is
  // armed, which is the overwhelmingly common case.
  void supersedePendingRenderStatus();
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
