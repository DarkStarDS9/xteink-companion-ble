#include "CrossPointState.h"

#include <Logging.h>

#include <algorithm>
#include <cstring>

bool CrossPointState::isRecentSleep(uint16_t idx, uint8_t checkCount) const {
  const uint8_t effectiveCount = std::min(checkCount, recentSleepFill);
  for (uint8_t i = 0; i < effectiveCount; i++) {
    const uint8_t slot = (recentSleepPos + SLEEP_RECENT_COUNT - 1 - i) % SLEEP_RECENT_COUNT;
    if (recentSleepImages[slot] == idx) return true;
  }
  return false;
}

void CrossPointState::pushRecentSleep(uint16_t idx) {
  recentSleepImages[recentSleepPos] = idx;
  recentSleepPos = (recentSleepPos + 1) % SLEEP_RECENT_COUNT;
  if (recentSleepFill < SLEEP_RECENT_COUNT) recentSleepFill++;
}

void CrossPointState::toJson(JsonDocument& doc) const {
  doc["openEpubPath"] = openEpubPath;
  JsonArray recentArr = doc["recentSleepImages"].to<JsonArray>();
  for (int i = 0; i < SLEEP_RECENT_COUNT; i++) recentArr.add(recentSleepImages[i]);
  doc["recentSleepPos"] = recentSleepPos;
  doc["recentSleepFill"] = recentSleepFill;
  doc["readerActivityLoadCount"] = readerActivityLoadCount;
  doc["lastSleepFromReader"] = lastSleepFromReader;
  doc["showBootScreen"] = showBootScreen;
  doc["companionOfflineBrowse"] = companionOfflineBrowse;

  // Grouped under one nested object rather than scattered top-level keys --
  // these five fields are only ever meaningful together (see the header's
  // doc comment on OfflineBrowsePosition).
  JsonObject pos = doc["companionOfflineBrowsePosition"].to<JsonObject>();
  pos["peerKey"] = companionOfflineBrowsePosition.peerKey;
  pos["screen"] = static_cast<uint8_t>(companionOfflineBrowsePosition.screen);
  pos["listIndex"] = companionOfflineBrowsePosition.listIndex;
  pos["listCursor"] = companionOfflineBrowsePosition.listCursor;
  pos["listWindowStart"] = companionOfflineBrowsePosition.listWindowStart;
  pos["galleryIndex"] = companionOfflineBrowsePosition.galleryIndex;
}

bool CrossPointState::fromJson(JsonVariantConst doc) {
  openEpubPath = doc["openEpubPath"] | "";
  memset(recentSleepImages, 0, sizeof(recentSleepImages));
  JsonArrayConst recentArr = doc["recentSleepImages"];
  const int actualCount =
      recentArr.isNull() ? 0 : std::min(static_cast<int>(recentArr.size()), static_cast<int>(SLEEP_RECENT_COUNT));
  for (int i = 0; i < actualCount; i++) recentSleepImages[i] = recentArr[i] | static_cast<uint16_t>(0);
  recentSleepPos = doc["recentSleepPos"] | static_cast<uint8_t>(0);
  if (recentSleepPos >= SLEEP_RECENT_COUNT) recentSleepPos = actualCount > 0 ? recentSleepPos % SLEEP_RECENT_COUNT : 0;
  recentSleepFill = doc["recentSleepFill"] | static_cast<uint8_t>(0);
  recentSleepFill = static_cast<uint8_t>(std::min(static_cast<int>(recentSleepFill), actualCount));
  // Migrate legacy single-image field from old state.json (pre-recency-buffer).
  // Only seeds the buffer if the new buffer is empty (fresh migration, not a resave).
  if (recentSleepFill == 0 && !doc["lastSleepImage"].isNull()) {
    const uint8_t legacy = doc["lastSleepImage"] | static_cast<uint8_t>(UINT8_MAX);
    if (legacy != UINT8_MAX) pushRecentSleep(static_cast<uint16_t>(legacy));
  }
  readerActivityLoadCount = doc["readerActivityLoadCount"] | static_cast<uint8_t>(0);
  lastSleepFromReader = doc["lastSleepFromReader"] | false;
  showBootScreen = doc["showBootScreen"] | true;
  companionOfflineBrowse = doc["companionOfflineBrowse"] | false;

  JsonVariantConst posDoc = doc["companionOfflineBrowsePosition"];
  // read as const char*, not | std::string("") -- see PersistableStore.h's
  // doc comment on why that fallback drags the JSON serializer into flash.
  companionOfflineBrowsePosition.peerKey = posDoc["peerKey"] | "";
  const uint8_t screenRaw = posDoc["screen"] | static_cast<uint8_t>(OfflineBrowseScreen::Picker);
  companionOfflineBrowsePosition.screen = screenRaw <= static_cast<uint8_t>(OfflineBrowseScreen::Image)
                                              ? static_cast<OfflineBrowseScreen>(screenRaw)
                                              : OfflineBrowseScreen::Picker;
  companionOfflineBrowsePosition.listIndex = posDoc["listIndex"] | static_cast<uint16_t>(0);
  companionOfflineBrowsePosition.listCursor = posDoc["listCursor"] | static_cast<uint16_t>(0);
  companionOfflineBrowsePosition.listWindowStart = posDoc["listWindowStart"] | static_cast<uint16_t>(0);
  companionOfflineBrowsePosition.galleryIndex = posDoc["galleryIndex"] | static_cast<uint16_t>(0);
  return true;
}
