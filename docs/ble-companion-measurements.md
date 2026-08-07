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

**As of 2026-08-07.** This section is the handoff. Everything below it is the historical record.

## Where the code is

| Branch | State |
|---|---|
| `companion` | up to `84c6b70b`. All research docs, the parameter fixes, the render instrumentation. **Good state.** |
| `worktree-bridge-cse_*` | two further commits **not landed**, both untested by a real session: `81f0947c` (Idle holdoff) and `bee97fe1` (drop latency to 0 while a session holds the screen) |

The two unlanded commits are flashed to the reader. They are deliberately unlanded because the two
changes before them were regressions that reached the user's hands — see "How this went wrong" below.
**Land them only after a session confirms them.**

The reverted experiment `f1c261a2` and its revert `801ca7fd` also show as unlanded by `git cherry`.
That is correct and intentional: they cancel out and `companion` never had either.

## What the link does now

One profile negotiation per connection, no per-article churn.

| Profile | Interval | Latency | Timeout | When |
|---|---|---|---|---|
| `Session` | 15 ms (min == max, the permitted equality) | **0** | 4 s | an app holds the screen |
| `Idle` | 15–30 ms (iOS grants 30) | 30 | 4 s | no foreground session for ≥ `kIdleHoldoffMs` (10 s) |

`Session` is requested in `onConnect`. `tick()` calls `requestConnParamsForSessionState()`, which
holds `Session` unless the screen has been unowned for the full holdoff.

## What is proven

- Guideline violations were real; fixing them made iOS grant our requests (60 of 61).
- iOS grants **Interval Max**, essentially always (2 of 82 grants came back just inside the range).
- Parameter requests were being **silently dropped** when one was already in flight; fixed with
  deferral + retry, and the profile now only moves on a matching grant.
- The controller **wakes on demand**: with latency 30 in force (562 of 3724 events attended), all 56
  notifications reached the air within 0–4 connection events. Peripheral→central is never delayed by
  latency.
- The **render path and the panel are fine**: ~550 ms total, of which ~382 ms is the panel waveform,
  via the same helper the reader uses for ordinary page turns. Handoff and lock contention are ~0.
- Push-to-visible is **~1.2–1.5 s**, not the ~4 s originally reported.

## What is NOT proven

- That the current build is good. **No session has run against `bee97fe1`.**
- That `0x22` is gone. It has not recurred since the guideline fix, but there has been no soak.
- Anything about battery. Never measured on this fork (Q5).

## Known open defects

1. **A batch resolved by the 3 s safety net sends no `RENDER_STATUS`.** `g_pendingBatchPushId` is 0
   on that path, so the firmware stays silent by design. The panel updates and the phone waits out
   its own render timeout. Firmware-side fix: answer *something* so a slow batch degrades to "late"
   rather than "silent". **Not yet done** — it is a protocol-semantics change.
2. **SpokenFeeds gates audio playback on the render acknowledgement.** Observed 2026-08-07: an
   article's `activateSessionAttempt` fired only after `render wait timed out after 3.0 seconds`,
   13 s after `playNewsStart`. This is an app-side coupling, not firmware, and it converts any
   firmware hiccup into user-visible silence. **Arguably the highest-value fix available**, and it
   inverts the fork's own dumb-firmware/smart-phone principle.
3. **Three notifications per button tap** (press + one hold tick + release). Deliberate — the phone
   needs live hold feedback — but for a plain tap the middle one is redundant.
4. **G6, content-id readback**, from the gap analysis: never started. Still the cheapest way to stop
   a reconnect re-pushing content the panel already shows.

## How this went wrong, so it does not repeat

Three corrections had to be issued during this work, and two of them shipped to the user as
regressions:

- The ~3 s panel figure came from **host-side serial receive timestamps**. USB serial delivers in
  bursts; 1 ms of host time covered 1924 ms of firmware time. Use the firmware's `millis()`.
- The 531 ms "render handoff" came from **differencing two adjacent log lines** whose intervals
  overlap. Instrument the interval you want; do not subtract nearby lines.
- **Latency 10 shipped on reasoning, not measurement.** The mechanism (wake-on-demand) was tested
  rigorously; the state machine built on top of it was not tested at all before being handed over.
  It regressed median batch commit from 634 ms to 2916 ms.

**The rule this implies: measure the change you are shipping, not just the principle behind it.** A
verified mechanism does not validate the policy wrapped around it. And when a symptom appears, stop
and re-plan rather than patching the symptom — the last stretch of this work was, fairly, described
as bug-driven development.

## What to do next, in order

1. **Run a session against the current build** and check: `conn params granted ... latency=0` once per
   connection, title/body wire times near 60 ms, batch commit well under 1 s, no `commit flag
   missed`. Then land `81f0947c` and `bee97fe1`.
2. **Fix open defect 1** (silent safety-net path) — small, firmware-local, removes a whole class of
   "screen updated but nothing happened".
3. **Raise open defect 2 with the app** — the audio/render coupling.
4. **Q5, the battery A/B discharge.** It gates every remaining power decision, including R1.
5. Only then revisit power: R1 (phone quiet-period hint) and Idle's latency value.

Do not start 5 before 4. The whole reason the power work went wrong was optimising an unquantified
gain.

---

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

**Q1. Does the `0x22` still occur at all under compliant parameters?**
600 s clean is suggestive, not proof — the historic failure was intermittent. Needs a long soak
(30+ min) with real playback traffic, serial only, no sniffer required.

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
- **`system_profiler` is unavailable in this sandbox**; use `ioreg` to enumerate USB.
- **Don't identify a board by its `/dev/cu.*` name alone.** The reader (native USB CDC) is
  `usbmodem*`; the sniffer's CP2102N is `usbserial*`. Getting these the wrong way round risks
  flashing sniffer firmware onto the reader.
- **A capture that produced no output may not have run at all.** An early check here reported "no
  sniffer present" when the command itself did not exist — the failure mode the research rules in
  the DO/DON'T doc warn about, reproduced in miniature.
