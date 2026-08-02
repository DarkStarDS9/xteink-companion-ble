import CoreBluetooth
import Foundation

/// A device found while scanning.
public struct CompanionDevice: Identifiable, Equatable, @unchecked Sendable {
    public let id: UUID
    public let name: String
    public let rssi: Int
    let peripheral: CBPeripheral

    public static func == (lhs: CompanionDevice, rhs: CompanionDevice) -> Bool { lhs.id == rhs.id }
}

/// Where the client is in the session lifecycle. Poll ``CompanionClient/state``
/// for a snapshot, or drive UI off the ``CompanionEvent/stateChanged`` events —
/// `awaitingUserConfirmation` in particular has real UI attached ("confirm on
/// your reader").
public enum CompanionSessionState: Equatable, Sendable {
    case disconnected
    case connecting
    /// Connected and capability read; the handshake is in flight.
    case handshaking
    /// The device is showing a pairing prompt. Tell the user to look at it.
    case awaitingUserConfirmation
    case denied(HelloDeniedReason)
    /// Session established; assets are being reconciled.
    case reconcilingAssets
    /// Session established and assets current, but this session does not own the
    /// screen. Pushes will throw ``CompanionError/noScreen``.
    case idle
    /// This session owns the screen. Pushes are legal.
    case hasScreen
}

/// Everything the device tells the app, in one stream.
public enum CompanionEvent: Sendable {
    case bluetoothStateChanged(isAvailable: Bool)
    case discovered(CompanionDevice)
    case stateChanged(CompanionSessionState)
    /// Link is up and the capability characteristic has been read. The handshake
    /// starts automatically after this.
    case connected(CompanionCapabilities)
    /// The device is showing a pairing prompt. Tell the user to look at it.
    case pairingPending
    case pairingDenied(HelloDeniedReason)
    /// The screen was granted — on the first `acquireScreen()` and again after
    /// every preemption. **Re-push everything here**: the device retains nothing
    /// for a session that was not foreground, and any push that was in flight
    /// when the screen was lost was discarded.
    case gainedScreen
    /// Handshake complete; assets are being synced and, if requested, the screen
    /// acquired. Content pushes are legal from here on but will be dropped until
    /// ``CompanionEvent/foreground`` arrives.
    case sessionEstablished(sessionId: UInt8)
    case assetSynced(field: CompanionField, result: AssetResult)
    case lostScreen(BackgroundReason)
    case acquireDenied(AcquireDeniedReason)
    case buttonEvent(CompanionButtonEvent)
    case imageStatus(ImageResult)
    /// v9: progress marker during an in-flight image push, roughly every 32
    /// chunks — see ``SessionNotification/imageChunkAck``. Diagnostic only;
    /// `pushImage` still resolves from the final `imageStatus`.
    case imageChunkAck(seq: UInt16)
    /// v10: a title/body push's CHUNK sequence number skipped ahead of what
    /// the device expected, under Write Without Response — see
    /// ``SessionNotification/fieldSeqGap``. The device already dropped the
    /// field; a caller that wants it on screen has to re-push it.
    case fieldSeqGap(field: CompanionField)
    case disconnected(reason: String?)
    case failure(CompanionError)
}

/// Supplies the per-peer assets the device stores and versions by tag.
///
/// The UI declaration is mandatory — without one the device refuses `ACQUIRE`.
/// The icon is optional but is what puts the app on the sleep screen.
public protocol CompanionAssetProvider: AnyObject, Sendable {
    var uiDeclaration: UiDeclaration { get }
    /// Raw 1-bpp bitmap, row-major, MSB first, exactly
    /// ``CompanionCapabilities/iconByteCount`` bytes for the advertised icon
    /// size. `nil` to not have an icon.
    func icon(for capabilities: CompanionCapabilities) -> Data?
}

/// A ``CompanionAssetProvider`` for apps that just want to hand over two values.
public final class StaticAssetProvider: CompanionAssetProvider, @unchecked Sendable {
    public let uiDeclaration: UiDeclaration
    private let iconBitmap: Data?

    public init(uiDeclaration: UiDeclaration, icon: Data? = nil) {
        self.uiDeclaration = uiDeclaration
        self.iconBitmap = icon
    }

    public func icon(for capabilities: CompanionCapabilities) -> Data? {
        guard let iconBitmap, iconBitmap.count == capabilities.iconByteCount else { return nil }
        return iconBitmap
    }
}

/// The client. One instance per app; it owns a `CBCentralManager`, so do not
/// create several.
///
/// Typical use:
/// ```swift
/// let client = CompanionClient(identity: identity,
///                               assets: StaticAssetProvider(uiDeclaration: .readerDefault()))
/// Task { for await event in client.events { handle(event) } }
/// client.startScanning()
/// // ... on `.discovered`:
/// client.connect(device)      // handshake, asset sync and ACQUIRE happen automatically
/// // ... on `.foreground`:
/// try await client.push(title: "Headline", body: text, contentId: id)
/// ```
public final class CompanionClient: NSObject, @unchecked Sendable {
    // MARK: Configuration

    private let identity: CompanionIdentity
    private let tokenStore: CompanionTokenStore
    private let assets: CompanionAssetProvider
    private let log: @Sendable (String) -> Void
    /// Backing store for ``isVerboseLoggingEnabled``; guarded by ``lock``.
    private var verboseLogging: Bool
    /// Whether the handshake should end by asking for the screen. This is an
    /// app-intent flag, never a lifecycle mirror — see ``acquireScreen()``.
    private var wantsScreen: Bool

    // MARK: Bluetooth state

    private let queue = DispatchQueue(label: "CompanionKit.central")
    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var contentChar: CBCharacteristic?
    private var buttonChar: CBCharacteristic?
    private var capabilityChar: CBCharacteristic?
    private var statusChar: CBCharacteristic?
    private var sessionChar: CBCharacteristic?

    private let lock = StateLock()
    private var capabilities: CompanionCapabilities?
    private var sessionId: UInt8 = CompanionProtocol.noSession
    private var ownsScreen = false
    private var assetsReconciled = false
    private var sessionState: CompanionSessionState = .disconnected
    private var helloTag: UInt16 = 0
    /// The most recent content-id pushed on this session, for the staleness
    /// filter — see ``CompanionButtonEvent`` delivery in ``handleButtonEvent``.
    private var lastContentId: Data?

    /// Serializes every multi-packet operation. Two overlapping pushes would
    /// interleave their frames on one characteristic and reassemble as garbage —
    /// the device drops a partial field the moment a new START arrives.
    private let gate = CommandGate()

    private var writeContinuations: [CheckedContinuation<Void, Error>] = []
    private var pendingHello: Pending<SessionMessage>?
    private var pendingAcquire: Pending<Void>?
    private var pendingAssetAcks: [UInt8: Pending<AssetResult>] = [:]
    private var pendingImageStatus: Pending<ImageResult>?

    private var eventContinuation: AsyncStream<CompanionEvent>.Continuation?
    /// Events from the device. A single consumer; iterate it in a `Task`.
    public let events: AsyncStream<CompanionEvent>

    // MARK: Lifecycle

    /// - Parameters:
    ///   - acquireScreenOnConnect: whether the handshake should end by asking for
    ///     the screen. `true` suits an app that connects because it wants to
    ///     display something. Pass `false` if the app connects for other reasons
    ///     and will call ``acquireScreen()`` later; you can also change it at any
    ///     time with ``acquireScreen()`` / ``releaseScreen()`` before connecting.
    ///   - verboseLogging: whether per-packet BLE transport chatter reaches
    ///     `log`. Off by default, and it should stay off in shipping builds —
    ///     see ``isVerboseLoggingEnabled``.
    ///   - log: diagnostics sink. Defaults to dropping them — apps with their own
    ///     event log should pass a closure rather than hunting in os_log.
    public init(identity: CompanionIdentity,
                tokenStore: CompanionTokenStore = KeychainTokenStore(),
                assets: CompanionAssetProvider,
                acquireScreenOnConnect: Bool = true,
                verboseLogging: Bool = false,
                log: @escaping @Sendable (String) -> Void = { _ in }) {
        self.identity = identity
        self.tokenStore = tokenStore
        self.assets = assets
        self.wantsScreen = acquireScreenOnConnect
        self.verboseLogging = verboseLogging
        self.log = log

        var continuation: AsyncStream<CompanionEvent>.Continuation!
        self.events = AsyncStream { continuation = $0 }
        super.init()
        self.eventContinuation = continuation
        self.central = CBCentralManager(delegate: self, queue: queue)
    }

    /// Whether per-packet BLE transport chatter is written to the `log` sink.
    /// Default `false`, and deliberately so.
    ///
    /// `log` is not a developer-only os_log in practice: consumer apps wire it
    /// into user-visible diagnostic tapes (SpokenFeeds' "Copy Tape" JSONL
    /// export, for one), which people are asked to paste into bug reports. A
    /// line that fires per packet or per push drowns that tape — the WWR
    /// ready-with-no-waiters line alone fired twice per push, so a session of
    /// twenty-five articles buried everything else under fifty lines of it.
    ///
    /// So the rule for this flag: anything that fires on the happy path goes
    /// behind it; anything rare and actionable (the WWR wedge warning,
    /// sequence-gap drops, state transitions, pairing denials) stays
    /// unconditional. Toggle it at runtime from a debug menu when chasing a
    /// transport bug.
    public var isVerboseLoggingEnabled: Bool {
        get { lock.lock(); defer { lock.unlock() }; return verboseLogging }
        set { lock.lock(); verboseLogging = newValue; lock.unlock() }
    }

    /// Logs only when ``isVerboseLoggingEnabled``. The message is an
    /// autoclosure so an unwanted line costs no string interpolation at all —
    /// this is called from inside the WWR chunk loop, which runs hundreds of
    /// times per image push.
    private func logVerbose(_ message: @autoclosure () -> String) {
        lock.lock()
        let enabled = verboseLogging
        lock.unlock()
        guard enabled else { return }
        log(message())
    }

    /// The device's self-description, once connected.
    public var deviceCapabilities: CompanionCapabilities? {
        lock.lock(); defer { lock.unlock() }
        return capabilities
    }

    /// True while this session owns the screen. Content pushed at any other time
    /// is dropped by the device, so ``push(title:body:contentId:)`` throws rather
    /// than let that happen silently.
    ///
    /// Named after the screen, not after a lifecycle: the protocol's "background"
    /// and the phone OS's "app is backgrounded" are unrelated states, and an app
    /// can be in either without the other.
    public var hasScreen: Bool {
        lock.lock(); defer { lock.unlock() }
        return ownsScreen
    }

    /// Snapshot of the session lifecycle. Also delivered as
    /// ``CompanionEvent/stateChanged`` for UI that wants to observe it.
    public var state: CompanionSessionState {
        lock.lock(); defer { lock.unlock() }
        return sessionState
    }

    // MARK: Discovery

    public func startScanning() {
        // allowDuplicates: true — the advertised name lives only in the scan-response
        // packet (the primary advertisement is service-UUID-only to fit the legacy
        // 31-byte PDU; see the firmware's CompanionBle.cpp). With duplicates
        // suppressed, CoreBluetooth may deliver only the first-seen packet and never
        // redeliver once the scan response arrives, leaving didDiscover stuck without
        // a name for that peripheral for the rest of the scan.
        central.scanForPeripherals(withServices: [CBUUID(string: CompanionProtocol.serviceUUID)],
                                   options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
    }

    public func stopScanning() {
        central.stopScan()
    }

    public func connect(_ device: CompanionDevice) {
        stopScanning()
        lock.lock()
        peripheral = device.peripheral
        lock.unlock()
        device.peripheral.delegate = self
        transition(to: .connecting)
        central.connect(device.peripheral, options: nil)
    }

    public func disconnect() {
        lock.lock()
        let target = peripheral
        lock.unlock()
        if let target { central.cancelPeripheralConnection(target) }
    }

    /// Ask for the screen.
    ///
    /// **Call this when your app wants the display, not when it comes to the
    /// phone's foreground.** This package deliberately does not observe
    /// `UIApplication` or `scenePhase`: an audio app playing in a pocket, a
    /// navigation app, anything that keeps doing useful work while
    /// OS-backgrounded should keep holding the screen, and wiring ownership to
    /// the app lifecycle would break exactly that case.
    ///
    /// The device's policy is last-requester-wins, unconditionally. Safe to call
    /// before connecting — it is remembered and applied after the handshake.
    public func acquireScreen() {
        lock.lock()
        wantsScreen = true
        let session = sessionId
        let holding = ownsScreen
        lock.unlock()

        guard session != CompanionProtocol.noSession, !holding else { return }
        Task { try? await acquire() }
    }

    /// Give up the screen. Call it when you no longer want the display — not
    /// because the phone backgrounded your app. See ``acquireScreen()``.
    public func releaseScreen() {
        lock.lock()
        wantsScreen = false
        let session = sessionId
        let holding = ownsScreen
        lock.unlock()

        guard session != CompanionProtocol.noSession, holding else { return }
        writeSession(SessionCodec.encodeRelease(sessionId: session))
    }

    // MARK: Content

    /// Pushes any combination of title, body and content-id as one atomic batch:
    /// the device commits and redraws only when the last field lands, so a
    /// headline never appears seconds before its body.
    ///
    /// Text over ``CompanionCapabilities/maxTextFieldLength`` is truncated by the
    /// device; content-id over 32 bytes likewise. Pass a fresh `contentId` with
    /// every update if you care about correlating button events to what was on
    /// screen when they were pressed.
    public func push(title: String? = nil,
                     body: String? = nil,
                     contentId: Data? = nil,
                     tags: TagStateUpdate? = nil) async throws {
        var fields: [(CompanionField, Data)] = []
        if let title { fields.append((.title, Data(title.utf8))) }
        if let body { fields.append((.body, Data(body.utf8))) }
        if let contentId { fields.append((.contentId, contentId.prefix(CompanionProtocol.maxContentIdLength))) }
        // Last, so it shares the batch's final flag: content and its tags then
        // commit in one redraw and there is never a frame where new content
        // wears the previous content's tags. Tags are not reset by a push, so
        // passing them here is the only way to change both atomically.
        if let tags { fields.append((.tagState, tags.encoded())) }
        guard !fields.isEmpty else { return }

        try await serialized { [self] in
            let session = try requireSession()
            self.lock.lock(); self.lastContentId = contentId; self.lock.unlock()
            for (index, field) in fields.enumerated() {
                try await sendField(field.0, payload: field.1,
                                    sessionId: session,
                                    isFinal: index == fields.count - 1)
            }
        }
    }

    /// Convenience over ``push(title:body:contentId:tags:)`` for a string content-id.
    public func push(title: String? = nil,
                     body: String? = nil,
                     contentId: String,
                     tags: TagStateUpdate? = nil) async throws {
        try await push(title: title, body: body, contentId: Data(contentId.utf8), tags: tags)
    }

    /// Pushes an already-dithered, already-encoded PNG and waits for the device
    /// to report how it went.
    ///
    /// **Encoding is the caller's job** — this package deliberately has no
    /// dithering or PNG code in it. Produce an 8-bit grayscale PNG at exactly
    /// ``CompanionCapabilities/screenPixelWidth`` x
    /// ``CompanionCapabilities/screenPixelHeight``, using only the four values in
    /// ``CompanionCapabilities/imageQuantizationLevels``. A larger image is
    /// scaled down on-device, which resamples and destroys the dither.
    ///
    /// Takes several seconds: BLE transfer plus a two-pass grayscale settle.
    /// `progress` is called on an arbitrary queue with 0...1.
    ///
    /// A transient failure is retried once, automatically — see
    /// ``isTransientImageFailure(_:)`` for which results qualify and why. The
    /// retry re-sends the whole image (there is no partial-resume anywhere in
    /// the protocol), so `progress` restarts from 0 and climbs to 1 a second
    /// time. That is the honest report: a bar that jumps back to the start is
    /// exactly what is happening on the wire. A caller that wants to say so in
    /// its UI can watch for the progress value decreasing.
    @discardableResult
    public func pushImage(_ png: Data, progress: (@Sendable (Double) -> Void)? = nil) async throws -> ImageResult {
        try await serialized { [self] in
            // Note the retry lives *inside* `serialized`: the gate is acquired
            // once for both attempts. Retrying by re-calling `pushImage` would
            // try to re-acquire a gate this task already holds, which is a
            // straight deadlock — CommandGate is a plain non-reentrant mutex.
            // Holding it across the retry is also what we want semantically:
            // another push interleaving between the two attempts would make the
            // device discard whichever field arrived first.
            let first = try await attemptImagePush(png, progress: progress)
            if first == .displayed { return first }
            guard Self.isTransientImageFailure(first) else { throw CompanionError.imageRejected(first) }

            log("image push failed with \(first); retrying once")
            // Re-checked rather than reused from the first attempt: the screen
            // can have been preempted, or the link dropped, while the failed
            // transfer was in flight. `attemptImagePush` calls `requireSession`
            // again, which throws in either case instead of spending another
            // ~104 KB of airtime on frames the device will discard.
            let second = try await attemptImagePush(png, progress: progress)
            guard second == .displayed else { throw CompanionError.imageRejected(second) }
            return second
        }
    }

    /// One transfer attempt. Assumes the command gate is already held.
    private func attemptImagePush(_ png: Data,
                                  progress: (@Sendable (Double) -> Void)?) async throws -> ImageResult {
        let session = try requireSession()
        if let limit = deviceCapabilities?.maxImageFieldLength, png.count > limit {
            throw CompanionError.payloadTooLarge(field: .image, bytes: png.count, limit: limit)
        }
        return try await withPendingImageStatus {
            try await self.sendField(.image, payload: png, sessionId: session, isFinal: true, progress: progress)
        }
    }

    /// Whether a non-`displayed` image result is worth one more try.
    ///
    /// Retrying is not free — a full-screen image is ~104 KB and takes seconds —
    /// so this retries only where the same bytes plausibly succeed next time,
    /// and never where a second attempt would just buy the same failure at
    /// double the wait. Case by case:
    ///
    /// - ``ImageResult/sequenceGap``: **retry.** A CHUNK went missing under
    ///   Write Without Response, which has no delivery guarantee and no
    ///   retransmission anywhere in this protocol — one dropped packet
    ///   currently kills the entire transfer. That is precisely the failure a
    ///   retry exists for, and it is per-packet luck rather than anything about
    ///   the image.
    /// - ``ImageResult/storageFailed``: **retry.** The device stages the image
    ///   to an SD scratch file; a failed write is a card/filesystem hiccup, not
    ///   a property of the payload. If the card is genuinely gone the retry
    ///   fails the same way and the caller sees the error one attempt later.
    /// - ``ImageResult/rejectedSize``: **no.** Deterministic, and computable
    ///   from the capability characteristic — the same bytes are the same size
    ///   next time.
    /// - ``ImageResult/decodeFailed``: **no.** Also deterministic in the payload:
    ///   a complete transfer that the device could not make sense of will not
    ///   make more sense on a second reading. (A *truncated* transfer is not
    ///   this case — the device reports that as a sequence gap.)
    /// - ``ImageResult/unknown``: **no.** A result byte this package does not
    ///   recognise carries no claim that it is transient, and blind retrying on
    ///   "don't know" is how a future permanent rejection turns into a doubled
    ///   wait for every caller.
    private static func isTransientImageFailure(_ result: ImageResult) -> Bool {
        switch result {
        case .sequenceGap, .storageFailed:
            return true
        case .displayed, .decodeFailed, .rejectedSize, .unknown:
            return false
        }
    }

    /// Runs `body` with the command gate held, so only one multi-packet
    /// operation is ever in flight on the Content characteristic.
    private func serialized<T>(_ body: () async throws -> T) async throws -> T {
        await gate.acquire()
        do {
            let value = try await body()
            await gate.release()
            return value
        } catch {
            await gate.release()
            throw error
        }
    }

    /// Sets one declared tag, without re-pushing content.
    ///
    /// Use this when a tag changes but the content did not — the user saved the
    /// article already on screen, playback started, a sync finished. When the
    /// content is changing too, pass a ``TagStateUpdate`` to
    /// ``push(title:body:contentId:tags:)`` instead so the two commit together.
    ///
    /// `id` must be one your ``UiDeclaration`` declared; the device ignores any
    /// other. Tags do **not** clear themselves when you push new content.
    public func setTag(_ id: UInt8, _ state: TagState) {
        lock.lock()
        let session = sessionId
        let characteristic = statusChar
        let target = peripheral
        lock.unlock()
        guard session != CompanionProtocol.noSession, let characteristic, let target else { return }
        target.writeValue(Data([session, id, state.rawValue]), for: characteristic, type: .withoutResponse)
    }

    // MARK: Handshake

    private func beginHandshake() {
        Task { [self] in
            do {
                try await handshake()
            } catch let error as CompanionError {
                emit(.failure(error))
            } catch {
                emit(.failure(.malformedMessage))
            }
        }
    }

    private func handshake() async throws {
        transition(to: .handshaking)
        guard let capabilities = deviceCapabilities else { throw CompanionError.malformedCapabilities }
        let deviceKey = capabilities.deviceIdHex
        let storedToken = tokenStore.token(forDeviceId: deviceKey)

        let tag = UInt16.random(in: 1 ... UInt16.max)
        lock.lock(); helloTag = tag; lock.unlock()

        let reply = try await withPendingHello {
            self.writeSession(SessionCodec.encodeHello(helloTag: tag,
                                                       appId: self.identity.appId,
                                                       installId: self.identity.installId,
                                                       token: storedToken,
                                                       displayName: self.identity.displayName,
                                                       userName: self.identity.userName))
        }

        guard case let .helloOK(_, session, token, assetTags) = reply else {
            if case let .helloDenied(_, reason) = reply {
                log("pairing denied: \(reason)")
                // A denied token is the interesting case: the device forgot us
                // (peer directory deleted, or evicted). Drop it so the next
                // attempt asks for a fresh pairing instead of re-presenting a
                // token the device will keep rejecting.
                if reason == .userRejected || reason == .timeout {
                    tokenStore.setToken(nil, forDeviceId: deviceKey)
                }
                transition(to: .denied(reason))
                emit(.pairingDenied(reason))
                throw CompanionError.pairingDenied(reason)
            }
            throw CompanionError.malformedMessage
        }

        tokenStore.setToken(token, forDeviceId: deviceKey)
        lock.lock(); sessionId = session; assetsReconciled = false; lock.unlock()
        emit(.sessionEstablished(sessionId: session))

        transition(to: .reconcilingAssets)
        try await syncAssets(reported: assetTags, capabilities: capabilities, sessionId: session)
        lock.lock(); assetsReconciled = true; let wants = wantsScreen; lock.unlock()
        transition(to: .idle)

        // Only now is ACQUIRE legal: the device refuses it from a peer with no
        // stored button map.
        if wants { try await acquire() }
    }

    /// Pushes only the assets whose stored tag differs from ours. The device
    /// never compares or hashes anything — staleness is entirely this side's
    /// conclusion.
    private func syncAssets(reported: [UInt8: AssetTag],
                            capabilities: CompanionCapabilities,
                            sessionId session: UInt8) async throws {
        let declaration = assets.uiDeclaration
        if reported[CompanionField.uiDeclaration.rawValue] != declaration.tag {
            let result = try await sendAsset(.uiDeclaration,
                                             payload: declaration.encodedAsset(),
                                             sessionId: session)
            emit(.assetSynced(field: .uiDeclaration, result: result))
            guard result == .stored else {
                throw CompanionError.assetRejected(field: .uiDeclaration, result: result)
            }
        }

        guard capabilities.supportsIcons, let icon = assets.icon(for: capabilities) else { return }
        let iconTag = AssetTag.contentHash(of: icon)
        if reported[CompanionField.icon.rawValue] != iconTag {
            var payload = iconTag.bytes
            payload.append(icon)
            let result = try await sendAsset(.icon, payload: payload, sessionId: session)
            emit(.assetSynced(field: .icon, result: result))
            // A rejected icon is cosmetic: the app still works, it just does not
            // appear on the sleep screen. Not worth failing the connection over.
        }
    }

    private func acquire() async throws {
        // needsScreen: false — acquiring the screen is how ownsScreen becomes
        // true in the first place; requiring it here made ACQUIRE impossible
        // to ever call after a preemption (see requireSession(needsScreen:)).
        let session = try requireSession(needsScreen: false)
        try await withPendingAcquire {
            self.writeSession(SessionCodec.encodeAcquire(sessionId: session))
        }
    }

    // MARK: Sending

    /// The session id to stamp on outgoing frames, or a thrown error explaining
    /// why the push would have gone nowhere.
    ///
    /// Both gates matter. The device silently drops a frame from a session that
    /// does not own the screen, and it refuses `ACQUIRE` from a peer whose assets
    /// have not landed — so an app that pushes a beat early gets a blank reader
    /// and no diagnostic at all. Failing here turns that into a thrown error at
    /// the call site.
    private func requireSession(needsScreen: Bool = true) throws -> UInt8 {
        lock.lock(); defer { lock.unlock() }
        guard peripheral != nil, contentChar != nil else { throw CompanionError.notConnected }
        guard sessionId != CompanionProtocol.noSession else { throw CompanionError.noSession }
        // Assets must be reconciled before ACQUIRE is legal (the device
        // refuses it otherwise) as well as before any content push.
        guard assetsReconciled else { throw CompanionError.noScreen }
        if needsScreen {
            guard ownsScreen else { throw CompanionError.noScreen }
        }
        return sessionId
    }

    private func sendField(_ field: CompanionField,
                           payload: Data,
                           sessionId: UInt8,
                           isFinal: Bool,
                           progress: (@Sendable (Double) -> Void)? = nil) async throws {
        lock.lock()
        let characteristic = contentChar
        let target = peripheral
        lock.unlock()
        guard let characteristic, let target else { throw CompanionError.notConnected }

        // The image field's bulk CHUNKs go out over Write Without Response as
        // of v9, joined by title/body in v10 — see
        // docs/companion-display-protocol.md "Image field" and "Title/body
        // fields". A write-with-response round trip costs ~120ms regardless
        // of the negotiated connection interval (measured on real hardware,
        // see companion-bench), which floors a bulk image transfer and, for
        // title/body, made a rapid sequence of full-page pushes visibly fall
        // behind the content driving them. WWR has no such round trip, but
        // drops CoreBluetooth's own delivery guarantee, which is why these
        // fields' CHUNK format carries a sequence number (device-side gap
        // detection) — image additionally gets a periodic ack (see
        // ``SessionMessage/imageChunkAck``); title/body are short enough that
        // ``SessionMessage/fieldSeqGap`` at END is the only signal. START and
        // END stay Write, so the phone still gets a reliable begin/end ack;
        // every other field is untouched (Write throughout, same as pre-v9).
        let chunkWriteType: CBCharacteristicWriteType =
            (field == .image || field == .title || field == .body) ? .withoutResponse : .withResponse
        let attPayload = target.maximumWriteValueLength(for: chunkWriteType)
        let framer = ContentFramer(field: field,
                                   sessionId: sessionId,
                                   payload: payload,
                                   isFinal: isFinal,
                                   maxChunkPayload: ContentFramer.chunkPayloadSize(forATTPayload: attPayload, field: field))
        let total = framer.packetCount
        var sent = 0
        for packet in framer {
            if chunkWriteType == .withoutResponse, packet.first == CompanionProtocol.opChunk {
                await waitUntilReadyForWriteWithoutResponse(on: target)
                target.writeValue(packet, for: characteristic, type: .withoutResponse)
            } else {
                try await write(packet, to: characteristic, on: target)
            }
            sent += 1
            progress?(Double(sent) / Double(total))
        }
    }

    /// CoreBluetooth's own backpressure for Write Without Response: the OS
    /// buffers a bounded number of un-acked WWR writes and calls
    /// `peripheralIsReady(toSendWriteWithoutResponse:)` once space frees up.
    /// Writing past that without waiting drops the excess silently at the OS
    /// layer — this is what keeps the image CHUNK loop from outrunning it.
    private var writeWithoutResponseWaiters: [WriteWithoutResponseWaiter] = []
    /// Monotonic id per wait, so the log can pair an enter with its resume and
    /// name the one that never came back. Introduced by the 2026-08-03
    /// link-robustness investigation and kept: the fallback message below is
    /// useless without a way to identify which wait tripped it.
    private var writeWithoutResponseWaitCounter = 0
    /// How long a wait may sit unresumed before the fallback resumes it anyway.
    ///
    /// Bound justification: the negotiated connection interval on this device is
    /// 15-60 ms, and CoreBluetooth frees WWR buffer space within a connection
    /// event or two — a normal wait resolves in well under 150 ms. Two seconds is
    /// therefore more than an order of magnitude past any legitimate wait, so the
    /// fallback cannot fire during healthy operation and add spurious dropped
    /// chunks; and it is short enough that even the pathological case (every
    /// chunk waiting the full timeout) fails the transfer inside the image
    /// status timeout rather than parking the caller. It is also comfortably
    /// under BLE's supervision timeout, so a genuinely dead link produces a
    /// disconnect (which resumes all waiters) rather than a long series of
    /// fallbacks.
    private static let writeWithoutResponseFallbackSeconds: UInt64 = 2

    /// Waits until CoreBluetooth will accept another Write Without Response.
    ///
    /// The whole point of this function is a lost-wakeup race that was live from
    /// v9 (image CHUNKs over WWR) until 2026-08-03. The old shape read
    /// `canSendWriteWithoutResponse` *outside* the lock and only then took the
    /// lock to enqueue its continuation, while
    /// ``peripheralIsReady(toSendWriteWithoutResponse:)`` drains the queue under
    /// that same lock. Interleave them and the wakeup is lost outright: the
    /// sender sees `canSend == false`, CoreBluetooth fires `peripheralIsReady`
    /// and drains an empty queue, and only then does the sender enqueue. Nothing
    /// will ever resume that continuation — CoreBluetooth re-fires
    /// `peripheralIsReady` only after a further write attempt, and the only loop
    /// that would make one is suspended awaiting this very continuation.
    ///
    /// The cost of losing it is out of all proportion to the width of the
    /// window. This runs inside ``sendField``, inside ``push``/``pushImage``,
    /// which hold the ``CommandGate`` — so a single lost wakeup wedges the gate
    /// for the life of the connection, every later push queues behind a holder
    /// that never returns, and the device sits frozen on its last frame until
    /// the link drops and `CommandGate.reset()` bails everyone out.
    ///
    /// Two independent mitigations, because the failure mode is so expensive:
    ///
    /// 1. The `canSend` re-check happens *inside* the lock, atomically with the
    ///    decision to enqueue. That closes the race as such: either we observe
    ///    room and never enqueue, or we enqueue before any drain can miss us.
    /// 2. A bounded fallback resumes the waiter anyway if the wakeup somehow
    ///    still never arrives, so no future CoreBluetooth behaviour can turn a
    ///    lost edge back into a permanently wedged gate. Worst case we write a
    ///    chunk the OS drops, the device reports a sequence gap, and the push
    ///    fails or (for images) retries — recoverable, unlike a hang.
    private func waitUntilReadyForWriteWithoutResponse(on target: CBPeripheral) async {
        var waiter: WriteWithoutResponseWaiter?

        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            lock.lock()
            // The re-check and the append are one critical section; this is the
            // actual fix. `peripheralIsReady` cannot slip between them, because
            // it takes the same lock to drain.
            if target.canSendWriteWithoutResponse {
                lock.unlock()
                continuation.resume()
                return
            }
            writeWithoutResponseWaitCounter += 1
            let created = WriteWithoutResponseWaiter(id: writeWithoutResponseWaitCounter,
                                                     continuation: continuation)
            writeWithoutResponseWaiters.append(created)
            lock.unlock()
            waiter = created
            armWriteWithoutResponseFallback(for: created, on: target)
        }

        // Cancelling only stops the sleep; the resume-once guard inside
        // ``WriteWithoutResponseWaiter`` is what makes the cancel/fire race safe,
        // not this call.
        waiter?.cancelFallback()
    }

    /// Arms the bounded fallback for one waiter. Split out so the enqueue
    /// critical section above stays short and obviously non-suspending.
    private func armWriteWithoutResponseFallback(for waiter: WriteWithoutResponseWaiter,
                                                 on target: CBPeripheral) {
        let task = Task { [weak self] in
            try? await Task.sleep(nanoseconds: Self.writeWithoutResponseFallbackSeconds * 1_000_000_000)
            guard !Task.isCancelled, let self else { return }
            self.lock.lock()
            self.writeWithoutResponseWaiters.removeAll { $0 === waiter }
            let pendingCount = self.writeWithoutResponseWaiters.count
            let canSend = target.canSendWriteWithoutResponse
            self.lock.unlock()
            // `resume()` returns false if `peripheralIsReady` or the disconnect
            // teardown got there first, in which case there was no wedge and
            // there is nothing to report.
            guard waiter.resume() else { return }
            self.log("WWR WEDGE: waiter #\(waiter.id) unresumed after "
                + "\(Self.writeWithoutResponseFallbackSeconds)s "
                + "(pending=\(pendingCount), canSendWriteWithoutResponse=\(canSend)) "
                + "— peripheralIsReady never fired for it; resuming it anyway so the "
                + "command gate cannot wedge. The chunk about to be written may be "
                + "dropped by the OS and show up as a sequence gap.")
        }
        waiter.attachFallback(task)
    }

    /// Assets are pushed *before* `ACQUIRE` is legal, so they deliberately skip
    /// the screen-ownership gate in ``requireSession(needsScreen:)``.
    private func sendAsset(_ field: CompanionField, payload: Data, sessionId: UInt8) async throws -> AssetResult {
        try await withPendingAssetAck(field) {
            try await self.sendField(field, payload: payload, sessionId: sessionId, isFinal: false)
        }
    }

    private func writeSession(_ data: Data) {
        lock.lock()
        let characteristic = sessionChar
        let target = peripheral
        lock.unlock()
        guard let characteristic, let target else { return }
        target.writeValue(data, for: characteristic, type: .withResponse)
    }

    private func write(_ data: Data, to characteristic: CBCharacteristic, on target: CBPeripheral) async throws {
        try await withCheckedThrowingContinuation { continuation in
            lock.lock()
            writeContinuations.append(continuation)
            lock.unlock()
            target.writeValue(data, for: characteristic, type: .withResponse)
        }
    }

    // MARK: Pending-reply plumbing

    private func withPendingHello(_ body: @escaping () async throws -> Void) async throws -> SessionMessage {
        // 35 s: the device's own pairing-prompt timeout is 30 s, so a shorter
        // client timeout would give up while the user is still reaching for the
        // button.
        let pending = Pending<SessionMessage>()
        lock.lock(); pendingHello = pending; lock.unlock()
        return try await awaitReply(pending, timeout: 35, body: body)
    }

    private func withPendingAcquire(_ body: @escaping () async throws -> Void) async throws {
        let pending = Pending<Void>()
        lock.lock(); pendingAcquire = pending; lock.unlock()
        _ = try await awaitReply(pending, timeout: 10, body: body)
    }

    private func withPendingAssetAck(_ field: CompanionField,
                                     _ body: @escaping () async throws -> Void) async throws -> AssetResult {
        let pending = Pending<AssetResult>()
        lock.lock(); pendingAssetAcks[field.rawValue] = pending; lock.unlock()
        return try await awaitReply(pending, timeout: 15, body: body)
    }

    private func withPendingImageStatus(_ body: @escaping () async throws -> Void) async throws -> ImageResult {
        // Generous: the device stages to SD and then runs a two-pass grayscale
        // settle after the last byte arrives.
        let pending = Pending<ImageResult>()
        lock.lock(); pendingImageStatus = pending; lock.unlock()
        return try await awaitReply(pending, timeout: 120, body: body)
    }

    /// Runs `body` (which sends something) and waits for the matching
    /// notification to resolve `pending`, or for `timeout` to elapse.
    ///
    /// The timeout is armed against this specific ``Pending`` rather than
    /// against "whatever is outstanding", so a late timer from a completed
    /// operation cannot cancel the next one.
    private func awaitReply<T>(_ pending: Pending<T>,
                               timeout: TimeInterval,
                               body: @escaping () async throws -> Void) async throws -> T {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<T, Error>) in
            pending.arm(continuation)
            Task {
                do {
                    try await body()
                } catch {
                    pending.fail(error)
                    return
                }
                try? await Task.sleep(nanoseconds: UInt64(timeout * 1_000_000_000))
                pending.fail(CompanionError.timedOut)
            }
        }
    }

    /// Fails every outstanding reply. Called on disconnect, where nothing that
    /// is waiting can ever be answered.
    private func failPending(with error: Error) {
        lock.lock()
        let hello = pendingHello; pendingHello = nil
        let acquire = pendingAcquire; pendingAcquire = nil
        let acks = pendingAssetAcks; pendingAssetAcks = [:]
        let image = pendingImageStatus; pendingImageStatus = nil
        lock.unlock()

        hello?.fail(error)
        acquire?.fail(error)
        acks.values.forEach { $0.fail(error) }
        image?.fail(error)
    }

    private func emit(_ event: CompanionEvent) {
        eventContinuation?.yield(event)
    }

    private func transition(to newState: CompanionSessionState) {
        lock.lock()
        let changed = sessionState != newState
        sessionState = newState
        lock.unlock()
        guard changed else { return }
        log("state -> \(newState)")
        emit(.stateChanged(newState))
    }

    // MARK: Session notification handling

    private func handleSessionMessage(_ message: SessionMessage) {
        lock.lock()
        let expectedTag = helloTag
        let currentSession = sessionId
        lock.unlock()

        switch message {
        case let .helloOK(tag, _, _, _), let .helloDenied(tag, _):
            // Notifications reach every app sharing this phone's link — a reply
            // to somebody else's HELLO is not ours.
            guard tag == expectedTag else { return }
            lock.lock(); let pending = pendingHello; pendingHello = nil; lock.unlock()
            pending?.resume(message)

        case let .helloPending(tag):
            guard tag == expectedTag else { return }
            transition(to: .awaitingUserConfirmation)
            emit(.pairingPending)

        case let .foreground(session):
            guard session == currentSession else { return }
            lock.lock(); ownsScreen = true; let pending = pendingAcquire; pendingAcquire = nil; lock.unlock()
            pending?.resume(())
            transition(to: .hasScreen)
            // Fires on the first grant and on every re-grant after preemption.
            // The device retains nothing for a session that lost the screen, so
            // this is always "push everything again", not just the first time.
            emit(.gainedScreen)

        case let .background(session, reason):
            guard session == currentSession else { return }
            // Deliberately does NOT re-acquire. Two apps that both auto-reacquire
            // on preemption ping-pong the screen forever; last-requester-wins only
            // works if the losing side accepts the loss. Whether to ask again is
            // the app's call.
            lock.lock(); ownsScreen = false; lastContentId = nil; lock.unlock()
            log("lost the screen: \(reason)")
            transition(to: .idle)
            emit(.lostScreen(reason))

        case let .acquireDenied(session, reason):
            guard session == currentSession else { return }
            lock.lock(); let pending = pendingAcquire; pendingAcquire = nil; lock.unlock()
            pending?.fail(CompanionError.acquireDenied(reason))
            log("ACQUIRE denied: \(reason)")
            emit(.acquireDenied(reason))

        case let .assetAck(session, assetId, result, _):
            guard session == currentSession else { return }
            lock.lock(); let pending = pendingAssetAcks.removeValue(forKey: assetId); lock.unlock()
            pending?.resume(result)

        case let .imageStatus(session, result):
            guard session == currentSession else { return }
            lock.lock(); let pending = pendingImageStatus; pendingImageStatus = nil; lock.unlock()
            pending?.resume(result)
            emit(.imageStatus(result))

        case let .imageChunkAck(session, seq):
            guard session == currentSession else { return }
            emit(.imageChunkAck(seq: seq))

        case let .fieldSeqGap(session, fieldByte):
            guard session == currentSession else { return }
            guard let field = CompanionField(rawValue: fieldByte) else { return }
            log("field 0x\(String(format: "%02x", fieldByte)) dropped: CHUNK sequence gap")
            emit(.fieldSeqGap(field: field))
        }
    }
}

// MARK: - CBCentralManagerDelegate

extension CompanionClient: CBCentralManagerDelegate {
    public func centralManagerDidUpdateState(_ central: CBCentralManager) {
        emit(.bluetoothStateChanged(isAvailable: central.state == .poweredOn))
    }

    public func centralManager(_ central: CBCentralManager,
                               didDiscover peripheral: CBPeripheral,
                               advertisementData: [String: Any],
                               rssi RSSI: NSNumber) {
        // peripheral.name deliberately excluded: it's CoreBluetooth's own cached
        // GAP name for this peripheral's Bluetooth address, which can go stale
        // (persists across reflashes and even device reboots) and silently
        // disagree with what's actually being broadcast right now. Trust only
        // the live advertisement/scan-response payload.
        let name = (advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? "Companion"
        emit(.discovered(CompanionDevice(id: peripheral.identifier,
                                         name: name,
                                         rssi: RSSI.intValue,
                                         peripheral: peripheral)))
    }

    public func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        peripheral.discoverServices([CBUUID(string: CompanionProtocol.serviceUUID)])
    }

    public func centralManager(_ central: CBCentralManager,
                               didFailToConnect peripheral: CBPeripheral,
                               error: Error?) {
        emit(.failure(.notConnected))
    }

    public func centralManager(_ central: CBCentralManager,
                               didDisconnectPeripheral peripheral: CBPeripheral,
                               error: Error?) {
        lock.lock()
        sessionId = CompanionProtocol.noSession
        ownsScreen = false
        assetsReconciled = false
        lastContentId = nil
        capabilities = nil
        contentChar = nil; buttonChar = nil; capabilityChar = nil; statusChar = nil; sessionChar = nil
        let writes = writeContinuations
        writeContinuations = []
        let wwrWaiters = writeWithoutResponseWaiters
        writeWithoutResponseWaiters = []
        lock.unlock()

        writes.forEach { $0.resume(throwing: CompanionError.disconnected) }
        // Can't throw (CheckedContinuation<Void, Never>) -- waking it just lets
        // an in-flight image push's loop proceed to its next packet, which then
        // fails immediately via requireSession()/target-is-nil the same way any
        // other post-disconnect send would. `resume()` is a no-op for any waiter
        // the fallback timer already claimed.
        wwrWaiters.forEach { $0.resume() }
        failPending(with: CompanionError.disconnected)
        Task { await gate.reset() }
        transition(to: .disconnected)
        emit(.disconnected(reason: error?.localizedDescription))
    }
}

// MARK: - CBPeripheralDelegate

extension CompanionClient: CBPeripheralDelegate {
    public func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: {
            $0.uuid == CBUUID(string: CompanionProtocol.serviceUUID)
        }) else {
            emit(.failure(.notConnected))
            return
        }
        peripheral.discoverCharacteristics(nil, for: service)
    }

    public func peripheral(_ peripheral: CBPeripheral,
                           didDiscoverCharacteristicsFor service: CBService,
                           error: Error?) {
        lock.lock()
        for characteristic in service.characteristics ?? [] {
            switch characteristic.uuid.uuidString.uppercased() {
            case CompanionProtocol.contentCharacteristicUUID: contentChar = characteristic
            case CompanionProtocol.buttonCharacteristicUUID: buttonChar = characteristic
            case CompanionProtocol.capabilityCharacteristicUUID: capabilityChar = characteristic
            case CompanionProtocol.statusCharacteristicUUID: statusChar = characteristic
            case CompanionProtocol.sessionCharacteristicUUID: sessionChar = characteristic
            default: break
            }
        }
        let session = sessionChar
        let button = buttonChar
        let capability = capabilityChar
        lock.unlock()

        guard let session, let capability else {
            // No Session characteristic means pre-v6 firmware. There is no
            // fallback path by design — v6 was a clean break. There's no real
            // reported version to read at this point (the characteristic that
            // would carry it doesn't exist), so 5 is a stand-in for "obviously
            // pre-v6" rather than a value actually read off the device.
            emit(.failure(.unsupportedProtocolVersion(reported: 5, expected: CompanionProtocol.version)))
            return
        }
        peripheral.setNotifyValue(true, for: session)
        if let button { peripheral.setNotifyValue(true, for: button) }
        peripheral.readValue(for: capability)
    }

    public func peripheral(_ peripheral: CBPeripheral,
                           didUpdateValueFor characteristic: CBCharacteristic,
                           error: Error?) {
        guard let value = characteristic.value else { return }

        switch characteristic.uuid.uuidString.uppercased() {
        case CompanionProtocol.capabilityCharacteristicUUID:
            guard let parsed = CompanionCapabilities(value) else {
                emit(.failure(.malformedCapabilities))
                return
            }
            guard parsed.protocolVersion == CompanionProtocol.version else {
                emit(.failure(.unsupportedProtocolVersion(reported: parsed.protocolVersion, expected: CompanionProtocol.version)))
                return
            }
            lock.lock(); capabilities = parsed; lock.unlock()
            emit(.connected(parsed))
            beginHandshake()

        case CompanionProtocol.sessionCharacteristicUUID:
            log("session notify: \(value.map { String(format: "%02x", $0) }.joined())")
            guard let message = SessionCodec.decode(value) else { return }
            handleSessionMessage(message)

        case CompanionProtocol.buttonCharacteristicUUID:
            guard let event = CompanionButtonEvent(value) else { return }
            lock.lock()
            let session = sessionId
            let expectedContentId = lastContentId
            lock.unlock()
            guard event.sessionId == session else { return }
            // Staleness filter. The device echoes the content-id that was on
            // screen when the button was pressed; if it is not the one we last
            // pushed, the press was for content we have since replaced (a
            // reconnect race, a fast skip) and acting on it would apply the
            // user's intent to the wrong article.
            //
            // An *empty* echo is deliberately NOT treated as stale. It means the
            // session has no content-id at all — the user pressed a button before
            // the first push, or the app does not use content-ids. That is
            // uncorrelated, not stale, and there is nothing wrong to act on. An
            // app that pushes a content-id with every update will never see one
            // once content is up, so the permissive reading costs it nothing;
            // the strict reading would silently swallow every button press from
            // an app that uses no content-ids at all.
            if let expectedContentId, !event.contentId.isEmpty, event.contentId != expectedContentId {
                log("dropping stale button event (content-id mismatch)")
                return
            }
            emit(.buttonEvent(event))

        default:
            break
        }
    }

    public func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
        lock.lock()
        let waiters = writeWithoutResponseWaiters
        writeWithoutResponseWaiters = []
        lock.unlock()
        // DIAGNOSTIC: an empty drain used to be the precondition for the
        // lost-wakeup race — this callback landing between a sender's
        // `canSendWriteWithoutResponse` check and its append. It is no longer
        // dangerous (the check and the append are now one critical section, so
        // a sender arriving late simply observes the freed buffer space and
        // never suspends) and it is not on its own evidence of anything: it
        // also happens benignly whenever no send is in flight at all.
        //
        // Verbose-only: it fires twice per push on real hardware, which is far
        // too much for a shipping app's diagnostic tape. See
        // ``isVerboseLoggingEnabled``.
        if waiters.isEmpty {
            logVerbose("WWR ready fired with no waiters queued")
        }
        waiters.forEach { $0.resume() }
    }

    public func peripheral(_ peripheral: CBPeripheral,
                           didWriteValueFor characteristic: CBCharacteristic,
                           error: Error?) {
        lock.lock()
        let continuation = writeContinuations.isEmpty ? nil : writeContinuations.removeFirst()
        lock.unlock()
        if let error {
            continuation?.resume(throwing: error)
        } else {
            continuation?.resume()
        }
    }
}

// MARK: - Small helpers

/// A FIFO mutex for multi-packet operations. Overlapping pushes would interleave
/// their frames on a single characteristic, and the device discards a partial
/// field as soon as a new START arrives — so the first push would simply vanish.
actor CommandGate {
    private var busy = false
    private var waiting: [CheckedContinuation<Void, Never>] = []

    func acquire() async {
        if !busy {
            busy = true
            return
        }
        await withCheckedContinuation { waiting.append($0) }
    }

    func release() {
        if waiting.isEmpty {
            busy = false
        } else {
            waiting.removeFirst().resume()
        }
    }

    /// Called on disconnect, where whatever held the gate is never coming back to
    /// release it. Without this, one push left in flight when the peripheral drops
    /// wedges the gate forever — every push after a reconnect just queues behind a
    /// holder that no longer exists, and `serialized` never returns or throws. Woken
    /// waiters proceed straight into `requireSession()`, which throws immediately
    /// against the now-cleared session, so this cannot let two pushes run for real.
    func reset() {
        let waiters = waiting
        waiting.removeAll()
        busy = false
        waiters.forEach { $0.resume() }
    }
}

/// A recursive mutex guarding this client's mutable state.
///
/// Not `NSRecursiveLock`: its `lock()`/`unlock()` are marked unavailable from
/// async contexts (an error under the Swift 6 language mode), and half of this
/// client's state access happens inside `async` functions. The critical sections
/// are all a few assignments long and never suspend, so a plain pthread mutex is
/// the correct primitive rather than something to be talked out of.
final class StateLock: @unchecked Sendable {
    private var mutex = pthread_mutex_t()

    init() {
        var attributes = pthread_mutexattr_t()
        pthread_mutexattr_init(&attributes)
        pthread_mutexattr_settype(&attributes, Int32(PTHREAD_MUTEX_RECURSIVE))
        pthread_mutex_init(&mutex, &attributes)
        pthread_mutexattr_destroy(&attributes)
    }

    deinit { pthread_mutex_destroy(&mutex) }

    func lock() { pthread_mutex_lock(&mutex) }
    func unlock() { pthread_mutex_unlock(&mutex) }
}

/// One suspended sender waiting for CoreBluetooth to accept another Write
/// Without Response.
///
/// Exists purely to make the resume exactly-once. Three parties hold a reference
/// and each has a legitimate reason to wake this waiter:
///
/// - `peripheralIsReady(toSendWriteWithoutResponse:)`, the normal path;
/// - the bounded fallback timer, if that callback never arrives;
/// - the disconnect teardown, which resumes everything outstanding.
///
/// They race freely — the drain copies the array before resuming, so removal
/// from the array is *not* a claim on the continuation — and double-resuming a
/// `CheckedContinuation` is a hard crash, not a warning. So the continuation
/// itself is the claim token: ``resume()`` takes it out under this object's own
/// lock and nils it in the same critical section, and only the caller that
/// actually took a non-nil continuation resumes it. Everyone else gets `false`
/// and does nothing. Same trick as ``Pending``, minus the error channel
/// (`CheckedContinuation<Void, Never>` cannot fail).
final class WriteWithoutResponseWaiter: @unchecked Sendable {
    let id: Int
    private var continuation: CheckedContinuation<Void, Never>?
    private var fallback: Task<Void, Never>?
    private let lock = NSLock()

    init(id: Int, continuation: CheckedContinuation<Void, Never>) {
        self.id = id
        self.continuation = continuation
    }

    /// Attaches the fallback timer. If the waiter was already resumed in the
    /// window between enqueue and arming — `peripheralIsReady` can fire that
    /// fast — the timer is cancelled immediately instead of being retained.
    func attachFallback(_ task: Task<Void, Never>) {
        lock.lock()
        let alreadyResumed = continuation == nil
        if !alreadyResumed { fallback = task }
        lock.unlock()
        if alreadyResumed { task.cancel() }
    }

    /// - Returns: `true` if this call is the one that resumed the continuation,
    ///   `false` if somebody else got there first.
    @discardableResult
    func resume() -> Bool {
        lock.lock()
        let taken = continuation
        continuation = nil
        lock.unlock()
        taken?.resume()
        return taken != nil
    }

    func cancelFallback() {
        lock.lock()
        let task = fallback
        fallback = nil
        lock.unlock()
        task?.cancel()
    }
}

/// One outstanding request/reply. Resuming twice traps in Swift, and both a
/// notification and a timeout can plausibly fire, so the continuation is taken
/// under a lock and whoever loses the race is a no-op.
final class Pending<T>: @unchecked Sendable {
    private var continuation: CheckedContinuation<T, Error>?
    private let lock = NSLock()

    func arm(_ continuation: CheckedContinuation<T, Error>) {
        lock.lock(); defer { lock.unlock() }
        self.continuation = continuation
    }

    func resume(_ value: T) { take()?.resume(returning: value) }
    func fail(_ error: Error) { take()?.resume(throwing: error) }

    private func take() -> CheckedContinuation<T, Error>? {
        lock.lock(); defer { lock.unlock() }
        let taken = continuation
        continuation = nil
        return taken
    }
}
