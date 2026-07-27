# Companion Firmware Roadmap

This is the roadmap for **this fork** — a general-purpose BLE companion-display firmware for
Xteink hardware. It is not upstream CrossPoint Reader's roadmap; see
[crosspoint-reader/crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) for that.

## What this fork is

Upstream CrossPoint is a dedicated e-reader. This fork repurposes the same hardware and SDK as a
**BLE-attached second screen driven by a phone**. The device boots straight into
`CompanionModeActivity` ([src/main.cpp:423](src/main.cpp)) and has no Home/reader entry path in
normal operation.

The important consequence for planning: **the firmware serves more than one consumer app.** It is a
platform, not the device half of a single product. Today that means:

| Consumer | Repo | Status |
|---|---|---|
| SpokenFeeds audio companion | `DarkStarDS9/SpokenFeeds` (iOS) | Shipped, protocol v5 |
| Polaroid camera app | not started, separate app | Design sketch |
| Offline article reader | undecided — may fold into SpokenFeeds | Idea |

Anything app-specific belongs in that app's repo. What lives here is the **protocol, the on-device
UI, and the capability surface** every consumer shares. The authoritative wire reference is
[docs/companion-display-protocol.md](docs/companion-display-protocol.md).

---

## Shipped — Protocol v5, text companion

GATT service `7c9c0000-…0001`, four characteristics (content / button-event / capability / status).
Phone pushes title/body/content-id; device notifies raw button identity plus hold duration. See the
protocol doc for the full field and framing definition.

---

## Planned

### 1. Image push — "Polaroid" camera companion

**Consumer:** a new, separate iPhone camera app. Not SpokenFeeds. This is explicitly a fork-only
capability — upstream's `SCOPE.md` excludes non-reading interactive apps, so it can never go
upstream.

**Concept:** iPhone captures a photo, dithers it client-side, pushes it over BLE; the reader
displays it as a low-fi print. Slow draw (BLE transfer + two-pass grayscale settle) is acceptable
and thematically fitting — no need to optimize for speed.

**Protocol delta:** new `kFieldImage = 0x04`, reusing the existing START/CHUNK/END framing and
the content characteristic — no new characteristic. `0x04` is the next free id
(`kFieldContentId = 0x03`, [src/CompanionBle.h:117](src/CompanionBle.h)). Needs a uint16 length
field to clear the current `kMaxFieldLen = 4096` cap ([src/CompanionBle.h:48](src/CompanionBle.h));
sketch proposes a 65 KB ceiling. Capability characteristic goes to v6.

**Payload:** pre-dithered PNG — not raw bitmap, not JPEG.

**Decided: dither on the phone, not on-device.** Avoids JPEG-compression-vs-dither artifact
conflicts, gives full creative control over dither style (Atkinson / Floyd–Steinberg / ordered),
and lets the firmware quantize-pass-through with `useDithering = false`
([lib/Epub/Epub/converters/ImageToFramebufferDecoder.h:18](lib/Epub/Epub/converters/ImageToFramebufferDecoder.h))
provided the phone pre-quantizes to exactly `{0, 85, 170, 255}`.

**Reuse (verified):**
- 4-level grayscale via two-pass overlay: `RenderMode::GRAYSCALE_LSB/MSB` and
  `preconditionGrayscale()` ([lib/GfxRenderer/GfxRenderer.h:30,282](lib/GfxRenderer/GfxRenderer.h)),
  on top of the native 1-bit framebuffer.
- PNG/JPEG → framebuffer decoders built for EPUB cover art
  ([lib/Epub/Epub/converters/](lib/Epub/Epub/converters/)), including
  `applyBayerDither4Level()` ([DitherUtils.h:15](lib/Epub/Epub/converters/DitherUtils.h)).

**Known integration snag:** `PngToFramebufferConverter::decodeToFramebuffer()` takes a
`const std::string& imagePath` ([PngToFramebufferConverter.h:9](lib/Epub/Epub/converters/PngToFramebufferConverter.h))
— it decodes from SD, not from RAM. Stream BLE chunks straight to a scratch SD file rather than
buffering in RAM; a third large allocation will not fit alongside NimBLE (~63 KB) and the 48 KB
framebuffer on a no-PSRAM ESP32-C3.

**Open:** on-device UI/mode design, "developing" indicator UX, photo orientation/crop,
corrupt-transfer cleanup, exact size cap (needs real encoder output to tune).

**Prerequisite — review prior art before designing the on-device UI.** Two CrossPoint forks already
solved multi-mode selection on this hardware:

* [`0x1abin/crossmux`](https://github.com/0x1abin/crossmux) — apps hub with mini-games, tools, standby faces
* [`zakerytclarke/crosspoint-reader-apps`](https://github.com/zakerytclarke/crosspoint-reader-apps) — app support framework

Read how they handle mode entry, exit, and per-mode state before designing ours. Not a base to
adopt (see [SCOPE.md §6](SCOPE.md)) — just the wheel that already exists. The same prior art applies
to item 2's boot-path re-wiring.

**Non-firmware, decided:** the camera app will be **source-available** (not "open source" — the
mislabeling is what drew the HashiCorp/Elastic/Redis backlash), under an existing named license
(PolyForm Noncommercial/Shield, or Sentry's FSL) plus a CLA granting exclusive distribution
rights. CodeRabbit's free tier only requires a public repo — no OSI-license check — so this does
not block free code review. No license chosen yet.

**Detailed sketch:** [docs/companion-image-protocol-sketch.md](docs/companion-image-protocol-sketch.md)

---

### 2. Offline article sync

**Consumer:** undecided — most likely SpokenFeeds, possibly its own app.

**Concept:** sync a list of articles to the device for offline text reading with no phone
connection, then reconcile read/favourite status and usage data back on reconnect.

**Status: experimental spec, not committed work.** This cuts against the product's audio-first
premise (TTS voice cloning). Treat it as an offline fallback / differentiator, not a core path, and
write the spec before spending engineering time.

**Mostly reuse, not new build:**
- The reader UI, library/home activities, bookmarks, and EPUB rendering are **already compiled into
  the companion binary** — they are merely unreachable, because `main.cpp` boots directly into
  `CompanionModeActivity` and never routes into them. `ActivityManager::goToReader()` still exists
  ([src/activities/ActivityManager.h:89](src/activities/ActivityManager.h)).
- Full read/write SD HAL exists: `Storage` singleton ([lib/hal/HalStorage.h](lib/hal/HalStorage.h)),
  thread-safe, general-purpose. Companion mode currently uses it only for `/crash_report.txt`.
- Generic JSON persistence exists:
  [lib/Serialization/PersistableStore.h](lib/Serialization/PersistableStore.h), already backing
  settings and app state.

**The one genuinely new piece is the protocol.** v5 is single-item, ephemeral, phone→device only,
~4096-byte cap. There is no bulk list transfer, no persisted state, and no device→phone data pull.

**Proposed architecture — dumb firmware, smart phone:**
- One JSON file per article (or one JSON log) via the existing `PersistableStore` / `HalStorage`.
  No new storage engine.
- Device records **raw events only**: button presses and on-screen dwell time per article. No
  on-device interpretation — "read vs skimmed", "favourited" and similar are the phone's job.
- Phone pushes the article list and content down; device accumulates an event log; on reconnect the
  phone pulls and reconciles.
- **Merge, don't overwrite.** Sync must union device events newer than a last-synced cursor into
  phone state and write back a merged result. A naive last-writer-wins file swap silently drops
  everything recorded while offline.
- **Append, don't rewrite.** The event log is append-only on-device, compacted only after the phone
  confirms a successful sync — otherwise every button press triggers a full-file rewrite
  (SD/flash wear plus write cost).

**Scope for a real spec:** (1) new BLE opcodes for bulk list transfer and device→phone pull —
extending the existing chunking, not a new transport; (2) JSON schema for article list and
per-article event log; (3) phone-side cursor-based merge logic; (4) re-wiring the companion boot
path to optionally enter the dormant reader/library activity.

**Note on (4):** the boot path in `main.cpp` is the one place this fork deliberately diverges from
upstream, and it conflicts on nearly every upstream sync. If this item is picked up, that
divergence shrinks — the fork would move back *toward* upstream's routing structure rather than
further from it. Worth factoring into the sync cost.

---

## Watch

**`upstream/feat-bluetooth` — a second BLE consumer.** That branch adds `src/BleInput.cpp` and
`BleButtonMapActivity`: the device acting as BLE **central**, receiving from a page-turner remote.
Opposite direction from our peripheral role, same stack. It carries a commit titled *"Stop BLE
before initializing WiFi in activities"*, which is the coexistence problem in miniature.

If it merges to `develop`, we inherit central + peripheral + WiFi contending on a single-core C3
where NimBLE already costs ~63 KB. Check its state before any large protocol work, rather than
discovering the collision during a sync.
