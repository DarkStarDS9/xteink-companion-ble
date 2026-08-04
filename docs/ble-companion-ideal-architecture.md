# BLE companion link: ideal architecture

Companion to [ble-companion-do-and-dont.md](ble-companion-do-and-dont.md), which is the evidence
base. This document designs the link **as if no code existed**, from the constraints established
there. It was written without reading this repository's implementation.

Two workloads:

- **Workload A — periodic small pushes.** The phone pushes ~200–900 bytes of text plus a small state
  update every 7–60 s, for hours, usually while backgrounded. The reader sends button events.
  Targets: content visible ≈1 s after the push, button → phone < 200 ms, link survives hours.
- **Workload B — occasional bulk.** A ~100 KB full-screen image, ideally in a couple of seconds.
  Otherwise idle.

---

## 1. The five facts the design is built on

Everything below follows from these. If one of them turns out to be false, the design changes.

1. **Only the peripheral may request connection parameters; only the central may grant them.** iOS
   exposes no API to influence them from the phone side. Parameter *policy* is therefore forced into
   firmware, no matter how much we would prefer it on the phone.
2. **Peripheral latency is asymmetric.** It delays phone→reader delivery by up to
   `interval × (1 + latency)`. It does **not** delay reader→phone traffic at all, because the
   peripheral may transmit at any anchor point it chooses.
3. **Control procedures are the fragile part of the link**, not data. One in flight at a time,
   collisions are rejected outright and never retried, and an unclosed procedure kills the link at
   exactly 40 s. iOS initiates data-length and MTU itself, and on recent versions PHY too.
4. **The radio is not the bottleneck for workload A.** 900 bytes is ~90–135 ms without DLE and
   ~15–30 ms with it. The e-ink refresh (0.4–1 s) is the dominant term by an order of magnitude.
5. **Data Length Extension is worth ~7× on workload B** and roughly nothing on workload A. Interval,
   MTU and PHY are all second-order once DLE is on, because the transfer becomes interval-bound.

---

## 2. Connection lifecycle

The design is a state machine over the life of one connection. The governing principle:

> **Say nothing on the control plane until the central has finished talking.** Every reported
> failure mode in this area is a race with iOS's own setup procedures. The cheapest fix for a race
> is not to enter it.

### Stage 0 — Advertising

| | Interval | Duration |
|---|---|---|
| Fast | 20–30 ms | first 30 s after any disconnect |
| Slow | 1 s | indefinitely thereafter |

Rationale: fast advertising makes reconnect quick when the user has just walked back into range or
the link glitched, which is when it matters; slow advertising afterwards costs little and keeps the
device findable for the whole session. The reader advertises for as long as a session is nominally
open, so that iOS's pending connect (see §5) can complete without user action.

### Stage 1 — Quiesce (connect → link settled)

**The firmware initiates nothing.** Not a parameter request, not DLE, not PHY, not MTU. It observes
and records what iOS does: data length change, MTU, PHY update, encryption.

Exit condition, whichever is **later**:

- observed completion of iOS's setup (data-length and MTU events seen, encryption established), or
- **5 s** since connect.

The 5 s is `TGAP(conn_pause_peripheral)` from the Core Spec — a spec recommendation for exactly this
situation. It is *not* an Apple rule; the Accessory Design Guidelines say only "at the appropriate
time". Using the event-driven condition as the primary and the timer as a floor means we neither
race iOS nor wait pointlessly on a fast link.

During quiesce the link is fully usable for **data**. Content can be pushed immediately; only the
control plane is silent. This matters: quiesce must not delay first paint.

### Stage 2 — One parameter request

After quiesce, the firmware issues **exactly one** connection parameter request, for the mode it
wants. Then:

| Outcome | Response |
|---|---|
| Granted | Adopt, record the granted values |
| Rejected | **Accept it.** Record and continue at iOS's parameters |
| No response | Nothing to do — the host cannot cancel an LL procedure. Log it |

**Never retry in a loop.** A retry loop is how a peripheral turns a transient rejection into the
iOS-26 pathology reported on Apple's forums, where repeated connection updates cause disconnects.

**The firmware must be fully correct at whatever parameters iOS chooses.** The request is an
optimisation, not a precondition. If every request is refused for the entire session, both workloads
must still function — slower, but correct. This is the single most important robustness property in
the design, because it makes the whole parameter layer optional.

### Stage 3 — Steady state

Mode transitions per §3, rate-limited, each one still subject to the "exactly one, no retry" rule.

### Stage 4 — Loss and recovery

On disconnect the reader **leaves the panel exactly as it is** (e-ink costs nothing to hold, and
stale-but-correct beats blank) and returns to Stage 0 fast advertising. Recovery cost is minimised
by content addressing, §4.3 — not by reconnecting faster.

---

## 3. Parameter modes

Three modes. All ADG-compliant. Interval is always a multiple of 15 ms.

| Mode | Interval | Latency | Timeout | Effective interval | Reader wakes | When |
|---|---|---|---|---|---|---|
| **ACTIVE** | 15 ms | 0 | 4 s | 15 ms | 67 /s | during a transfer, +3 s after the last byte |
| **READY** | 30 ms | 0 | 4 s | 30 ms | 33 /s | default for an open session |
| **DORMANT** | 30 ms | 30 | 4 s | 930 ms | 1.07 /s | long declared quiet — **disabled today, see §6** |

ADG compliance check for DORMANT, the only non-trivial one:
`30 × (30+1) = 930 ms ≤ 2 s` ✓ and `930 × 3 = 2.79 s < 4 s` ✓ and latency `30 ≤ 30` ✓.

**Why DORMANT keeps a 30 ms interval instead of using a long one.** This is the design's one genuine
piece of cleverness, and it falls straight out of fact 2. A long interval would penalise *both*
directions; high latency on a short interval penalises only the direction we can afford to penalise.

```
DORMANT: interval 30 ms, latency 30
  phone → reader:  up to 930 ms   (acceptable: pushes are announced, see §3.2)
  reader → phone:  up to  30 ms   (unaffected: the reader simply declines to skip)
  wakeups:         1.07/s instead of 33/s  — 31× fewer
```

A button press stays a ≤30 ms event in the sleepiest mode available. Responsiveness and battery are
not actually in tension here; they only appear to be if you reach for the interval knob.

**Why the timeout is 4 s.** ADG permits 2–6 s. Short detects a dead link sooner; long survives
transient interference without dropping. Reconnect is expensive (§4.3 reduces but does not eliminate
it) and the panel holds correct-if-stale content meanwhile, so resilience is worth more than
detection speed. 4 s leaves headroom above DORMANT's 2.79 s floor without sitting at the ADG ceiling.

### 3.1 Transition policy

Each transition costs one control procedure — the fragile thing. So:

- **Minimum 10 s between parameter requests.** Hard rate limit, no exceptions.
- **ACTIVE is entered on demand and left on a 3 s idle timer**, so a burst of pushes doesn't thrash.
- **DORMANT is entered only on an explicit declared quiet of ≥60 s** (§3.2), never on a heuristic.

For workload A the practical consequence is deliberate and worth stating plainly: **with articles
changing every 7–60 s, the link mostly just sits in READY.** Cycling ACTIVE↔DORMANT around every
article would mean a control procedure every few seconds — precisely the traffic pattern implicated
in the reported iOS disconnects, spent to save power we cannot currently save anyway. The correct
answer for workload A is a stable mode, not a clever one.

### 3.2 The traffic hint — how "smart phone" is honoured without lying about it

The phone knows things the reader cannot: that audio will play for another 40 s, that a photo is
about to be sent, that the session is ending. That knowledge **cannot** be turned into peripheral
sleep directly — no such mechanism exists at any layer (see D1 §4, I1). The only lever is the
connection-parameter machinery, and only the peripheral may operate it.

So the split is:

- **The phone declares intent**, via one small GATT characteristic: *"bulk transfer starting"*,
  *"expect quiet for ≥N seconds"*, *"session ending"*. Facts about the future, not commands.
- **The firmware owns mechanism**: a fixed, tiny table mapping declared intent → mode, plus the
  rate limiting. It never interprets *why*.

This keeps the fork's dumb-firmware principle intact under a constraint that superficially violates
it. The firmware is not deciding policy; it is executing a lookup because it is the only side
holding the lever. If the hint characteristic is never written, the firmware sits in READY forever
and everything still works.

**Do not** let the phone write raw interval/latency values. That would put ADG compliance and
collision safety in the app, across multiple consumer apps, where a single bad actor breaks the
link for everyone.

---

## 4. Data plane

### 4.1 Push a burst, not a conversation

Both workloads are one-directional bulk. The design rule is **zero application-level round trips
inside a transfer**:

- Write Command (no response), never Write Request.
- No per-chunk application ACK. The link layer already acknowledges every packet and retransmits
  losses automatically; a second acknowledgement layer buys nothing and costs a full round trip per
  chunk.
- **No inter-chunk pacing delay on the phone.** Flow control is `canSendWriteWithoutResponse` and
  `peripheralIsReadyToSendWriteWithoutResponse`, exclusively. Any fixed sleep between chunks is pure
  additive loss: at 5 chunks for 900 bytes, a 200 ms pacing delay costs 1 s against a 100 ms
  transfer.
- The reader **must not block the BLE path while receiving**. Accumulate the whole payload, then
  render. An e-ink refresh in the middle of a transfer stalls the link for up to a second and, at a
  4 s supervision timeout, starts eating real margin.

Expected cost, workload A, READY mode, no DLE:

```
  push queued → next anchor      ≤  30 ms
  35 fragments @ ~5/event        ~ 105 ms
  render                          400–1000 ms
                                 ------------
  total                           535–1135 ms
```

The ≈1 s target is met **only** if the panel refresh is at the fast end. This is a panel decision,
not a link decision — see §7.

### 4.2 Bulk transfer (workload B)

100 KB is the one case where the radio genuinely is the constraint, and where DLE is decisive:
~1.4 s with it, ~10–15 s without. Design:

- Enter ACTIVE for the duration.
- **Offset-addressed chunks**, so a drop mid-transfer resumes from the last acknowledged offset
  rather than restarting. A restart of a 10 s transfer is a far worse outcome than the drop itself.
- The reader streams to scratch storage rather than holding a second full-screen buffer — the RAM
  budget on this part does not have room for one.
- Do not request 2M PHY from the firmware. Recent iOS initiates PHY changes itself and has been
  reported to disconnect rather than fall back when its own upgrade fails; the gain is small once
  DLE makes the transfer interval-bound. Accept whatever iOS chooses.

### 4.3 Content addressing — the fix for expensive reconnects

Reconnects are unavoidable on a mobile link over hours. What is avoidable is paying full price for
each one.

- The reader stores the **content-id of what is currently on the panel**, in storage that survives
  both a disconnect and a reset.
- It exposes that id in a characteristic the phone reads on connect.
- **The phone re-pushes only on mismatch.**

For workload A, most drops happen between article changes, so the common case becomes: reconnect,
read id, match, push nothing, panel was never wrong. The stale-content window collapses from
"handshake + full re-push" to "nothing happened".

### 4.4 Button events

- One notification characteristic; raw button identity and hold duration, no interpretation, per the
  fork's existing principle.
- **A monotonic sequence number**, so the phone can detect loss across a reconnect.
- A small queue of unsent events, so a press during a drop is delivered on reconnect rather than
  lost. A button press that silently does nothing is worse than one that acts late.
- Latency is ≤ one connection interval in every mode: ≤30 ms wire, comfortably inside the 200 ms
  target. If measured button response exceeds 200 ms, the connection interval is not the cause —
  look at the phone's own processing.

---

## 5. Idle behaviour, and what runs on which side

**When the link is idle the reader does nothing at all**: holds the panel (zero power on e-ink),
attends anchor points per the current mode, and never polls, never probes, never re-requests
parameters. The supervision timeout is the liveness mechanism; adding an application-level heartbeat
on top of it would burn events to learn something the link layer already tells us.

| Concern | Side | Why |
|---|---|---|
| Content, layout, semantics | Phone | Existing principle; nothing here changes it |
| Deciding *when* to push | Phone | Only it knows |
| Declaring future traffic | Phone | Only it knows |
| Parameter mechanism + ADG compliance | **Firmware** | **Forced** — no iOS API exists |
| Rate limiting parameter requests | Firmware | Must survive a misbehaving app |
| Rendering, panel timing | Firmware | Hardware-local |
| Raw button reporting | Firmware | Existing principle |
| Remembering displayed content-id | Firmware | It is the one that knows what is on the glass |
| Retry / resumption policy | Phone | It holds the source data |

The one deliberate exception to "policy lives on the phone" is the parameter state machine, and it
is an exception because the platform leaves no choice. It is kept as small as a lookup table so the
principle bends rather than breaks.

**On the phone side**, use iOS 17+ `CBConnectPeripheralOptionEnableAutoReconnect` and keep a pending
connect outstanding at all times during a session. A pending connect does not time out in the
background, so iOS reconnects without user action when the reader comes back. Combined with §4.3
this makes a drop close to a non-event.

---

## 6. What is deferred, and why it is safe to defer

**Power.** The Arduino build has no BLE modem sleep, no light sleep and no DFS — they are compile
-time options absent from the shipped libraries. Therefore **DORMANT saves almost nothing today**:
skipping connection events does not help much while the radio stays powered between them regardless.

DORMANT is specified anyway, and shipped **disabled**, because the cost of specifying it now is a
table row and the cost of retrofitting it later is a redesign. When modem sleep becomes available,
enabling it is a configuration change plus a measurement — not new structure. That is the concrete
sense in which deferring the ESP-IDF rebuild does not compromise the architecture.

**What would change the answer:** if measurement showed that skipping events saves meaningful
current *even without* modem sleep, DORMANT becomes worth enabling immediately. That is a
one-afternoon experiment (§7) and it is worth doing before assuming otherwise.

**2M PHY** is deferred for the reasons in §4.2: small gain, real risk, and iOS drives it anyway.

---

## 7. What must be measured before this is more than a hypothesis

In priority order. The first one gates everything else.

1. **Sniffer capture of the first 5 s of ten connections.** Establishes who initiates what, whether
   collisions occur, what parameters are actually granted, and which connections reach 40 s. Until
   this exists, §2's quiesce design is a well-motivated guess.
2. **Effective connection interval on a live link.** If it is far longer than assumed, it alone
   explains both slow pushes and multi-second write stalls, with no defect anywhere.
3. **Panel refresh time, measured, for the actual content of workload A.** It is the largest term in
   the ≈1 s target. If a partial refresh is usable for text updates, that is worth more to this
   target than anything on the radio — and if it is not, the ≈1 s target should be renegotiated
   rather than chased.
4. **Does this stack actually transmit at the next anchor point when latency is configured high?**
   The design's headline property — sleepy but instantly responsive — depends on the peripheral
   declining to skip when it has data. That behaviour is spec-correct and is what a conformant
   controller must do, but it has not been verified on this stack. Cheap to test: set latency high,
   press a button, measure. **If this fails, DORMANT is worthless and should be deleted rather than
   disabled.**
5. **Current draw vs. peripheral latency, without modem sleep** (§6).

---

## 8. Summary of the choices

| Decision | Choice | Driven by |
|---|---|---|
| Who requests parameters | Firmware only, once per stage, rate-limited | No iOS API exists; collisions are unretried |
| When | After quiesce: iOS setup observed, floor 5 s | Every reported failure is a setup race |
| On rejection | Accept and continue | Retry loops are the documented pathology |
| Steady state | READY, 30 ms / latency 0 | 30 ms wire cost either direction; panel dominates anyway |
| Sleep strategy | Short interval + high latency, never a long interval | Latency is asymmetric; buttons stay fast |
| Workload A mode churn | None — stay in READY | Procedure churn costs more than it saves |
| Flow control | `canSendWriteWithoutResponse` only, no pacing, no app ACKs | 900 B is ~100 ms; every round trip is visible |
| Bulk transfer | DLE, offset-addressed, resumable, ACTIVE | DLE is ~7×; restarts cost more than drops |
| Reconnect cost | Content-id comparison, re-push only on mismatch | Most drops need no re-push at all |
| PHY | Leave it to iOS | Small gain, reported iOS disconnect on failure |
| Power | Specified, disabled, table-driven | No sleep mode exists on this build yet |
