#include "CompanionTestConsole.h"

#ifdef COMPANION_TEST_CONSOLE

#include <Logging.h>

#include <cstdio>

#include "CompanionPeerStore.h"

namespace companiontest {

namespace {

// Every reply is prefixed so a host can pick it out of the log stream without
// parsing timestamps or guessing at line shapes.
constexpr const char* kPrefix = "CT:";

// One virtual press at a time, matching the hardware: InputManager::getHeldTime()
// is a single global timer because only one physical button can be down at once,
// and injection has to behave the same way or it would test a path the device
// cannot actually produce.
struct VirtualPress {
  bool active = false;
  companionble::ButtonId button = companionble::ButtonId::Confirm;
  unsigned long startMs = 0;
  unsigned long holdMs = 0;
  bool pressDelivered = false;
  bool releasePending = false;
};
VirtualPress g_press;

ScreenNameProvider g_screenNameProvider = nullptr;
TagStateProvider g_tagStateProvider = nullptr;

const char* tagStateName(uint8_t state) {
  switch (static_cast<companionble::TagState>(state)) {
    case companionble::TagState::Hidden:
      return "hidden";
    case companionble::TagState::Outline:
      return "outline";
    case companionble::TagState::Filled:
      return "filled";
  }
  return "?";
}

void reply(const char* format, ...) {
  char buffer[192];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  logSerial.printf("%s%s\n", kPrefix, buffer);
}

const char* routingName(companionble::ButtonRouting routing) {
  switch (routing) {
    case companionble::ButtonRouting::None:
      return "none";
    case companionble::ButtonRouting::Remote:
      return "remote";
    case companionble::ButtonRouting::LocalPagePrev:
      return "page_prev";
    case companionble::ButtonRouting::LocalPageNext:
      return "page_next";
    case companionble::ButtonRouting::LocalSleep:
      return "sleep";
  }
  return "?";
}

void reportState() {
  const char* screenName = g_screenNameProvider ? g_screenNameProvider() : "unknown";
  const char* peerKey = companionble::foregroundPeerKey();
  reply("state screen=%s connected=%d sessions=%u foreground=%u peer=%s heap=%u", screenName,
        companionble::isConnected() ? 1 : 0, static_cast<unsigned>(companionble::activeSessionCount()),
        static_cast<unsigned>(companionble::foregroundSessionId()), peerKey[0] ? peerKey : "-",
        static_cast<unsigned>(ESP.getFreeHeap()));
}

void reportPeers() {
  char keys[companionpeer::kMaxPeers][companionpeer::kPeerKeyLen];
  const size_t count = companionpeer::listPeers(keys, companionpeer::kMaxPeers);
  reply("peers count=%u", static_cast<unsigned>(count));
  for (size_t i = 0; i < count; ++i) {
    uint8_t uiTag[4];
    uint8_t iconTag[4];
    companionpeer::assetTag(keys[i], companionpeer::kAssetUiDeclaration, uiTag);
    companionpeer::assetTag(keys[i], companionpeer::kAssetIcon, iconTag);
    const std::string name = companionpeer::displayName(keys[i]);
    reply("peer key=%s name=%s ui=%02x%02x%02x%02x icon=%02x%02x%02x%02x", keys[i], name.c_str(), uiTag[0],
          uiTag[1], uiTag[2], uiTag[3], iconTag[0], iconTag[1], iconTag[2], iconTag[3]);
  }
}

void reportCapabilities() {
  size_t length = 0;
  const uint8_t* value = companionble::capabilityValue(length);
  char hex[3 * 32 + 1] = {0};
  size_t offset = 0;
  for (size_t i = 0; i < length && offset + 3 < sizeof(hex); ++i) {
    offset += snprintf(hex + offset, sizeof(hex) - offset, "%02x", value[i]);
  }
  reply("cap len=%u %s", static_cast<unsigned>(length), hex);
}

// Injects a press. `holdMs == 0` is a tap: press and release in the same loop
// iteration. Anything longer runs the real repeat-while-held path in the
// activity, which is the point — that path is otherwise only reachable by a
// human holding a button.
void injectButton(int buttonId, unsigned long holdMs) {
  if (buttonId < 0 || buttonId > static_cast<int>(companionble::ButtonId::Power)) {
    reply("err btn: id %d out of range", buttonId);
    return;
  }
  g_press = VirtualPress();
  g_press.active = true;
  g_press.button = static_cast<companionble::ButtonId>(buttonId);
  g_press.startMs = millis();
  g_press.holdMs = holdMs;
  reply("btn id=%d hold=%lu", buttonId, holdMs);
}

}  // namespace

void update() {
  if (!g_press.active || !g_press.pressDelivered) return;
  if (millis() - g_press.startMs >= g_press.holdMs) {
    g_press.releasePending = true;
    g_press.active = false;
  }
}

bool wasPressed(companionble::ButtonId button) {
  if (!g_press.active || g_press.pressDelivered || g_press.button != button) return false;
  g_press.pressDelivered = true;
  // A zero-length hold is a tap: release on the very next poll, so the activity
  // sees a complete press/release pair without the host having to time anything.
  if (g_press.holdMs == 0) {
    g_press.active = false;
    g_press.releasePending = true;
  }
  return true;
}

bool isPressed(companionble::ButtonId button) {
  return g_press.active && g_press.pressDelivered && g_press.button == button;
}

bool wasReleased(companionble::ButtonId button) {
  if (!g_press.releasePending || g_press.button != button) return false;
  g_press.releasePending = false;
  return true;
}

unsigned long heldTimeMs() { return g_press.pressDelivered ? millis() - g_press.startMs : 0; }

bool holdInProgress() { return g_press.active || g_press.releasePending; }

void setScreenNameProvider(ScreenNameProvider provider) { g_screenNameProvider = provider; }

void setTagStateProvider(TagStateProvider provider) { g_tagStateProvider = provider; }

bool handleCommand(const String& command) {
  if (command == "CPING") {
    reply("pong v6");
    return true;
  }
  if (command == "CSTATE") {
    reportState();
    return true;
  }
  if (command == "CPEERS") {
    reportPeers();
    return true;
  }
  if (command == "CCAP") {
    reportCapabilities();
    return true;
  }
  if (command == "CRESET") {
    // Returns the device to never-paired, so first-contact enrollment can be
    // re-tested without physically clearing the SD card between runs.
    companionpeer::forgetAllPeers();
    reply("reset ok");
    return true;
  }
  if (command.startsWith("CBTN")) {
    String args = command.substring(4);
    args.trim();
    const int space = args.indexOf(' ');
    const int buttonId = (space < 0 ? args : args.substring(0, space)).toInt();
    const unsigned long holdMs = space < 0 ? 0UL : static_cast<unsigned long>(args.substring(space + 1).toInt());
    injectButton(buttonId, holdMs);
    return true;
  }
  if (command == "CTAGS") {
    if (!g_tagStateProvider) {
      reply("tags none: no provider");
      return true;
    }
    TagReport report[companionble::kMaxTags];
    const uint8_t count = g_tagStateProvider(report, companionble::kMaxTags);
    reply("tags count=%u", static_cast<unsigned>(count));
    for (uint8_t i = 0; i < count; ++i) {
      reply("tag id=%u state=%s label=%s", static_cast<unsigned>(report[i].id), tagStateName(report[i].state),
            report[i].label);
    }
    return true;
  }
  if (command == "CUI") {
    const char* peerKey = companionble::foregroundPeerKey();
    if (peerKey[0] == '\0') {
      reply("ui none: no foreground peer");
      return true;
    }
    uint8_t raw[companionpeer::kMaxUiDeclarationLen];
    const size_t length =
        companionpeer::readAssetBody(peerKey, companionpeer::kAssetUiDeclaration, raw, sizeof(raw));
    if (length < 1) {
      reply("ui none: peer %s has no stored declaration", peerKey);
      return true;
    }

    const uint8_t buttonCount = raw[0];
    reply("ui peer=%s buttons=%u", peerKey, static_cast<unsigned>(buttonCount));
    size_t offset = 1;
    for (uint8_t i = 0; i < buttonCount && offset + 3 <= length; ++i) {
      const uint8_t id = raw[offset];
      const uint8_t routing = raw[offset + 1];
      const uint8_t labelLen = raw[offset + 2];
      offset += 3;
      if (offset + labelLen > length) break;
      char label[64] = {0};
      const size_t copy = labelLen < sizeof(label) - 1 ? labelLen : sizeof(label) - 1;
      memcpy(label, raw + offset, copy);
      offset += labelLen;
      reply("button id=%u routing=%s label=%s", static_cast<unsigned>(id),
            routingName(static_cast<companionble::ButtonRouting>(routing)), label);
    }

    // The tag section is optional — an app with no tags simply ends here.
    if (offset >= length) {
      reply("ui tags=0");
      return true;
    }
    const uint8_t tagCount = raw[offset++];
    reply("ui tags=%u", static_cast<unsigned>(tagCount));
    for (uint8_t i = 0; i < tagCount && offset + 2 <= length; ++i) {
      const uint8_t id = raw[offset];
      const uint8_t labelLen = raw[offset + 1];
      offset += 2;
      if (offset + labelLen > length) break;
      char label[32] = {0};
      const size_t copy = labelLen < sizeof(label) - 1 ? labelLen : sizeof(label) - 1;
      memcpy(label, raw + offset, copy);
      offset += labelLen;
      reply("tag id=%u label=%s", static_cast<unsigned>(id), label);
    }
    return true;
  }
  return false;
}

}  // namespace companiontest

#endif  // COMPANION_TEST_CONSOLE
