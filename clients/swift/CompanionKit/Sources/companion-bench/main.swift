import CompanionKit
import Foundation

setbuf(stdout, nil)

/// Thread-safe latch for a value set once from pushImage's progress callback,
/// which fires on an arbitrary queue (see CompanionClient.pushImage's doc).
final class ElapsedBox: @unchecked Sendable {
    private let lock = NSLock()
    private var stored: Double?
    var value: Double? { lock.lock(); defer { lock.unlock() }; return stored }
    func setIfUnset(_ v: Double) {
        lock.lock(); defer { lock.unlock() }
        if stored == nil { stored = v }
    }
}

// A macOS-native benchmark harness: exercises the exact same CompanionKit code
// path (CoreBluetooth negotiation, ContentFramer chunk sizing off
// maximumWriteValueLength(for:.withResponse)) that Snap2Ink runs on a real
// iPhone. Its only job is to isolate "is bleak/Python slow" from "is the Mac's
// own CoreBluetooth/radio slow" for scripts/push_companion_content.py's
// numbers -- see the investigation in the companion firmware repo for context.
//
// Usage: swift run companion-bench [path-to-raw-2bpp-image]
//   (defaults to /tmp/bench_image.raw, the same file the Python script uses)

let imagePath = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "/tmp/bench_image.raw"

guard let imageData = FileManager.default.contents(atPath: imagePath) else {
    FileHandle.standardError.write("Could not read \(imagePath)\n".data(using: .utf8)!)
    exit(1)
}
print("Loaded \(imageData.count) bytes from \(imagePath)")

// A stable, dev-tool-only identity, mirroring SCRIPT_APP_ID in
// push_companion_content.py -- its own peer directory and icon tile on the
// device, distinct from any real app.
let identity = CompanionIdentity(
    appId: Data([0x2f, 0x1d, 0x7b, 0x64, 0x9c, 0x3e, 0x4a, 0x55, 0x8f, 0x21, 0x0c, 0x7b, 0x5e, 0x9a, 0x3d, 0x10]),
    installId: {
        let key = "CompanionBench.installId"
        if let existing = UserDefaults.standard.data(forKey: key) { return existing }
        var bytes = [UInt8](repeating: 0, count: 16)
        _ = SecRandomCopyBytes(kSecRandomDefault, 16, &bytes)
        let fresh = Data(bytes)
        UserDefaults.standard.set(fresh, forKey: key)
        return fresh
    }(),
    displayName: "Companion Bench (Mac)"
)

// UserDefaults, not Keychain: this is exactly the "macOS dev harness" case
// UserDefaultsTokenStore's own doc comment calls out.
let tokenStore = UserDefaultsTokenStore(prefix: "CompanionBench.token.")
let assets = StaticAssetProvider(uiDeclaration: .readerDefault())

let client = CompanionClient(
    identity: identity,
    tokenStore: tokenStore,
    assets: assets,
    acquireScreenOnConnect: true,
    log: { print("  [log] \($0)") }
)

var pushStarted = false
var connectStarted = false

let listener = Task {
    for await event in client.events {
        switch event {
        case .bluetoothStateChanged(let available):
            print("Bluetooth available: \(available)")
            if available {
                print("Scanning for companion device...")
                client.startScanning()
            }
        case .discovered(let device):
            guard !connectStarted else { continue }
            connectStarted = true
            print("Found \(device.name) (\(device.id)), connecting...")
            client.connect(device)
        case .stateChanged(let state):
            print("state -> \(state)")
            if state == .awaitingUserConfirmation {
                print("  >>> Press CONFIRM on the device to approve pairing. <<<")
            }
        case .connected(let caps):
            print("Capabilities: \(caps.screenPixelWidth)x\(caps.screenPixelHeight)px, max image \(caps.maxImageFieldLength)B")
        case .gainedScreen:
            guard !pushStarted else { continue }
            pushStarted = true
            print("Screen acquired. Pushing \(imageData.count)-byte image...")
            let start = DispatchTime.now()
            // Tracked separately from the overall result: pushImage's await also
            // covers the device's post-transfer decode + grayscale settle, which
            // can run long enough for the link to drop before the status
            // notification arrives -- in which case pushImage throws with the
            // BLE-transfer time already elapsed and worth reporting on its own,
            // same split scripts/push_companion_content.py's --image path makes.
            let transferElapsedBox = ElapsedBox()
            do {
                let result = try await client.pushImage(imageData) { progress in
                    let pct = Int(progress * 100)
                    if pct % 10 == 0 {
                        FileHandle.standardOutput.write("\r  \(pct)%".data(using: .utf8)!)
                    }
                    if progress >= 1.0 {
                        transferElapsedBox.setIfUnset(Double(DispatchTime.now().uptimeNanoseconds - start.uptimeNanoseconds) / 1_000_000_000)
                    }
                }
                let elapsedS = Double(DispatchTime.now().uptimeNanoseconds - start.uptimeNanoseconds) / 1_000_000_000
                if let transferElapsedS = transferElapsedBox.value {
                    let bytesPerSec = Double(imageData.count) / transferElapsedS
                    print("\n  BLE transfer: \(String(format: "%.2f", transferElapsedS))s (\(String(format: "%.0f", bytesPerSec)) B/s)")
                }
                print("Done: \(result), total \(String(format: "%.2f", elapsedS))s (including on-device decode + settle)")
            } catch {
                if let transferElapsedS = transferElapsedBox.value {
                    let bytesPerSec = Double(imageData.count) / transferElapsedS
                    print("\n  BLE transfer: \(String(format: "%.2f", transferElapsedS))s (\(String(format: "%.0f", bytesPerSec)) B/s)")
                }
                print("Push failed (after transfer completed: \(transferElapsedBox.value != nil)): \(error)")
            }
            exit(0)
        case .failure(let error):
            print("Failure: \(error)")
            exit(1)
        case .disconnected(let reason):
            print("Disconnected: \(reason ?? "unknown")")
            if !pushStarted { exit(1) }
        case .imageChunkAck(let seq):
            print("\n  chunk ack: seq=\(seq)")
        default:
            break
        }
    }
}

// Keep the process alive for the async event loop above; the handlers above
// call exit(...) once the benchmark completes or fails. Scanning itself starts
// from the .bluetoothStateChanged handler above, once the central is actually
// powered on -- starting it eagerly here races central manager init and
// CoreBluetooth silently drops the scan request while state is still .unknown.
_ = await listener.value
