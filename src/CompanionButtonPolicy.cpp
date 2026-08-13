#include "CompanionButtonPolicy.h"

namespace companionbuttons {

ButtonDecision decide(companionble::ButtonRouting routing, bool peerConnected) {
  using companionble::ButtonRouting;

  switch (routing) {
    case ButtonRouting::None:
      return {ButtonRouting::None, false, false};

    case ButtonRouting::Remote:
      // Nothing to deliver the press to (and no hint worth drawing) once the
      // link is gone -- see the 2026-08-12 fix this module was extracted
      // for: labelFor() used to keep drawing a Remote-routed label after
      // disconnect because buttons[] is not cleared on disconnect.
      return {ButtonRouting::None, peerConnected, peerConnected};

    case ButtonRouting::LocalPagePrev:
    case ButtonRouting::LocalPageNext:
    case ButtonRouting::LocalSleep:
    case ButtonRouting::LocalListMoveUp:
    case ButtonRouting::LocalListMoveDown:
    case ButtonRouting::LocalListSwitchLeft:
    case ButtonRouting::LocalListSwitchRight:
    case ButtonRouting::LocalListToggleCheck:
    case ButtonRouting::LocalListBack:
      return {routing, false, true};
  }
  // Unreachable: the switch above is exhaustive over every ButtonRouting
  // enumerator (no default:, so a new one is a compile error, not a silent
  // fallthrough). A return is still required to satisfy -Wreturn-type.
  return {ButtonRouting::None, false, false};
}

}  // namespace companionbuttons
