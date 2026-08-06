# BLE companion link: measurement log

Running record of what has actually been **measured** on hardware, as opposed to designed
([ble-companion-ideal-architecture.md](ble-companion-ideal-architecture.md)), researched
([ble-companion-do-and-dont.md](ble-companion-do-and-dont.md)) or inferred
([ble-companion-gap-analysis.md](ble-companion-gap-analysis.md)).

Newest session first. Each entry records the build, the method, what it proved, what it **dis**proved,
and what it could not see. Hypotheses that died are kept, not deleted — a disproved hypothesis is the
most expensive kind of knowledge to re-acquire.

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

**P6. The panel is ~3.0 s from commit to visible**, not the ~2.2 s recorded in the code comments.
Worked example: commit at 526.305 → layout logged at 526.977 (219 ms `clearScreen`→`displayBuffer`)
→ render complete 529.312. The driver's own `X3_DRF (382 ms)` wait accounts for only a fraction of it.

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

## Open questions, in priority order

**Q1. Does the `0x22` still occur at all under compliant parameters?**
600 s clean is suggestive, not proof — the historic failure was intermittent. Needs a long soak
(30+ min) with real playback traffic, serial only, no sniffer required.

**Q2. Is the 373 ms ramp worth paying on every article?**
It exists because the link relaxes to Near after `kConnIdleRelaxMs` = 3 s of quiet, and articles
arrive every 7–60 s, so essentially every article pays it. Raising the relax threshold above the
typical inter-article gap would remove ~373 ms from every push, at the cost of holding a 15 ms
interval for longer. **This is a power-vs-latency judgement with real numbers on both sides and has
not been made** — it needs the battery measurement that has never been run (below).

**Q3. What does the panel actually spend 3.0 s on?**
It is the single largest term in push-to-visible, ~3× everything else combined, and it is not
instrumented beyond one `clearScreen`→`displayBuffer` figure and a driver wait that does not add up
to the total. A partial-refresh path for text updates would be worth more than every remaining link
optimisation put together — one already exists for tag-only redraws.

**Q4. Does the peripheral really transmit at the next anchor point when latency is high?**
The sleepy-mode design in the architecture doc rests on this and it is still unverified. Test: set
Deep's latency high, press a button, measure. If it fails, that design should be deleted rather than
disabled.

**Q5. Battery.** No measurement has ever been taken on this fork; there is no current-sense hardware,
so the plan of record is an A/B discharge-to-empty rather than a bench meter. Q2 cannot be settled
without it.

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
