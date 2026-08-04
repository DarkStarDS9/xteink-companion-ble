# BLE companion link: gap analysis

Phase 3 of the architecture review. Compares the implementation as of v11 against
[ble-companion-ideal-architecture.md](ble-companion-ideal-architecture.md), grounded in
[ble-companion-do-and-dont.md](ble-companion-do-and-dont.md).

Covers both sides: this firmware, and the `CompanionKit` Swift client that drives it.

**Three premises this review started from turned out to be wrong.** They are corrected in §0 first,
because two of them change the conclusions.

---

## 0. Corrections to the starting premises

**C1. There *is* a custom sdkconfig, and BLE modem sleep is enabled.**
`platformio.ini:148-160` sets `CONFIG_BT_CTRL_MODEM_SLEEP=y`, `..._MODE_1=y` and
`CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL=y` through a rebuild-on-first-build mechanism that recompiles
the ESP-IDF core libraries. This is not stock arduino-esp32 behaviour, and the Phase 1 finding
("no sleep mode is available to us at all") was wrong for this repo.

What is genuinely blocked is narrower and better documented than I assumed: `CONFIG_PM_ENABLE` +
tickless idle *link*, but `libfreertos.a` ships prebuilt per variant and is not recompiled by that
mechanism, so `elf2image` fails on a corrupt `.text.prvGetExpectedIdleTime` segment
(`platformio.ini:161-175`). Light sleep needs a from-source FreeRTOS build.

The comment also independently reaches the same conclusion Phase 1 did about the missing crystal —
main XTAL, because the internal RC is too inaccurate for a live connection and no Xteink board wires
up a 32 kHz part.

**Consequence: peripheral latency really does buy power here.** Phase 2 shipped its sleepy mode
disabled on the reasoning that skipping events saves nothing without modem sleep. That reasoning
does not apply. The sleepy mode should be live.

**C2. The negotiated ATT MTU is ~515, not 185.**
`NimBLEDevice::setMTU(185)` (`CompanionBle.cpp:1739`) only sets NimBLE's *initiating* preference;
iOS initiates the exchange and negotiates ~515, confirmed on hardware
(`CompanionBle.cpp:686-695`). Chunk sizing correctly derives from `BLE_ATT_MTU_MAX` rather than the
185. All Phase 1/2 arithmetic using 185 was pessimistic — real chunks are ~508 bytes of payload, so
a 900-byte push is **two** chunks, not five.

**C3. The panel does not settle in 0.4–1 s. It takes ~2.2 s.**
Measured, and recorded in both repos: *"the wire transfer for a full-page text push completes in
~0.24 s while the panel's own settle takes a further ~2.2 s"* (`CompanionClient.swift:383-386`;
`CompanionBle.h:223-227`; `docs/companion-display-protocol.md:743-745`), with ~1.7 s quoted for
steady state.

**This reframes the whole of symptom 2.** A ~1 s "push → visible" target is not merely tight, as
Phase 1 concluded — it is **unreachable by a factor of two before BLE does anything at all**. The
radio is already delivering in 0.24 s. Nothing on the link can fix this.

---

## 1. Verdict in one paragraph

The data plane is in good shape and the client is genuinely well built — no chunk pacing, correct
`canSendWriteWithoutResponse` gating, chunk size derived from the negotiated MTU, SD writes moved off
the BLE host task. **Essentially all of the remaining risk is concentrated in the control plane**:
three link-layer procedures fired simultaneously inside `onConnect`, and a connection-parameter
ladder that re-negotiates two to three times per article using profiles that violate Apple's stated
limits. That is precisely the region Phase 1 identified as fragile, and the firmware's own comments
already suspect it. Symptom 3 looks like an already-fixed client bug. Symptom 2 is mostly the panel.
Symptom 4 is a real, cheap-to-close design gap.

---

## 2. Gaps, most consequential first

### G1 — Three LLCP procedures fired simultaneously inside `onConnect` 🔴

**Ideal:** initiate nothing until iOS's own setup has quiesced (D2 §2 stage 1).

**Actual:** all three, synchronously, in the connect callback (`CompanionBle.cpp:1581-1611`), with a
comment stating the intent is to request *"the tight profile right away … before anything else marks
the link busy"*:

```cpp
noteBleActivity();                                    // → updateConnParams(Busy)   :1586
server->updatePhy(handle, BLE_GAP_LE_PHY_2M_MASK, …); //                            :1592
server->setDataLen(handle, 251);                      //                            :1609
```

**Why this is the headline finding.** Only one link-layer control procedure may be in flight at a
time; the rest queue. Meanwhile iOS is running *its own* data-length and MTU exchange — the
Accessory Design Guidelines say the central does this (§36.7) — and on recent versions its own PHY
update. So at connect there are up to six procedures contending across two initiators. Collisions
are rejected outright with `LL_REJECT_EXT_IND` and **nothing retries them**; a procedure that never
closes kills the link at exactly 40 s with HCI `0x22`.

This matches every property of symptom 1: the 40.000 s constant is the spec's procedure-response
timeout, and *intermittency per connection* is what a race looks like — a fixed misconfiguration
would fail every time.

**Cost:** the periodic unexplained disconnect, which the protocol doc lists as an open item
(*"Long-run link stability. A periodic disconnect reported in real SpokenFeeds use is unexplained
and did not reproduce from a Mac"* — note the Mac harness is a *different central*, so this is
expected and not reassuring).

**Load-bearing?** No. The stated benefit — being tight before the first push — is worth ~100 ms on a
transfer whose visible latency is dominated by a 2.2 s panel settle.

### G2 — Two of the three connection profiles violate Apple's stated limits 🔴

`requestConnParams` (`CompanionBle.cpp:330-365`) calls
`updateConnParams(handle, interval, interval, latency, timeout)` — note **min == max**.

| Profile | Interval | Latency | Timeout | ADG verdict |
|---|---|---|---|---|
| Busy | 15 ms | 0 | 6 s | **compliant** (min==max==15 ms is the one permitted equality) |
| Near | 60 ms | 2 | 12 s | **violates twice**: timeout > 6 s; min==max at 60 ms |
| Deep | 150 ms | 1 | 12 s | **violates twice**: same two rules |

Apple's rules require `2 s ≤ timeout ≤ 6 s`, and `Interval Min + 15 ms ≤ Interval Max` *unless* both
equal 15 ms. A non-compliant request "may be rejected".

**This explains an observation already recorded in the code.** The comment at
`CompanionBle.cpp:236-252` notes that when *"iOS instead declined the request and the link stayed on
Near params, the connection was stable indefinitely"*. Rejection is the expected response to a
non-compliant request — so the ladder is partly inert, and the part that isn't inert is the part
that correlates with instability.

The 12 s timeout was added as *"precautionary insurance, NOT as a demonstrated fix"*
(`CompanionBle.cpp:232-234`). It is not insurance; it is a rule violation that invites rejection.

**Cost:** parameter requests that mostly fail, plus the LLCP traffic of asking. Worst of both.

### G3 — Parameter churn: two to three renegotiations per article 🔴

**Ideal:** one request per stage, minimum 10 s apart, and for this workload *stay in one mode*.

**Actual:** `noteBleActivity()` requests Busy on connect, on **every** characteristic write, and on
every button event (`CompanionBle.cpp:371-374`, call sites `:1085, :1202, :1533, :1586, :1908`).
`tick()` relaxes to Near after 3 s of quiet and to Deep after 30 s (`:1853-1883`). The only guard is
idempotence (`:332`) — no rate limit.

With articles every 7–60 s the steady-state pattern is: **Busy → (3 s) → Near → (30 s) → Deep →
Busy…**, i.e. two or three control procedures per article, forever, each one a collision
opportunity against iOS's own activity.

The firmware's own diagnostic table already says this out loud (`CompanionBle.cpp:1540-1549`):
*"0x22/0x28 point at the busy<->idle parameter-update churn colliding with itself."*

**Cost:** continuous exposure to the G1 failure mode for the entire session, not just at connect.

### G4 — The sleep ladder uses the wrong knob 🟠

**Ideal:** short interval + high latency. Latency delays phone→reader only; the reader still
transmits at the next anchor point, so buttons stay fast.

**Actual:** the ladder lengthens the *interval* (15 → 60 → 150 ms) and keeps latency low (0, 2, 1).

**Cost, concretely:** in Deep, a button press waits up to **150 ms** on the wire before it can even
be sent — most of the 200 ms end-to-end budget, spent before iOS sees it. The equivalent power saving
via latency would cost **0 ms**: `15 ms interval, latency 30` gives a 465 ms effective interval for
pushes with a **15 ms** button path, and is ADG-compliant with a 2 s timeout
(`15×31 = 465 ms ≤ 2 s`; `465×3 = 1.4 s < 2 s`).

The recent change dropping Deep's latency from 4 to 1 (`CompanionBle.cpp:236-252`, and commit
`6f5a5f1e`) moved *further* in the wrong direction — understandably, since latency=4 correlated with
`0x22` disconnects. But per G1/G3 the likely culprit is the procedure that changes latency, not
latency itself. **With modem sleep genuinely enabled (C1), latency is also the knob that actually
saves current** — so this profile gives up the power benefit and pays the responsiveness cost.

### G5 — A text push spends its entire wire time on two acked writes 🟠

**Ideal:** zero application-level round trips inside a transfer.

**Actual:** `START` and `END` frames always use Write **With** Response, even for fields whose
CHUNKs use Write Without Response (`docs/companion-display-protocol.md:676-699`;
`CompanionClient.swift:743-763`).

The arithmetic is striking. At MTU ~515 a 900-byte body is **two** CHUNKs — perhaps 30 ms of wire
time. But the client measured a Write-With-Response round trip at *"~120 ms regardless of the
negotiated connection interval"* (`CompanionClient.swift:731-733`). Two of them is 240 ms:

> **The measured 0.24 s wire time for a text push is, to within rounding, entirely the START and END
> round trips.** The actual content is nearly free.

**Cost:** ~240 ms per push, ~10% of the visible latency. Modest next to the panel — but it is the
whole of the controllable part, and it directly contradicts the design rule that a transfer should
contain no round trips.

### G6 — Reconnect always costs a full re-push 🟠 (symptom 4)

**Ideal:** reader remembers the content-id on the glass across disconnect *and* reset; phone reads it
and re-pushes only on mismatch.

**Actual:** content-id is held only in the RAM of a per-link session (`CompanionBle.cpp:387-391`),
destroyed on disconnect. There is no read-back opcode — the protocol is push-only in that direction.
The client re-pushes unconditionally on every `.gainedScreen`, explicitly clearing its dedupe cache
(`CompanionDeviceService.swift:419-428`).

Reconnect therefore costs: Capability read → HELLO/HELLO_OK → ACQUIRE/FOREGROUND → full content
re-push. **Two round trips before any content**, plus asset pushes on digest mismatch.

**Cost:** exactly symptom 4. And the common case is wasteful — most drops happen *between* article
changes, so the content that gets re-pushed is usually the content already on the panel.

**This is the cheapest real win available.** The panel already holds content across disconnect
deliberately (`CompanionModeActivity.cpp:1095-1096`); it just cannot prove what it holds.

### G7 — Button events can be silently lost 🟠

**Ideal:** monotonic sequence number, small queue of unsent events.

**Actual:** neither. `notifyButtonEvent` returns `false` and drops the event if the link is down or
nobody holds foreground (`CompanionBle.cpp:1905-1907`); the caller does not buffer or resend
(`CompanionModeActivity.cpp:1441-1449`). There is no sequence number in the payload
(`CompanionBle.h:325-329`).

**Cost:** a press during a reconnect does nothing, with no way for either side to know. For
play/pause on audio this is a user-visible failure, and the dropped event is indistinguishable from a
press that never happened. Content-id echoing guards against acting on *stale* content, but not
against loss.

### G8 — A 100 KB image transfer is not resumable 🟠

**Ideal:** offset-addressed, resume from last good offset.

**Actual:** any sequence gap aborts the whole transfer and discards the partial file
(`CompanionBle.cpp:1295-1303`, `:882-893`); recovery is a fresh START. Measured transfer is 1.9–3.8 s
depending on profile (`CompanionBle.cpp:200-209`).

**Cost:** a drop at 90% costs the entire transfer. The client's single automatic retry
(`CompanionClient.swift:474-514`) makes this survivable, at the price of doubling the transfer.

### G9 — Client leaves iOS reconnection facilities unused 🟡

`connect(peripheral, options: nil)` (`CompanionClient.swift:320`) — no
`CBConnectPeripheralOptionEnableAutoReconnect`. No `CBCentralManagerOptionRestoreIdentifierKey` and
no `willRestoreState`, so a terminated app cannot be relaunched into its session. Recovery is an
immediate rescan (`CompanionDeviceService.swift:457-461`).

`bluetooth-central` and `audio` are both in `UIBackgroundModes` (`Info.plist:74-79`), so the
foundation is there.

**Cost:** slower and less reliable recovery than the platform offers, compounding G6.

### G10 — No fast/slow advertising ladder 🟡

No advertising interval is set at all; NimBLE defaults apply (`CompanionBle.cpp:1777-1797`).
Advertising does restart automatically on disconnect (`:1675-1679`), which is the important half.
**Cost:** low — reconnect speed and idle advertising power are both unoptimised, neither is a
reported symptom.

---

## 3. What the symptoms actually appear to be

| # | Symptom | Assessment |
|---|---|---|
| 1 | `0x22` at 40.000 s, intermittent | **G1 + G3.** Spec-exact timeout constant, and intermittency = race. The firmware's own comments already suspect the churn. Needs the capture to confirm. |
| 2 | 4–6 s push→visible | **Mostly the panel (C3).** 0.24 s wire + ~2.2 s settle ≈ 2.4 s floor. The remaining ~2 s is unexplained — leading candidate is `kPendingBatchTimeoutMs` (3 s), see below. Not a throughput problem. |
| 3 | WWR blocked 2+ s | **Very likely already fixed.** A lost-wakeup race in the client's WWR waiter was live from v9 until 2026-08-03, fixed by moving the `canSendWriteWithoutResponse` check inside the lock (`CompanionClient.swift:766-875`). The 2 s figure matches the fallback timer exactly. Worth re-measuring before spending anything on it. |
| 4 | Reconnect ⇒ full re-push | **G6.** Real, by design, cheap to close. |
| 5 | CPU must be pinned | **Load-bearing and proven — do not touch.** See §4. |

**On the ~2 s gap in symptom 2**, the specific thing I would look at first: atomic multi-field
batches wait on a `kPendingBatchTimeoutMs = 3000` safety net if the final field's `0x80` flag never
arrives (`CompanionModeActivity.cpp:150, 1174-1184`). If a push's field ordering or final-flag
placement doesn't match what the firmware expects, *every* push waits the full 3 s before
committing. 3 s + 2.2 s settle ≈ 5.2 s, and "roughly 40% exceed 3 s" is what a
sometimes-hit-sometimes-not timeout looks like. This is a hypothesis, testable by logging whether
the safety net fires — no sniffer needed.

---

## 4. Load-bearing — do not delete

Each of these was reproduced on hardware and fixes something real:

- **Session-scoped power lock** (`CompanionBle.cpp:112-136`). A scoped version let the CPU drop
  before a central's connection attempt landed, so *no central ever completed a connection*. This
  is symptom 5, and it is **not** the "BLE needs 160 MHz" folklore Phase 1 doubted — it is about
  connection *establishment*. Keep. (It may become narrowable once light sleep is reachable, but
  that is not today.)
- **Image writes on a dedicated task** (`CompanionBle.cpp:659-680, 761-854`). Inline SD I/O on the
  BLE host task *"caused real disconnects on real hardware"* at 15 ms intervals. This is exactly the
  D2 §4.1 rule, already implemented, and it matters more than any parameter tuning.
- **Chunk sizing from `BLE_ATT_MTU_MAX`, not the requested 185** (`CompanionBle.cpp:686-698`). Fixes
  a real failure where an iPhone negotiating ~515 had every CHUNK rejected.
- **Write Without Response for CHUNKs, with correct gating** (`CompanionClient.swift:743-763,
  823-849`) — including the lock-ordering fix and the bounded fallback.
- **`disconnectReasonName` table** (`CompanionBle.cpp:1540-1578`). The reason this review could
  reach a specific conclusion at all. Keep, and keep reading it.
- **Deferred rendering off the BLE callback** (`CompanionModeActivity.cpp:172-222, 1129-1324`).

## 5. Workaround, not mechanism — deletable once the cause is fixed

- **Deep latency 4 → 1** (`CompanionBle.cpp:236-252`). Treats the symptom of G1/G3. Its own comment
  hedges appropriately (*"Should keep being watched … the original bug was grant-dependent/
  intermittent"*). If the churn goes and the profiles become compliant, latency should go **up**,
  not down — that is where the power saving lives (C1).
- **12 s supervision timeout** (`CompanionBle.cpp:232-234`). Self-described as not a demonstrated
  fix, and it is an ADG violation (G2). Should return to ≤6 s.
- **Client's 2 s WWR fallback** (`CompanionClient.swift:856`). Defense-in-depth for a bug now fixed.
  Harmless; keep, but it should never fire — if it does, that is a signal, not a cure.
- **Instrumentation marked `// TODO remove once the real bottleneck is identified`**
  (`CompanionBle.cpp:586, 756`) and the `// DIAGNOSTIC` blocks. The bottleneck is now identified
  (C3: the panel). These can go once the numbers are trusted.

## 6. Suggested order

No code has been changed. In dependency order, cheapest and most certain first:

1. **Measure before changing anything on the control plane.** The sniffer capture from D2 §7
   confirms or refutes G1/G3 directly. Everything in §2's red band depends on it.
2. **Test the `kPendingBatchTimeoutMs` hypothesis** (§3). Log-only, no hardware, and it may account
   for the whole unexplained part of symptom 2.
3. **Re-measure symptom 3** against the post-2026-08-03 client before spending anything on it.
4. **G6 (content-id readback)** — self-contained, needs a protocol addition, closes symptom 4 and
   makes every future drop cheaper. Independent of the control-plane work.
5. **G2 then G3 then G1** — make the profiles compliant, then stop the churn, then move the connect
   -time procedures behind a quiesce. Do these as separate changes so the sniffer can attribute the
   effect of each.
6. **G4** — only after G1–G3 are settled, since it changes latency, the parameter most implicated in
   the observed disconnects.
7. **Renegotiate the ≈1 s target** (C3) or move it to the panel: a partial-refresh path for text
   would be worth more than everything else in this document combined. One already exists for
   tag-only redraws (`CompanionModeActivity.cpp:1582-1588`).

A note on sequencing: items 5 and 6 touch the protocol's most fragile area, and the protocol doc is
explicit that neither consumer app has been rebuilt against v11 yet
(`docs/companion-display-protocol.md:40-77`). Landing measurement first is not caution for its own
sake — without it, any improvement in a race is indistinguishable from luck.
