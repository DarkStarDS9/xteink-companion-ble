#include "CompanionTestConsole.h"

#ifdef COMPANION_TEST_CONSOLE

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "CompanionPeerStore.h"
#include "CompanionUiDeclaration.h"

namespace companiontest {

namespace {

// Every reply is prefixed so a host can pick it out of the log stream without
// parsing timestamps or guessing at line shapes.
constexpr const char* kPrefix = "CT:";

// Every peer companion_e2e_test.py enrolls must use a display name starting
// with this, so CRESETTEST can tell "harness left this behind" apart from a
// real app registration. Keep in sync with TEST_PEER_NAME_PREFIX in
// scripts/companion_protocol.py.
constexpr const char* kTestPeerNamePrefix = "[E2E] ";

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
ListNavStateProvider g_listNavStateProvider = nullptr;

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
    case companionble::ButtonRouting::LocalListMoveUp:
      return "list_move_up";
    case companionble::ButtonRouting::LocalListMoveDown:
      return "list_move_down";
    case companionble::ButtonRouting::LocalListSwitchLeft:
      return "list_switch_left";
    case companionble::ButtonRouting::LocalListSwitchRight:
      return "list_switch_right";
    case companionble::ButtonRouting::LocalListToggleCheck:
      return "list_toggle_check";
    case companionble::ButtonRouting::LocalBack:
      return "back";
    case companionble::ButtonRouting::LocalGalleryPrev:
      return "gallery_prev";
    case companionble::ButtonRouting::LocalGalleryNext:
      return "gallery_next";
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

void setListNavStateProvider(ListNavStateProvider provider) { g_listNavStateProvider = provider; }

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
  if (command == "CRESETTEST") {
    // Same idea as CRESET but scoped to peers the harness itself created --
    // every one it enrolls is named with kTestPeerNamePrefix. Leaves real,
    // manually-paired app registrations (snap2ink, SpokenFeeds, ...) alone,
    // so running the e2e suite against a reader that also has real app data
    // on its SD card no longer costs that data.
    const size_t removed = companionpeer::forgetPeersWithNamePrefix(kTestPeerNamePrefix);
    reply("reset ok removed=%u", static_cast<unsigned>(removed));
    return true;
  }
  if (command.startsWith("CFORGET")) {
    // Deletes one enrolled peer by key, leaving every other peer intact --
    // for clearing a stray/leftover peer that doesn't carry the harness's
    // kTestPeerNamePrefix (so CRESETTEST above wouldn't touch it either).
    String arg = command.substring(7);
    arg.trim();
    if (arg.isEmpty()) {
      reply("forget error: usage CFORGET <key>");
      return true;
    }
    const bool removed = companionpeer::forgetPeer(arg.c_str());
    reply(removed ? "forget ok key=%s" : "forget error: no such peer key=%s", arg.c_str());
    return true;
  }
  if (command.startsWith("CLS")) {
    // Ad hoc SD directory listing for debugging the image gallery on real
    // hardware — not something a client needs, so it's not wired through the
    // BLE protocol at all, just this serial console.
    String path = command.substring(3);
    path.trim();
    if (path.isEmpty()) path = "/";
    const std::vector<String> entries = Storage.listFiles(path.c_str(), 64);
    reply("ls %s count=%u", path.c_str(), static_cast<unsigned>(entries.size()));
    for (const auto& entry : entries) {
      reply("  %s", entry.c_str());
    }
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
  if (command == "CLIST") {
    // Read-only, and justified exactly as CTAGS above is: it reports state the
    // device already holds on SD, it adds no capability, and it is the only way
    // a host harness can see the check-off diff without a phone (CLAUDE.md,
    // "The phone is not a test harness").
    const char* peerKey = companionble::foregroundPeerKey();
    if (peerKey[0] == '\0') {
      reply("list none: no foreground peer");
      return true;
    }

    uint32_t revision = 0;
    uint16_t total = 0;
    // Entries are streamed a window at a time rather than through a whole
    // companiontodo::Diff: 1096 bytes of stack for a diagnostic print is not a
    // trade this part can make, and readListStateEntries() exists precisely so
    // no caller has to.
    uint8_t entries[32 * 3];
    constexpr uint16_t kWindow = sizeof(entries) / 3;

    // maxEntries 0 asks for the header alone. A peer with no stored diff at all
    // reports the same revision=0 count=0 as one whose edits are all synced
    // away, which is the honest answer: neither has anything pending.
    companionpeer::readListStateEntries(peerKey, 0, 0, nullptr, &revision, &total);
    reply("list peer=%s revision=%u count=%u", peerKey, static_cast<unsigned>(revision), static_cast<unsigned>(total));

    for (uint16_t offset = 0; offset < total;) {
      const size_t got = companionpeer::readListStateEntries(peerKey, offset, kWindow, entries, nullptr, nullptr);
      if (got == 0) break;
      for (size_t i = 0; i < got; ++i) {
        const uint16_t id = static_cast<uint16_t>(entries[i * 3] | (entries[i * 3 + 1] << 8));
        reply("listitem id=%u checked=%u", static_cast<unsigned>(id), static_cast<unsigned>(entries[i * 3 + 2]));
      }
      offset = static_cast<uint16_t>(offset + got);
    }
    return true;
  }
  if (command == "CLISTNAV") {
    // Read-only, justified the same way CLIST is above: it reports state
    // render() already holds (companiontodo::Nav's live position) rather than
    // making a test diff a 52 KB screenshot to prove a cursor moved.
    ListNavReport report;
    if (!g_listNavStateProvider || !g_listNavStateProvider(&report) || !report.onListScreen) {
      reply("listnav none: not on Screen::List");
      return true;
    }
    reply("listnav listIndex=%u listCount=%u cursor=%u windowStart=%u itemCount=%u",
          static_cast<unsigned>(report.listIndex), static_cast<unsigned>(report.listCount),
          static_cast<unsigned>(report.cursor), static_cast<unsigned>(report.windowStart),
          static_cast<unsigned>(report.itemCount));
    return true;
  }
  if (command == "CUI" || command.startsWith("CUI ")) {
    String arg = command == "CUI" ? "" : command.substring(4);
    arg.trim();
    const char* peerKey = arg.length() ? arg.c_str() : companionble::foregroundPeerKey();
    if (peerKey[0] == '\0') {
      reply("ui none: no foreground peer");
      return true;
    }
    uint8_t raw[companionpeer::kMaxUiDeclarationLen];
    const size_t length = companionpeer::readAssetBody(peerKey, companionpeer::kAssetUiDeclaration, raw, sizeof(raw));
    if (length < 1) {
      reply("ui none: peer %s has no stored declaration", peerKey);
      return true;
    }

    // Validate and read the header through the shared codec; only the per-entry
    // labels are walked here. See CompanionUiDeclaration.h's note on
    // kBodyFirstButtonOffset for why this offset is not spelled out locally.
    companionui::DeclarationInfo info;
    if (companionui::parseBody(raw, length, &info) != companionui::ParseResult::Ok) {
      reply("ui none: peer %s has an unparseable declaration", peerKey);
      return true;
    }

    const uint8_t buttonCount = info.buttonCount;
    reply("ui peer=%s shape=%u buttons=%u", peerKey, static_cast<unsigned>(info.shape),
          static_cast<unsigned>(buttonCount));
    size_t offset = companionui::kBodyFirstButtonOffset;
    for (uint8_t i = 0; i < buttonCount && offset + 3 <= length; ++i) {
      // Byte 0 packs the button id into its low nibble and behaviour flags
      // into its high nibble -- see CompanionBle.h's
      // kButtonIdMask/kButtonFlagAlsoNotify/kButtonFlagLocalOnlyOffline.
      // Masked here so a declaration using the flags is legible instead of
      // printing a raw id like "id=142".
      const uint8_t rawByte0 = raw[offset];
      const uint8_t id = rawByte0 & companionble::kButtonIdMask;
      const uint8_t routing = raw[offset + 1];
      const uint8_t labelLen = raw[offset + 2];
      offset += 3;
      if (offset + labelLen > length) break;
      char label[64] = {0};
      const size_t copy = labelLen < sizeof(label) - 1 ? labelLen : sizeof(label) - 1;
      memcpy(label, raw + offset, copy);
      offset += labelLen;
      char flagsSuffix[32] = {0};
      if (rawByte0 & companionble::kButtonFlagAlsoNotify) strcat(flagsSuffix, " +notify");
      if (rawByte0 & companionble::kButtonFlagLocalOnlyOffline) strcat(flagsSuffix, " +offline_only");
      reply("button id=%u routing=%s label=%s%s", static_cast<unsigned>(id),
            routingName(static_cast<companionble::ButtonRouting>(routing)), label, flagsSuffix);
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

    // Up to two optional trailing bytes: tag render style, then capabilities.
    // Both are absent-if-out-of-buffer, same convention as the loops above.
    if (offset < length) {
      reply("ui style=%u", static_cast<unsigned>(raw[offset++]));
    }
    if (offset < length) {
      reply("ui capabilities=0x%02x", static_cast<unsigned>(raw[offset++]));
    }
    return true;
  }
  return false;
}

}  // namespace companiontest

#endif  // COMPANION_TEST_CONSOLE
