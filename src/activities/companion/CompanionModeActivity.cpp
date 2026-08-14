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

#include "CompanionBatchModel.h"
#include "CompanionBle.h"
#include "CompanionButtonPolicy.h"
#include "CompanionPeerStore.h"
#include "CompanionTestConsole.h"
#include "CompanionTodoDocument.h"
#include "CompanionUiDeclaration.h"
#include "CrossPointState.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "MappedInputManager.h"
#include "Memory.h"
#include "SilentRestart.h"
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

// The title/body/tag-state atomic-batch state machine -- see
// CompanionBatchModel.h. NOT thread-safe on its own; every call below runs
// inside a g_mux critical section, exactly as the plain globals it replaced
// did.
CompanionBatchModel g_batchModel;

uint8_t g_pendingTagId = 0;
uint8_t g_pendingTagState = 0;
volatile bool g_pendingStatusReady = false;

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
// This image push's END byte 2, latched here so it survives the hop from the
// image write-behind task's callback (onImageStaged, below) to loop()'s
// drain, and from there into handlePendingImage()'s arm. An image push is
// always a single field, so — unlike the batch pushId below — there is no
// "only when final" subtlety: whatever pushId rode this transfer's END is
// the one RENDER_STATUS must echo.
uint8_t g_pendingImagePushId = 0;
volatile bool g_pendingImageReady = false;

// A kFieldListDoc push landed and was stored to SD (companionble::
// ListDocStoredCallback) -- which peer, so loop() can decide whether it's
// worth a reload (only when that peer's document is the one currently on
// Screen::List; see the drain site). No document content rides this hop --
// it's already on SD, and re-reading it is a main-loop-task job per
// docs/companion-todo-list-design.md §8.
char g_pendingListDocPeerKey[companionpeer::kPeerKeyLen] = {0};
volatile bool g_pendingListDocReady = false;

// Runs on the NimBLE host task — hand the field to the batch model and let
// CompanionModeActivity::loop() (main loop task) do the rest via poll().
void onContentField(uint8_t field, const uint8_t* data, size_t len, bool final, companionble::FieldOutcome outcome,
                    uint8_t pushId) {
  portENTER_CRITICAL(&g_mux);
  g_batchModel.onField(field, data, len, final, outcome, pushId, millis());
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

void onImageStaged(const char* peerKey, const char* path, const uint8_t* contentId, size_t contentIdLen,
                   uint8_t pushId) {
  portENTER_CRITICAL(&g_mux);
  snprintf(g_pendingImagePath, sizeof(g_pendingImagePath), "%s", path ? path : "");
  snprintf(g_pendingImagePeerKey, sizeof(g_pendingImagePeerKey), "%s", peerKey ? peerKey : "");
  const size_t n = contentIdLen > sizeof(g_pendingImageContentId) ? sizeof(g_pendingImageContentId) : contentIdLen;
  if (n > 0) memcpy(g_pendingImageContentId, contentId, n);
  g_pendingImageContentIdLen = static_cast<uint8_t>(n);
  g_pendingImagePushId = pushId;
  g_pendingImageReady = true;
  portEXIT_CRITICAL(&g_mux);
}

void onListDocStored(const char* peerKey) {
  portENTER_CRITICAL(&g_mux);
  snprintf(g_pendingListDocPeerKey, sizeof(g_pendingListDocPeerKey), "%s", peerKey ? peerKey : "");
  g_pendingListDocReady = true;
  portEXIT_CRITICAL(&g_mux);
}

// ---------------------------------------------------------------------------
// ToDo List document walkers (Screen::List) -- both are thin, stateless-per-
// call companiontodo::Visitor implementations that re-walk the whole stored
// document every time they're used (see CompanionModeActivity::
// reloadListView()'s doc comment for why that's the right cost model here).
// Kept out of the class itself since a companiontodo::Visitor override set
// is glue, not state worth exposing -- CompanionTodoNav.h is where the
// actual cursor/paging logic lives and is unit tested.
// ---------------------------------------------------------------------------

// Pass 1: how many lists does this document have, and (only meaningful when
// targetListIndex is in range) how many items does that one list hold. Two
// numbers, no text -- cheap enough to throw away and redo if reloadListView()
// discovers targetListIndex needs to be clamped after this returns.
class ListCountingVisitor : public companiontodo::Visitor {
 public:
  explicit ListCountingVisitor(int targetListIndex) : targetListIndex_(targetListIndex) {}

  void onListStart(uint16_t, const char*, uint8_t) override {
    inTarget_ = (static_cast<int>(listIndex_) == targetListIndex_);
    itemsInThisList_ = 0;
  }
  void onItem(uint16_t, bool, const char*, uint8_t) override {
    if (inTarget_) itemsInThisList_++;
  }
  void onListEnd(uint16_t) override {
    if (inTarget_) targetItemCount_ = itemsInThisList_;
    listIndex_++;
    inTarget_ = false;
  }

  uint16_t listCount() const { return listIndex_; }
  uint16_t targetItemCount() const { return targetItemCount_; }

 private:
  int targetListIndex_;
  uint16_t listIndex_ = 0;
  bool inTarget_ = false;
  uint16_t itemsInThisList_ = 0;
  uint16_t targetItemCount_ = 0;
};

// The document's revision and nothing else. A four-byte read of listDocBuf
// would answer the same question, but only by copying the wire layout out of
// CompanionTodoDocument.h into a second place -- and that header is explicit
// that it is the single source of truth for this format. One extra in-RAM
// walk buys that; it is paid once per SD load, not per keypress.
class ListRevisionVisitor : public companiontodo::Visitor {
 public:
  void onDocument(uint32_t revision) override { revision_ = revision; }
  uint32_t revision() const { return revision_; }

 private:
  uint32_t revision_ = 0;
};

// Pass 2: the target list's title and every row (group header + item) that
// falls in [windowStart, windowStart + capacity) of its flat item sequence.
// A group's header is emitted once, right before the first in-window item
// that belongs to it -- so a window that starts mid-group still shows that
// group's label, and an empty label (the "ungrouped" bucket, design doc §2)
// never emits a header row at all.
class ListRowVisitor : public companiontodo::Visitor {
 public:
  // `diff` may be null -- no check-off state was loadable, so the document's
  // own `checked` is what the user sees (Phase A behaviour). It is borrowed
  // for the duration of one walk and never stored beyond it.
  ListRowVisitor(int targetListIndex, uint16_t windowStart, uint16_t capacity, std::string* titleOut,
                 std::vector<CompanionListRow>* rowsOut, const companiontodo::Diff* diff)
      : targetListIndex_(targetListIndex),
        windowStart_(windowStart),
        windowEnd_(static_cast<uint32_t>(windowStart) + capacity),
        titleOut_(titleOut),
        rowsOut_(rowsOut),
        diff_(diff) {}

  void onListStart(uint16_t, const char* title, uint8_t titleLen) override {
    inTarget_ = (static_cast<int>(listIndex_) == targetListIndex_);
    if (inTarget_) titleOut_->assign(title, titleLen);
  }
  void onGroupStart(uint16_t, const char* label, uint8_t labelLen) override {
    if (!inTarget_) return;
    groupLabel_.assign(label, labelLen);
    groupHeaderEmitted_ = false;
  }
  void onItem(uint16_t itemId, bool checked, const char* text, uint8_t textLen) override {
    if (!inTarget_) return;
    if (itemIndex_ >= windowStart_ && itemIndex_ < windowEnd_) {
      if (!groupLabel_.empty() && !groupHeaderEmitted_) {
        CompanionListRow header;
        header.isHeader = true;
        header.text = groupLabel_;
        rowsOut_->push_back(std::move(header));
        groupHeaderEmitted_ = true;
      }
      CompanionListRow row;
      row.documentChecked = checked;
      row.checked = diff_ ? diff_->effectiveChecked(itemId, checked) : checked;
      row.itemId = itemId;
      row.itemFlatIndex = static_cast<int>(itemIndex_);
      row.text.assign(text, textLen);
      rowsOut_->push_back(std::move(row));
    }
    itemIndex_++;
  }
  void onListEnd(uint16_t) override {
    listIndex_++;
    inTarget_ = false;
  }

 private:
  int targetListIndex_;
  uint32_t windowStart_;
  uint32_t windowEnd_;
  std::string* titleOut_;
  std::vector<CompanionListRow>* rowsOut_;
  const companiontodo::Diff* diff_;
  uint16_t listIndex_ = 0;
  bool inTarget_ = false;
  uint32_t itemIndex_ = 0;
  std::string groupLabel_;
  bool groupHeaderEmitted_ = false;
};

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
  listVisibleRows.clear();
  listNav.reset();
  listDocTitle.clear();
  listPeerKey.clear();
  listDocLoaded = false;
  freeListDocBuf();  // defensive; onExit() already frees this on the normal path
  clearUiDeclaration();
  cachedFontId = kCompanionFontId;
  computeViewport();

  companionble::setContentFieldCallback(onContentField);
  companionble::setStatusCallback(onStatus);
  companionble::setPairingRequestCallback(onPairingRequest);
  companionble::setForegroundChangeCallback(onForegroundChange);
  companionble::setImageStagedCallback(onImageStaged);
  companionble::setListDocStoredCallback(onListDocStored);
#ifdef COMPANION_TEST_CONSOLE
  // The console reports the screen without knowing what a screen is.
  g_screenNameActivity = this;
  companiontest::setScreenNameProvider(
      []() -> const char* { return g_screenNameActivity ? g_screenNameActivity->screenName() : "none"; });
  companiontest::setTagStateProvider([](companiontest::TagReport* out, uint8_t maxTags) -> uint8_t {
    return g_screenNameActivity ? g_screenNameActivity->reportTags(out, maxTags) : 0;
  });
  companiontest::setListNavStateProvider([](companiontest::ListNavReport* out) -> bool {
    return g_screenNameActivity ? g_screenNameActivity->reportListNav(out) : false;
  });
#endif

  if (offlineBrowse) {
    // Offline browse resume: BLE is either started at boot or never started
    // that boot (see enterOfflineBrowseMode()/handlePickerInput()'s comments)
    // -- deliberately skip ensureStarted() here rather than call and
    // immediately stop it. Screen::StartFailed means "BLE never came up"
    // (heap floor or NimBLE init failure), which is a different condition
    // from "we chose not to start it" and is a dead end (loop() no-ops on
    // it), so this must not route there.
    //
    // Populate pickerPeerKeys via the shared helper rather than calling
    // enterOfflineBrowseMode() -- that function only populates the vector to
    // decide whether the picker would be empty before committing to a
    // reboot; it never actually shows the picker itself (see its comment).
    // This is the call that populates it for real, for the screen that is
    // actually about to render.
    buildPickerPeerKeys();

    // Try to resume straight into whatever peer/screen/cursor was saved
    // (CrossPointState::companionOfflineBrowsePosition, kept live by
    // syncOfflineBrowsePosition() as the previous boot navigated) before
    // falling back to the plain picker. A copy, not a reference: the
    // fallback path below may overwrite APP_STATE.companionOfflineBrowsePosition
    // itself, and reading through a reference to the thing just cleared
    // would silently pick up the cleared value instead of what was saved.
    const CrossPointState::OfflineBrowsePosition savedPos = APP_STATE.companionOfflineBrowsePosition;
    const bool hadSavedPeer = !savedPos.peerKey.empty();
    // Reuses buildPickerPeerKeys()'s own eligibility filter (image-capable OR
    // declared LIST shape, still enrolled) rather than re-deriving it --
    // that is what "peer no longer enrolled, or its document/gallery is
    // gone" resolves to for a peer that dropped out of this same list.
    const bool peerStillEligible = hadSavedPeer && std::find(pickerPeerKeys.begin(), pickerPeerKeys.end(),
                                                             savedPos.peerKey) != pickerPeerKeys.end();

    bool restoredIntoContent = false;
    if (peerStillEligible && savedPos.screen == CrossPointState::OfflineBrowseScreen::List) {
      const ListNavPosition navPos{savedPos.listIndex, savedPos.listCursor, savedPos.listWindowStart};
      if (enterListDocument(savedPos.peerKey, Screen::GalleryPicker, &navPos)) {
        restoredIntoContent = true;
      } else {
        // Still enrolled and eligible, but the document itself is gone or no
        // longer parses (enterListDocument() otherwise leaves screen == List
        // showing its own "nothing yet" placeholder, which is not the same
        // as actually falling back to the picker) -- undo that and fall
        // through to the picker below.
        freeListDocBuf();
        listPeerKey.clear();
        // enterListDocument() already loaded savedPos.peerKey's map into
        // `buttons` (via loadButtonRoutingForPeer()) before the document read
        // failed -- clear it now that this is falling back to the picker
        // rather than staying on Screen::List, same as leaveListScreen()
        // does for the equivalent live edge (an offline picker return must
        // never leave a browsed peer's map to be misread by whatever the
        // picker shows next -- see `buttons`' doc comment).
        clearUiDeclaration();
        screen = Screen::GalleryPicker;
      }
    } else if (peerStillEligible && savedPos.screen == CrossPointState::OfflineBrowseScreen::Image) {
      loadGalleryForPeer(savedPos.peerKey);
      if (!galleryImages.empty()) {
        // Same fix as selectGalleryPickerPeer()'s image branch: `buttons` was
        // reset to all-None by clearUiDeclaration() earlier in onEnter(), so
        // without this, savedPos.peerKey's declared LocalGalleryPrev/Next/
        // LocalBack routing would be silently dead for the whole resumed
        // session -- see loadButtonRoutingForPeer()'s doc comment.
        loadButtonRoutingForPeer(savedPos.peerKey);
        browsingPeerKey = savedPos.peerKey;
        galleryPickerBrowsing = true;
        // loadGalleryForPeer() already points at the most recent image;
        // override with the saved one only if still in range -- the peer may
        // have fewer images now than when this was saved.
        galleryIndex = savedPos.galleryIndex < galleryImages.size() ? savedPos.galleryIndex : galleryImages.size() - 1;
        displayedImagePath = galleryImages[galleryIndex];
        screen = Screen::Image;
        restoredIntoContent = true;
      }
      // Else: the gallery is gone (every image removed since) -- fall
      // through to the picker below.
    }
    // Else: no peer was saved, or the saved peer is no longer enrolled or no
    // longer eligible -- falls straight through to the picker below.

    if (!restoredIntoContent) {
      // Whatever was saved (if anything) did not pan out. Clear it -- but
      // only actually spend the SD write on it if there was something to
      // clear, so a boot with nothing saved (the common case: still at the
      // picker, or offline browse just started) costs no more than before
      // this feature.
      if (hadSavedPeer) APP_STATE.companionOfflineBrowsePosition = CrossPointState::OfflineBrowsePosition{};
      if (pickerPeerKeys.empty()) {
        // The picker itself would be empty too: this is the loop guard
        // against rebooting straight back into this same dead end. Clear
        // the flag and fall back to the normal idle screen instead of
        // showing an empty picker or rebooting again.
        APP_STATE.companionOfflineBrowse = false;
        APP_STATE.saveToFile();
        chooseIdleScreen();
      } else {
        pickerCursor = 0;
        screen = Screen::GalleryPicker;
        if (hadSavedPeer) APP_STATE.saveToFile();
      }
    }

    idleSinceMs = millis();
    renderer.clearScreen();
    if (screen == Screen::GalleryPicker) {
      renderGalleryPicker();
    } else if (screen == Screen::List) {
      renderList();
    } else if (screen == Screen::Image) {
      renderImage();
    } else if (screen == Screen::IconGrid) {
      renderIconGrid();
    } else {
      renderWaiting();
    }
    return;
  }

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
  companionble::setListDocStoredCallback(nullptr);
#ifdef COMPANION_TEST_CONSOLE
  companiontest::setScreenNameProvider(nullptr);
  companiontest::setTagStateProvider(nullptr);
  g_screenNameActivity = nullptr;
#endif
  companionble::stop();
  freeListDocBuf();  // leaving the activity entirely -- see the header's audit note

  portENTER_CRITICAL(&g_mux);
  g_pendingStatusReady = false;
  g_pendingForegroundReady = false;
  g_pendingPairingReady = false;
  g_pendingImageReady = false;
  g_pendingListDocReady = false;
  g_batchModel.reset();
  portEXIT_CRITICAL(&g_mux);
}

// ---------------------------------------------------------------------------
// Button map
// ---------------------------------------------------------------------------

void CompanionModeActivity::clearUiDeclaration() {
  for (auto& spec : buttons) {
    spec.routing = companionble::ButtonRouting::None;
    spec.flags = 0;
    spec.label.clear();
  }
  // A just-cleared map is trivially unbound -- no need to route this through
  // recomputeButtonsAnyBound()/companionbuttons::anyBound() when the answer
  // is already known.
  buttonsAnyBound = false;
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

// Parses the button-entry section of a UI declaration body -- `offset` is
// already past the 2-byte header and is left just past the last button entry,
// so a caller that also wants the trailing tag section (loadUiDeclaration())
// can continue from there. `out` is not cleared first; callers do that (see
// clearUiDeclaration() and loadButtonRoutingForPeer()) so this stays a pure
// parse.
void CompanionModeActivity::parseButtonEntries(const uint8_t* raw, size_t len, size_t& offset, uint8_t buttonEntries,
                                               ButtonSpec (&out)[kButtonCount]) {
  for (uint8_t i = 0; i < buttonEntries && offset + 3 <= len; ++i) {
    // Byte 0 packs the (hardware-limited) button id into its low nibble and
    // behaviour flags into its high nibble -- see CompanionBle.h's
    // kButtonIdMask/kButtonFlagAlsoNotify/kButtonFlagLocalOnlyOffline. Every
    // declaration written before the flags existed has the high nibble
    // clear, so this parses byte-identically for them.
    const uint8_t buttonId = raw[offset] & companionble::kButtonIdMask;
    const uint8_t flags = static_cast<uint8_t>(raw[offset] & ~companionble::kButtonIdMask);
    const uint8_t routing = raw[offset + 1];
    const uint8_t labelLen = raw[offset + 2];
    offset += 3;
    if (offset + labelLen > len) break;

    // POWER is firmware-owned in every app: a wedged app must never be able to
    // make the device un-sleepable.
    if (buttonId < kButtonCount && buttonId != static_cast<uint8_t>(companionble::ButtonId::Power) &&
        routing <= companionble::kMaxButtonRouting) {
      out[buttonId].routing = static_cast<companionble::ButtonRouting>(routing);
      out[buttonId].flags = flags;
      out[buttonId].label.assign(reinterpret_cast<const char*>(raw + offset), labelLen);
    }
    offset += labelLen;
  }
}

// Reads `peerKey`'s persisted UI declaration and fills `buttons` with its
// button map, for Screen::List or a browsed Screen::Image gallery -- not
// list-specific despite the name this replaced (loadListButtonRouting()); the
// two screens share the exact same declared-map/no-default/clear-on-the-way-
// out model. No default: a peer that never declared a LocalList*/
// LocalGallery*/LocalBack routing for a button leaves it in `None`, same as
// clearUiDeclaration() leaves every other peer's map before a declaration is
// read. Deliberately ignores the tag section -- neither screen has a tag row.
//
// Does NOT itself clear `buttons` on the way OUT (a caller leaving the
// screen for the offline picker must do that once it knows where it's going
// -- see `buttons`' doc comment and leaveListScreen()); it only resets the
// map fresh on the way IN, same as loadUiDeclaration() does before it
// parses.
void CompanionModeActivity::loadButtonRoutingForPeer(const std::string& peerKey) {
  for (auto& spec : buttons) {
    spec.routing = companionble::ButtonRouting::None;
    spec.flags = 0;
    spec.label.clear();
  }
  buttonsAnyBound = false;
  if (peerKey.empty()) return;

  uint8_t raw[companionpeer::kMaxUiDeclarationLen];
  const size_t len =
      companionpeer::readAssetBody(peerKey.c_str(), companionpeer::kAssetUiDeclaration, raw, sizeof(raw));
  companionui::DeclarationInfo info;
  if (companionui::parseBody(raw, len, &info) != companionui::ParseResult::Ok) return;

  size_t offset = companionui::kBodyFirstButtonOffset;
  parseButtonEntries(raw, len, offset, info.buttonCount, buttons);
  recomputeButtonsAnyBound();
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
  // Validate through the shared codec rather than re-deriving the layout here.
  // storeAsset() refuses anything that does not parse, so a stored declaration
  // that fails now means a corrupt card, not an old client -- leaving the
  // cleared state (no buttons, no tags) is the safe answer either way.
  companionui::DeclarationInfo info;
  if (companionui::parseBody(raw, len, &info) != companionui::ParseResult::Ok) return;

  size_t offset = companionui::kBodyFirstButtonOffset;
  parseButtonEntries(raw, len, offset, info.buttonCount, buttons);
  recomputeButtonsAnyBound();

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

uint8_t CompanionModeActivity::flagsFor(companionble::ButtonId button) const {
  const size_t index = static_cast<size_t>(button);
  return index < kButtonCount ? buttons[index].flags : 0;
}

const char* CompanionModeActivity::labelFor(companionble::ButtonId button) const {
  const size_t index = static_cast<size_t>(button);
  if (index >= kButtonCount) return "";
  // An empty label hides the hint entirely — the convention drawButtonHints()
  // itself checks. Goes through companionbuttons::decide() rather than just
  // testing routing != None so a Remote-routed button's hint disappears once
  // the peer disconnects, not only when the routing itself is None (and now,
  // for a LOCAL_ONLY_OFFLINE button, once a peer connects).
  const bool peerConnected = !foregroundPeerKey.empty();
  return companionbuttons::decide(buttons[index].flags, buttons[index].routing, peerConnected).showHint
             ? buttons[index].label.c_str()
             : "";
}

// Recomputes buttonsAnyBound (see its doc comment) from the current
// `buttons` contents. Delegates to companionbuttons::anyBound()
// (CompanionButtonPolicy.h) so the "did this peer bind anything" predicate
// stays host-tested (test/companion_button_policy/) rather than living only
// here, where CompanionModeActivity.cpp's hardware/BLE/HAL coupling puts it
// out of host-test reach.
void CompanionModeActivity::recomputeButtonsAnyBound() {
  companionble::ButtonRouting routings[kButtonCount];
  for (size_t i = 0; i < kButtonCount; ++i) routings[i] = buttons[i].routing;
  buttonsAnyBound = companionbuttons::anyBound(routings, kButtonCount);
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

  // ToDo List layout: one line for the current list's own title
  // (cachedTitleFontId, bold) plus a gap, then as many rows as fit at
  // kCompanionFontId. Recomputed here alongside linesPerPage above, for the
  // same triggers (screen rotation, touch/no-touch margin change). This is
  // an approximate row *capacity*, not an exact one -- a group header takes
  // a row too, and listNav only ever counts *items* (see CompanionTodoNav.h),
  // so a window that includes headers can render a row or two more than
  // this many; renderList() just stops drawing once it runs out of vertical
  // space rather than treating this as a hard cap.
  constexpr int kListTitleBottomSpacing = 6;
  listRowHeight = renderer.getLineHeight(cachedFontId);
  const int listViewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom -
                                 renderer.getLineHeight(cachedTitleFontId) - kListTitleBottomSpacing;
  const int listRows = listRowHeight > 0 ? listViewportHeight / listRowHeight : 1;
  listVisibleCapacity = static_cast<uint8_t>(std::max(1, listRows));
  listNav.setVisibleCapacity(listVisibleCapacity);
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
  // This path bypasses main.cpp's enterDeepSleep() entirely -- it calls
  // startDeepSleep() directly, unlike the power-button/general-timeout path,
  // which saves APP_STATE for us before ever reaching
  // Activity::customDeepSleep(). Without this, an offline-browse idle
  // timeout (companionOfflineBrowsePosition, kept current in RAM only by
  // syncOfflineBrowsePosition() -- see its doc comment) would power off with
  // the last-navigated position never reaching disk, silently losing exactly
  // the "sleep while browsing, wake up in the same place" case this exists
  // for. Unconditional, not offline-browse-only: harmless on any other idle
  // sleep, since nothing else in this activity mutates APP_STATE in RAM
  // without also going through saveToFile() itself.
  APP_STATE.saveToFile();
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
    // the handover, that clears it. Screen::List joins Image here for the
    // same reason: a LIST peer's document is exactly as much "content" as a
    // pushed photo, and a dropped link mid-shopping-trip must not blank it.
    idleSinceMs = millis();
    if (!haveContent && screen != Screen::Image && screen != Screen::List) chooseIdleScreen();
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
    listVisibleRows.clear();
    listNav.reset();
    listDocTitle.clear();
    listPeerKey.clear();
    listDocLoaded = false;
    freeListDocBuf();  // a different peer just took the screen -- see the header's audit note
    updateTitleLayout();

    // A LIST peer lands on Screen::List directly rather than the
    // unconditional Screen::Text below -- see docs/companion-declared-shape-
    // design.md §2-§3: its document may already be on SD from a previous
    // session, and unlike Text/Image, List holds a cursor/window that a
    // reactive "whatever pushed last" model can't safely arbitrate. This SD
    // read is fine here (once per foreground change, on the main loop task —
    // see readDeclaredShape()'s own doc comment); it must never happen on
    // the NimBLE host task or per push. A peer with no declaration, or one
    // that fails to parse, falls back to Text -- the pre-existing, safe
    // default -- exactly as every other reader of this SD state already
    // treats "unreadable" as "none".
    companionble::ContentShape shape = companionble::ContentShape::Text;
    if (companionpeer::readDeclaredShape(foregroundPeerKey.c_str(), &shape) &&
        shape == companionble::ContentShape::List) {
      enterListDocument(foregroundPeerKey, Screen::Text);
    } else {
      screen = Screen::Text;
    }
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
                                               const uint8_t* contentId, size_t contentIdLen, uint8_t pushId) {
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
    companionble::notifyRenderStatus(companionble::RenderResult::DecodeFailed, pushId);
    return;
  }
  ImageDimensions dims{};
  if (!decoder->getDimensions(path, dims)) {
    LOG_ERR("CMA", "staged image %s did not decode", path.c_str());
    companionble::notifyRenderStatus(companionble::RenderResult::DecodeFailed, pushId);
    return;
  }

  RenderLock lock;
  // Arm the one status this push is owed. Set under the lock, alongside the
  // state that makes render() take the Screen::Image branch, so the render
  // task can never observe one without the other. Unconditional: a previous
  // push that never got its answer (link dropped mid-settle) must not stop
  // this one from being answered — but it does get told it lost the screen
  // first, rather than being silently overwritten. renderAwaitingStatus only
  // ends up true when pushId is non-zero -- see renderAwaitingPushId's doc
  // comment -- so an image pushed with pushId 0 settles onto the panel with
  // no RENDER_STATUS traffic at all.
  supersedePendingRenderStatus();
  renderAwaitingStatus = pushId != 0;
  renderAwaitingPushId = pushId;
  displayedImagePath = path;
  foregroundPushedImageThisSession = true;
  galleryPickerBrowsing = false;  // a live push always wins over picker browsing
  browsingPeerKey.clear();
  refreshGalleryForForeground();
  // A live push always wins, even over a DIFFERENT peer's document being
  // locally browsed through the offline picker (screen == List here does not
  // imply peerKey == listPeerKey) -- see the header's audit note.
  if (screen == Screen::List) freeListDocBuf();
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
  // No-ops unless this is an offline-picker browse of galleryPickerBrowsing's
  // peer -- see syncOfflineBrowsePosition()'s doc comment; a live foreground
  // peer's own gallery (galleryPickerBrowsing false) never persists here.
  syncOfflineBrowsePosition();
  requestUpdate();
}

// Button::Up/Down gallery prev/next, active only in Screen::Image, dispatched
// through whichever peer's gallery is on screen -- `buttons`, loaded by
// loadButtonRoutingForPeer() (a live foreground peer's own gallery, or a
// browsed peer reached through the offline picker; see that function's doc
// comment) or by loadUiDeclaration() for the ordinary live foreground case.
// Same declared-not-fixed model as handleListNav(), through the same
// companionbuttons::decide() (flags/connection state included) -- but UNLIKE
// handleListNav(), Screen::Image does NOT claim every button it sees.
//
// A LIST peer's declared shape makes it safe for Screen::List to claim all
// six of its buttons unconditionally (docs/companion-todo-list-design.md §5):
// a LIST peer's map is known to be exclusively for its own list before the
// screen is ever reached. Screen::Image has no equivalent guarantee -- an
// image-capable peer's map is also its ordinary Text-screen map, so Up/Down
// left unbound (None) or routed to something else entirely (most importantly
// Remote, for an app like Snap2Ink using Up/Down as a camera shutter) must
// fall through to handleMappedButton() instead of being silently eaten here.
// companionbuttons::isGalleryNavAction() (CompanionButtonPolicy.h,
// host-tested) is the one-line rule this hinges on: only a press that
// resolves to LocalGalleryPrev/LocalGalleryNext is ours.
//
// This deliberately does NOT use Left/Right: on real hardware those are the
// bottom front buttons, and every app map seen so far (including scripts/
// push_companion_content.py's default) routes them to LOCAL_PAGE_PREV/NEXT
// for paging buffered text — i.e. what users call "the page-turn buttons".
// Up/Down are the side buttons, physically the pair toward the top of the
// device.
namespace {
struct GalleryNavButton {
  MappedInputManager::Button role;
  companionble::ButtonId id;
};
constexpr GalleryNavButton kGalleryNavButtons[] = {
    {MappedInputManager::Button::Up, companionble::ButtonId::Up},
    {MappedInputManager::Button::Down, companionble::ButtonId::Down},
};
}  // namespace

bool CompanionModeActivity::handleGalleryNav() {
  if (screen != Screen::Image || galleryImages.size() < 2) return false;

  // !foregroundPeerKey.empty(), not the `connected` member -- see
  // CompanionButtonPolicy.h's doc comment on why decide() takes this exact
  // test as its peerConnected argument.
  const bool peerConnected = !foregroundPeerKey.empty();

  for (const auto& nav : kGalleryNavButtons) {
    // Routing decided BEFORE buttonWasPressed() is even called, deliberately
    // -- buttonWasPressed() is not side-effect-free on both builds it runs
    // on. Real hardware: a const query over InputManager's once-per-loop
    // edge state (non-consuming, may be asked twice). Under
    // COMPANION_TEST_CONSOLE: companiontest::wasPressed() (CompanionTest
    // Console.cpp:151-161) sets pressDelivered and returns false on every
    // later call for the same injected press -- CONSUMING. If this loop
    // called buttonWasPressed() first the way the old isGalleryClaimable()
    // callers did NOT (it tested the routing first for exactly this
    // reason), an Up/Down this peer routed to Remote would already have
    // been consumed by the time isGalleryNavAction() said "not mine",
    // making handleMappedButton() never see it -- reintroducing trap #1
    // under the test console specifically, even though real hardware would
    // look fine (non-consuming query tolerates being asked twice). Resolve
    // first, consume only once known to be ours.
    const auto decision = companionbuttons::decide(flagsFor(nav.id), routingFor(nav.id), peerConnected);
    if (!companionbuttons::isGalleryNavAction(decision.action)) continue;  // not ours -- try the other button
    if (!buttonWasPressed(nav.role, nav.id)) continue;

    // ALSO_NOTIFY means a local action's press is *also* relayed to the app,
    // on top of the local move below -- see decide()'s doc comment. Safe to
    // fire only now that buttonWasPressed() has actually confirmed a press
    // -- decide() alone says nothing about whether this button was pressed
    // this loop.
    if (decision.notify) notifyHeldButton(nav.id);

    if (decision.action == companionble::ButtonRouting::LocalGalleryPrev) {
      showGalleryImage(galleryIndex == 0 ? galleryImages.size() - 1 : galleryIndex - 1);
    } else {
      showGalleryImage(galleryIndex + 1 >= galleryImages.size() ? 0 : galleryIndex + 1);
    }
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// ToDo List (Screen::List) -- docs/companion-todo-list-design.md §5.
// ---------------------------------------------------------------------------

// See the header's doc comment. Caller must already hold a RenderLock (or be
// somewhere render() cannot run concurrently, e.g. onEnter()) -- this only
// mutates state and sets `screen`, so it composes inside a caller's existing
// lock rather than nesting its own (RenderLock.h: the underlying semaphore
// is not recursive). applyForegroundChange() already holds one for its whole
// body; the picker entry point added in this feature's second commit takes
// its own around this call, mirroring selectGalleryPickerPeer().
bool CompanionModeActivity::enterListDocument(const std::string& peerKey, Screen returnTo,
                                              const ListNavPosition* restore) {
  listPeerKey = peerKey;
  listReturnScreen = returnTo;
  listNav.reset();
  // See the header's doc comment: this must land between reset() and the
  // reload pass below so setListCount()/setCurrentList() (inside
  // reloadListView()) clamp it against whatever the document actually has.
  if (restore) listNav.restorePosition(restore->listIndex, restore->cursor, restore->windowStart);
  loadButtonRoutingForPeer(peerKey);
  loadListDocBuf();
  reloadListView();
  screen = Screen::List;
  return listDocLoaded;
}

// See the header's doc comment. Always starts from a clean slate -- any
// buffer held for a previously-viewed peer (e.g. the picker moving from one
// LIST tile straight to another) is freed first, so this never leaks one
// buffer into another's lifetime.
void CompanionModeActivity::loadListDocBuf() {
  freeListDocBuf();
  if (listPeerKey.empty()) return;

  const size_t size = companionpeer::listDocumentSize(listPeerKey.c_str());
  if (size == 0) return;

  auto buf = makeUniqueNoThrow<uint8_t[]>(size);
  if (!buf) {
    LOG_ERR("CMA", "could not allocate %u bytes for peer %s's list document", static_cast<unsigned>(size),
            listPeerKey.c_str());
    return;
  }

  const size_t read = companionpeer::readListDocument(listPeerKey.c_str(), buf.get(), size);
  if (read != size) return;

  listDocBuf = std::move(buf);
  listDocBufLen = size;
  loadListDiff();
}

// See the header's doc comment. Every caller of loadListDocBuf() gets the
// diff refreshed for free, which is what makes the "a push landed under a live
// list screen" path correct: storeListDocument() has just deleted
// list_state.bin, and re-reading here is what stops the now-dead in-RAM diff
// from being re-applied to a document it was never taken against.
void CompanionModeActivity::loadListDiff() {
  listDiff.reset();

  // 1096 bytes (CompanionTodoDiff.h's MEMORY note), heap rather than a member
  // because it is only wanted while Screen::List is up; stack is out of the
  // question at this size on this part's task stacks.
  auto diff = makeUniqueNoThrow<companiontodo::Diff>();
  if (!diff) {
    LOG_ERR("CMA", "could not allocate %u bytes for peer %s's list check-off state; browsing read-only",
            static_cast<unsigned>(sizeof(companiontodo::Diff)), listPeerKey.c_str());
    return;
  }
  diff->clear();

  ListRevisionVisitor rev;
  const bool haveRevision =
      companiontodo::parseDocument(listDocBuf.get(), listDocBufLen, rev) == companiontodo::ParseResult::Ok;

  if (companionpeer::readListState(listPeerKey.c_str(), *diff) &&
      (!haveRevision || diff->revision() != rev.revision())) {
    // NOT a merge decision, and deliberately not one: this refuses to apply a
    // diff to a document it demonstrably was not taken against. The normal
    // path cannot produce a mismatch -- storeListDocument() clears the state
    // file whenever a document lands -- so reaching here means the two SD
    // writes disagree: a crash between them, a hand-edited card, or a
    // document that no longer parses so its revision cannot be confirmed at
    // all. Applying deviations keyed by itemId to an unknown document would
    // silently tick the wrong boxes, which is worse than losing edits the
    // phone can re-push.
    LOG_INF("CMA", "peer %s's list state is for revision %u, document is %u -- discarding", listPeerKey.c_str(),
            static_cast<unsigned>(diff->revision()), static_cast<unsigned>(rev.revision()));
    companionpeer::clearListState(listPeerKey.c_str());
    diff->clear();
  }

  diff->setRevision(rev.revision());
  listDiff = std::move(diff);
}

void CompanionModeActivity::freeListDocBuf() {
  listDocBuf.reset();
  listDocBufLen = 0;
  // Freed with the document, never separately: a diff without the document it
  // deviates from cannot be rendered or toggled against, and every caller of
  // this function is leaving Screen::List.
  listDiff.reset();
}

// See the header's doc comment on `recountTotals`. Three shapes of walk,
// cheapest first:
//   - listPeerKey empty or listDocBuf unset: nothing to walk at all.
//   - !recountTotals: exactly one companiontodo::parseDocument() pass over
//     listDocBuf, windowed against listNav's already-known counts (the
//     Up/Down case).
//   - recountTotals: one counting pass to learn listCount and (usually) the
//     current list's itemCount, PLUS a second counting pass in the one case
//     where the first pass targeted a list index that turned out to be
//     stale (the document shrank under a listIndex a caller hadn't yet
//     re-validated -- see ListCountingVisitor's doc comment), PLUS the
//     windowed pass. Each pass is a pure in-RAM walk of listDocBuf now (no
//     SD, no allocation) -- loadListDocBuf() is what pays the SD cost, once,
//     before this is ever called, so calling this on every nav press is
//     cheap by design; it was NOT always this cheap (see this feature's
//     commit message for the per-keypress SD+JSON cost it replaced).
void CompanionModeActivity::reloadListView(bool recountTotals) {
  listDocLoaded = false;
  listDocTitle.clear();
  listVisibleRows.clear();

  if (listPeerKey.empty() || !listDocBuf) {
    listNav.setListCount(0);
    return;
  }

  if (recountTotals) {
    const int originalTarget = static_cast<int>(listNav.listIndex());
    ListCountingVisitor counting(originalTarget);
    if (companiontodo::parseDocument(listDocBuf.get(), listDocBufLen, counting) != companiontodo::ParseResult::Ok ||
        counting.listCount() == 0) {
      listNav.setListCount(0);
      return;
    }
    listNav.setListCount(counting.listCount());  // may clamp listIndex()

    uint16_t itemCount = counting.targetItemCount();
    if (static_cast<int>(listNav.listIndex()) != originalTarget) {
      // The document has fewer lists than listIndex() assumed (e.g. the app
      // re-pushed a shorter document while this list was on screen) --
      // setListCount() just clamped it, so the count above targeted a list
      // that is no longer at that position. Re-count against the clamped
      // index rather than trust a number that describes the wrong list.
      ListCountingVisitor recount(static_cast<int>(listNav.listIndex()));
      companiontodo::parseDocument(listDocBuf.get(), listDocBufLen, recount);
      itemCount = recount.targetItemCount();
    }
    listNav.setCurrentList(itemCount);  // clamps cursor()/windowStart()
  }

  ListRowVisitor rows(static_cast<int>(listNav.listIndex()), listNav.windowStart(), listNav.visibleCapacity(),
                      &listDocTitle, &listVisibleRows, listDiff.get());
  if (companiontodo::parseDocument(listDocBuf.get(), listDocBufLen, rows) == companiontodo::ParseResult::Ok)
    listDocLoaded = true;
}

// Physical buttons handleListNav() is willing to claim on Screen::List. Power
// is firmware-owned everywhere (see parseButtonEntries()) and never reaches
// here.
//
// *Whether* a press among these six is claimed does not depend on flags --
// see the loop's own comment below for that. *What* the button then does is
// flag-dependent, exactly as it is for handleMappedButton(): a
// LOCAL_ONLY_OFFLINE LocalList* action goes through companionbuttons::
// decide() the same way, so it collapses to no-op while a peer holds the
// foreground link, and ALSO_NOTIFY relays the press to the app on top of
// whatever it does locally.
namespace {
struct ListNavButton {
  MappedInputManager::Button role;
  companionble::ButtonId id;
};
constexpr ListNavButton kListNavButtons[] = {
    {MappedInputManager::Button::Back, companionble::ButtonId::Back},
    {MappedInputManager::Button::Confirm, companionble::ButtonId::Confirm},
    {MappedInputManager::Button::Up, companionble::ButtonId::Up},
    {MappedInputManager::Button::Down, companionble::ButtonId::Down},
    {MappedInputManager::Button::Left, companionble::ButtonId::Left},
    {MappedInputManager::Button::Right, companionble::ButtonId::Right},
};
}  // namespace

// See the header's doc comment. Caller must already hold a RenderLock, same
// precondition as enterListDocument().
void CompanionModeActivity::leaveListScreen() {
  screen = listReturnScreen;
  freeListDocBuf();  // leaving Screen::List -- see the header's audit note
  // Only the offline-picker return edge needs `buttons` cleared: a live
  // foreground LIST peer's Screen::Text return keeps its own map as-is (it
  // already IS that same peer's map -- loadUiDeclaration() loaded it just
  // before applyForegroundChange() called enterListDocument()). The picker
  // return, though, can hand the screen to a DIFFERENT peer's gallery next
  // (selectGalleryPickerPeer()'s non-LIST branch, which never itself loads a
  // button map) -- without this, that gallery's handleGalleryNav() would
  // read the just-left LIST peer's routingFor(Up)/routingFor(Down) instead
  // of the None an offline browse with no foreground peer should always see.
  if (listReturnScreen == Screen::GalleryPicker) clearUiDeclaration();
  syncOfflineBrowsePosition();
  requestUpdate();
}

bool CompanionModeActivity::handleListNav() {
  if (screen != Screen::List) return false;

  // Which physical button performs which List action is now the browsed
  // peer's own choice (`buttons`, loaded by enterListDocument() -- see its
  // doc comment), the same "declared, not fixed" model every other screen's
  // ButtonRouting already uses (docs/companion-multi-app-design.md §7). There
  // is no default routing: a button this peer never bound to a LocalList*
  // routing does nothing when pressed. The press is still claimed (returns
  // true) regardless, though -- List's declared-shape exclusivity
  // (docs/companion-declared-shape-design.md §2-§3) means these six buttons
  // are never meant for some other peer's general map while this screen is
  // up, whether or not the browsed peer chose to bind them to anything, or
  // decide() below collapses its bound action to a no-op for this press.

  // !foregroundPeerKey.empty(), not the `connected` member -- see
  // CompanionButtonPolicy.h's doc comment on why decide() takes this exact
  // test as its peerConnected argument. Loop-invariant, so computed once
  // rather than per button.
  const bool peerConnected = !foregroundPeerKey.empty();

  for (const auto& nav : kListNavButtons) {
    if (!buttonWasPressed(nav.role, nav.id)) continue;

    const auto decision = companionbuttons::decide(flagsFor(nav.id), routingFor(nav.id), peerConnected);

    // ALSO_NOTIFY means a local action's press is *also* relayed to the app,
    // on top of whatever the switch below does locally -- see decide()'s doc
    // comment. Fired once here rather than duplicated into every local case.
    if (decision.notify) notifyHeldButton(nav.id);

    switch (decision.action) {
      case companionble::ButtonRouting::LocalBack: {
        RenderLock lock;
        leaveListScreen();
        return true;
      }
      case companionble::ButtonRouting::LocalListToggleCheck: {
        // listVisibleRows is already the window listNav.cursor() indexes into, and
        // already carries every item's id -- so the row under the cursor is a scan
        // of a handful of rows, not a second walk of the document.
        const CompanionListRow* target = nullptr;
        for (const auto& row : listVisibleRows) {
          if (!row.isHeader && row.itemFlatIndex == static_cast<int>(listNav.cursor())) {
            target = &row;
            break;
          }
        }
        // No diff (allocation failed -- see listDiff's doc comment) or nothing
        // selectable under the cursor (an empty list): the press is still consumed,
        // so it never leaks out to some other handler from this screen.
        if (!listDiff || target == nullptr) return true;

        // Captured only to decide whether to auto-advance below; the redraw
        // still rebuilds every row through effectiveChecked() rather than
        // trusting this copy.
        bool newChecked = false;
        if (!listDiff->applyToggle(target->itemId, target->documentChecked, &newChecked)) {
          // The table is full and applyToggle() mutated nothing. Saying so out loud
          // is the point: an edit the user made and the device dropped in silence
          // is the one failure mode this feature cannot have. Note this is reached
          // before any RenderLock is taken -- showTransientMessage() takes its own,
          // and the semaphore is not recursive (RenderLock.h).
          showTransientMessage(tr(STR_COMPANION_LIST_TOO_MANY_EDITS), Screen::List);
          return true;
        }
        // SD OUTSIDE THE RENDER LOCK, deliberately. The lock serialises this task
        // against the render task, and everything the render task reads for this
        // screen is listVisibleRows/listNav -- neither of which is touched until
        // the locked block below. listDiff is read only by ListRowVisitor, i.e.
        // only from inside reloadListView(), so mutating it and persisting it here
        // races nothing; holding the lock across an SD write would instead stall
        // the panel for the write's duration for no protection at all.
        if (!companionpeer::writeListState(listPeerKey.c_str(), *listDiff)) {
          // Undo rather than show a check the card did not keep. applyToggle() is
          // its own inverse, and the second call cannot fail: it either removes
          // the entry the first one added, or re-inserts into a slot just vacated.
          LOG_ERR("CMA", "could not persist peer %s's list state; reverting the toggle", listPeerKey.c_str());
          listDiff->applyToggle(target->itemId, target->documentChecked, nullptr);
          return true;
        }
        // Announce only after the write succeeded, so the phone is never told to
        // pull a state the card does not hold. A peer that is connected while the
        // user works through the list learns each edit live instead of only at its
        // next HELLO.
        companionble::notifyListStateAvail(listPeerKey.c_str());

        // Checking an item off moves the cursor to the next one, so working
        // down a list is a single button held rather than a check-then-move
        // pair; there's no move on an uncheck (no use case for it yet -- YAGNI)
        // and none at the last item, where there's nothing to advance to.
        const bool advanced = newChecked && listNav.moveDown();

        RenderLock lock;
        // Full re-walk rather than poking the one row: the row vector is rebuilt
        // from the document on every other view change too, and one in-RAM parse
        // is cheaper than a second code path that has to stay consistent with it.
        reloadListView(/*recountTotals=*/false);
        if (advanced) syncOfflineBrowsePosition();
        requestUpdate();
        return true;
      }
      case companionble::ButtonRouting::LocalListMoveUp:
        if (listNav.moveUp()) {
          RenderLock lock;
          reloadListView(/*recountTotals=*/false);
          syncOfflineBrowsePosition();
          requestUpdate();
        }
        return true;
      case companionble::ButtonRouting::LocalListMoveDown:
        if (listNav.moveDown()) {
          RenderLock lock;
          reloadListView(/*recountTotals=*/false);
          syncOfflineBrowsePosition();
          requestUpdate();
        }
        return true;
      case companionble::ButtonRouting::LocalListSwitchLeft:
        if (listNav.switchListLeft()) {
          RenderLock lock;
          reloadListView();
          syncOfflineBrowsePosition();
          requestUpdate();
        }
        return true;
      case companionble::ButtonRouting::LocalListSwitchRight:
        if (listNav.switchListRight()) {
          RenderLock lock;
          reloadListView();
          syncOfflineBrowsePosition();
          requestUpdate();
        }
        return true;
      default:
        // Not bound to a List action by this peer, or a LOCAL_ONLY_OFFLINE
        // LocalList* action decide() just collapsed to None because a peer
        // is connected (decision.notify, handled above, already relayed the
        // press in that case) -- claimed, no-op either way, with one
        // exception: firmware supplies its own Back-leaves-the-screen
        // fallback, but ONLY when the peer bound NOTHING at all
        // (!buttonsAnyBound -- the whole map, not just Back; see that
        // member's doc comment). With bindings fully app-declared and no
        // defaults, a LIST peer that binds no Back leaves this screen with
        // no way out but the power button, and an offline browse has no BLE
        // for a phone to push a corrected map. A peer that bound anything
        // at all is taken at face value, absence of Back included -- that
        // is a deliberate app choice, not an omission to paper over. See
        // the comment above the loop for why every other case still
        // returns true rather than falling through to handleMappedButton().
        if (nav.id == companionble::ButtonId::Back && !buttonsAnyBound) {
          RenderLock lock;
          leaveListScreen();
        }
        return true;
    }
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

// Populates pickerPeerKeys from every enrolled peer (companionpeer::listPeers()
// — ungrouped, unlike the decorative grid's listIconTiles(), since two
// installs of the same app must stay two separate tiles here) that declared
// either the image-gallery capability or the LIST content shape, capped at
// kMaxIconTiles the same as the decorative grid's tile budget. Callers decide
// what an empty result means -- shared by enterOfflineBrowseMode() (which
// only needs to know whether the picker would be empty before committing to
// a reboot) and onEnter()'s offline-resume branch (which needs the real
// list to render).
//
// One shared picker for both image galleries and ToDo lists, not two separate
// entry points: this is the same "local SD browse of content the app already
// pushed" argument for both (docs/companion-multi-app-design.md §8's
// gallery-picker-is-not-a-launcher reasoning, and
// docs/companion-todo-list-design.md §4's identical argument for a LIST
// peer's document) — the tile grid is one idle-screen affordance for "browse
// what an app already gave the device," and which peers qualify is an ORed
// capability/shape check, not a second UI. selectGalleryPickerPeer() is what
// actually branches on shape once a tile is chosen.
void CompanionModeActivity::buildPickerPeerKeys() {
  char keys[companionpeer::kMaxPeers][companionpeer::kPeerKeyLen];
  const size_t total = companionpeer::listPeers(keys, companionpeer::kMaxPeers);

  pickerPeerKeys.clear();
  for (size_t i = 0; i < total && pickerPeerKeys.size() < companionpeer::kMaxIconTiles; ++i) {
    companionble::ContentShape shape = companionble::ContentShape::Text;
    const bool isListPeer =
        companionpeer::readDeclaredShape(keys[i], &shape) && shape == companionble::ContentShape::List;
    if (companionpeer::isImageCapable(keys[i]) || isListPeer) pickerPeerKeys.emplace_back(keys[i]);
  }
}

// See the header's doc comment. Written into APP_STATE directly (rather than
// a local member CompanionModeActivity later copies over) so it is already
// correct by the time either deep-sleep path saves it, whichever order that
// happens to run in relative to any per-screen bookkeeping -- see this
// feature's commit message for why that ordering would otherwise matter.
void CompanionModeActivity::syncOfflineBrowsePosition() {
  if (!offlineBrowse) return;
  auto& pos = APP_STATE.companionOfflineBrowsePosition;
  if (screen == Screen::List) {
    pos.peerKey = listPeerKey;
    pos.screen = CrossPointState::OfflineBrowseScreen::List;
    pos.listIndex = listNav.listIndex();
    pos.listCursor = listNav.cursor();
    pos.listWindowStart = listNav.windowStart();
  } else if (screen == Screen::Image && galleryPickerBrowsing) {
    pos.peerKey = browsingPeerKey;
    pos.screen = CrossPointState::OfflineBrowseScreen::Image;
    pos.galleryIndex = static_cast<uint16_t>(galleryIndex);
  } else if (screen == Screen::GalleryPicker) {
    // Back at the picker with nothing chosen -- the resume default, so a
    // stale peer/screen from earlier this same browse must not survive.
    pos = CrossPointState::OfflineBrowsePosition{};
  }
  // Any other screen (IconGrid, Waiting, ...) is not a resumable
  // offline-browse position -- e.g. the picker-would-be-empty fallback in
  // onEnter() lands on IconGrid/Waiting with offlineBrowse still true for
  // the rest of this boot even though it already cleared and persisted the
  // flag itself. Nothing calls this function from those screens, but left
  // unhandled here too rather than asserted against, since landing there is
  // not itself a bug.
}

// Confirm on the idle icon grid. Despite the name this does not itself show
// the picker -- it only calls buildPickerPeerKeys() to learn whether the
// picker WOULD be non-empty, then reboots into offline-browse mode (BLE off)
// to actually open it; onEnter()'s offline-resume branch is what populates
// pickerPeerKeys for real and renders Screen::GalleryPicker after the reset.
// Falls back to a transient message, staying on IconGrid with BLE still up,
// rather than rebooting into an empty picker.
void CompanionModeActivity::enterOfflineBrowseMode() {
  buildPickerPeerKeys();

  if (pickerPeerKeys.empty()) {
    showTransientMessage(tr(STR_COMPANION_PICKER_EMPTY), Screen::IconGrid);
    return;
  }

  // The picker would genuinely open: this is the IconGrid->GalleryPicker
  // offline-browse edge. BLE stays advertising on the icon grid (that's how a
  // phone reaches an idle device) but a picker browse is a deliberate offline
  // act with no need for the radio, and there is no runtime NimBLE
  // teardown/restart in this firmware -- ensureStarted() must still only ever
  // run once per boot. So: persist the mode and reboot into it rather than
  // just switching `screen`. Explicitly stop() BLE first -- onExit() will NOT
  // run (a reset is not an activity transition), and we want any connected
  // central dropped cleanly here rather than left to time out.
  APP_STATE.companionOfflineBrowse = true;
  APP_STATE.saveToFile();
  companionble::stop();
  silentRestart();
}

// Confirm on the picker grid: for a LIST peer, open its stored ToDo List
// document (the offline entry point docs/companion-todo-list-design.md §4
// and docs/companion-declared-shape-design.md §3 describe -- entirely a
// local SD read, no BLE involved regardless of whether that peer is
// currently connected, exactly like the gallery case below). Otherwise, load
// the highlighted peer's stored gallery, or say there's nothing to show yet.
void CompanionModeActivity::selectGalleryPickerPeer() {
  if (pickerCursor >= pickerPeerKeys.size()) return;
  const std::string peerKey = pickerPeerKeys[pickerCursor];

  companionble::ContentShape shape = companionble::ContentShape::Text;
  if (companionpeer::readDeclaredShape(peerKey.c_str(), &shape) && shape == companionble::ContentShape::List) {
    RenderLock lock;
    // Back returns to this picker, not straight to the icon grid -- the user
    // came from here and most likely wants to look at another peer's tile
    // next, same as leaving a gallery reached through the picker does below.
    enterListDocument(peerKey, Screen::GalleryPicker);
    syncOfflineBrowsePosition();
    requestUpdate();
    return;
  }

  loadGalleryForPeer(peerKey);
  if (galleryImages.empty()) {
    showTransientMessage(tr(STR_COMPANION_GALLERY_EMPTY), Screen::GalleryPicker);
    return;
  }
  // Fills `buttons` with this peer's declared map -- `buttons` is cleared to
  // all-None in offline mode (clearUiDeclaration(), no live foreground peer
  // to have loaded it), so without this call handleGalleryNav()'s
  // routingFor(Up)/routingFor(Down) and the Back handling below would always
  // read None for a peer entered straight from the picker, leaving its
  // declared LocalGalleryPrev/Next/LocalBack routing permanently dead. See
  // loadButtonRoutingForPeer()'s doc comment; mirrors the LIST branch above,
  // which gets this for free through enterListDocument().
  loadButtonRoutingForPeer(peerKey);

  RenderLock lock;
  browsingPeerKey = peerKey;
  galleryPickerBrowsing = true;
  screen = Screen::Image;
  syncOfflineBrowsePosition();
  requestUpdate();
}

// Input for the picker itself and for leaving a gallery reached through it.
// Tried before the foreground-button dispatch in loop(), same as
// handleGalleryNav(). IconGrid/GalleryPicker navigation (Up/Down/Left/Right/Confirm)
// is firmware-owned with no button map involved at all -- those are picker
// chrome, not a peer's own screen. The one exception is the browsed gallery's
// Back below, which -- like Up/Down inside it (handleGalleryNav()) -- now
// consults `buttons`, the browsed peer's own declared map (most peers in the
// picker have no live session at all, hence loadButtonRoutingForPeer()
// rather than a live foreground load).
bool CompanionModeActivity::handlePickerInput() {
  if (screen == Screen::IconGrid) {
    if (buttonWasPressed(MappedInputManager::Button::Confirm, companionble::ButtonId::Confirm)) {
      enterOfflineBrowseMode();
      return true;
    }
    return false;
  }

  if (screen == Screen::GalleryPicker) {
    // defensive; onEnter()'s offline-resume branch is what guarantees this
    // non-empty (its own empty case clears the flag and falls back to the
    // idle screen instead of ever landing here)
    if (pickerPeerKeys.empty()) return false;
    // Side Up/Down and the front-labeled </> (Left/Right) are both accepted here --
    // the button-hint row (see mapLabels() near the GalleryPicker render path) prints
    // </> on the user-configured front Left/Right buttons, so those must actually
    // move the cursor, not just the unlabeled side buttons.
    if (buttonWasPressed(MappedInputManager::Button::Up, companionble::ButtonId::Up) ||
        buttonWasPressed(MappedInputManager::Button::Left, companionble::ButtonId::Left)) {
      RenderLock lock;
      pickerCursor = pickerCursor == 0 ? pickerPeerKeys.size() - 1 : pickerCursor - 1;
      requestUpdate();
      return true;
    }
    if (buttonWasPressed(MappedInputManager::Button::Down, companionble::ButtonId::Down) ||
        buttonWasPressed(MappedInputManager::Button::Right, companionble::ButtonId::Right)) {
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
      // Leaving the offline-browse mode: the GalleryPicker->IconGrid edge.
      // enterOfflineBrowseMode() reboots into this screen rather than
      // switching to it live (see its comment), so reaching Screen::GalleryPicker
      // at all means this boot is an offline-browse resume and BLE was never
      // started -- companionble::stop() has nothing to do, asserted here
      // rather than called. Persist the cleared flag and reboot back to the
      // icon grid, which is what re-arms ensureStarted() on the next boot.
      APP_STATE.companionOfflineBrowse = false;
      // The browse is over, not just paused -- clear the saved resume
      // position too, rather than leave a stale peer/screen behind for a
      // future browse that happens to start from a boot where onEnter()
      // never re-populates this (defensive: the position writes above
      // already keep this at Picker/empty by the time we can reach here,
      // but "leaving offline browse" should not depend on that).
      APP_STATE.companionOfflineBrowsePosition = CrossPointState::OfflineBrowsePosition{};
      APP_STATE.saveToFile();
      silentRestart();
      return true;
    }
    return false;
  }

  if (screen == Screen::Image && galleryPickerBrowsing) {
    if (buttonWasPressed(MappedInputManager::Button::Back, companionble::ButtonId::Back)) {
      // Firmware-owned, unconditional -- deliberately NOT routed through
      // `buttons`/decide(), unlike handleListNav()'s LocalBack case.
      // buttonsAnyBound answers "did this peer declare a scheme at all", not
      // "did this peer give me a way out of THIS screen", and those differ
      // precisely because Screen::Image is not shape-exclusive the way
      // Screen::List is -- the same asymmetry that makes handleGalleryNav()
      // above refuse to claim-all. galleryPickerBrowsing implies no live
      // peer here (foregroundPeerKey is always empty by the time this runs
      // -- see the disconnect-fallback branch in loop() and the
      // offline-browse-only entry points below), so consulting the map
      // could only ever subtract the way out: LocalPagePrev/Next and
      // LocalList* are documented no-ops off their own screens, Remote
      // collapses to decide()'s None when disconnected (its notify is
      // always false offline, so it was never reachable here either), and
      // this branch never dispatched LocalSleep even when bound. A peer
      // that binds Back to LocalSleep leaves the gallery here rather than
      // sleeping -- true before this screen had declared bindings at all,
      // and strictly better than trapping the user with no way out but the
      // power button.
      RenderLock lock;
      screen = Screen::GalleryPicker;
      galleryPickerBrowsing = false;
      syncOfflineBrowsePosition();
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

  // Ensure the connection profile (interval/peripheral-latency) matches
  // current session state; see CompanionBle.h's tick() doc comment.
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
        // This can take the screen away from a document being locally
        // browsed through the offline picker (screen == List here does not
        // imply the disconnecting peer is listPeerKey) -- see the header's
        // audit note.
        if (screen == Screen::List) freeListDocBuf();
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
      // See CompanionBatchModel::resetOnDisconnect()'s doc comment: a
      // standalone pending tag push (not bound to the batch the link just
      // killed) is deliberately left alone.
      g_batchModel.resetOnDisconnect();
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
  bool gotListDoc = false;
  std::string newTitle;
  std::string newBody;
  uint8_t newTagId = 0;
  uint8_t newTagStateValue = 0;
  bool gotTagState = false;
  bool tagStateWasInBatch = false;
  uint8_t newTagStateBuf[1 + 2 * companionble::kMaxTags] = {0};
  uint8_t newTagStateLen = 0;
  char newForegroundKey[companionpeer::kPeerKeyLen] = {0};
  char newForegroundName[companionpeer::kMaxNameLen + 1] = {0};
  char newPairingName[companionpeer::kMaxNameLen + 1] = {0};
  char newImagePath[sizeof(g_pendingImagePath)] = {0};
  char newImagePeerKey[sizeof(g_pendingImagePeerKey)] = {0};
  uint8_t newImageContentId[sizeof(g_pendingImageContentId)] = {0};
  uint8_t newImageContentIdLen = 0;
  uint8_t newImagePushId = 0;
  char newListDocPeerKey[sizeof(g_pendingListDocPeerKey)] = {0};

  portENTER_CRITICAL(&g_mux);
  const CompanionBatchModel::PollResult batchResult = g_batchModel.poll(millis());
  bool poisoned = false;
  // Only meaningful when commit ends up true, and even then only when the
  // final-flagged field's END actually arrived (the normal case) rather than
  // the kTimeoutMs safety net inside CompanionBatchModel::poll(), in which
  // case batchResult.pushId is still 0 (its cleared-at-rest value) because no
  // field of this batch was ever final-flagged before the timeout fired --
  // there is no client-chosen id to answer with, so 0 ("no answer wanted") is
  // the honest value here, not a bug.
  uint8_t batchPushId = 0;
  if (batchResult.resolution != CompanionBatchModel::Resolution::None) {
    commit = true;
    if (batchResult.timedOut) {
      // Safety net: the final-flagged field's END never arrived in time (e.g. the
      // app crashed or lost the connection mid-push). Apply whatever we have
      // rather than leaving the screen stuck on stale content indefinitely.
      // A poisoned batch resolves here too — it still has to be *cleared*, or the
      // poison would leak into the next batch, it is just discarded rather than
      // applied.
      LOG_ERR("CMA", "content batch commit flag missed after %lu ms, applying pending fields anyway",
              static_cast<unsigned long>(CompanionBatchModel::kTimeoutMs));
    }
    // How long the batch sat between its first field landing and committing.
    // The timedOut branch above only shouts when it fires at kTimeoutMs; this
    // catches the sub-threshold waits, which are invisible today and are the
    // part of push-to-visible time nothing else accounts for.
    if (batchResult.elapsedValid) {
      LOG_DBG("CMA", "content batch committed %lu ms after its first field",
              static_cast<unsigned long>(batchResult.elapsedSinceFirstFieldMs));
    }
    poisoned = batchResult.resolution == CompanionBatchModel::Resolution::Discard;
    batchPushId = batchResult.pushId;
    gotTitle = batchResult.hasTitle;
    gotBody = batchResult.hasBody;
    if (gotTitle) newTitle.assign(reinterpret_cast<const char*>(g_batchModel.titleData()), g_batchModel.titleLen());
    if (gotBody) newBody.assign(reinterpret_cast<const char*>(g_batchModel.bodyData()), g_batchModel.bodyLen());
  }
  if (batchResult.hasTagState) {
    memcpy(newTagStateBuf, g_batchModel.tagStateData(), g_batchModel.tagStateLen());
    newTagStateLen = g_batchModel.tagStateLen();
    tagStateWasInBatch = batchResult.tagStateInBatch;
    gotTagState = true;
  }
  if (g_pendingStatusReady) {
    newTagId = g_pendingTagId;
    newTagStateValue = g_pendingTagState;
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
    memcpy(newImagePeerKey, g_pendingImagePeerKey, sizeof(newImagePeerKey));
    memcpy(newImageContentId, g_pendingImageContentId, sizeof(newImageContentId));
    newImageContentIdLen = g_pendingImageContentIdLen;
    newImagePushId = g_pendingImagePushId;
    g_pendingImageReady = false;
    gotImage = true;
  }
  if (g_pendingListDocReady) {
    memcpy(newListDocPeerKey, g_pendingListDocPeerKey, sizeof(newListDocPeerKey));
    g_pendingListDocReady = false;
    gotListDoc = true;
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
    // batch. A standalone tag push is untouched — see
    // CompanionBatchModel::PollResult::tagStateInBatch.
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
    // happen to land. batchPushId is the id from the batch's own final-flagged
    // field (or 0 if the client asked for no answer, or if the timeout safety
    // net above fired before any field was ever final-flagged) --
    // notifyRenderStatus() itself no-ops on 0.
    companionble::notifyRenderStatus(companionble::RenderResult::SequenceGap, batchPushId);
  }

  if (gotPairing) {
    RenderLock lock;
    pairingAppName = newPairingName;
    pairingDeadlineMs = millis() + kPairingTimeoutMs;
    // A pairing prompt from a NEW app can land while a different, already-
    // enrolled peer's document is being locally browsed via the offline
    // picker -- see the header's audit note.
    if (screen == Screen::List) freeListDocBuf();
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
    handlePendingImage(newImagePath, newImagePeerKey, newImageContentId, newImageContentIdLen, newImagePushId);
  }

  if (gotListDoc && screen == Screen::List && listPeerKey == newListDocPeerKey) {
    // The peer whose document is currently on screen just pushed a new one
    // (docs/companion-todo-list-design.md §6: always a whole-document
    // replace) -- re-walk it so the push actually appears, same "learn about
    // it, don't carry the payload across" discipline as every other
    // host-task -> main-loop handoff here. A push from any OTHER peer, or
    // one that lands while this peer is showing some other screen, is
    // silently ignored: there is nothing on screen for it to invalidate.
    RenderLock lock;
    loadListDocBuf();  // re-read: the new document may be a different size
    reloadListView();
    requestUpdate();
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
    // rather than being silently overwritten. supersede runs regardless of
    // whether *this* push wants an answer -- the previous one's expectation
    // still has to be resolved, since this push is taking the screen either
    // way -- but the arm itself only actually fires RENDER_STATUS traffic
    // later if batchPushId is non-zero (renderAwaitingStatus false otherwise;
    // notifyRenderPushResult() no-ops when it is).
    supersedePendingRenderStatus();
    renderAwaitingStatus = batchPushId != 0;
    renderAwaitingPushId = batchPushId;
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
    // replacing a gallery -- or a different peer's document -- being browsed
    // locally through the picker (screen == List here does not imply this
    // push's peer is listPeerKey) -- see the header's audit note.
    if (screen == Screen::List) freeListDocBuf();
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
  //
  // A claimed press restarts the idle clock -- but ONLY if one is already
  // running (idleSinceMs != 0). idleSinceMs == 0 is not "the clock reads
  // zero," it means "no idle sleep from this state at all": checkIdleTimers()
  // returns immediately when it sees 0, and applyForegroundChange() sets it
  // to exactly 0 the moment a peer TAKES the foreground (a live app driving
  // the screen must never be deep-slept out from under it). The USB-power
  // branch of checkIdleTimers() that this used to cite is not a
  // counterexample -- it runs only after that same 0 check has already
  // passed, so it is only ever restarting a clock that was already armed,
  // never arming one that was deliberately disarmed. Assigning millis()
  // unconditionally here made the same mistake in reverse: a live LIST peer
  // foreground disarms the clock (idleSinceMs == 0), but pressing a button on
  // Screen::List still went through handleListNav() and re-armed it, so the
  // device would deep-sleep mid-session under a connected app 5 minutes
  // later -- worse than the bug this is fixing. Gating on idleSinceMs != 0
  // fixes the real case (offline browsing after a disconnect, where the
  // clock is already running and just needs restarting so mid-browse input
  // doesn't let it expire) without arming anything that was off. A device
  // left untouched on one of these screens is unaffected either way: still
  // sleeps after kWaitingIdleSleepMs when the clock was running, still never
  // sleeps when a live peer disarmed it.
  if (handleGalleryNav() && idleSinceMs != 0) idleSinceMs = millis();

  // Same reasoning as handleGalleryNav() above: the icon grid, the gallery
  // picker, and a gallery reached through it are all firmware-owned screens
  // with no foreground peer's button map to defer to, so this is tried
  // regardless of whether foregroundPeerKey is empty.
  if (handlePickerInput()) {
    if (idleSinceMs != 0) idleSinceMs = millis();
    return;
  }

  // Screen::List claims Up/Down/Left/Right/Confirm/Back unconditionally --
  // see handleListNav()'s own comment for why that's safe specifically for
  // this screen. Tried here, before the foreground button-map dispatch
  // below, for the same "no live session required" reasoning as
  // handleGalleryNav()/handlePickerInput() above: the offline icon-grid
  // picker entry point (this feature's second commit) reaches Screen::List
  // with no foreground peer at all.
  if (handleListNav()) {
    if (idleSinceMs != 0) idleSinceMs = millis();
    return;
  }

  // Deliberately NOT gated on a live foreground session, for the same reason
  // handleGalleryNav() above isn't: a dropped link does not take the article off
  // the screen (see the disconnect branch of loop() — "Content is deliberately
  // NOT cleared here"), so the *local* page buttons have to keep paging that
  // retained content. Gating the whole map on foregroundPeerKey left LEFT/RIGHT
  // dead for the entire gap between a disconnect and the app's reconnect, over
  // an article the user could still see — indistinguishable from a wedged
  // device, and reported as exactly that. buttons[] survives a disconnect (only
  // the next peer's loadUiDeclaration() resets it), so routingFor() still gives
  // the departed app's answer. Only REMOTE presses need a live session, and
  // handleMappedButton() is what drops those.
  //
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

bool CompanionModeActivity::reportListNav(companiontest::ListNavReport* out) const {
  if (screen != Screen::List) return false;
  out->onListScreen = true;
  out->listIndex = listNav.listIndex();
  out->listCount = listNav.listCount();
  out->cursor = listNav.cursor();
  out->windowStart = listNav.windowStart();
  out->itemCount = listNav.itemCount();
  return true;
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
    case Screen::List:
      return "list";
    case Screen::Message:
      return "message";
  }
  return "?";
}

// Applies one button press according to the foreground app's map. Returns true
// if the press was consumed.
bool CompanionModeActivity::handleMappedButton(MappedInputManager::Button role, companionble::ButtonId id) {
  if (!buttonWasPressed(role, id)) return false;

  // !foregroundPeerKey.empty(), not the `connected` member -- see
  // CompanionButtonPolicy.h's doc comment on why decide() takes this exact
  // test as its peerConnected argument.
  const bool peerConnected = !foregroundPeerKey.empty();
  const auto decision = companionbuttons::decide(flagsFor(id), routingFor(id), peerConnected);

  // ALSO_NOTIFY means a local action's press is *also* relayed to the app,
  // on top of whatever the switch below does locally -- see decide()'s doc
  // comment. Fired once here rather than duplicated into every local case.
  if (decision.notify) notifyHeldButton(id);

  switch (decision.action) {
    case companionble::ButtonRouting::None:
      // Either genuinely unrouted, Remote-routed with nothing to deliver the
      // press to, or a LOCAL_ONLY_OFFLINE button suppressed by a connected
      // peer (decide() collapses the local action to None in all three
      // cases). decision.notify, handled above, is what tells them apart:
      // it already fired notifyHeldButton() when there was somewhere to
      // deliver the press. Without it, notifyButtonEvent() already no-ops
      // when disconnected, but going through notifyHeldButton() anyway would
      // start hold-tracking for a press no app will ever hear, leaving
      // holdActive set until the button is released.
      return decision.notify;

    case companionble::ButtonRouting::Remote:
      // decide() never returns Remote as the local action -- see the None
      // case above.
      return false;

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

    case companionble::ButtonRouting::LocalListMoveUp:
    case companionble::ButtonRouting::LocalListMoveDown:
    case companionble::ButtonRouting::LocalListSwitchLeft:
    case companionble::ButtonRouting::LocalListSwitchRight:
    case companionble::ButtonRouting::LocalListToggleCheck:
    case companionble::ButtonRouting::LocalBack:
      // Handled only by handleListNav() -- see that member's doc comment. A
      // browsed gallery's Back (handlePickerInput()'s Screen::Image case) is
      // firmware-owned and unconditional, not routed through `buttons` at
      // all, so LocalBack bound to that screen's Back button is never even
      // looked at there.
      return false;

    case companionble::ButtonRouting::LocalGalleryPrev:
    case companionble::ButtonRouting::LocalGalleryNext:
      // Handled only by handleGalleryNav() -- see that member's doc comment.
      return false;
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
    case Screen::List:
      renderList();
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

void CompanionModeActivity::renderWaiting(bool inverted, const char* label, bool showOfflineHint) {
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
  if (showOfflineHint && !mappedInput.hasTouch()) {
    // Only Confirm does anything on Screen::IconGrid (handlePickerInput()) --
    // it reboots into offline browse -- so this is the only slot filled.
    const auto labels = mappedInput.mapLabels("", tr(STR_BROWSE_OFFLINE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
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
    renderWaiting(inverted, label, /*showOfflineHint=*/true);
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

  if (!mappedInput.hasTouch()) {
    // Only Confirm does anything on Screen::IconGrid (handlePickerInput()) --
    // it reboots into offline browse -- so this is the only slot filled.
    const auto labels = mappedInput.mapLabels("", tr(STR_BROWSE_OFFLINE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  if (inverted) renderer.invertScreen();
  renderer.displayBuffer();
}

// The interactive counterpart to renderIconGrid(): one tile per peer that
// declared the image-gallery capability (pickerPeerKeys, built by
// buildPickerPeerKeys() — not grouped by appId, unlike the decorative grid,
// since two installs of the same app have two separate galleries and must
// stay two separate tiles). A thick outline marks the cursor; the existing
// thin "currently connected" marker still applies if that peer also happens
// to hold a live session. A short label under each tile (userName falling
// back to displayName) is what actually tells two installs of the same app
// apart, since their icon and app name alone would be identical.
void CompanionModeActivity::renderGalleryPicker() {
  const size_t count = pickerPeerKeys.size();
  if (count == 0) {
    // Shouldn't happen — onEnter()'s offline-resume branch only switches to
    // this screen when pickerPeerKeys is non-empty — but fail safe rather
    // than draw an empty grid.
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

  if (!mappedInput.hasTouch()) {
    // handlePickerInput()'s GalleryPicker branch reads Up/Down (side buttons)
    // and Left/Right (front buttons, labeled below) interchangeably to move
    // the cursor through the flat tile list, plus Confirm/Back -- the same
    // 4-slot back/confirm/prev/next idiom every other local activity uses,
    // e.g. FileBrowserActivity.cpp.
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), "<", ">");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
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
  companionble::notifyRenderStatus(result, renderAwaitingPushId);
}

// Both arm sites (handlePendingImage() and loop()'s content-commit block) set
// renderAwaitingPushId unconditionally (whether or not the push that just
// landed wants an answer -- see each site's own comment) and each also sets
// `screen` — so the push that lands second decides which branch render()
// takes, and the first one's render never happens at all. The expectation it
// armed was previously just overwritten, leaving its caller to wait out the
// full client-side timeout for an answer the device already knew would never
// come.
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

// Screen::List: the current list's title, its groups (an empty label is the
// "ungrouped" bucket, design doc §2 -- no heading drawn for it), and its items
// with a checkbox glyph reflecting `checked` plus a cursor marker on the item
// under listNav.cursor().
//
// This function knows nothing about the check-off diff, and that is on
// purpose: reloadListView() has already folded any local deviation into
// row.checked (ListRowVisitor), so adding on-device toggling did not touch a
// line of the drawing code below. Anything that makes rendering consult the
// diff directly is a step back from that.
void CompanionModeActivity::renderList() {
  const int titleLineHeight = renderer.getLineHeight(cachedTitleFontId);
  const int titleY = cachedOrientedMarginTop;

  if (!listDocLoaded || listNav.listCount() == 0) {
    // A LIST peer with nothing pushed yet, or a document that failed to
    // parse -- same "waiting for content" idea as Screen::Text's
    // haveContent==false branch, just phrased for a list.
    renderer.drawCenteredText(cachedTitleFontId, renderer.getScreenHeight() / 2, tr(STR_COMPANION_LIST_EMPTY), true,
                              EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Title, with an "index/count" indicator when Left/Right actually does
  // something -- same idiom as renderPage()'s page-count indicator.
  renderer.drawText(cachedTitleFontId, cachedOrientedMarginLeft, titleY, listDocTitle.c_str(), true,
                    EpdFontFamily::BOLD);
  if (listNav.listCount() > 1) {
    char counter[16];
    snprintf(counter, sizeof(counter), "%u/%u", static_cast<unsigned>(listNav.listIndex()) + 1,
             static_cast<unsigned>(listNav.listCount()));
    const int counterWidth = renderer.getTextWidth(SMALL_FONT_ID, counter);
    renderer.drawText(SMALL_FONT_ID, cachedOrientedMarginLeft + viewportWidth - counterWidth, titleY, counter, true);
  }

  constexpr int kListTitleBottomSpacing = 6;
  constexpr int kCheckboxSize = 14;
  constexpr int kCheckboxGap = 8;
  const int itemIndent = kCheckboxSize + kCheckboxGap;
  // Selection outline padding: clears the checkbox on the left and gives
  // descenders (e.g. "g") room below the baseline, instead of hugging the
  // row's raw text extent the way a 0-padding rect did.
  //
  // drawRoundedRect's stroke is inset from its bounding box (ink occupies
  // columns x..x+lineWidth-1), unlike the checkbox's plain drawRect, whose
  // 1px stroke sits exactly on the boundary column. So matching the visible
  // ink-to-ink gap to kCheckboxGap means padding out by the stroke width too
  // -- padding = kCheckboxGap alone would read as kCheckboxGap - lineWidth.
  constexpr int kListSelectionBorderThickness = 2;
  constexpr int kListSelectionPaddingX = kCheckboxGap + kListSelectionBorderThickness;
  constexpr int kListSelectionPaddingTop = 3;
  constexpr int kListSelectionPaddingBottom = 4;
  constexpr int kListSelectionCornerRadius = 4;
  const int bottomLimit = renderer.getScreenHeight() - cachedOrientedMarginBottom;
  const int rowHeight = listRowHeight > 0 ? listRowHeight : renderer.getLineHeight(cachedFontId);

  // The scroll thumb (drawn after the row loop, below) sits INSIDE
  // viewportWidth -- kScrollBarRightOffset(5) + kScrollBarWidth(4) in from
  // the right edge, not past it -- so the cursor outline's normal right-side
  // overshoot (kListSelectionPaddingX past viewportWidth) doesn't clear it;
  // confirmed on a hardware screenshot (scripts/debug_scrollbar_shot.py) that
  // the outline's border lands squarely on the thumb's column. When the
  // thumb is visible, pull the outline's right edge in to end short of the
  // thumb (with kScrollBarClearance to spare) instead of merely dropping the
  // overshoot -- it has to give up viewport width the thumb is using, not
  // just the padding past it.
  constexpr int kScrollBarWidth = 4;
  constexpr int kScrollBarRightOffset = 5;
  constexpr int kScrollBarClearance = 4;
  const bool scrollBarVisible = static_cast<int>(listNav.itemCount()) > static_cast<int>(listNav.visibleCapacity());
  const int selectionRightOvershoot =
      scrollBarVisible
          ? -(kScrollBarWidth + kScrollBarRightOffset + kScrollBarClearance)
          : kListSelectionPaddingX;

  int y = titleY + titleLineHeight + kListTitleBottomSpacing;

  if (listVisibleRows.empty()) {
    // listNav.empty() true: the current list has zero items (a legal, if
    // unusual, pushed list -- design doc §2 places no floor on item count).
    renderer.drawText(cachedFontId, cachedOrientedMarginLeft, y, tr(STR_COMPANION_LIST_NO_ITEMS));
  }

  for (const auto& row : listVisibleRows) {
    if (y + rowHeight > bottomLimit) break;  // ran out of vertical room -- see computeViewport()'s cap note

    if (row.isHeader) {
      renderer.drawText(cachedFontId, cachedOrientedMarginLeft, y, row.text.c_str(), true, EpdFontFamily::BOLD);
      y += rowHeight;
      continue;
    }

    const bool isCursor = row.itemFlatIndex == static_cast<int>(listNav.cursor());
    if (isCursor) {
      // Rounded outline around the whole row, padded clear of the checkbox
      // and the row's descenders -- same cursor idiom as renderGalleryPicker()'s
      // tile-selection marker, but no longer flush with the row's raw extent.
      renderer.drawRoundedRect(cachedOrientedMarginLeft - kListSelectionPaddingX, y - kListSelectionPaddingTop,
                               viewportWidth + kListSelectionPaddingX + selectionRightOvershoot,
                               rowHeight + kListSelectionPaddingTop + kListSelectionPaddingBottom,
                               kListSelectionBorderThickness, kListSelectionCornerRadius, true);
    }

    const int boxY = y + (rowHeight - kCheckboxSize) / 2;
    renderer.drawRect(cachedOrientedMarginLeft, boxY, kCheckboxSize, kCheckboxSize, true);
    if (row.checked) {
      renderer.fillRect(cachedOrientedMarginLeft + 3, boxY + 3, kCheckboxSize - 6, kCheckboxSize - 6, true);
    }
    renderer.drawText(cachedFontId, cachedOrientedMarginLeft + itemIndent, y, row.text.c_str(), true);
    if (row.checked) {
      // Strike-through: a thin rule through the text's vertical middle,
      // same fillRect idiom BaseTheme uses for its tab-selection underline.
      constexpr int kStrikeThroughThickness = 2;
      const int textWidth = renderer.getTextWidth(cachedFontId, row.text.c_str());
      const int strikeY = y + rowHeight / 2 - kStrikeThroughThickness / 2;
      renderer.fillRect(cachedOrientedMarginLeft + itemIndent, strikeY, textWidth, kStrikeThroughThickness, true);
    }
    y += rowHeight;
  }

  // Scroll position thumb: renderList() hand-draws its own rows instead of
  // going through GUI.drawList() (see the class comment above), so it can't
  // reach RoundedRaffTheme::drawScrollBar()/LyraTheme::drawList()'s inline
  // scrollbar either -- both are private to their theme's .cpp. Same
  // itemCount/pageStartIndex/pageItems math as those, fed straight from
  // listNav, the source those themes' callers would have used anyway.
  if (scrollBarVisible) {
    const int itemCount = static_cast<int>(listNav.itemCount());
    const int pageItems = static_cast<int>(listNav.visibleCapacity());
    const int barX = cachedOrientedMarginLeft + viewportWidth - kScrollBarRightOffset - kScrollBarWidth;
    const int barY = titleY + titleLineHeight + kListTitleBottomSpacing;
    const int barH = bottomLimit - barY;
    const int thumbH = std::max(10, (barH * pageItems) / itemCount);
    const int maxStart = std::max(1, itemCount - pageItems);
    const int maxTravel = std::max(1, barH - thumbH);
    const int clampedStart = std::clamp(static_cast<int>(listNav.windowStart()), 0, maxStart);
    const int thumbY = barY + (clampedStart * maxTravel) / maxStart;
    renderer.fillRect(barX, thumbY, kScrollBarWidth, thumbH, true);
  }

  if (!mappedInput.hasTouch()) {
    // Hints come straight from the browsed peer's declared labels (see
    // labelFor()), the same idiom Screen::Text uses for its own map -- there
    // is no fixed physical meaning left to show a fixed string for. A
    // switch-list button's hint is hidden when there is only one list to
    // switch between, same "hide, don't grey out" convention renderPage()
    // uses for page-turn buttons at either end.
    const char* backLabel = labelFor(companionble::ButtonId::Back);
    const char* confirmLabel = labelFor(companionble::ButtonId::Confirm);
    const char* leftLabel = labelFor(companionble::ButtonId::Left);
    const char* rightLabel = labelFor(companionble::ButtonId::Right);
    if (routingFor(companionble::ButtonId::Left) == companionble::ButtonRouting::LocalListSwitchLeft &&
        listNav.listCount() <= 1) {
      leftLabel = "";
    }
    if (routingFor(companionble::ButtonId::Right) == companionble::ButtonRouting::LocalListSwitchRight &&
        listNav.listCount() <= 1) {
      rightLabel = "";
    }
    const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, leftLabel, rightLabel);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
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
