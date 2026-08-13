#include "CompanionButtonPolicy.h"

namespace companionbuttons {

ButtonDecision decide(uint8_t flags, companionble::ButtonRouting routing, bool peerConnected) {
  using companionble::ButtonRouting;

  switch (routing) {
    case ButtonRouting::None:
      return {ButtonRouting::None, false, false};

    case ButtonRouting::Remote:
      // Nothing to deliver the press to (and no hint worth drawing) once the
      // link is gone -- see the 2026-08-12 fix this module was extracted
      // for: labelFor() used to keep drawing a Remote-routed label after
      // disconnect because buttons[] is not cleared on disconnect. Flags are
      // inert here -- ALSO_NOTIFY is idempotent (Remote already notifies
      // exactly when connected) and LOCAL_ONLY_OFFLINE has no local action
      // to suppress.
      return {ButtonRouting::None, peerConnected, peerConnected};

    case ButtonRouting::LocalPagePrev:
    case ButtonRouting::LocalPageNext:
    case ButtonRouting::LocalSleep:
    case ButtonRouting::LocalListMoveUp:
    case ButtonRouting::LocalListMoveDown:
    case ButtonRouting::LocalListSwitchLeft:
    case ButtonRouting::LocalListSwitchRight:
    case ButtonRouting::LocalListToggleCheck:
    case ButtonRouting::LocalBack:
    case ButtonRouting::LocalGalleryPrev:
    case ButtonRouting::LocalGalleryNext: {
      const bool alsoNotify = flags & companionble::kButtonFlagAlsoNotify;
      const bool offlineOnly = flags & companionble::kButtonFlagLocalOnlyOffline;
      const bool localRuns = !(offlineOnly && peerConnected);
      const bool notify = alsoNotify && peerConnected;
      const ButtonRouting action = localRuns ? routing : ButtonRouting::None;
      const bool showHint = localRuns || notify;
      return {action, notify, showHint};
    }
  }
  // Unreachable: the switch above is exhaustive over every ButtonRouting
  // enumerator (no default:, so a new one is a compile error, not a silent
  // fallthrough). A return is still required to satisfy -Wreturn-type.
  return {ButtonRouting::None, false, false};
}

bool anyBound(const companionble::ButtonRouting* routings, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (routings[i] != companionble::ButtonRouting::None) return true;
  }
  return false;
}

bool isGalleryNavAction(companionble::ButtonRouting action) {
  return action == companionble::ButtonRouting::LocalGalleryPrev ||
         action == companionble::ButtonRouting::LocalGalleryNext;
}

}  // namespace companionbuttons
