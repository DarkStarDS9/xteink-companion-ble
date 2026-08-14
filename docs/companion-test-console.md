# Companion Serial Test Console

A serial command surface for driving Companion Mode from a host machine, so the
v6 protocol can be tested automatically instead of by hand.

**Compiled out of every shipping build.** Only `[env:test]` defines
`COMPANION_TEST_CONSOLE`. Implementation: `src/CompanionTestConsole.{h,cpp}`.

```bash
pio run -e test -t upload --upload-port /dev/cu.usbmodem21201
```

**Put the shipping build back before the device returns to normal use.** The test
build is fine to leave on a device being worked on, but it is not what an end
user should be running:

```bash
pio run -t upload --upload-port /dev/cu.usbmodem21201
```

> **Status: both the console and the harness are verified on hardware.**
> Every command below has been exercised on a real X3 over USB serial, and as
> of protocol v11 `scripts/companion_e2e_test.py` runs green against a
> `[env:test]` build — 66 assertions, 0 failures — driving BLE and injected
> button presses at the same time. The macOS Bluetooth-permission problem
> described under "The harness" is still real and is still what decides whether
> it can run at all; see "Run it from a real terminal, not tmux".

## Why it exists

v6 enrollment requires a **physical CONFIRM press on the device** to accept an
unknown peer. A BLE script driving the handshake from a dev machine cannot
produce that press, so without this the single most important new path in v6 —
first-contact pairing — would stay manually tested forever, along with
everything downstream of it.

With button injection, one host can drive both halves of a test: BLE over its
own radio, buttons over USB serial. That is what makes
`scripts/companion_e2e_test.py` possible, and it is the first automated
on-target testing this repo has had.

## What it deliberately is not

A second control plane. Every command is either a read of state that already
exists or an injection at the same seam the physical buttons feed. **Nothing
here can do something a phone cannot do over BLE.** If it could, the tested path
would stop being the shipped path and the tests would be worth nothing.

Injection lands at `CompanionModeActivity`'s button-routing seam
(`buttonWasPressed()` and friends), so an injected press goes through the real
button map lookup, the real routing switch, the real hold-tick accounting and
the real notify path. The only thing it skips is GPIO debounce, which is not
what any of these tests are about.

## Memory

One `VirtualPress` struct (~16 bytes) and a function pointer. No line buffer —
commands arrive through `main.cpp`'s existing `CMD:` dispatcher and this only
borrows its `String`. Measured cost of the test build over the default build:
**+16 bytes RAM, +3.0 KB flash**, and the default build's RAM is unchanged.

## Commands

Every command is sent as a line prefixed `CMD:`. Every reply is prefixed `CT:`
so a host can pick replies out of the log stream without parsing timestamps.

| Command | Reply | Purpose |
|---|---|---|
| `CMD:CPING` | `CT:pong v6` | Liveness, and confirms a test build is flashed |
| `CMD:CSTATE` | `CT:state screen=… connected=… sessions=… foreground=… peer=… heap=…` | Assertable snapshot |
| `CMD:CPEERS` | `CT:peers count=N` then one `CT:peer …` per peer | Enrolled peers with asset tags |
| `CMD:CCAP` | `CT:cap len=23 <hex>` | Capability block without a BLE read |
| `CMD:CUI` | `CT:ui …`, then one `CT:button …` and one `CT:tag …` per entry | The foreground app's declared buttons and tags |
| `CMD:CTAGS` | `CT:tags count=N`, then one `CT:tag id=… state=… label=…` | Live tag state — see below |
| `CMD:CLIST` | `CT:list peer=… revision=… count=N`, then one `CT:listitem id=… checked=…` per entry | The foreground peer's stored ToDo List check-off diff — see below |
| `CMD:CLISTNAV` | `CT:listnav listIndex=… listCount=… cursor=… windowStart=… itemCount=…`, or `CT:listnav none: not on Screen::List` | Live ToDo List cursor/paging position — see below |
| `CMD:CBTN <id> [holdMs]` | `CT:btn id=… hold=…` | Inject a button press |
| `CMD:CRESET` | `CT:reset ok` | Delete all peer state — back to never-paired |
| `CMD:CRESETTEST` | `CT:reset ok removed=N` | Delete only peers named with the harness's `[E2E] ` prefix |

Button ids match the protocol's own: `0` BACK, `1` CONFIRM, `2` LEFT, `3` RIGHT,
`4` UP, `5` DOWN, `6` POWER.

`screen` is one of `start_failed`, `waiting`, `waiting_app`, `icon_grid`,
`gallery_picker`, `pairing`, `text`, `image`, `list`, `message`.

### `CBTN` and hold duration

`CMD:CBTN 1` is a tap: press and release land in the same loop iteration.

`CMD:CBTN 1 1500` holds CONFIRM for 1.5 seconds, which drives the *real*
repeat-while-held path — the device emits a button-event notification about
every 100 ms with a rising duration, then a final one with `isFinal` set. That
path is otherwise only reachable by a human holding a button, so it was
effectively untestable before.

### `CTAGS`

The Status characteristic is **write-without-response**: nothing tells an app
whether its tag write landed. This is the one piece of protocol state a BLE
client genuinely cannot confirm for itself, and reading it back over serial is
how a test asserts it did. It is also how the harness checks that tags survive a
content push (they must) and clear on a foreground handover (they must).

### `CLIST`

Reports the foreground peer's `list_state.bin` — the check-off diff the device
holds against the ToDo List document that peer pushed. Read-only, and justified
exactly as `CTAGS` is: it exposes no capability, only state already on SD.

Entries are the DEVIATIONS from the pushed document, not absolute checkbox
states (see `src/CompanionTodoDiff.h`), so `count=0` means "nothing pending",
whether the user never touched a checkbox or toggled one back to where the
document had it. `revision` is the document revision the edits were made
against; the device never interprets it. A peer with no stored diff at all
reports `revision=0 count=0`, the same as one with nothing pending.

The diff is cleared unconditionally whenever a new list document lands, so
`CMD:CLIST` immediately after a `kFieldListDoc` push always reports `count=0`.

### `CLISTNAV`

Reports `companiontodo::Nav`'s live cursor/paging position — the same numbers
`render()` reads — while `Screen::List` is up. Off that screen it replies
`CT:listnav none: not on Screen::List`.

This exists so a test can prove "the cursor moved" or "the list switched"
without a `CMD:SCREENSHOT` diff. A screenshot dump is not exclusive against the
device's own serial logging (see "One process, one serial port" below): a log
line landing mid-dump shifts every byte after it and the harness has to detect
and retry, which is fragile and was observed to abort a run outright on a
`[env:test]` build (`ENABLE_SERIAL_LOG` + `LOG_LEVEL=2`). `CLISTNAV` reads the
same state directly, at no shared-port risk at all.

`listIndex`/`listCount` are which list is showing and how many lists the
document has; `cursor` is the 0-based flat item index under the selection
marker; `windowStart` is the first visible row (paginates once `cursor` scrolls
past the viewport); `itemCount` is the current list's item count, ignoring
group headers — all four are `companiontodo::Nav`'s own accessors
(`src/CompanionTodoNav.h`), reported verbatim.

### `CRESET` / `CRESETTEST`

`CRESET` deletes every peer directory and the index. Without something like it,
re-testing first-contact enrollment means physically pulling the SD card between
runs, which is the kind of friction that stops a test suite from being run.

`companion_e2e_test.py` does not use plain `CRESET` — a reader's SD card can
carry real, manually-paired app registrations (snap2ink, SpokenFeeds, ...)
alongside the harness's own, and `CRESET` cannot tell those apart. Every
`Session` the harness creates is named with the `TEST_PEER_NAME_PREFIX`
(`"[E2E] "`) prefix (`scripts/companion_e2e_test.py`), and `CRESETTEST` only
deletes peers whose stored name starts with that — see
`companionpeer::forgetPeersWithNamePrefix()` (`src/CompanionPeerStore.cpp`).
The harness runs it both before a run (unless `--keep-peers`) and
unconditionally in a `finally` block after, so a crash mid-run doesn't leave
test peers behind to pile up toward the 32-peer cap and start evicting real
registrations. Plain `CRESET` remains for manual use — e.g. actually wanting to
return the device to never-paired.

## Typical session

```
CMD:CRESET          -> CT:reset ok
                       (host now connects over BLE and sends HELLO)
CMD:CSTATE          -> CT:state screen=pairing connected=1 sessions=0 foreground=0 peer=- heap=50724
CMD:CBTN 1          -> CT:btn id=1 hold=0
                       (host receives HELLO_OK)
CMD:CPEERS          -> CT:peers count=1
                       CT:peer key=a1b2c3d4 name=Dev Pusher ui=00000000 icon=00000000
                       (host pushes the button map, ACQUIREs, pushes content)
CMD:CUI             -> CT:ui peer=a1b2c3d4 buttons=4
                       CT:button id=2 routing=page_prev label=<
                       CT:ui tags=2
                       CT:tag id=0 label=Saved
```

A button entry using the byte-0 behaviour flags (see
`docs/companion-multi-app-design.md` §7) prints them as a suffix on the same
line, e.g. `routing=local_sleep label=Sleep +notify` or `label=Sync
+offline_only`; a button using both prints `+notify +offline_only`. `id=` is
always the masked 0-6 button id, never the raw declaration byte.

```
CMD:CSTATE          -> CT:state screen=text connected=1 sessions=1 foreground=1 peer=a1b2c3d4 heap=47764
CMD:CBTN 0 1200     -> CT:btn id=0 hold=1200
                       (host receives ~12 button-event notifications then a final)
```

`CMD:SCREENSHOT` (upstream's, present in any serial-log build) dumps the
framebuffer, which is how a host checks that the icon grid and image render
actually drew something rather than trusting a state string.

## The harness

`scripts/companion_e2e_test.py` drives all of the above alongside BLE. See its
`--help`; it needs `bleak` and `pyserial`, and covers first-contact enrollment
with the on-device confirm, token-based silent reconnect, `ACQUIRE` denial for a
peer with no UI declaration, an atomic content batch and the
`RENDER_STATUS(Displayed, pushId)` that answers it (including that `pushId` 0 is
answered with silence), both sequence-gap paths — a text batch that loses a
field must be discarded whole, not half-applied — a held-button round trip, tags
(atomic, state-only, and undeclared-id rejection), preemption between two
simulated apps on one link, and an image push checked pixel-for-pixel against
`CMD:SCREENSHOT`.

It carries no wire format of its own: that lives in
`scripts/companion_protocol.py`, shared with `scripts/push_companion_content.py`.
Keep it that way. The duplicate copy this harness used to carry froze at v6
while the pusher was kept current, and the harness then refused to start for
five consecutive protocol versions without anyone noticing.

```bash
pio run -e test -t upload --upload-port /dev/cu.usbmodem21201
python scripts/companion_e2e_test.py --port /dev/cu.usbmodem21201
```

### Run it from a real terminal, not tmux

macOS grants Bluetooth per *responsible process*. A process started inside tmux
inherits tmux's identity, and a plain binary like tmux generally cannot be
granted Bluetooth at all — the request fails immediately with
`BleakBluetoothNotAvailableError … DENIED_BY_UNKNOWN` and no permission prompt
is ever shown. The same applies to any agent or automation harness.

Open Terminal.app or iTerm directly and run it there, where macOS will prompt.
An Apple-signed interpreter (`/usr/bin/python3`) prompts more reliably than a
Homebrew build. The script runs on Python 3.9 upward.

This is what kept the harness from being executed for so long. It has since run
green on hardware from a shell with Bluetooth access; if yours is refused, that
is this permission problem and not the harness.

### One process, one serial port

The harness holds `--port` for its whole run. Leave a serial monitor (or a
second copy of the harness) attached and it will fail at `CMD:CPING` and tell
you to reflash a build it is already talking to.

`CMD:SCREENSHOT`'s dump is not exclusive against the device's own logging:
anything that logs while the framebuffer is streaming lands *inside* the dump
and shifts every byte after it. The harness detects that (the bytes between the
header and the footer no longer match the declared size) and retries rather
than diffing a shifted framebuffer, but a device logging heavily can make the
screenshot checks fail for reasons that have nothing to do with what was drawn.

The same exclusivity applies to flashing: a serial capture left running holds
the port, and `pio run -t upload` will fail against it. Stop the reader before
reflashing, and restart it after.

### Use `CBTN` rather than asking a human to hit a 30-second window

v6 enrollment needs a physical CONFIRM, and the on-device prompt times out after
30 s. Asking a person to press it on cue is the wrong reflex twice over: it
fails whenever they are not standing at the device, and it cannot be retried
unattended. Flash `[env:test]` and send `CMD:CBTN 1` — that is the entire reason
this console exists (see "Why it exists" above).

The enrollment token lives on the SD card, so it survives reflashing: one
injected CONFIRM enrolls a host for every later run, across firmware builds.
Only `CMD:CRESET` (or pulling the card) undoes it.

Reflash `[env:default]` when the automation is done. A build that can be driven
over serial should not be left on a device in normal use.

### A deep-slept device cannot be flashed, and only POWER wakes it

Companion Mode deep-sleeps `kWaitingIdleSleepMs` (5 minutes) after the last
central disconnects. The USB CDC port keeps enumerating afterwards, so
`/dev/cu.usbmodem*` is still listed and everything looks normal — but the CPU is
down, esptool fails with `Failed to connect to ESP32-C3: No serial data
received`, and toggling DTR/RTS produces nothing at all.

There is no software way out: serial, BLE and the reset line all need the part
that is powered down. It takes a physical POWER press — exactly the dependency
this console exists to avoid — so it is worth planning around rather than
rediscovering. The countdown starts when the phone disconnects, not when you
stop typing, so a long unattended soak that ends with the app closing the link
leaves about five minutes to start the next flash.

A different message, `Invalid head of packet (0x00): Possible serial noise or
corruption`, is the *other* upload failure and is usually transient — retry once
before assuming anything is wrong.
