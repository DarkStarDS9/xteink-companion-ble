# TODO: rename "foreground" to "acquired"

## Problem

"Foreground" is used throughout the companion code to mean "this peer/session
is the one currently connected and driving the display" -- **not** "the phone
app is in the foreground of the phone's UI." The two meanings are easy to
conflate when reading the code cold, and the mixed-up expectation has caused
confusion at least once.

The protocol already has the right word for this concept: **ACQUIRE/RELEASE**
are the existing verbs (`docs/companion-display-protocol.md`), and what
"foreground" currently names is exactly the state ACQUIRE produces. **"Acquired"**
is the natural replacement -- it matches vocabulary already in use (e.g. "ACQUIRE
now granted" in the e2e harness) and reads better than generic alternatives like
"active"/"current", which are already used for other unrelated things nearby
(e.g. `setForegroundActive`'s own `active` bool parameter would collide
awkwardly with an "active" rename).

## Scope survey (as of companion @ 8b57a0a3, 2026-08-07)

~275 occurrences of `foreground`/`Foreground` across 19 files, splitting into
two categories:

### 1. Pure internal C++/Python symbols -- free to rename, no cross-repo impact

~205 occurrences, 13 files. The wire byte (`0x84`) never changes; only these
names would:

- `src/CompanionBle.h`: `foregroundSessionId()` (:309), `foregroundPeerKey()`
  (:316), `ForegroundChangeCallback` (:428), `setForegroundChangeCallback()`
  (:429)
- `src/CompanionBle.cpp` (~46 occurrences): `kSessForeground` (:48, mirrors
  wire opcode 0x84 -- rename the symbol, not the value), `g_foreground`,
  `g_foregroundCb`, `g_pendingForegroundReady`, `g_pendingForegroundName`,
  `g_pendingForegroundKey`, `applyForegroundChange()`, `notifyForeground()`,
  `foregroundPushedImageThisSession`
- `src/CompanionConnPolicy.h/.cpp` (~12): `setForegroundActive()` (.h:119),
  `noForegroundSinceMs_` (.h:178), plus comments
- `src/CompanionPeerStore.h:17`: comment reference
- `src/CompanionTestConsole.h/.cpp` (~6): uses `foregroundPeerKey()`/
  `foregroundSessionId()`; debug console field `foreground=%u`
  (CompanionTestConsole.cpp:77) -- private text debug protocol between
  firmware and the e2e Python script, not consumer-facing
- `src/activities/companion/CompanionModeActivity.h/.cpp` (~76, heaviest
  use): `onForegroundChange()`, `syncConnPolicyForegroundState()`,
  `refreshGalleryForForeground()`, `gotForeground`, `newForegroundName`,
  `newForegroundKey`, `current_foreground`
- `test/companion_conn_policy/CompanionConnPolicyTest.cpp` (22): test names
  like `ForegroundReacquisitionResetsTheHoldoffClock`, calls to
  `setForegroundActive`
- `scripts/companion_protocol.py:89`: `SESS_FOREGROUND = 0x84` (Python mirror
  of the opcode)
- `scripts/companion_e2e_test.py` (24): `outcome[0] == "foreground"` tuple
  tag, `console.state().get("foreground")` (reads the test-console debug
  string, not the wire)

### 2. Documented protocol vocabulary -- needs cross-repo coordination

The BLE notification opcode `0x84` is documented by the name `FOREGROUND` in:

- `docs/companion-display-protocol.md:376` -- `0x84 FOREGROUND sessionId`
- `docs/companion-multi-app-design.md:304`

Renaming this is a shared-vocabulary change, **not a wire break** (the byte
value is unaffected), but consumer-app repos (SpokenFeeds, Snap2Ink) may
reference the name `FOREGROUND` in their own code/docs/comments. Do this in
its own commit, separate from the internal rename, and give those repos a
heads-up (or update in lockstep) rather than letting their references go
stale silently.

## Suggested plan when this gets picked up

1. Commit 1: rename all internal C++/Python symbols (category 1 above) to
   "acquired" vocabulary -- e.g. `acquiredSessionId()`, `acquiredPeerKey()`,
   `AcquiredChangeCallback`, `setAcquiredActive()`. Mechanical, no behavior
   change, host tests should pass unchanged (rerun
   `scripts/run_companion_tests.sh`).
2. Commit 2 (separate, coordinated): rename the documented `FOREGROUND`
   notification name to `ACQUIRED` in both protocol docs, and message the
   SpokenFeeds/Snap2Ink sessions (via agent-mailbox) about the vocabulary
   change so their code/docs can follow.
