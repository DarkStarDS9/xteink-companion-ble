# BLE companion link: measurement log

Running record of what has actually been **measured** on hardware, as opposed to designed
([ble-companion-ideal-architecture.md](ble-companion-ideal-architecture.md)), researched
([ble-companion-do-and-dont.md](ble-companion-do-and-dont.md)) or inferred
([ble-companion-gap-analysis.md](ble-companion-gap-analysis.md)).

Newest session first. Each entry records the build, the method, what it proved, what it **dis**proved,
and what it could not see. Hypotheses that died are kept, not deleted — a disproved hypothesis is the
most expensive kind of knowledge to re-acquire.

---

# CURRENT STATE — read this first

**As of 2026-08-07, consolidated (second pass, same day).** This section is the handoff, and it
tracks **every live line, not just the one you are sitting in** — a session that updates only its
own line recreates the cross-worktree amnesia the earlier 2026-08-07 consolidation had to undo.
Cross-repo sessions (e.g. run from a SpokenFeeds checkout) that touch firmware must update this
section too. Before analyzing anything, run `git worktree list` and `git cherry companion <branch>`
for every listed branch.

## Where the code is

No unlanded lines remain. `companion` (this commit) is the consolidated trunk: parameter fixes,
render instrumentation, the 2026-08-04 DLE root-cause (DLE requested nowhere), the
disconnected-page-buttons fix, `worktree-bridge-cse_01AXp4…`'s pair (`81f0947c` Idle holdoff +
`bee97fe1` latency-0-while-foreground), `CompanionBatchModel` + `CompanionConnPolicy` host suites
(149/149), the e2e harness's spokenfeeds/soak modes, and today's DLE A/B proof.

**The reader currently runs the trunk TEST build** (`pio run -e test`, serial console active) —
not a stale unlanded build; a reflash from trunk changes nothing behavioural.

## What is proven

- Guideline violations were real; fixing them made iOS grant our requests (60 of 61). iOS grants
  Interval Max essentially always.
- The controller wakes on demand: peripheral→central is never delayed by latency.
- Render path ~550 ms total (~382 ms panel waveform). Push-to-visible ~1.2–1.5 s.
- **The 40 s `0x22` was the DLE request at connect.** Every captured `0x22` sits at
  39992–40001 ms = TPRT, a spec constant anchored at connect. Removing `setDataLen()` from
  `onConnect` took a link that had never survived 40 s to a 7m20s soak (2026-08-04, real iPhone).
  Two further `0x22` at exactly 40001 ms on 2026-08-07 — on a build with the Session/Idle rework
  and `setDataLen` still present — ruled the profile design out. The 2026-08-06 capture of iOS
  completing `LL_LENGTH_REQ → RSP` on a healthy connection is not a counterexample: procedures
  racing at connect sometimes collide, and a collided procedure times out at exactly TPRT.
- Peripheral latency is not free for phone→reader traffic: latency 10 multiplied every inbound
  duration ~12× (title 30→361 ms, body 31→390 ms, batch commit 242→~2900 ms) and made the 3 s
  batch safety net a coin flip. Latency belongs to `Idle` only.
- The sleep-inhibit fix `fefd310d` is hardware-verified (user, 2026-08-07, including physical
  unplug).
- The commit→`RENDER_STATUS` latency measurement now has n=2, not n=1: the iPhone
  (`reader_serial7.log`) and the macOS e2e harness (21/21 pushes, wire times 56–74 ms,
  commit→`RENDER_STATUS` medians ~1.2 s) independently land in the same range.
- **The DLE fix is host-A/B-provable, and was proven that way, on 2026-08-07.** Same Mac central,
  same day, same harness: a DLE-carrying build died at 39996 ms with HCI `0x22` (TPRT); the same
  build with the request removed then survived a 3-minute soak (`--soak 3`, session held
  foreground, two content-push rounds). This closes Q1/N3-class doubt and disproves the standing
  claim that a macOS harness cannot show DLE behaviour — macOS was assumed to self-negotiate DLE
  harmlessly and does not.

## Known open defects

1. **A batch resolved by the 3 s safety net sends no `RENDER_STATUS`** (pushId is 0 on that path).
   Cheap fix, not a protocol change: every field's END already carries the batch's pushId on the
   wire — latch it from the batch's *first* END and answer late instead of never. Separately, 3 s
   is tuned like a slow-link timeout when its real job is "the phone died" — reconsider value and
   trigger (link-idle, not wall time). Now fixable red-first against `CompanionBatchModel` — the
   seam already exists, use it.
2. **SpokenFeeds gates audio on the render ack** — app-side; converts any firmware hiccup into
   user-audible silence.
3. Three notifications per plain button tap (the middle hold-tick is redundant for a tap).
4. G6 content-id readback: never started; cheapest way to stop a reconnect re-pushing content the
   panel already shows.
5. Snap2Ink must stop routing Up/Down to Remote (dependency from the 2026-07-30 gallery-nav
   revert; never checked since).

## What to do next, in order

1. **Safety-net fix, red-first** against `CompanionBatchModel` (defect 1 above) — the extraction
   makes this a host-test-first fix rather than another hardware-only guess.
2. **SpokenFeeds audio coupling** (defect 2) + a reconnect-reACQUIRE check: a session that pushes
   without holding foreground gets silence by design (`RENDER_STATUS` is withheld for non-foreground
   sessions), which is worth an app-side audit — it is easy to trip into from the app side without
   noticing, as today's harness gap (spokenfeeds silently depending on buttonmap having run first)
   showed on the firmware side.
3. **Q5, the battery A/B discharge** — gates all remaining power work (including R1 and Idle's
   latency value). Do not start power work before it.

## How this went wrong, so it does not repeat

The three measurement corrections from the 2026-08-04→07 stretch stand: use firmware `millis()`,
never host-side serial timestamps; instrument the interval you want, never difference adjacent log
lines; **measure the change you ship, not just the principle behind it** — a verified mechanism
does not validate the policy wrapped around it.

The worst failure was **cross-worktree amnesia**: the 2026-08-04 session that root-caused the 40 s
`0x22` ran from a SpokenFeeds project dir and committed into a firmware worktree; no firmware-side
handoff ever saw it. Its proven fix sat unlanded for three days while a parallel line
re-investigated the same bug and reached a weaker conclusion. Hence the rules at the top of this
section.

---

## Session 5 — 2026-08-07, consolidation: DLE A/B proof, landings, harness fix, kit extraction

### Proved

- The DLE fix is host-A/B-provable from the Mac alone: a DLE-carrying build died at 39996 ms
  (HCI `0x22`, TPRT) against the macOS central via `scripts/companion_e2e_test.py`; the same
  harness against DLE-free trunk then survived a 3-minute soak (`--soak 3`, session held
  foreground, two content-push rounds). Same Mac, same day, same harness — the only variable was
  the DLE request. This closes Q1/N3-class doubt.

### Disproved

- The standing claim that "a macOS harness cannot show this [DLE]" — macOS was assumed to
  self-negotiate DLE harmlessly, so it would neither benefit nor fail either way. It does fail,
  identically to the original iPhone failure mode, when the request is present.

### Other work landed today

- `worktree-bridge-cse_01AXp4…`'s pair (`81f0947c` Idle holdoff, `bee97fe1` latency-0-while-foreground)
  landed on `companion` — no unlanded lines remain (see "Where the code is" above).
- `CompanionBatchModel` + `CompanionConnPolicy` host suites reached 149/149.
- Found and fixed a harness bug: the spokenfeeds e2e group silently depended on the buttonmap
  group having run first (it needs the session to hold the screen; `RENDER_STATUS` is deliberately
  silent for non-foreground sessions). Running `--only enrollment,spokenfeeds` produced three
  misleading timeout FAILs. spokenfeeds now checks foreground and arranges its own if buttonmap
  didn't run first, failing fast with one clear message if it can't get the screen.
- CompanionKit (the Swift reference client) extracted with full history to its own repo,
  https://github.com/DarkStarDS9/CompanionKit (tag `11.0.0`); both consumer apps now depend on it
  via SPM instead of a vendored copy under `clients/swift/CompanionKit`.

### The 01AXp4 line's independent read of the same morning (landed from `60456d44`)

The parallel worktree session analyzed `reader_serial7.log` on its own at 13:31 and produced the
cleanest summary table of the optimization arc — kept here because it is the before/after record in
one place:

| | Old ladder | Latency-10 regression | Latency-0 + holdoff |
|---|---|---|---|
| Title / body wire time | 298–598 ms | 360–690 ms | **30 / 31 ms** |
| Batch commit | 634 ms median | 2916 ms median | **242 ms** |
| Parameter negotiations | 2–3 per article | flapping | **1 per connection** |
| Safety-net fires | occasional | 4 of 11 | **0** |

It also recorded a 2.1-hour connection ending in a clean `0x13`, and the boot-time `0x22` at
40.001 s whose parameter negotiation the logger missed — and concluded, correctly on its evidence,
that the `0x22`'s "cause is still unidentified." **Superseded the same afternoon:** that build still
fired the DLE request in `onConnect` (`setDataLen` was only removed on trunk, `ca5e518a`), and the
host A/B above identified the cause and proved the fix. The entry stands as written because reaching
a weaker conclusion from a narrower window is exactly what the doc's ethos says to preserve.

---

## Equipment

- **Sniffer:** SONOFF ZBDongle-P (CC2652P + CP2102N) running NCC Group **Sniffle 1.11.0**
  (`sniffle_cc1352p1_cc2652p1.hex`). Tooling in `~/tools/sniffle` (venv, `cc2538-bsl`, `python_cli`).
  Flashed via the CP2102N's automatic bootloader entry — no BOOT button needed.
- **Capture:** `sniff_receiver.py -s /dev/cu.usbserial-21230 -m <READER_MAC> -o out.pcap`.
  Must be started **before** the phone connects: Sniffle derives the hop parameters from the
  `CONNECT_IND` and cannot join a connection already in progress. Filter on the **reader's** MAC.
- **Reader:** `7C:E8:B1:6F:02:16`, public address, legacy `ADV_IND`, ~−39 dBm at desk range.
- **Serial:** reader on `/dev/cu.usbmodem212401` at 115200, logged with relative timestamps.
  **The serial logger holds the port — stop it before flashing**, or `esptool` fails with
  "No serial data received".
- Artefacts kept in `~/tools/sniffle/captures/` (untracked; this is a public repo).

---

## Session 1 — 2026-08-06, ~10 min, real iPhone app, ~28 article pushes

**Build:** `98cac367` (text-path instrumentation) + `049363f9` (guideline-compliant parameters).
**Method:** sniffer + reader serial on one timeline; audio playback through ~28 article changes,
including idle spells long enough to reach both relax stages.

### Proved

**P1. The guideline-compliance fix was a real defect fix.** 60 of 61 parameter requests were granted.
iOS now grants Near (75 ms, latency 2) and Deep (165 ms, latency 1) — the profiles whose 12 s
supervision timeout and min==max interval previously had it declining. Grant census:

| Granted | Count |
|---|---|
| 15 ms, latency 0 | 28 |
| 75 ms, latency 2 | 28 |
| 165 ms, latency 1 | 4 |

**P2. iOS grants Interval *Max*, always.** Asked 60–75 → got 75 ms, on every one of 28 grants.
Asked 150–165 → got 165 ms. The spread required for compliance therefore made the link *slower*
than the profile intended, until corrected in `0fb3440e` (ask 45–60 to run at 60).

**P3. A parameter request issued while another is outstanding is silently lost — traced end to end.**

```
307.163  requested deep  (120-132, latency 1)
307.476  requested busy  (12-12,  latency 0)   <- 313 ms later, deep still outstanding
308.180  GRANTED deep: 165 ms, latency 1        <- the central answered the FIRST request
310.478  ERR conn params NOT honoured — asked 12-12, link is 132 / latency 1
310.710  ERR content batch commit flag missed after 3000 ms
```

The push ran at Deep's 330 ms effective interval, its final field's END missed the 3 s batch window,
and the article reached the panel ~6 s after it was sent. Because the old code latched the profile at
*request* time, the firmware believed it was Busy and never retried. Fixed in `0fb3440e`.

**P4. Push-to-visible decomposes as follows** (n=28 batches):

| Term | Median | Worst |
|---|---|---|
| Title (field 0x01) wire time | **373 ms** | 658 ms |
| Body (field 0x02) wire time | 152 ms | 512 ms |
| First field → batch commit | **677 ms** | **3009 ms** |
| Commit → visible (panel) | **~3.0 s** | — |
| **Total push → visible** | **~4.0 s** | ~6 s |

**P5. The 373 ms title time is a constant, not a transfer.** It appeared in 17 of 28 pushes, and a
19-byte title costs the same as an 80-byte one. It is the connection-parameter ramp from the relaxed
profile back to Busy, paid on the first field of **every article**. The body, arriving after the ramp
has landed, costs 152 ms.

**P6. ~~The panel is ~3.0 s from commit to visible.~~ WITHDRAWN — this was a measurement error.**
See the Session 2 correction below. The host-side timestamps used here are distorted by USB serial
burst delivery and must not be used for sub-second timing; the firmware's own `millis()` (the second
bracketed field on every line) is the only trustworthy clock in these logs.

**P7. Link stability under compliant parameters.** ~600 s of continuous connection, **zero `0x22`,
zero `0x08`**. The one disconnect was `0x13` remote-user-terminated — the app closing the link
deliberately — confirmed in the capture as a clean `LL_TERMINATE_IND`.

### Disproved

**D1. "Three LLCP procedures colliding in `onConnect` cause the 40 s `0x22`" — not supported.**
This was the leading hypothesis of the gap analysis (G1). The capture shows the connect sequence
completing cleanly in ~370 ms with **no `LL_REJECT_EXT_IND` and no `LL_UNKNOWN_RSP` anywhere in the
entire capture**:

```
97.519 CONNECT_IND
97.531 LL_VERSION_IND x2
97.561 LL_FEATURE_REQ + LL_PERIPHERAL_FEATURE_REQ   <- both sides, 0.3 ms apart
97.592 LL_PHY_REQ -> LL_PHY_RSP -> LL_PHY_UPDATE_IND
97.651 LL_LENGTH_REQ -> RSP  (twice: ours racing the central's)
97.861 LL_CONNECTION_PARAM_REQ -> LL_CONNECTION_UPDATE_IND
```

Both sides genuinely do initiate simultaneously — visible in the feature exchange and the duplicated
data-length procedure — and both stacks handle it. **Caveat:** no `0x22` occurred during this
session, so this describes a healthy connection, not a failing one. It weakens "collision at connect
is the norm"; it does not prove collisions never happen.

The harm that *was* observed from parameter churn is P3 — a collision between the firmware's **own**
two requests, mid-session, not between firmware and central at connect.

### Could not see

- **Nothing after t=142.6 in the capture.** Sniffle lost connection sync ~45 s in, most likely across
  the `LL_CHANNEL_MAP_IND` updates at t=103–117. So the LLCP record covers connection setup only —
  the P3 failure at t≈307 is known from the serial log, not the air. Long connections need either
  repeated short captures or a different tool.
- **No `0x22` was captured at all**, so the original symptom remains unreproduced under the new
  parameters. Encouraging, not conclusive: the pre-fix failure was intermittent and grant-dependent.
- **No power data.** The reader was USB-powered for the serial log throughout.

---

## Session 2 — 2026-08-06, 42 pushes, post-fix

**Build:** `0fb3440e` (deferred/retried parameter requests + interval-max correction).

### Fixes verified

| Check | Result |
|---|---|
| Granted intervals | **60.00 ms** (39) and **150.00 ms** (9) — the values actually wanted |
| `conn params NOT honoured` | **0** (was 1) |
| New `unanswered … retrying` path | fired twice and recovered |
| Batch commit | median 634 ms, max 2770 ms — no longer reaching the 3 s timeout |
| Disconnects | none |

Two grants came back at 58.75 ms and 148.75 ms, i.e. *just inside* the requested range rather than at
the max — so "iOS always grants Interval Max" (P2) is very nearly, but not strictly, true.

### P8 — the panel is not the bottleneck, and P6 was wrong

Recomputed on the firmware clock across 42 pushes:

| Stage | Mean | Owner |
|---|---|---|
| Batch commit → render starts | **531 ms** | firmware — render-task handoff |
| `clearScreen` → `displayBuffer` | 167 ms | firmware — layout + drawing |
| `displayBuffer` → complete | **485 ms** | **the panel** |
| First field → batch commit | ~634 ms | firmware + link (incl. the ramp, P5) |
| **Total push → visible** | **~1.8 s** | |

The underlying waveform wait is `X3_DRF (382 ms)` in 33 of 45 renders, with three at ~936 ms — the
periodic full refresh from `displayWithRefreshCycle`, **the same helper the reader uses for ordinary
page turns**. So the companion path is not using a slower waveform than a page turn, and the panel is
the *smallest* of the four terms.

**This reverses the gap analysis's framing.** Three of the four terms are firmware-side; the panel is
not where push-to-visible time goes. The largest single item, the 531 ms between committing a batch
and the render starting, has never been examined.

Unattributed: the driver reports 382 ms while wall-clock around `displayBuffer` is 485 ms.

### Method error worth remembering

P6 claimed a ~3.0 s panel. That came from timestamping serial lines as the **host** received them.
USB serial delivers in bursts: in one worked example 1 ms of host time covered 1924 ms of firmware
time. Every sub-second figure taken that way is meaningless. The firmware's `millis()` is present on
every log line and is the only clock to use.

---

## Session 3 — 2026-08-06, 17 renders / 13 pushes, render pipeline instrumented

**Build:** `bae14ec9` (per-render `queued->notify / notify->lock / render` split in `ActivityManager`).

### P9 — there is no handoff latency and no lock contention. The 531 ms did not exist.

| Interval | Mean | Max |
|---|---|---|
| `requestUpdate()` → `xTaskNotify` | **0.3 ms** | 1 ms |
| notify → `RenderLock` acquired | **0.0 ms** | 0 ms |
| `render()` itself | 913 ms (skewed by two outliers) | — |

Typical `render()` durations: 473, 535, 542, 544, 554, 563, 569, 575, 578, 580, 582, 627, 643, 716,
800 ms, plus two at ~3300 ms.

**Both hypotheses for the 531 ms are dead.** It was neither the deferred-notification path nor
`RenderLock` contention with an in-flight render — both measure at essentially zero. The time is
*inside* `render()`, and it always was.

Reconciling with the existing draw log: `clearScreen`→`displayBuffer` is 167 ms mean (P8) and the
panel waveform is 382 ms, which sums to ~549 ms — matching the typical `render()` figure directly.
So:

```
render() ~= 167 ms  layout + drawing   (ours)
         +  382 ms  panel waveform     (physics)
         =  ~550 ms
```

**Corrected total: push → visible ≈ 1.2–1.5 s**, split roughly ~634 ms first-field→commit (of which
300–600 ms is the parameter ramp, P5) and ~550 ms commit→visible.

The two ~3300 ms renders are presumably the periodic full refresh; not isolated, and with n=17 the
tail is not well characterised.

### Method error, the second of the same kind

P8 attributed 531 ms to a "render-task handoff" by differencing the timestamps of *adjacent log
lines* — commit, then the `clearScreen`→`displayBuffer` line — and assuming the gap was scheduling.
It was not: those intervals overlap, because the draw log is emitted partway through the very work
being measured. Direct instrumentation shows the handoff is ~0.

Twice now a confident number has come from inferring durations from log-line adjacency (first the
host clock in P6, then overlapping intervals here). **Instrument the interval you want to measure;
do not difference two lines that happen to sit near each other.**

---

## Session 4 — 2026-08-06, latency wake-on-demand test

**Build:** `f1c261a2`, a throwaway experiment (since reverted): ladder disabled, link pinned to a
single negotiation of interval 30 ms / **latency 30** / timeout 6 s. 12 button presses, which
produced 56 notifications (each press emits press + hold-ticks + release, ~300 ms tick cadence).

**Question:** peripheral latency is a permission to skip, not an obligation. Does *this* controller
still transmit at the next anchor point when it has data queued, or does it hold data until its next
scheduled wake? The whole fixed-parameter design depends on the answer.

### P10 — it wakes on demand. Confirmed.

- **iOS granted latency 30** outright: `interval=30.00ms latency=30 timeout=6000ms`.
- **Latency was genuinely in force** (the check that stops this being a vacuous test): the peripheral
  participated in only **562 of 3724** connection events, and the dominant idle gap between events it
  attended was exactly **31** (= latency + 1), 98 times. It really was sleeping through 30 events.
- **All 56 notifications reached the air within 0–4 connection events (0–120 ms)** of being queued.
  None waited for the ~930 ms boundary.

**Confounders checked.** The central transmitted at ~99% of all events, yet the peripheral still
attended only 15% — so constant central traffic does not drag it awake. Of the 14 "cold"
burst-openers, only 1 had non-empty central traffic nearby; the other 13 cannot be attributed to a
central-initiated exchange. One burst fired ~450 ms *before* the mandatory deadline, which is only
explicable as interrupt-driven. Sniffer sync was clean for the whole connection. Reader-to-capture
clock correlation carries ±20–50 ms uncertainty, which is why a few deltas read slightly negative;
it does not affect the event-gap conclusion.

**Also:** no `0x22` and no `0x08` across the session, at latency 30 — far beyond the latency 4 that
historically correlated with `0x22`. Consistent with renegotiation having been the culprit rather
than the latency value. One session is not a soak; not settled.

### The constraint this test also established

The peripheral wakes early only when **it** has data to send. It cannot pre-emptively listen harder
for *incoming* data — no host API exists for "ignore latency for a while". So a phone→reader push
still pays up to `latency × interval` on its **first packet**, and the only way to vary that
dynamically is renegotiation, i.e. the thing being removed. Latency is therefore a standing trade,
chosen once:

| Config | Mean first-packet delay | Worst | Idle wake-ups |
|---|---|---|---|
| Old ladder (60 ms/lat 2 + ~360 ms ramp) | ~450 ms | ~540 ms | 5.5/s |
| **Fixed 30 ms, latency 10** | **150 ms** | 300 ms | **3/s** |
| Fixed 30 ms, latency 30 | 465 ms | 930 ms | 1.07/s |

Latency 10 beats the old ladder on *both* axes, so it is not a compromise. Latency 30 trades a worse
tail for more saving and is deferred to Q5 (battery).

Latency is self-cancelling during traffic — once packets flow the peripheral is not skipping — so
bulk image transfers need no separate profile.

---

## Resolved design questions

### R1 — "Can the phone hint a quiet period, and the reader wake itself just before it ends?"

Asked 2026-08-07. Proposal: the phone pushes an article plus "this will play for 32 s"; the reader
sleeps deeply and, at ~T−1 s, simply stops skipping connection events — no renegotiation — so it is
fully attentive by the time the next article arrives.

**Verdict: the scheme is sound and buildable. The "stop skipping" step needs a parameter update on
this stack, but that update now costs ~180 ms and does not slow data, so the scheme works as
described.**

**At spec level the premise is correct.** Peripheral latency is a maximum permitted skip, not an
obligation; a peripheral may attend every connection event whenever it chooses. "Stop skipping" is
legal with no air exchange. The obstacle is the stack API — latency is applied by the *controller*,
and the host needs a way to command it.

**NimBLE has no such command.** Verified by reading the vendored source in
`.pio/libdeps/default/NimBLE-Arduino/src/nimble/nimble/host/`, not inferred:

| Path | What it is |
|---|---|
| `ble_gap_update_params()` → `NimBLEServer::updateConnParams()` | full connection-parameter update procedure |
| `ble_gap_subrate_req()`, `ble_gap_set_default_subrate()` (`ble_gap.h` ~3343/3363) | BT 5.3 Connection Subrating; HCI `0x007D`/`0x007E` (`hci_common.h:1233,1242`) |

Greps for `slave_latency_disable`, `periph_latency.*disable` and force-awake variants returned
**empty** outside those two paths. For comparison, Nordic's SoftDevice *does* expose the local,
no-air-exchange toggle as `BLE_GAP_OPT_SLAVE_LATENCY_DISABLE` via `sd_ble_opt_set()` — the capability
is real, just absent here. **SOURCE-VERIFIED.**

**Connection Subrating is the standardised form of this idea, and is unavailable on three counts:**
compiled out in this build (`CONFIG_BT_NIMBLE_SUBRATE` unset → `MYNEWT_VAL_BLE_CONN_SUBRATING` = 0);
ESP32-C3 *controller* support unconfirmed (Espressif advertises "BLE 5.4 certified", which does not
imply every optional 5.3 feature is implemented); and **iOS central support unconfirmed, leaning
negative** — no Apple documentation found either way. And even fully enabled it is still an air
exchange (`LL_SUBRATE_REQ`/`LL_SUBRATE_IND`), i.e. negotiation-lite, not a local flag.

**Why the fallback is nevertheless cheap now.** The old objection priced a parameter update at
~900 ms, because the old design moved the **connection interval** and the instant is ~6 events
counted at the *old, slow* interval — with data crawling throughout. With the interval fixed at
30 ms and only latency changing, the same procedure is **~6 × 30 ms ≈ 180 ms, and the link runs at
full speed the whole time it is pending**. A T−1 s wake-ahead has roughly 5× the margin it needs.

**Cost of building it:** two control procedures per article (sleep after the push, wake before the
article ends). That is the churn pattern behind the dropped-request failure (P3) — but that bug is
fixed (deferral + retry), the interval no longer moves, and a 1 s margin is not a 313 ms race.

**Why it is not being built yet.** The benefit is unquantified: roughly 3/s → 1.07/s idle wake-ups,
against a battery nobody has measured (Q5). The A/B discharge test settles it and needs no protocol
work. If wake-ups dominate, this is worth building and the design is ready; the phone-side shape is
already sketched in [ble-companion-ideal-architecture.md](ble-companion-ideal-architecture.md) §3.2.

**One free variant needs no protocol at all:** when the user skips using the *reader's* buttons, the
reader is the one sending the button event, so it already knows a push is imminent and can drop its
own latency at that instant without being told. In companion mode the unpredictable input arrives at
the reader first, which makes the hint scheme more reliable, not less.

---

## Open questions, in priority order

**Q1. ~~Does the `0x22` still occur at all under compliant parameters?~~ CLOSED 2026-08-07.**
The macOS A/B (Session 5) reproduced the collision on demand with the DLE request present and
showed a clean 3-minute soak with it absent, same build otherwise. That is the causal proof this
question was waiting on — a longer real-traffic soak (30+ min) is still worth doing before power
work, but as confidence-building, not to establish causation.

**Q2. ~~Is the 373 ms ramp worth paying on every article?~~ RESOLVED — the ramp is gone.**
The question assumed the interval had to move. It does not: peripheral latency delivers the same
idle saving with no renegotiation, and P10 confirmed this controller wakes on demand. The ladder was
replaced with two fixed profiles chosen by session state (latency 10 with a foreground session,
30 without), negotiated once per connection. **Unverified in the field** — needs a session on the new
build to confirm the ramp has actually disappeared from title wire times.

**Q3. ~~Why is there 531 ms between a batch committing and the render starting?~~ ANSWERED: there
isn't.** Handoff and lock contention both measure ~0 (P9). The render pipeline is ~550 ms, of which
~382 ms is the panel waveform and ~167 ms our layout and drawing. **There is little left to win
here** — the remaining controllable term in push-to-visible is the parameter ramp (Q2), not
rendering.

**Q3b (new, low priority). What are the ~3300 ms renders?** Two of 17. Presumably the periodic full
refresh from `displayWithRefreshCycle`. If they land on article pushes rather than on idle redraws,
they are worth moving; if they are already opportunistic, they are free. Not isolated yet.

**Q4. Does the peripheral really transmit at the next anchor point when latency is high?**
The sleepy-mode design in the architecture doc rests on this and it is still unverified. Test: set
Deep's latency high, press a button, measure. If it fails, that design should be deleted rather than
disabled.

**Q5. Battery.** No measurement has ever been taken on this fork; there is no current-sense hardware,
so the plan of record is an A/B discharge-to-empty rather than a bench meter. **This is now the
gating measurement for the whole power line of work** — R1 (the phone traffic hint), the choice
between latency 10 and latency 30, and whether idle wake-ups matter at all against the framebuffer
and panel, all wait on it. Everything it gates is designed and ready; none of it should be built
until it says the gain is real.

**Q6 (new). Does the ESP32-C3 controller implement LE Connection Subrating, and does iOS support it
as central?** Both unconfirmed (R1). If both are yes, enabling `CONFIG_BT_NIMBLE_SUBRATE` would give
a lighter mechanism than a full parameter update for R1's wake-ahead. Low priority: the parameter
update is already cheap enough at a fixed interval, so this would be an optimisation of something
that is no longer expensive.

---

## Method notes worth not re-learning

- **Stop the serial logger before flashing.** It holds `/dev/cu.usbmodem212401`; `esptool` then fails
  with "No serial data received", which reads like a hardware fault and isn't.
- **`grep` goes binary-silent on a log that spans a reflash.** Flashing injects NUL bytes into the
  capture; `grep -o` then matches nothing and prints nothing, rather than saying "binary file
  matches". An empty result from such a log is **not** a negative result, it is a failed check —
  use `grep -a`. Cost one wrong "the reader never connected" conclusion on 2026-08-07.
- **`system_profiler` is unavailable in this sandbox**; use `ioreg` to enumerate USB.
- **Don't identify a board by its `/dev/cu.*` name alone.** The reader (native USB CDC) is
  `usbmodem*`; the sniffer's CP2102N is `usbserial*`. Getting these the wrong way round risks
  flashing sniffer firmware onto the reader.
- **A capture that produced no output may not have run at all.** An early check here reported "no
  sniffer present" when the command itself did not exist — the failure mode the research rules in
  the DO/DON'T doc warn about, reproduced in miniature.
