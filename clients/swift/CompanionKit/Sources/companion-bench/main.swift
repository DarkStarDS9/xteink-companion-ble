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

// A macOS-native harness: exercises the exact same CompanionKit code path
// (CoreBluetooth negotiation, ContentFramer chunk sizing off
// maximumWriteValueLength) that Snap2Ink and SpokenFeeds run on a real iPhone.
// It began as a throughput benchmark isolating "is bleak/Python slow" from "is
// the Mac's own CoreBluetooth/radio slow"; the scenarios below extend it into
// the link-robustness investigation of 2026-08-03 (disconnects, stale titles,
// second-image failures).
//
// Usage: swift run companion-bench [scenario] [options]
//
//   image                          push one image, report throughput (the original benchmark)
//   image-repeat  [n] [gapS]       T4: n images with gapS seconds of quiet between them.
//                                  Isolates "image 2 starts on the relaxed
//                                  connection profile" -- image 1 is the only
//                                  transfer that begins on an already-busy link.
//   text-burst    [n] [gapS]       T2: n full-page title+body pushes gapS seconds apart.
//                                  Reproduces SpokenFeeds' burst pattern; watch for
//                                  the device dropping a title on a sequence gap while
//                                  the body lands, which is what strands a stale title.
//   idle-hold     [holdS]          T1: acquire the screen, push once, then sit silent for
//                                  holdS seconds. Answers "why does it disconnect on a
//                                  clock" -- pair with the device's HCI reason log.
//
// Options (any position):
//   --image <path>                 raw 2bpp image (default /tmp/bench_image.raw)
//
// Every scenario prints wall-clock-stamped lines so the output interleaves
// cleanly with a timestamped serial capture from the device.

// MARK: - Argument parsing

var rawArgs = Array(CommandLine.arguments.dropFirst())
var imagePath = "/tmp/bench_image.raw"
if let flagIndex = rawArgs.firstIndex(of: "--image") {
    guard flagIndex + 1 < rawArgs.count else {
        FileHandle.standardError.write("--image needs a path\n".data(using: .utf8)!)
        exit(2)
    }
    imagePath = rawArgs[flagIndex + 1]
    rawArgs.removeSubrange(flagIndex...(flagIndex + 1))
}

let scenario = rawArgs.first ?? "image"
let positional = Array(rawArgs.dropFirst())
func positionalInt(_ index: Int, default fallback: Int) -> Int {
    guard index < positional.count, let value = Int(positional[index]) else { return fallback }
    return value
}
func positionalDouble(_ index: Int, default fallback: Double) -> Double {
    guard index < positional.count, let value = Double(positional[index]) else { return fallback }
    return value
}

let startedAt = Date()
let stamp: DateFormatter = {
    let f = DateFormatter()
    f.dateFormat = "HH:mm:ss.SSS"
    return f
}()
func say(_ message: String) {
    print("[\(stamp.string(from: Date()))] [+\(String(format: "%6.2f", Date().timeIntervalSince(startedAt)))s] \(message)")
}

// Only the image-bearing scenarios need the file; text-burst and idle-hold run
// without one, so a missing /tmp/bench_image.raw must not block them.
let needsImage = scenario == "image" || scenario == "image-repeat"
var imageData = Data()
if needsImage {
    guard let loaded = FileManager.default.contents(atPath: imagePath) else {
        FileHandle.standardError.write("Could not read \(imagePath)\n".data(using: .utf8)!)
        exit(1)
    }
    imageData = loaded
    say("Loaded \(imageData.count) bytes from \(imagePath)")
}

// MARK: - Synthetic article text

/// A body sized like a real SpokenFeeds article page: long enough to span many
/// CHUNKs, which is the condition under which a dropped Write Without Response
/// packet actually becomes likely. A one-packet body would never reproduce the
/// bug regardless of how fast we push.
func syntheticBody(index: Int) -> String {
    let paragraph = """
    Article \(index) body. The quick brown fox jumps over the lazy dog while the \
    companion display renders a full page of text pushed over Bluetooth Low Energy \
    from a phone that has no idea how long an electrophoretic panel actually takes \
    to settle. This sentence exists to occupy chunks.
    """
    return String(repeating: paragraph + "\n\n", count: 6)
}

func syntheticTitle(index: Int) -> String {
    "Article \(index) — a deliberately distinguishable headline"
}

// MARK: - Client setup

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

let tokenStore = UserDefaultsTokenStore(prefix: "CompanionBench.token.")
let assets = StaticAssetProvider(uiDeclaration: .readerDefault())

let client = CompanionClient(
    identity: identity,
    tokenStore: tokenStore,
    assets: assets,
    acquireScreenOnConnect: true,
    log: { say("  [kit] \($0)") }
)

// MARK: - Scenario bodies

// `imageData` is passed in rather than read from the top-level binding: a
// top-level `var` is main-actor isolated, and reading it from these nonisolated
// async helpers is an error under the Swift 6 language mode.
func runImageOnce(label: String, imageData: Data) async {
    let start = DispatchTime.now()
    // Tracked separately from the overall result: pushImage's await also covers
    // the device's post-transfer decode + grayscale settle, which can run long
    // enough for the link to drop before the status notification arrives -- in
    // which case pushImage throws with the BLE-transfer time already elapsed and
    // worth reporting on its own.
    let transferElapsedBox = ElapsedBox()
    do {
        let result = try await client.pushImage(imageData) { progress in
            if progress >= 1.0 {
                transferElapsedBox.setIfUnset(
                    Double(DispatchTime.now().uptimeNanoseconds - start.uptimeNanoseconds) / 1_000_000_000)
            }
        }
        let elapsedS = Double(DispatchTime.now().uptimeNanoseconds - start.uptimeNanoseconds) / 1_000_000_000
        if let transferElapsedS = transferElapsedBox.value {
            let bytesPerSec = Double(imageData.count) / transferElapsedS
            say("\(label) BLE transfer: \(String(format: "%.2f", transferElapsedS))s "
                + "(\(String(format: "%.0f", bytesPerSec)) B/s)")
        }
        say("\(label) done: \(result), total \(String(format: "%.2f", elapsedS))s (incl. on-device decode + settle)")
    } catch {
        if let transferElapsedS = transferElapsedBox.value {
            let bytesPerSec = Double(imageData.count) / transferElapsedS
            say("\(label) BLE transfer: \(String(format: "%.2f", transferElapsedS))s "
                + "(\(String(format: "%.0f", bytesPerSec)) B/s)")
        }
        say("\(label) FAILED (transfer had completed: \(transferElapsedBox.value != nil)): \(error)")
    }
}

/// T2/T4 share this shape: the push is wrapped in its own timeout watch so a
/// wedged command gate shows up as "push N never returned" rather than the
/// harness silently hanging. The watch only reports -- it cannot cancel the
/// underlying await, which is precisely the condition being measured.
func runTextBurst(count: Int, gapSeconds: Double) async {
    say("text-burst: \(count) pushes, \(gapSeconds)s apart")
    for index in 1...count {
        let title = syntheticTitle(index: index)
        let body = syntheticBody(index: index)
        say("push \(index)/\(count): title=\(title.count)B body=\(body.count)B")
        let started = Date()
        let finished = ElapsedBox()
        let watchdog = Task {
            try? await Task.sleep(nanoseconds: 20 * 1_000_000_000)
            if finished.value == nil {
                say("!! push \(index) has not returned after 20s — command gate is wedged "
                    + "(expect nothing further to work until the link drops)")
            }
        }
        do {
            try await client.push(title: title, body: body, contentId: "bench-\(index)")
            finished.setIfUnset(Date().timeIntervalSince(started))
            say("push \(index) ok in \(String(format: "%.2f", Date().timeIntervalSince(started)))s")
        } catch {
            finished.setIfUnset(Date().timeIntervalSince(started))
            say("push \(index) FAILED after \(String(format: "%.2f", Date().timeIntervalSince(started)))s: \(error)")
        }
        watchdog.cancel()
        if index < count {
            try? await Task.sleep(nanoseconds: UInt64(gapSeconds * 1_000_000_000))
        }
    }
    say("text-burst complete")
}

func runImageRepeat(count: Int, gapSeconds: Double, imageData: Data) async {
    say("image-repeat: \(count) images, \(gapSeconds)s of quiet between them")
    for index in 1...count {
        // The gap before each push after the first is the whole point: it is
        // longer than the firmware's 3 s kConnIdleRelaxMs, so every image except
        // the first begins on the relaxed 150 ms / latency-4 profile.
        if index > 1 {
            say("quiet for \(gapSeconds)s (link should relax to the idle profile)")
            try? await Task.sleep(nanoseconds: UInt64(gapSeconds * 1_000_000_000))
        }
        await runImageOnce(label: "image \(index)/\(count):", imageData: imageData)
    }
    say("image-repeat complete")
}

func runIdleHold(holdSeconds: Double) async {
    say("idle-hold: one push, then \(holdSeconds)s of silence")
    do {
        try await client.push(title: "Idle hold", body: syntheticBody(index: 0), contentId: "bench-idle")
        say("baseline push ok; going quiet now — watch the device log for the disconnect reason")
    } catch {
        say("baseline push FAILED: \(error)")
    }
    // Heartbeat only to the console, deliberately nothing on the wire: any
    // traffic would reset the firmware's idle timer and mask the effect.
    var elapsed = 0.0
    while elapsed < holdSeconds {
        try? await Task.sleep(nanoseconds: 5 * 1_000_000_000)
        elapsed += 5
        say("still holding, \(Int(elapsed))s / \(Int(holdSeconds))s of silence")
    }
    say("idle-hold complete without the harness sending anything")
}

// MARK: - Event loop

var scenarioStarted = false
var connectStarted = false

let listener = Task {
    for await event in client.events {
        switch event {
        case .bluetoothStateChanged(let available):
            say("Bluetooth available: \(available)")
            if available {
                say("Scanning for companion device...")
                client.startScanning()
            }
        case .discovered(let device):
            guard !connectStarted else { continue }
            connectStarted = true
            say("Found \(device.name) (\(device.id)), connecting...")
            client.connect(device)
        case .stateChanged(let state):
            say("state -> \(state)")
            if state == .awaitingUserConfirmation {
                say("  >>> Press CONFIRM on the device to approve pairing. <<<")
            }
        case .connected(let caps):
            say("Capabilities: \(caps.screenPixelWidth)x\(caps.screenPixelHeight)px, "
                + "max image \(caps.maxImageFieldLength)B")
        case .gainedScreen:
            guard !scenarioStarted else { continue }
            scenarioStarted = true
            say("Screen acquired. Running scenario '\(scenario)'.")
            switch scenario {
            case "image":
                await runImageOnce(label: "image:", imageData: imageData)
            case "image-repeat":
                await runImageRepeat(count: positionalInt(0, default: 3),
                                     gapSeconds: positionalDouble(1, default: 10),
                                     imageData: imageData)
            case "text-burst":
                await runTextBurst(count: positionalInt(0, default: 12),
                                   gapSeconds: positionalDouble(1, default: 3))
            case "idle-hold":
                await runIdleHold(holdSeconds: positionalDouble(0, default: 300))
            default:
                say("Unknown scenario '\(scenario)'. See the usage comment at the top of main.swift.")
                exit(2)
            }
            exit(0)
        case .fieldSeqGap(let field):
            // The device dropped this field under Write Without Response. For
            // title/body this is exactly the stale-title mechanism: the batch
            // still commits, so the surviving field lands and the dropped one
            // keeps whatever the previous article left on screen.
            say("!! device dropped field \(field) (CHUNK sequence gap)")
        case .renderStatus(let field, let result):
            say("render status: field=\(field) result=\(result)")
        case .imageChunkAck(let seq):
            say("chunk ack: seq=\(seq)")
        case .lostScreen(let reason):
            say("!! lost the screen: \(reason)")
        case .failure(let error):
            say("Failure: \(error)")
            exit(1)
        case .disconnected(let reason):
            say("!! Disconnected: \(reason ?? "unknown") "
                + "(scenario had started: \(scenarioStarted))")
            // A disconnect mid-scenario is a result, not a harness error: the
            // scenario's own await will fail out and report. Only bail here if
            // we never got far enough to run anything.
            if !scenarioStarted { exit(1) }
        default:
            break
        }
    }
}

// Keep the process alive for the async event loop above; the handlers above
// call exit(...) once the scenario completes or fails. Scanning itself starts
// from the .bluetoothStateChanged handler above, once the central is actually
// powered on -- starting it eagerly here races central manager init and
// CoreBluetooth silently drops the scan request while state is still .unknown.
_ = await listener.value
