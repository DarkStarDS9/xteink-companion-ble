# Companion Image Protocol — Design Sketch

**STATUS: superseded — adopted as field `0x04` in protocol v6.** The committed
wire contract is `docs/companion-display-protocol.md` (see "Image field"); this
sketch is kept for the reasoning that led there, not as a specification. Where
the two differ, the protocol doc wins.

Resolutions of this sketch's own open questions: staging goes to
`peers/<peerKey>/data/` per peer (option (a), streamed, never a RAM buffer);
there is no distinct "image mode" — the last completed push of either kind owns
the screen; button hints come from the peer's declared button map; the length
cap moved from uint16 to a uint32 START length; and a truncated staged file is
discarded on disconnect rather than decoded. Outcome reporting, which this
sketch did not have, is the `IMAGE_STATUS` notification.

## Motivating use case

A camera app on the phone captures a photo, dithers it client-side to the
panel's native 4-gray-level palette (own creative choice — Atkinson,
Floyd-Steinberg, ordered/Bayer, whatever look is wanted), and pushes the
result to the device to display full-screen, "Polaroid" style. Not
performance-sensitive — a multi-second transfer plus a visible grayscale
settle pass is an acceptable (even thematically fitting) "developing" delay.

## Why dither on the phone, not the device

The device already dithers images for EPUB cover art
(`lib/Epub/Epub/converters/PngToFramebufferConverter.cpp`,
`JpegToFramebufferConverter.cpp`, `applyBayerDither4Level` in
`DitherUtils.h`), but that path exists to turn arbitrary embedded book art
into something legible with zero client involvement — it is not the right
place to also host a JPEG decoder's compression artifacts fighting a second,
firmware-chosen dither pattern. Pushing an image the phone has *already*
quantized to the panel's exact 4-level palette and disabling the on-device
dither (`RenderConfig::useDithering = false`) means the panel shows the
client's bit pattern as-is — no double quantization, and the phone owns the
aesthetic.

### Quantization contract

`PngToFramebufferConverter.cpp`'s non-dithered path (`useDithering == false`)
does:

```cpp
ditheredGray = gray / 85;
if (ditheredGray > 3) ditheredGray = 3;
```

i.e. an 8-bit grayscale input value is bucketed into 4 levels by integer
division by 85. For a client-side dither to land exactly where intended,
encode each already-dithered pixel as one of the four values `{0, 85, 170,
255}` (not just "some value in each bucket") — that's the values this sketch
proposes documenting as the wire contract once implemented, so a client
doesn't have to reverse-engineer the bucket boundaries.

## Proposed wire format

Reuses the existing Content characteristic and START/CHUNK/END framing from
`docs/companion-display-protocol.md` — no new characteristic. Adds one new
field id:

```
kFieldImage = 0x04
```

START packet's total-length field is already a `uint16` (bytes 2..3,
little-endian), giving a 65,535-byte ceiling — enough headroom for a
compressed dithered image (see "Payload format" below) without widening the
START packet layout. CHUNK/END framing is unchanged.

### New length cap

Title/body share `kMaxFieldLen` (4096 bytes, `src/CompanionBle.h`). Image
needs its own, larger cap — proposed `kMaxImageFieldLen`, sized to the
payload format chosen below (see "Open question: payload format"). This is a
new constant, not a change to `kMaxFieldLen` — title/body's existing 4KB
cap and reassembly behavior stay exactly as they are.

### Capability characteristic (v6)

Extend the existing read-only capability value (currently 5 bytes: version,
width-chars, height-chars, max-content-length) with:

```
byte 5:      1 = image field supported, 0 = not (lets an old firmware build
             coexist on the wire without a client guessing from version alone)
bytes 6..7:  max image length, uint16 LE (kMaxImageFieldLen)
```

Bump protocol version to 6. Old clients that only read bytes 0..4 are
unaffected (this mirrors how v4's `0x80` flag and v5's button-event reshape
were additive).

## Payload format: pre-dithered PNG, not raw bitmap

Two options were considered:

1. **Raw packed pixels** — 2 bits/pixel × the panel (measured 528×792 on a real
   X3) = 104,544 bytes, no decode step, but
   too large for the `uint16` length field as 2bpp, and no compression at
   all — worse transfer time than the alternative for no code-size win, since
   a decoder already exists (next point).
2. **1-bit or grayscale PNG, decoded through the existing
   `PngToFramebufferConverter`** — reuses a decoder this firmware already
   ships and has running on real hardware for EPUB art, gets deflate
   compression for free, and fits comfortably under the `uint16` length cap
   for a dithered full-panel image (dithered images compress worse than photos
   — budget isn't as low as JPEG, but well under raw).

**Adopted: (2), at 2 bits per pixel rather than 8.** The sketch proposed 8-bit
grayscale restricted to `{0, 85, 170, 255}`; implementation found the packed
2-bit path is exactly equivalent and four times smaller. `expandSampleToByte`
turns a 2-bit sample into precisely those four values, and `isSupportedBitDepth`
already accepted depths 1/2/4 for grayscale, so this used the decoder as it
stood. See the protocol doc's "Image field" for the committed contract.

### Integration snag: `PngToFramebufferConverter` reads from `HalFile`, not RAM

`PngdecContext`'s I/O callbacks (`pngOpenWithHandle` et al.,
`PngToFramebufferConverter.cpp`) open the source PNG via `HalStorage`/SD
card, not from an in-memory buffer — that's the existing pattern for EPUB
cover art (which already lives on SD as part of the book). Two ways to close
this gap:

- **(a) Stage to a scratch SD file, decode from there** — as each CHUNK
  arrives, append its payload straight to an open file (e.g.
  `.crosspoint/companion/incoming_image.png`) instead of accumulating in a
  RAM buffer, then hand the finished path to the existing
  `PngToFramebufferConverter` entry point on END. Matches the file-based
  pattern the converter already assumes; costs an SD write+read cycle
  instead of one RAM copy — acceptable given "not instant" is already the
  accepted UX.
- **(b) Add a RAM-backed PNGdec I/O callback set** alongside the existing
  file-backed one. More code, avoids the SD round-trip, but duplicates
  functionality the file-backed path already covers and adds a second
  reassembly-buffer-sized heap allocation on top of NimBLE's own footprint
  (see "Memory budget" below).

**Leaning (a)** — streaming CHUNK payloads directly to an SD file avoids
holding the whole image in RAM at all (the reassembly buffer becomes a
handful of bytes — the size of one CHUNK packet — not the full image), which
matters given the existing heap pressure documented below. It also means
`kMaxImageFieldLen` is bounded by SD space and the `uint16` length field, not
by available heap — a materially safer ceiling on this hardware.

## Memory budget (why streaming-to-SD matters)

Existing, already-documented costs on this hardware
(`src/CompanionBle.h:kStartMinFreeHeap`, root `CLAUDE.md`):

- NimBLE init: ~63KB of the ESP32-C3's ~380KB usable RAM, gone for the
  duration of Companion Mode.
- Framebuffer: 48,000 bytes (single-buffer mode, no PSRAM).

If the image field's reassembly buffer accumulated the *entire* incoming PNG
in RAM before decoding (option (b) above, or a naive port of the
title/body's in-RAM reassembly pattern to a much bigger field), that's a
third large, simultaneous allocation stacked on top of the other two —
plausible to blow the heap budget on a device that already treats 80KB free
as the minimum bar to even start BLE. Streaming CHUNK payloads straight to
an SD file (option (a)) avoids this entirely: the only image-sized memory
user at any point is the PNG decoder's own per-row scratch state inside
`PngToFramebufferConverter`, which already runs today for EPUB covers under
this same heap budget.

## Not addressed by this sketch (left for implementation)

- On-device UI/activity state for "image mode" vs. the existing text-based
  Companion Mode screen (`CompanionModeActivity`) — whether it's a distinct
  mode, how it's entered/exited, what button hints apply.
- Whether a "developing" progress indicator is shown while CHUNKs arrive
  and while the two-pass grayscale settle (`GfxRenderer::RenderMode::
  GRAYSCALE_LSB`/`GRAYSCALE_MSB`, see `preconditionGrayscale()`) runs.
- Orientation handling for a captured photo (portrait phone photo vs. the
  panel's logical orientation) — likely a phone-side crop/rotate decision
  before dithering, not a firmware concern, but not decided here.
- Error handling for a truncated/corrupt staged PNG (partial SD file left
  behind by a mid-transfer disconnect) — the existing content-field timeout
  behavior (`docs/companion-display-protocol.md`'s atomic-push section)
  covers the BLE side; the staged-file cleanup path does not exist yet.
- Exact `kMaxImageFieldLen` value — needs picking against real dithered-PNG
  sizes from an actual client encoder, not guessed here.
