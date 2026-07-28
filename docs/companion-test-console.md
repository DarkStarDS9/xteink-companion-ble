# Companion Serial Test Console

A serial command surface for driving Companion Mode from a host machine, so the
v6 protocol can be tested automatically instead of by hand.

**Compiled out of every shipping build.** Only `[env:test]` defines
`COMPANION_TEST_CONSOLE`. Implementation: `src/CompanionTestConsole.{h,cpp}`.

```bash
pio run -e test -t upload --upload-port /dev/cu.usbmodem21201
```

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
| `CMD:CSTATE` | `CT:state screen=… connected=… sessions=… foreground=… peer=…` | Assertable snapshot |
| `CMD:CPEERS` | `CT:peers count=N` then one `CT:peer …` per peer | Enrolled peers with asset tags |
| `CMD:CCAP` | `CT:cap len=23 <hex>` | Capability block without a BLE read |
| `CMD:CUI` | `CT:ui …`, then one `CT:button …` and one `CT:tag …` per entry | The foreground app's declared buttons and tags |
| `CMD:CTAGS` | `CT:tags count=N`, then one `CT:tag id=… state=… label=…` | Live tag state — see below |
| `CMD:CBTN <id> [holdMs]` | `CT:btn id=… hold=…` | Inject a button press |
| `CMD:CRESET` | `CT:reset ok` | Delete all peer state — back to never-paired |

Button ids match the protocol's own: `0` BACK, `1` CONFIRM, `2` LEFT, `3` RIGHT,
`4` UP, `5` DOWN, `6` POWER.

`screen` is one of `start_failed`, `waiting`, `waiting_app`, `icon_grid`,
`pairing`, `text`, `image`.

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

### `CRESET`

Deletes every peer directory and the index. Without it, re-testing first-contact
enrollment means physically pulling the SD card between runs, which is the kind
of friction that stops a test suite from being run.

## Typical session

```
CMD:CRESET          -> CT:reset ok
                       (host now connects over BLE and sends HELLO)
CMD:CSTATE          -> CT:state screen=pairing connected=1 sessions=0 foreground=0 peer=-
CMD:CBTN 1          -> CT:btn id=1 hold=0
                       (host receives HELLO_OK)
CMD:CPEERS          -> CT:peers count=1
                       CT:peer key=a1b2c3d4 name=Dev Pusher ui=00000000 icon=00000000
                       (host pushes the button map, ACQUIREs, pushes content)
CMD:CUI             -> CT:ui peer=a1b2c3d4 buttons=4
                       CT:button id=2 routing=page_prev label=<
                       CT:ui tags=2
                       CT:tag id=0 label=Saved
CMD:CSTATE          -> CT:state screen=text connected=1 sessions=1 foreground=1 peer=a1b2c3d4
CMD:CBTN 0 1200     -> CT:btn id=0 hold=1200
                       (host receives ~12 button-event notifications then a final)
```

`CMD:SCREENSHOT` (upstream's, present in any serial-log build) dumps the
framebuffer, which is how a host checks that the icon grid and image render
actually drew something rather than trusting a state string.

## The harness

`scripts/companion_e2e_test.py` drives all of the above alongside BLE. See its
`--help`; it needs `bleak` and a serial port, and covers first-contact
enrollment with the on-device confirm, token-based silent reconnect, ACQUIRE
denial for a peer with no button map, preemption between two simulated apps, and
an image push.
