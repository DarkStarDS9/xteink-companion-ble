# CompanionKit

The Swift client for the Companion Display Protocol v6 — the BLE contract between an iOS app and an
Xteink e-ink companion display running this firmware.

It lives in the firmware repo on purpose. The protocol is the product; a client that ships alongside
the wire format cannot drift from it, and every consumer app gets the same handshake, the same token
handling and the same framer instead of three subtly different ones.

**Authoritative wire reference:** [`docs/companion-display-protocol.md`](../../../docs/companion-display-protocol.md).
If this package and that document disagree, the document is right.

## What it covers

- Discovery and connection (`CBCentralManager` filtered on the service UUID).
- The full `HELLO` handshake: pairing prompt, token enrollment, token persistence, and reconnecting
  silently afterwards.
- `appId` / `installId` management, with the install id minted once into `UserDefaults`.
- Asset digests: compares the tags the device reports against your button map and icon, and pushes
  only what changed.
- `ACQUIRE` / `RELEASE`, driven by the app's own foreground state.
- The START/CHUNK/END framer, with `sessionId` on every packet.
- Button-event decoding, filtered to your session.
- Image plumbing — chunking, progress, and the device's decode result.

## What it deliberately does not cover

**Image encoding and dithering.** `pushImage(_:)` takes PNG bytes you produced. The whole point of
the image field is that the phone owns the aesthetic — the device disables its own dither and shows
your bit pattern as-is. Bake your dither, encode an 8-bit grayscale PNG using only the values
`{0, 85, 170, 255}` at exactly the device's advertised pixel size, and hand it over.

## Requirements

iOS 15+ / macOS 12+. No third-party dependencies. Your app needs
`NSBluetoothAlwaysUsageDescription` in its Info.plist.

## Integrating

Add the firmware repo as a submodule of your app repo, then add the package from that path.

```bash
git submodule add https://github.com/DarkStarDS9/xteink-companion-ble.git Firmware
git -C Firmware checkout companion
```

**In Xcode:** File ▸ Add Package Dependencies… ▸ Add Local… ▸ pick
`Firmware/clients/swift/CompanionKit`. Then add the `CompanionKit` library to your app target's
Frameworks, Libraries, and Embedded Content.

**In a `Package.swift`:**

```swift
dependencies: [
    .package(path: "Firmware/clients/swift/CompanionKit")
],
targets: [
    .target(name: "App", dependencies: [.product(name: "CompanionKit", package: "CompanionKit")])
]
```

Pin the submodule to a firmware commit and bump it deliberately — the submodule pointer is what
records which protocol revision your app was built against. (Sync the firmware repo by merging, never
rebasing: a rebase rewrites SHAs and strands every consumer's pin.)

## Using it

```swift
import CompanionKit

// appId is a constant of your app. Generate it once, hardcode it, never change it.
let identity = CompanionIdentity(appId: UUID(uuidString: "6E7E0C2A-2B1D-4E44-9F0B-6D5C2E1A9F31")!,
                                 displayName: "Snap2Ink")

let buttons = ButtonMap([
    ButtonMapEntry(.confirm, .remote, label: "Shoot"),
    ButtonMapEntry(.left,  .remote, label: "Prev"),
    ButtonMapEntry(.right, .remote, label: "Next"),
    ButtonMapEntry(.back,  .localSleep, label: "Sleep")
])

let client = CompanionClient(identity: identity,
                             assets: StaticAssetProvider(buttonMap: buttons, icon: myIconBitmap))

Task {
    for await event in client.events {
        switch event {
        case .bluetoothStateChanged(let ready) where ready:
            client.startScanning()
        case .discovered(let device):
            client.connect(device)                 // handshake + asset sync + ACQUIRE are automatic
        case .pairingPending:
            show("Confirm on your display")
        case .foreground:
            try? await client.push(title: "Hello", body: "…", contentId: "1")
        case .buttonEvent(let press) where press.isFinal:
            handle(press.button, held: press.holdDuration)
        case .imageStatus(let result):
            print("developed:", result)
        default:
            break
        }
    }
}
```

Mirror the app's own lifecycle onto the device:

```swift
client.setForeground(scenePhase == .active)
```

The device's arbitration policy is last-requester-wins, unconditionally — it has no idea which of
your user's apps should own the screen, and does not try to guess. That is the whole of the
coordination between apps.

### Pushing an image

```swift
let capabilities = client.deviceCapabilities!
// Crop/rotate/scale to capabilities.screenPixelWidth x .screenPixelHeight,
// dither to capabilities.imageQuantizationLevels, encode 8-bit grayscale PNG.
let png = myEncoder.encode(photo, size: CGSize(width: capabilities.screenPixelWidth,
                                               height: capabilities.screenPixelHeight))

try await client.pushImage(png) { fraction in
    updateProgressBar(fraction)
}
```

`pushImage` returns when the device has decoded and displayed the image, or throws
`CompanionError.imageRejected`. Expect several seconds: BLE transfer plus a two-pass grayscale
settle. That slowness is wanted, not a defect to design around.

## Things that will bite you

- **`installId` must persist.** Regenerating it makes a new peer: a new pairing prompt, a new
  directory on the device, and the assets pushed again. The default `UserDefaults` storage handles
  this; if you supply your own, do not lose it across launches.
- **A button map is mandatory.** The device rejects `ACQUIRE` from a peer that has not declared what
  its buttons do. There is no default. This package pushes yours automatically, but you must supply
  one.
- **Both apps on a phone see every notification.** One BLE link is shared by the whole device, so
  filtering on `sessionId` is not optional. This package does it for you; if you decode notifications
  yourself, do it too.
- **Content pushed while backgrounded is dropped**, silently, by design. Check `hasScreen` or wait
  for `.foreground`.
- **The link is unencrypted and the token is sniffable.** Deliberate: the token skips a confirmation
  prompt, it is not a secret. Do not push anything confidential.
- **Session ids are per-connection.** Never persist one. The token is the thing that persists.

## Tests

```bash
swift test
```

Covers the framer, the session codec, the capability parser, the button map encoder, button-event
decoding and identity/token storage — everything that is pure bytes. The CoreBluetooth layer is not
unit-tested; it is verified against real hardware with the checklist at the end of the protocol
document.
