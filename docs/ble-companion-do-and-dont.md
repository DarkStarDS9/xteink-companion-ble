# BLE companion link: DO and DON'T

Research notes for the BLE architecture of this firmware (ESP32-C3 peripheral, iOS central).
This document is **descriptive, not prescriptive about our code** — it records what the platforms
and the specification actually permit, guarantee and forbid. It deliberately contains **no proposed
fixes** and was written **without reading this repository's source or design docs**, so that it
describes the problem domain rather than the current solution.

Every claim carries a confidence label:

| Label | Meaning |
|---|---|
| **SPEC** | Bluetooth Core Specification text, or a reference implementation of it |
| **VENDOR** | Apple / Espressif / silicon-vendor documentation |
| **COMMUNITY** | Reproducible field reports from multiple independent engineers |
| **FOLKLORE** | Widely repeated, no primary source found — treat as unproven |

Where sources disagree, or where a source could not be retrieved, that is stated inline rather than
smoothed over.

---

## 1. Mental model

### 1.1 A connection is a schedule, not a pipe

There is no stream. A BLE connection is an agreement to meet on the air at fixed instants called
**anchor points**, spaced one **connection interval** apart. Everything — data, control, keepalive —
happens inside the short **connection event** that starts at an anchor point. Between events both
radios are off and nothing can be transmitted at all, no matter how urgent.

Three consequences follow immediately, and most BLE surprises are one of them:

- **Latency is quantised.** Anything you want to send waits until the next anchor point. You cannot
  send "now".
- **Throughput is bytes-per-event × events-per-second.** Making the radio faster (2M PHY) does
  nothing if you are limited by how many packets the peer will schedule per event.
- **Idle costs the same as busy**, per event. An event where nothing happens still costs both sides
  a wake-up, a receive window, and two packets.

Within a connection event the two sides **strictly alternate**, central first, always:

```
anchor                                        anchor
  |                                             |
  v                                             v
  [C→P data][P→C][C→P data][P→C] ... (event ends)
       ^      ^
       |      +-- 150 µs T_IFS between every packet, in both directions
       +--------- even a "write without response" gets a link-layer packet back
```

"Write **Without Response**" is an *ATT-layer* term: it means no ATT response PDU. At the link layer
every packet is still acknowledged, via the SN/NESN bits in the packet header. This matters twice
over: it is why a peripheral empty packet appears in the airtime arithmetic below, and it is why an
unacknowledged packet is **automatically retransmitted at the next connection event** — packet loss
does not need application-level recovery, and packet loss is *not* an explanation for multi-second
stalls. (SPEC, via NimBLE's controller implementation; the SIG's own HTML spec pages returned only
navigation shells on every fetch attempt, so link-layer claims here are corroborated through
[Apache NimBLE `ble_ll_ctrl.c`](https://github.com/apache/mynewt-nimble/blob/master/nimble/controller/src/ble_ll_ctrl.c)
and vendor documentation rather than quoted from the PDF.)

### 1.2 The central owns the clock

The **central** (the phone) decides everything about the schedule: when anchor points fall, the
channel hopping, how long each event may run, and how many packets it is willing to exchange inside
one. The **peripheral** (the reader) may only *ask*. This is the single most important asymmetry in
the whole design space, and it is worth stating bluntly:

> The device that knows the least about the application — the phone's Bluetooth stack, arbitrating
> between your accessory, a watch, headphones and Wi-Fi on the same antenna — is the device that
> decides your latency and your throughput. Your firmware submits requests. The phone may reject
> them, silently scale them, or apply them minutes later.

### 1.3 The three parameters, and what they actually do

| Parameter | Units / range | Who decides | What it means |
|---|---|---|---|
| **Connection interval** | 1.25 ms steps, 7.5 ms – 4.0 s | Central | Spacing of anchor points |
| **Peripheral latency** | integer, 0 – 499 | Central (peripheral requests) | How many anchor points the peripheral may *skip* |
| **Supervision timeout** | 10 ms steps, 100 ms – 32 s | Central | Silence after which the link is declared dead |

Validity constraint (SPEC): `supervisionTimeout > (1 + latency) × interval × 2`.

**Peripheral latency is the subtle one, and it is asymmetric.** It is a permission, not an
obligation, and it applies only when the peripheral has nothing to say:

- **Peripheral → central is NOT delayed by latency.** The peripheral may wake and transmit at *any*
  anchor point it chooses. A button press therefore costs at most **one connection interval**, even
  if latency is 30.
- **Central → peripheral IS delayed by latency**, by up to the **effective interval**
  = `interval × (1 + latency)`, because the central cannot deliver anything until the peripheral
  next bothers to listen.

This asymmetry is the entire basis on which a battery-powered peripheral can be simultaneously
sleepy and responsive — and also the trap: raising latency to save power silently taxes every
push *from* the phone, including the first packet of a bulk transfer.

The peripheral must **not** exercise latency when it has data queued, or when a link-layer control
procedure with an *instant* (a future event number at which a change takes effect) is pending —
sleeping through the instant produces HCI error `0x28` "Instant Passed". The failure mode is well
evidenced ([Zephyr #14604](https://github.com/zephyrproject-rtos/zephyr/issues/14604)); the spec
clause that mandates staying awake could not be retrieved as primary text — see Open Questions.

### 1.4 Control procedures (LLCP) and the 40-second cliff

Parameter changes are not messages, they are **procedures**: multi-packet exchanges at the link
layer (`LL_LENGTH_REQ`/`RSP` for data length, `LL_PHY_REQ`/`LL_PHY_UPDATE_IND` for PHY,
`LL_CONNECTION_PARAM_REQ` for parameters, plus feature and version exchange).

Rules that matter (SPEC, via NimBLE):

- **One procedure in flight at a time.** A second is queued behind the first.
- **Collisions are rejected, not deferred.** If both sides start a procedure before either learns of
  the other's, the side that already has one running answers with `LL_REJECT_EXT_IND` carrying
  either `0x23` "LL Procedure Collision" (same procedure) or "Different Transaction Collision".
  The rejected initiator must decide for itself whether and when to retry. Nothing retries it for
  you.
- **Each side runs a 40-second procedure response timer** —
  `BLE_LL_CTRL_PROC_TIMEOUT_MS = 40000` in NimBLE. It is armed whenever that side has a "current"
  procedure awaiting the next PDU in the exchange, **including when merely responding** to one the
  peer opened. When it expires the controller drops the link with **HCI `0x22`, "LMP/LL Response
  Timeout"**.

Because the link layer retransmits unacknowledged packets automatically, a 40 s `0x22` does **not**
mean packets were lost. It means the opposite:

> The peer was alive and acknowledging the whole time, but never produced the specific reply PDU
> that the pending procedure was waiting for — or produced one that our stack failed to recognise as
> ending the procedure.

A disconnect at *almost exactly* 40.000 s after connect is therefore a near-certain signature of a
control procedure opened early in the connection and never closed. That it happens on some
connections and not others is the signature of a **race**, not of a configuration error.

### 1.5 Where the time goes in a transfer

For an application payload pushed by the phone:

```
app bytes → ATT Write Command (+3 B header) → L2CAP frame (+4 B header, first fragment only)
          → N link-layer fragments of ≤27 B (or ≤251 B with Data Length Extension)
          → ⌈N / packets-per-event⌉ connection events
          → × connection interval
```

Two regimes, and knowing which you are in tells you which knob is worth turning:

- **Air-time bound** (many small fragments): more packets per event helps, DLE helps enormously,
  2M PHY helps.
- **Interval bound** (few fragments, e.g. once DLE is on): only a shorter connection interval or
  fewer round trips helps. Making the radio faster does nothing.

---

## 2. DO

**D1. Assume the peripheral has no authority, only the right to petition.**
The peripheral is "responsible for the connection parameters" in the sense that it is the only side
that may ask; the central decides.
*Source:* Apple Accessory Design Guidelines R13 §36.6, retrieved as full text via archive.org OCR
after the live PDF exceeded fetch limits. **VENDOR.**

**D2. Keep every parameter request inside Apple's stated envelope.**
From ADG R13 §36.6, verbatim, and matching
[Apple QA1931](https://developer.apple.com/library/archive/qa/qa1931/_index.html):

- `Interval Min ≥ 15 ms` and `Interval Min mod 15 ms == 0`
- `Interval Min + 15 ms ≤ Interval Max` (or `Interval Min == Interval Max == 15 ms`)
- `Peripheral Latency ≤ 30`
- `2 s ≤ supervisionTimeout ≤ 6 s`
- `Interval Max × (Latency + 1) ≤ 2 s`
- `Interval Max × (Latency + 1) × 3 < supervisionTimeout`

Apple states a non-compliant request "may be rejected". **VENDOR.**
*Caveat:* R13 is the 2020 revision — the newest with retrievable full text. The current revision
(R26) could not be fetched. The limits **have** changed historically: R6 (2012) required
`Interval Min ≥ 20 ms` and `Slave Latency ≤ 4`. Do not assume these numbers are permanent.

**D3. Support Data Length Extension, and let iOS drive it.**
ADG R13 §36.7, verbatim: *"Accessories should support Data Packet Length Extension for best
performance… iOS devices and Mac computers operating as the Central will negotiate optimal data
packet lengths."* Apple says the central initiates. **VENDOR.**
DLE is worth far more than any other single lever — see §5.

**D4. Expect iOS to initiate MTU exchange, after data length.**
ADG R13 §36.11: *"An accessory that supports packet length extension shall perform the packet length
update procedure before performing the Exchange MTU Request handshake… When operating as ATT client,
the device will request the optimal MTU size."* iOS negotiates 185 B (iOS 10+) or up to 247 B
(iOS 11+); the app cannot influence it. **VENDOR** for the ordering rule, **COMMUNITY** for the
specific values ([Nordic DevZone](https://devzone.nordicsemi.com/f/nordic-q-a/44825/ios-mtu-size-why-only-185-bytes)).
185 is not arbitrary: `185 + 4 = 189 = 7 × 27`, i.e. exactly seven link-layer packets without DLE.

**D5. Use notifications for peripheral → central events, not indications; Write Command, not Write
Request, for bulk.** Each acknowledged variant costs an extra round trip — roughly one connection
interval of latency and about half the throughput.
*Source:* [Memfault, A Practical Guide to BLE Throughput](https://interrupt.memfault.com/blog/ble-throughput-primer). **COMMUNITY.**

**D6. Obey `canSendWriteWithoutResponse` on the iOS side, strictly.**
Write only when it is true; otherwise wait for `peripheralIsReadyToSendWriteWithoutResponse`. Do not
poll it, do not time-slice around it.
*Source:* [Apple, canSendWriteWithoutResponse](https://developer.apple.com/documentation/corebluetooth/cbperipheral/cansendwritewithoutresponse). **VENDOR.**

**D7. Treat a rejected control procedure as a normal, expected outcome.**
Collisions are rejected outright with `LL_REJECT_EXT_IND`. Any procedure the firmware initiates must
have a defined behaviour for "rejected" and for "no answer", because the link layer will not retry it.
**SPEC** (via NimBLE `ble_ll_ctrl_rej_ext_ind_make`).

**D8. Budget the panel, not just the radio.**
An e-ink full refresh is 0.4–1 s and is often the largest single term in "time until the user sees
it". A wire-latency target below the panel refresh time is unreachable by definition. **VENDOR**
(panel datasheets).

**D9. Account for Wi-Fi coexistence if the radio is shared.**
ESP32-C3 has one 2.4 GHz RF module time-shared between Wi-Fi and Bluetooth: *"Bluetooth can't
receive/transmit while Wi-Fi is receiving/transmitting, and vice versa."*
*Source:* [Espressif RF coexistence](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/coexist.html). **VENDOR.**

---

## 3. DON'T

**N1. Don't expect any iOS API to set connection parameters.** There is none — not in
`connect(_:options:)`, not in `CBConnectionEvent`, not gated behind an entitlement. The options
dictionary carries only notification flags, `StartDelay`, and (iOS 17+) `EnableAutoReconnect`.
**VENDOR** (documented absence — Apple never states "unsupported"; this is confirmed by exhaustive
review of the API surface, so treat it as a strong negative rather than a quotation).
*What goes wrong if you assume otherwise:* a design that puts parameter policy on the phone cannot
be implemented. Parameter changes must be initiated by the firmware, always.

**N2. Don't initiate a control procedure early in the connection without handling collision.**
iOS is an active initiator: it starts DLE and MTU exchange itself (ADG §36.7/§36.11), and on recent
hardware it initiates PHY updates too. A peripheral that fires its own request into the same window
is racing the central. Reported consequences, all on Apple's own forums:
[#101353](https://developer.apple.com/forums/thread/101353) — a peripheral-initiated PHY update
overlapping an iOS-initiated connection update, described as violating the spec's rule on
overlapping instants;
[#806973](https://developer.apple.com/forums/thread/806973) — iOS 26 / iPhone 17 disconnecting
rather than falling back to 1M when its own PHY upgrade fails;
[#806328](https://developer.apple.com/forums/thread/806328) — iOS 26 sending `LL_CONNECTION_UPDATE_IND`
roughly every 100 ms, causing peripheral disconnects; **this one carries a reply from a named Apple
engineer requesting diagnostics**, i.e. Apple acknowledges an open defect in this area.
**COMMUNITY**, except the last, which is **VENDOR-ACKNOWLEDGED (unresolved)**.
*What goes wrong:* a procedure that never completes, and a `0x22` disconnect at 40.000 s.

**N3. Don't assume "iOS ignores peripheral-initiated DLE/PHY".** We looked hard and found **no**
packet capture, and no vendor or engineer statement, confirming that specific claim in either
direction. What the evidence does show is iOS initiating these procedures itself. "Ignored request"
and "procedure collision" produce the same 40 s symptom but have different remedies, so the
distinction is not academic. **Currently unresolved — see Open Questions.**
One correction worth recording: the frequently repeated line "not all centrals trigger PHY update"
traces back to a Nordic DevZone thread that, when read directly, contains no iOS-specific text at
all. **FOLKLORE.**

**N4. Don't believe the "wait 5 seconds after connect before requesting parameters, per Apple" rule.**
It is not in the ADG. Both R6 (2012) and R13 (2020) say only that the accessory should request
*"at the appropriate time"* — no number anywhere. The 5 s figure is the Core Spec's generic
`TGAP(conn_pause_peripheral)` recommendation, misattributed to Apple through repetition.
The underlying advice (don't request during connection setup) is sound; the attribution and the
precise number are **FOLKLORE**.

**N5. Don't raise peripheral latency without pricing the push delay.** Latency multiplies the
central→peripheral delivery delay by `(1 + latency)`. Apple's own rules cap the effective interval
at 2 s, which means a fully compliant configuration can still legally delay every phone push by up
to two seconds — and will also throttle the drain rate of iOS's write queue to one event per
effective interval. **SPEC** for the mechanism, arithmetic in §5.

**N6. Don't expect DLE to be optional for large transfers.** Without it, a 100 KB image cannot
complete in "a couple of seconds" on any iOS device — see §5.3. **SPEC** (arithmetic).

**N7. Don't count on filling a connection event.** iOS caps packets per connection event well below
what the interval allows — field reports converge on roughly **4–6**, largely independent of
interval and PHY, versus a theoretical 22 at 15 ms / 1M / no DLE.
*Sources:* [Apple Developer Forums #713349](https://developer.apple.com/forums/thread/713349)
(sniffer observation, ~5 packets/event, ~600 kbps on iOS vs ~1200–1300 kbps on Android with
identical hardware — note the thread has **no Apple reply**);
[Punch Through](https://punchthrough.com/maximizing-ble-throughput-on-ios-and-android/). **COMMUNITY.**
Apple has never published a number. Do not hard-code one.

**N8. Don't assume arduino-esp32 gives you any power management** — *but see the correction below
for what this repo actually does.* The shipped precompiled C3 libraries have it switched off:
[`esp32-arduino-libs/esp32c3/sdkconfig`](https://github.com/espressif/esp32-arduino-libs/blob/idf-release/v5.1/esp32c3/sdkconfig)
contains `# CONFIG_BT_CTRL_MODEM_SLEEP is not set`, and no `CONFIG_PM_ENABLE` or
`CONFIG_FREERTOS_USE_TICKLESS_IDLE`. These are compile-time Kconfig options baked into `libbt.a` —
**a sketch cannot enable them**, and `esp_pm_configure()` returns `ESP_ERR_NOT_SUPPORTED` for
automatic light sleep when tickless idle is absent. The maintainer tracking issue
[arduino-esp32 #6563](https://github.com/espressif/arduino-esp32/issues/6563) is **open**, milestone
3.3.0, with the maintainer stating plainly: *"While it is possible to reduce current consumption
with ESP-IDF, this option is not accessible to Arduino users."* **VENDOR (repo).**
*What goes wrong:* every power estimate that assumes BLE modem sleep is wrong on a stock build, and
`delay()` busy-waits rather than sleeping.

> **CORRECTION (Phase 3).** This applies to *stock* arduino-esp32, not to this repo. This project has
> a `custom_sdkconfig` rebuild-on-first-build mechanism that recompiles the ESP-IDF core libraries,
> and it enables `CONFIG_BT_CTRL_MODEM_SLEEP=y`, `..._MODE_1=y` and
> `CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL=y` (`platformio.ini:148-160`). **BLE modem sleep is genuinely
> on here**, so peripheral latency does save real current.
> What remains blocked is narrower: `CONFIG_PM_ENABLE` + tickless idle link, but `libfreertos.a`
> ships prebuilt per variant and is not recompiled by that mechanism, so `elf2image` fails on a
> corrupt `.text.prvGetExpectedIdleTime` segment (`platformio.ini:161-175`). Light sleep needs a
> from-source FreeRTOS build. See [ble-companion-gap-analysis.md](ble-companion-gap-analysis.md) §0.

**N9. Don't treat multi-second `canSendWriteWithoutResponse == false` as an iOS bug by default.**
iOS's queue drains at the rate the *link* drains it. If the effective connection interval is long,
a multi-second stall is the arithmetically correct behaviour of a healthy stack, not a defect.
Genuine iOS defects in this area are also reported — notably stalls after peripherals are recovered
via `willRestoreState`, worked around by forcing a reconnect
([Apple forums #80376](https://developer.apple.com/forums/thread/80376),
[#98746](https://developer.apple.com/forums/thread/98746),
[Nordic DevZone](https://devzone.nordicsemi.com/f/nordic-q-a/86420/cansendwritewithoutresponse-maybe-always-false)) —
but the link-arithmetic explanation must be excluded first. **COMMUNITY.**

**N10. Don't cite "the CPU must be pinned at 160 MHz or BLE breaks" as established.** No primary
source was found for it. Espressif's power management documentation says the opposite in the
relevant case: *"Bluetooth is not affected by DFS between calls to `esp_bt_controller_enable()` and
`esp_bt_controller_disable()`"*
([power management](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/system/power_management.html)).
Targeted searches for BLE instability at 80 MHz returned only generic DFS caveats. **FOLKLORE until
proven** — with the practical footnote that on the Arduino build there is no DFS to enable anyway
(N8), so the question is currently moot rather than settled.

**N11. Don't blame the missing 32.768 kHz crystal for connection instability.** Espressif's own
low-power guide is explicit: the Bluetooth sleep-clock accuracy requirement is **500 ppm**; the
**main 40 MHz crystal meets it** and is used as the fallback — *"the system will fall back to the
main XTAL if the external crystal is not detected during BLE init"*. It is the internal **136 kHz RC
oscillator** that cannot meet 500 ppm and *"does not support connections"*.
*Source:* [Espressif, Low Power Mode in Bluetooth LE Scenarios (ESP32-C3)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/low-power-mode/low-power-mode-ble.html). **VENDOR.**
*What the missing crystal actually costs:* efficiency, not stability. Light sleep must keep the main
crystal powered, so the sleep current is much higher than a 32 kHz-equipped board would achieve.
No issue-tracker report was found tying "no 32 kHz crystal on C3" to disconnects.

---

## 4. Physically impossible / don't even ask

**I1. You cannot tell a connected peripheral "sleep for 30 seconds".** No such mechanism exists at
any layer. Not in the Core Spec, not as an Espressif vendor HCI extension. LE Power Control governs
*transmit power*, not scheduling. LE Ping is a liveness check. The complete set of levers for a
peripheral that stays connected is:

1. the connection interval, and
2. peripheral latency,

both of which are **negotiated parameters requested by the peripheral and granted by the central**,
not signals, and both of which change the *periodic* schedule rather than authorising an
arbitrary-length nap. Genuinely unbounded sleep requires **disconnecting**.

This is the honest answer to "the phone knows nothing will be pushed for 30 s — can it hand that
knowledge to the reader?" **The knowledge cannot be turned into sleep directly.** It can only be
turned into sleep *indirectly*: the phone tells the firmware (over GATT, at the application layer)
what to expect, and the **firmware** then petitions through the connection-parameter machinery. That
machinery is the only lever. Every use of it costs a control-procedure round trip, is subject to
rejection, and is a collision risk (N2). That this is the *only* available shape is a fact. **SPEC.**

> **REFINED (Phase 3, verified 2026-08-07).** Two things about this were framed too pessimistically.
>
> **The "wake up early" half needs no negotiation at spec level.** Peripheral latency is a *maximum
> permitted skip*, not an obligation — a peripheral may listen at every connection event whenever it
> likes. So "stop skipping now" is spec-legal without any air exchange. The obstacle is purely the
> **stack API**: latency is applied by the controller, and NimBLE exposes no way to command it.
> Verified by reading the vendored source, not asserted — greps for `slave_latency_disable` /
> `periph_latency.*disable` / force-awake variants come back **empty** outside two paths:
> `ble_gap_update_params()` (full parameter update) and `ble_gap_subrate_req()` (below). Nordic's
> SoftDevice *does* expose exactly this, as `BLE_GAP_OPT_SLAVE_LATENCY_DISABLE` via
> `sd_ble_opt_set()` — so the capability is real, just not here. **SOURCE-VERIFIED.**
>
> **Bluetooth 5.3 standardised it as LE Connection Subrating**, and NimBLE implements it:
> `ble_gap_subrate_req()` and `ble_gap_set_default_subrate()` (`ble_gap.h` ~3343/3363), mapping to
> HCI opcodes `BLE_HCI_OCF_LE_SET_DEFAULT_SUBRATE` (0x007D) and `BLE_HCI_OCF_LE_SUBRATE_REQ` (0x007E)
> (`hci_common.h:1233,1242`). Three blockers, stacked: it is **compiled out** here
> (`CONFIG_BT_NIMBLE_SUBRATE` unset, so `MYNEWT_VAL_BLE_CONN_SUBRATING` is 0); ESP32-C3 **controller**
> support is unconfirmed (Espressif advertises "BLE 5.4 certified", which does not imply every
> optional 5.3 feature is implemented); and **iOS central** support is unconfirmed, leaning negative.
> Even if all three resolved, `LL_SUBRATE_REQ`/`LL_SUBRATE_IND` is still an air exchange —
> negotiation-lite, not a local flag. **SOURCE-VERIFIED** for the API, **unverified** for chip/iOS.
>
> **And the cost of the fallback collapsed.** The "control-procedure round trip" above was priced
> when the design moved the *connection interval*, where the instant is ~6 events at the **old, slow**
> interval (~900 ms from a 150 ms profile) and data crawls throughout. With the interval fixed at
> 30 ms and only latency changing, the same procedure costs **~180 ms and does not slow data at all
> while pending**. A phone hint of the form "quiet for N seconds, wake at N−1" is therefore
> practical — the margin needed is a few hundred ms, not a second.
> See [ble-companion-measurements.md](ble-companion-measurements.md), "Resolved design questions".

**I2. You cannot get sub-interval latency in either direction.** Nothing can be delivered between
anchor points. The floor on any push is one connection interval, and Apple's floor on the interval
is 15 ms (11.25 ms only if HID is among the connected services).

**I3. You cannot make a 100 KB transfer take "a couple of seconds" without DLE.** Arithmetic in
§5.3: without DLE the best case is ~10 s and the realistic case ~15 s.

**I4. You cannot beat ~0.6 s for 100 KB, ever.** The theoretical link-layer ceiling at 2M PHY with
DLE is 173–180 KB/s ([Punch Through](https://punchthrough.com/ble-att-mtu-throughput/)), giving
≈0.58 s with zero overhead and back-to-back packets — unachievable in practice because real
connection events have gaps. Under iOS's observed packets-per-event cap the realistic floor is
**≈1.0–1.5 s**.

**I5. A 1-second "pushed → visible on the panel" target is not a BLE problem.** With a 0.4–1 s panel
refresh, the entire wire budget is 0–600 ms. The radio can meet that comfortably (§5.2 gives
90–525 ms for 900 bytes even *without* DLE) — but if the panel takes the full 1 s, the target is
unreachable regardless of what the link does. Any "make BLE faster" work aimed at this target is
optimising the smaller term.

**I6. A sub-200 ms button response is easy and is not in tension with sleeping.** Peripheral latency
does not delay peripheral→central traffic (§1.3), so button latency is bounded by **one connection
interval**, not the effective interval. At a 30 ms interval the wire cost is ≤30 ms even with
latency 30. If button response is measured above 200 ms, the connection interval is almost certainly
not the cause.

---

## 5. The arithmetic

> **CORRECTION (Phase 3).** The worked examples below assume ATT MTU 185 and a 0.4–1 s panel refresh.
> Both were measured wrong on this hardware: iOS negotiates **~515**, and the panel settle is
> **~2.2 s**. The arithmetic method stands; the numbers are pessimistic on MTU and optimistic on the
> panel. At MTU 515 a 900-byte push is two chunks, not five — and the measured 0.24 s wire time is
> then dominated entirely by the two Write-With-Response framing round trips, not by the payload.
> See [ble-companion-gap-analysis.md](ble-companion-gap-analysis.md) §0 and §2 G5.

### 5.1 Packet air time (LE 1M PHY, unencrypted)

Per-packet overhead: 1 B preamble + 4 B access address + 2 B header + 3 B CRC = **10 B = 80 µs**
(2M: 2 B preamble, and half the time). Encryption adds a 4 B MIC. `T_IFS = 150 µs` between every
packet, fixed, both PHYs.

A central→peripheral data packet is always followed by a peripheral packet (usually empty), so the
unit of airtime is a **pair**:

| Configuration | Data pkt | + IFS | + empty | + IFS | **Pair** |
|---|---|---|---|---|---|
| 1M, no DLE (27 B payload) | 296 µs | 150 | 80 | 150 | **676 µs** |
| 1M, DLE (251 B payload) | 2088 µs | 150 | 80 | 150 | **2468 µs** |
| 2M, no DLE | 152 µs | 150 | 44 | 150 | **496 µs** |
| 2M, DLE | 1048 µs | 150 | 44 | 150 | **1392 µs** |

Note 2M is ~77% faster, not 2×: `T_IFS` does not scale with the PHY.

### 5.2 A 900-byte text push, MTU 185, 1M PHY

Per ATT Write Command: 182 B of application data (`185 − 3`). 900 B → **5 writes**.
Each write becomes an L2CAP frame of `4 + 3 + data` bytes, fragmented to the link-layer PDU size.

**Without DLE** (27 B PDUs): `⌈189/27⌉ = 7` fragments per full write → **35 fragments**.
Pure airtime = 35 × 676 µs = **23.7 ms**. But the transfer is event-bound:

| Packets/event | Events | @15 ms interval | @30 ms |
|---|---|---|---|
| 1 | 35 | 525 ms | 1050 ms |
| 4 | 9 | **135 ms** | 270 ms |
| 6 | 6 | **90 ms** | 180 ms |

**With DLE** (251 B PDUs): `⌈189/251⌉ = 1` fragment per write → **5 fragments**, i.e. 1–2 events,
**15–30 ms**.

**The conclusion that matters:** the radio delivers 900 bytes in well under a *tenth* of a second in
every plausible configuration. An observed 4–6 s is not a throughput problem — it is two orders of
magnitude away from the transport's capability. Something is holding the payload before it reaches
the air, or the link's effective interval is far longer than assumed. See Open Questions.

### 5.3 A 100 KB image push

102,400 B ÷ 182 B per write = **563 writes**.

| Configuration | Fragments | Events @6/event | Time @15 ms |
|---|---|---|---|
| No DLE, MTU 185 | 3941 | 657 | **≈9.9 s** (≈14.8 s at 4/event) |
| DLE, MTU 185 | 563 | 94 | **≈1.4 s** (≈2.1 s at 4/event) |
| DLE, MTU 247 | 420 | 70 | **≈1.05 s** |

DLE changes this transfer by roughly **7×**. Interval, PHY and MTU are all second-order by
comparison, because once DLE is on the transfer is interval-bound rather than air-time bound —
2M PHY would cut airtime that is already idle.

### 5.4 The latency budget

- Button → phone: ≤ 1 connection interval + iOS delivery. **Not** affected by peripheral latency.
- Phone → panel: `effective interval` (up to `interval × (1 + latency)`) + transfer (§5.2) +
  panel refresh (0.4–1 s).

Worked example of the trap in N5: interval 60 ms with latency 30 is **not** ADG-compliant
(`60 × 31 = 1.86 s ≤ 2 s` passes, but requires `timeout > 5.58 s`, leaving only a sliver under the
6 s cap). A compliant-but-sleepy configuration can still legally add ~1.9 s to every push, and will
throttle iOS's write queue to one drain per 1.9 s — which would present as *both* slow pushes *and*
`canSendWriteWithoutResponse` stalling for seconds at a time, from a link that is behaving exactly
as configured.

### 5.5 Power reality on this build

| State | Current | BLE connection survives? |
|---|---|---|
| Active | 95–380 mA | yes |
| Modem sleep | radio gated between events | yes — **but disabled in arduino-esp32 (N8)** |
| Light sleep | ~130 µA claimed | yes, *if* sleep-clock accuracy ≤500 ppm — **unavailable (N8)** |
| Deep sleep | ~5 µA | **no** — link is lost |

All current figures are **COMMUNITY** aggregations citing Espressif's datasheet: direct fetches of
the datasheet PDF tables and of the `sleep_modes` documentation both failed to yield the numeric
tables, and that is recorded here rather than papered over.

One community measurement worth the caveat it carries: 500 ms interval + latency 4 + light sleep →
~0.15 mA average, with ~130 mA spikes during events
([Hubble Network](https://hubble.com/community/guides/esp32-power-consumption-in-ble-mode)).
Board and firmware unspecified. **COMMUNITY.**

**The operative fact for this fork:** on the stock Arduino framework there is no modem sleep, no
light sleep and no DFS. The interesting power question is therefore not "which sleep mode should we
use" but "does any sleep mode exist for us at all", and today the answer is no without rebuilding
the ESP-IDF libraries from source.

---

## 6. Open questions

Each of these is stated with the experiment that would settle it. None should be treated as
answered.

**Q1. Is the 40 s `0x22` a collision or an unanswered request?**
The evidence establishes the mechanism (an opened procedure that never closes) but not which
procedure or which side stalls. The leading hypothesis, on the strength of ADG §36.7/§36.11 saying
iOS initiates DLE and MTU itself and of Apple forum #101353, is a **collision race**: firmware and
iOS open procedures in the same window, one side rejects, and the rejected side fails to treat the
rejection as terminating its own procedure — leaving its 40 s timer armed. Intermittency per
connection fits a race; it does not fit a static misconfiguration.
*Experiment:* an nRF Sniffer or equivalent capture of the first five seconds of ten connections,
recording which side sends `LL_LENGTH_REQ` / `LL_PHY_REQ` / `LL_CONNECTION_PARAM_REQ` first, whether
a matching `_RSP` or `LL_REJECT_EXT_IND` follows, and correlating the connections that reach 40 s
with those that do not. This is the single highest-value measurement available and it settles Q1,
Q2 and much of Q3 at once.

**Q2. Does iOS complete a peripheral-initiated DLE or PHY procedure at all?**
Genuinely unknown. No capture and no vendor statement was found in either direction, despite
targeted searching. **Recorded as an unresolved negative result, not as "no".**
*Experiment:* as Q1, with the firmware forced to initiate each procedure in isolation, well after
connection setup has quiesced.

**Q3. What is the actual effective connection interval on a live link?**
Everything in §5.4 hinges on it, and it is currently assumed rather than measured. It is also the
one number that would confirm or kill the "slow pushes and WWR stalls are both just a long effective
interval" explanation, which fits symptoms 2 and 3 with no defect required.
*Experiment:* sniffer capture of anchor-point spacing; or, more cheaply, timestamp packet arrivals
in firmware over a minute of idle and a minute of pushing, and infer the interval and how many
events the peripheral actually attends.

**Q4. How many packets per connection event does iOS 26 actually grant this peripheral?**
All available numbers are third-party observations from older iOS versions, and Apple publishes
nothing.
*Experiment:* count fragments per event in a capture during a 100 KB push.

**Q5. Is the peripheral obliged to suspend latency while an instant-bearing procedure is pending?**
The failure mode (`0x28` Instant Passed) is well documented; the rule that prevents it could not be
sourced to spec text or to a code comment quoting spec text. Attempts to retrieve the Core Spec
sections directly failed on every attempt (the SIG's HTML returns navigation only, and the PDF
exceeds fetch limits).
*Experiment:* read the controller source rather than the spec — the behaviour is whatever the
shipping controller implements, and that is inspectable.

**Q6. What does the current ADG revision (R26) say?**
Only R6 (2012) and R13 (2020) could be read in full, and the limits demonstrably changed between
them. The numbers in D2 are five years old.
*Experiment:* obtain the current PDF by hand and re-check §36.6 / §36.7 / §36.11 — noting that
section numbers have also moved (§49.6 in some revisions).

**Q7. Would rebuilding ESP-IDF from source with modem sleep enabled be stable here?**
Modem sleep is off in the Arduino libraries, and there are C3-specific reports of it being
unreliable when enabled — e.g.
[esp-idf #9059](https://github.com/espressif/esp-idf/issues/9059), "esp32c3: Unreliable connection
with modem sleep and `CONFIG_ESP_PHY_MAC_BB_PD=y`". So the unavailable option may also be an
undesirable one.
*Experiment:* out of scope until Q1–Q3 are settled; power work before link stability would be
optimising an unreliable system.

**Q8. Does the single-core C3 miss connection events during a panel refresh?**
A 0.4–1 s blocking display operation on a single-core part, against an ADG-mandated supervision
timeout of at most 6 s, is a plausible route to the occasional `0x08`. Untested.
*Experiment:* log controller-visible event counters across a refresh, or capture during one.

---

## Sources that could not be retrieved

Recorded so that nobody re-derives these as facts later:

- **Bluetooth SIG Core Spec HTML** — every fetch of Vol 6 Part B sections returned navigation
  shells with no body text. All link-layer claims here are corroborated through NimBLE's
  implementation and vendor documentation instead.
- **Apple Accessory Design Guidelines, current revision** — exceeds the 10 MB fetch limit. R13
  (2020) was recovered via archive.org OCR; R26 was not.
- **ESP32-C3 datasheet power tables** — PDF text extraction failed; all mA/µA figures are
  third-party aggregations citing it.
- **Infineon/Cypress forum threads** on iOS parameter rejection and the `LL_LENGTH_REQ` timeout —
  HTTP 403 on direct fetch; only search snippets were available.
- **No packet capture of an iOS central answering a peripheral-initiated LLCP procedure** was found
  anywhere. This is the central evidentiary gap in the whole document.
