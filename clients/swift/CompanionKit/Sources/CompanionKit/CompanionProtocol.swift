import Foundation

/// Wire constants for the Companion Display Protocol.
///
/// The authoritative definition is `docs/companion-display-protocol.md` in this
/// repository. Everything here is a transcription of it — if the two disagree,
/// the document wins and this file is a bug.
public enum CompanionProtocol {
    /// The protocol version this package speaks. The capability characteristic's
    /// first byte must equal this; a mismatch is not negotiable in either
    /// direction, because v6 was a clean break from v5.
    public static let version: UInt8 = 6

    public static let serviceUUID = "7C9C0000-3E4A-4B1A-9C1E-6D8A1F2B0001"
    public static let contentCharacteristicUUID = "7C9C0001-3E4A-4B1A-9C1E-6D8A1F2B0001"
    public static let buttonCharacteristicUUID = "7C9C0002-3E4A-4B1A-9C1E-6D8A1F2B0001"
    public static let capabilityCharacteristicUUID = "7C9C0003-3E4A-4B1A-9C1E-6D8A1F2B0001"
    public static let statusCharacteristicUUID = "7C9C0004-3E4A-4B1A-9C1E-6D8A1F2B0001"
    public static let sessionCharacteristicUUID = "7C9C0005-3E4A-4B1A-9C1E-6D8A1F2B0001"

    // Content characteristic opcodes.
    public static let opStart: UInt8 = 0x01
    public static let opChunk: UInt8 = 0x02
    public static let opEnd: UInt8 = 0x03

    /// Top bit of a START packet's field byte: "this is the last field of an
    /// atomic push; commit and redraw when its END arrives".
    public static let finalFieldFlag: UInt8 = 0x80
    public static let fieldMask: UInt8 = 0x7F

    /// `sessionId` 0 is never valid and always means "no session".
    public static let noSession: UInt8 = 0x00

    /// Firmware-enforced cap, independent of the negotiated MTU.
    public static let maxContentIdLength = 32
}

/// Content characteristic field ids.
public enum CompanionField: UInt8, Sendable, CaseIterable {
    case title = 0x01
    case body = 0x02
    case contentId = 0x03
    case image = 0x04
    case buttonMap = 0x05
    case icon = 0x06
}

/// Session characteristic opcodes, phone -> device.
enum SessionOpcode: UInt8 {
    case hello = 0x01
    case bye = 0x02
    case acquire = 0x03
    case release = 0x04
}

/// Session characteristic opcodes, device -> phone.
enum SessionNotification: UInt8 {
    case helloOK = 0x81
    case helloPending = 0x82
    case helloDenied = 0x83
    case foreground = 0x84
    case background = 0x85
    case acquireDenied = 0x86
    case assetAck = 0x87
    case imageStatus = 0x88
}

public enum HelloDeniedReason: UInt8, Sendable {
    case userRejected = 0x00
    case timeout = 0x01
    case noSessionSlots = 0x02
    case malformed = 0x03
    case storage = 0x04
    /// Another pairing prompt is already on the device's screen. Retry once the
    /// user has dealt with it — this one is worth retrying, unlike a rejection.
    case busy = 0x05
    case unknown = 0xFF

    init(wire: UInt8) { self = HelloDeniedReason(rawValue: wire) ?? .unknown }
}

public enum BackgroundReason: UInt8, Sendable {
    case preempted = 0x00
    case released = 0x01
    case linkLost = 0x02
    case unknown = 0xFF

    init(wire: UInt8) { self = BackgroundReason(rawValue: wire) ?? .unknown }
}

public enum AcquireDeniedReason: UInt8, Sendable {
    case noButtonMap = 0x00
    case unknownSession = 0x01
    case unknown = 0xFF

    init(wire: UInt8) { self = AcquireDeniedReason(rawValue: wire) ?? .unknown }
}

public enum AssetResult: UInt8, Sendable {
    case stored = 0x00
    case rejectedSize = 0x01
    case rejectedFormat = 0x02
    case rejectedStorage = 0x03
    case unknown = 0xFF

    init(wire: UInt8) { self = AssetResult(rawValue: wire) ?? .unknown }
}

public enum ImageResult: UInt8, Sendable {
    case displayed = 0x00
    case decodeFailed = 0x01
    case rejectedSize = 0x02
    case storageFailed = 0x03
    case unknown = 0xFF

    init(wire: UInt8) { self = ImageResult(rawValue: wire) ?? .unknown }
}

/// How an indicator slot is drawn. Deliberately visual, not semantic: the device
/// knows "slot 2 is filled" and nothing else. "Saved", "playing", "unread" are
/// your app's vocabulary — map them onto slots yourself.
public enum IndicatorState: UInt8, Sendable {
    case hidden = 0x00
    case outline = 0x01
    case filled = 0x02
}

/// Indicator slots the device draws along the right edge of the title row.
public enum CompanionIndicator {
    public static let count = 4
}

/// Raw physical buttons. These mirror the firmware's own HAL indices; they are
/// not a protocol-specific renumbering, and their *meaning* is entirely the
/// app's to define via a ``ButtonMap``.
public enum CompanionButton: UInt8, Sendable, CaseIterable {
    case back = 0x00
    case confirm = 0x01
    case left = 0x02
    case right = 0x03
    /// One of the two side buttons. Which physical side is not knowable from
    /// firmware (shared X3/X4 binary) — if your app wants a consistent
    /// prev/next feel, make it a user-facing setting.
    case up = 0x04
    case down = 0x05
    /// Firmware-owned. Never notified, and an entry for it in a button map is
    /// ignored — a wedged app can never make the device un-sleepable.
    case power = 0x06
}

/// What the device does when a button fires. A closed set on purpose: this is
/// the entire list of things the firmware can do without asking the phone.
public enum ButtonRouting: UInt8, Sendable {
    /// No hint drawn, no notification — the button is dead in this app.
    case none = 0x00
    /// Notify the phone with the raw button id and hold duration.
    case remote = 0x01
    case localPagePrevious = 0x02
    case localPageNext = 0x03
    case localSleep = 0x04
}

public enum CompanionError: Error, Sendable {
    case bluetoothUnavailable
    case notConnected
    case noSession
    /// A push was attempted before the session owned the screen. The device
    /// would have dropped those frames with no diagnostic, so this fails loudly
    /// instead of letting the app watch a blank reader.
    case noScreen
    /// The device advertised a protocol version this package does not speak.
    case unsupportedProtocolVersion(UInt8)
    case malformedCapabilities
    case malformedMessage
    case pairingDenied(HelloDeniedReason)
    case acquireDenied(AcquireDeniedReason)
    case assetRejected(field: CompanionField, result: AssetResult)
    case imageRejected(ImageResult)
    case payloadTooLarge(field: CompanionField, bytes: Int, limit: Int)
    case timedOut
    case disconnected
}
