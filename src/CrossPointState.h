#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

class CrossPointState : public PersistableStore<CrossPointState> {
  CrossPointState() = default;

  friend class PersistableStore<CrossPointState>;

 public:
  static constexpr uint8_t SLEEP_RECENT_COUNT = 16;

  std::string openEpubPath;
  uint16_t recentSleepImages[SLEEP_RECENT_COUNT] = {};  // circular buffer of recent wallpaper indices
  uint8_t recentSleepPos = 0;                           // next write slot
  uint8_t recentSleepFill = 0;                          // valid entries (0..SLEEP_RECENT_COUNT)
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;
  bool showBootScreen = true;
  // True while the device is in the offline gallery/ToDo-list picker flow:
  // BLE was deliberately never started this boot, and a reboot is how the
  // flow is entered and left (see CompanionModeActivity's enterOfflineBrowseMode()
  // and handlePickerInput()). Not RTC memory: HalPowerManager::startDeepSleep()
  // powers the MCU off on X4 battery, which RTC does not survive.
  bool companionOfflineBrowse = false;

  // Which screen an offline browse (companionOfflineBrowse above) had open,
  // for OfflineBrowsePosition::screen below. A deliberately narrow,
  // CompanionModeActivity-independent enum (this header must not include
  // CompanionModeActivity.h) rather than a raw cast of that class's Screen
  // enum -- storing that enum's ordinal directly would silently change
  // meaning if Screen ever gets reordered.
  enum class OfflineBrowseScreen : uint8_t { Picker = 0, List = 1, Image = 2 };

  // Where an offline browse left off, so a reboot -- including a deep-sleep
  // wake, which IS a reboot on this part (see main.cpp and
  // CompanionModeActivity.cpp) -- can resume straight into the same
  // peer/screen/cursor instead of landing back on the picker. Only
  // meaningful while companionOfflineBrowse is true.
  //
  // Kept current in RAM as CompanionModeActivity navigates (plain struct
  // assignment, no saveToFile() -- see docs/companion-todo-list-design.md's
  // per-keypress-SD-cost rule) and flushed to disk only at the existing
  // deep-sleep persistence points (main.cpp's enterDeepSleep(),
  // CompanionModeActivity::checkIdleTimers()'s idle-timeout sleep).
  struct OfflineBrowsePosition {
    // companionpeer::kPeerKeyLen (9: 8 chars + NUL); empty = no peer chosen
    // yet (the picker itself was on screen). Deliberately a peer *key*, not
    // an index into listPeers() -- that list is MRU-ordered
    // (CompanionPeerStore.h's touch() semantics), so an index is not a
    // stable handle across a reboot.
    std::string peerKey;
    OfflineBrowseScreen screen = OfflineBrowseScreen::Picker;
    // companiontodo::Nav position, meaningful only when screen == List;
    // re-derived counts (list/item counts) are NOT persisted -- Nav reclamps
    // them against the current document on every reload.
    uint16_t listIndex = 0;
    uint16_t listCursor = 0;
    uint16_t listWindowStart = 0;
    // Gallery position, meaningful only when screen == Image.
    uint16_t galleryIndex = 0;
  };
  OfflineBrowsePosition companionOfflineBrowsePosition;

  static const char* getFilePath() { return "/.crosspoint/state.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Returns true if idx was shown within the last checkCount picks.
  // Walks backwards from the most recently written slot.
  bool isRecentSleep(uint16_t idx, uint8_t checkCount) const;

  void pushRecentSleep(uint16_t idx);
};

// Helper macro to access state
#define APP_STATE CrossPointState::getInstance()
