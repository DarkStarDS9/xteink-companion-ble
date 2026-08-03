import Foundation

/// Wire constants for the Companion Display Protocol.
///
/// The authoritative definition is `docs/companion-display-protocol.md` in this
/// repository. Everything here is a transcription of it — if the two disagree,
/// the document wins and this file is a bug.
public enum CompanionProtocol {
    /// The protocol version this package speaks. The capability characteristic's
    /// first byte must equal this; a mismatch is not negotiable in either
    /// direction, because v6 was a clean break from v5. Keep this in lockstep
    /// with `docs/companion-display-protocol.md`'s current version — a client
    /// on an older number than a connected device's firmware is just as broken
    /// as the reverse, and looks confusingly like the *device* needs an update
    /// when it's actually this package that's behind.
    public static let version: UInt8 = 11

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
    /// Buttons and tags in one versioned asset — see ``UiDeclaration``.
    case uiDeclaration = 0x05
    case icon = 0x06
    /// Which of the peer's declared tags are currently in which state.
    case tagState = 0x07
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
    /// Named `imageStatus` through v10, when it only ever answered an image
    /// push. v11 widens the payload with a trailing byte so the same opcode
    /// also answers a title/body/content-id/tag content batch — see
    /// ``RenderResult`` and `docs/companion-display-protocol.md`'s
    /// `RENDER_STATUS` section. That trailing byte was a `field` id
    /// (``CompanionField/image``/``CompanionField/body``) for exactly one day
    /// before landing here as a client-chosen `pushId` instead — see
    /// ``SessionMessage/renderStatus``.
    case renderStatus = 0x88
    /// v9: progress marker sent every ``ContentFramer/imageChunkAckInterval``
    /// chunks during an image push over Write Without Response, well before
    /// the final `renderStatus` — lets a client detect a stalled/diverged
    /// transfer early. Not flow control (CoreBluetooth's own
    /// `canSendWriteWithoutResponse` already handles that).
    case imageChunkAck = 0x89
    /// v10: sent when a title/body push's CHUNK sequence number skipped ahead
    /// of what the device expected — the field was dropped rather than
    /// rendered corrupt. See ``CompanionField/title``/``CompanionField/body``
    /// and `docs/companion-display-protocol.md` "Title/body fields".
    case fieldSeqGap = 0x8A
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
    case noUiDeclaration = 0x00
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

/// Outcome of a push, as reported by `RENDER_STATUS`. Named `ImageResult`
/// through v10, when it only ever answered an image push; renamed in the v11
/// generalization to a title/body/content-id/tag content batch too — see
/// ``CompanionEvent/renderStatus(result:)``. The cases needed no change:
/// ``sequenceGap`` and ``storageFailed`` are just as meaningful for a
/// discarded/failed text batch as for an image.
public enum RenderResult: UInt8, Sendable {
    case displayed = 0x00
    case decodeFailed = 0x01
    case rejectedSize = 0x02
    case storageFailed = 0x03
    /// v9: the device saw an image CHUNK's sequence number skip ahead of what
    /// it expected — a packet was lost or reordered under Write Without
    /// Response. Retrying the whole push (not just the missing chunk — there
    /// is no partial-resume protocol yet) is the right response, unlike
    /// ``storageFailed`` which points at the SD card instead.
    ///
    /// v11: also reported when a title/body/tag batch is discarded whole
    /// because one of its fields hit exactly this condition on the Content
    /// characteristic (see `FIELD_SEQ_GAP`) — the batch never renders, so the
    /// device answers immediately rather than leaving a caller waiting on
    /// ``CompanionClient/push(title:body:contentId:tags:awaitRender:)`` to
    /// time out.
    case sequenceGap = 0x04
    /// v11: a later push took the screen before this one reached the panel, so
    /// it will never render. A content push and an image push select different
    /// render branches on the device, and whichever lands second wins.
    ///
    /// Rare in practice. `push(awaitRender: false)` returns once the *wire*
    /// transfer completes (~0.24s measured) while the render it triggered still
    /// has ~1.7s to run, so an app can start an image push mid-render — but the
    /// device serialises the two behind its render lock, so the earlier push
    /// normally still reaches the panel and reports ``displayed``. Verified on
    /// hardware: a text push followed 0.3s later by an image answered
    /// ``displayed``. Expect this case to be uncommon, and handle it because it
    /// is cheap to, not because it is likely.
    ///
    /// **Not an error.** Nothing failed; the content was overtaken by something
    /// newer, which is usually exactly what the app intended. Retrying would
    /// re-push content the app has already moved on from.
    case superseded = 0x05
    case unknown = 0xFF

    init(wire: UInt8) { self = RenderResult(rawValue: wire) ?? .unknown }
}

/// How a declared tag is drawn. Visual, not semantic — what "on" means is your
/// app's business, and the device has no opinion.
public enum TagState: UInt8, Sendable {
    /// Declared but not drawn at all, and taking no space.
    case hidden = 0x00
    case outline = 0x01
    case filled = 0x02
}

/// How a peer's whole tag row is drawn. Orthogonal to ``TagState``: state says
/// which tag is on, this says what "on"/"off" look like for the whole row —
/// one choice per peer, not per tag.
public enum TagRenderStyle: UInt8, Sendable {
    /// Outline draws a box; filled draws a filled box with knocked-out
    /// (inverted) label text. The device's default when this byte is absent.
    case bordered = 0x00
    /// Outline draws nothing at all, same as hidden; filled draws the label
    /// as plain text, no box.
    case plain = 0x01
}

/// What this peer's app can do beyond title/body, declared as a trailing
/// bitmask on the UI declaration (right after ``TagRenderStyle``). Every
/// client in this repo emits this byte unconditionally, even when empty — see
/// ``UiDeclaration/encodedBody()``.
public struct PeerCapabilities: OptionSet, Sendable {
    public let rawValue: UInt8
    public init(rawValue: UInt8) { self.rawValue = rawValue }

    /// This app pushes photos (content field `0x04`) and wants a tile in the
    /// on-device gallery picker. Declared, not inferred from having pushed an
    /// image before — an app that supports photos but hasn't pushed one yet
    /// still belongs in the picker, showing "no images yet" rather than being
    /// invisible.
    public static let imageGallery = PeerCapabilities(rawValue: 1 << 0)
}

public enum CompanionTagLimits {
    /// Tags past this many are dropped by the device.
    public static let maxTags = 6
    /// Labels longer than this are truncated on a UTF-8 boundary. Matches the
    /// display-name cap; sized for localized labels rather than English ones.
    public static let maxLabelBytes = 24
}

/// Raw physical buttons. These mirror the firmware's own HAL indices; they are
/// not a protocol-specific renumbering, and their *meaning* is entirely the
/// app's to define via a ``UiDeclaration``.
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
    /// `reported` is the device's, `expected` is `CompanionProtocol.version` at
    /// the moment of the check — carry both rather than just `reported` so a
    /// consumer app can phrase the right message without redoing the
    /// comparison itself: `reported > expected` means the *device* is ahead
    /// (the app needs updating, not the firmware) and `reported < expected`
    /// means the reverse. Getting this backwards produces a confusing
    /// "upgrade your firmware" prompt on a device that's already newer than
    /// the app expects.
    case unsupportedProtocolVersion(reported: UInt8, expected: UInt8)
    case malformedCapabilities
    case malformedMessage
    case pairingDenied(HelloDeniedReason)
    case acquireDenied(AcquireDeniedReason)
    case assetRejected(field: CompanionField, result: AssetResult)
    case imageRejected(RenderResult)
    case payloadTooLarge(field: CompanionField, bytes: Int, limit: Int)
    case timedOut
    case disconnected
}
