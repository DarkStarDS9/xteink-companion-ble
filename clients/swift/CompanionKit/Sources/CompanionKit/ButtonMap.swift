import Foundation

/// One button's declaration: what it is called on screen and who handles it.
public struct ButtonMapEntry: Equatable, Sendable {
    public let button: CompanionButton
    public let routing: ButtonRouting
    /// Drawn verbatim in the on-screen hint row. Opaque to the device — a
    /// `.remote` button labelled "Shutter" is still just "notify button id 1".
    /// Bottom-row buttons show their label; the side buttons (`up`/`down`) have
    /// no hint position, so their labels are stored but not currently drawn.
    public let label: String

    public init(_ button: CompanionButton, _ routing: ButtonRouting, label: String = "") {
        self.button = button
        self.routing = routing
        self.label = label
    }
}

/// An app's complete button declaration. **Mandatory** — the device refuses
/// `ACQUIRE` from a peer with no stored map, so an app cannot reach the screen
/// without having said what its buttons do.
///
/// Ship a new map whenever an app update changes the scheme; the tag changes
/// with the content, the device picks it up on the next connect, and the user
/// does not re-pair.
public struct ButtonMap: Equatable, Sendable {
    public let entries: [ButtonMapEntry]

    public init(_ entries: [ButtonMapEntry]) {
        self.entries = entries.filter { $0.button != .power }
    }

    /// The asset body, without its 4-byte tag prefix.
    public func encodedBody() -> Data {
        var out = Data()
        out.append(UInt8(min(entries.count, 255)))
        for entry in entries.prefix(255) {
            let label = SessionCodec.truncateUTF8(entry.label, toByteCount: 255)
            out.append(entry.button.rawValue)
            out.append(entry.routing.rawValue)
            out.append(UInt8(label.count))
            out.append(label)
        }
        return out
    }

    /// Content-derived tag over the encoded body — what gets compared against
    /// the digest in `HELLO_OK`.
    public var tag: AssetTag { AssetTag.contentHash(of: encodedBody()) }

    /// The full field `0x05` payload: tag, then body.
    public func encodedAsset() -> Data {
        var out = tag.bytes
        out.append(encodedBody())
        return out
    }

    /// A reasonable starting point for a text-reading app: page locally with the
    /// bottom page buttons, forward the rest. Provided as a convenience, not as
    /// a default — there is no implicit map on the device.
    public static func readerDefault(confirmLabel: String = "Save") -> ButtonMap {
        ButtonMap([
            ButtonMapEntry(.left, .localPagePrevious, label: "<"),
            ButtonMapEntry(.right, .localPageNext, label: ">"),
            ButtonMapEntry(.confirm, .remote, label: confirmLabel),
            ButtonMapEntry(.back, .remote, label: "Back"),
            ButtonMapEntry(.up, .remote),
            ButtonMapEntry(.down, .remote)
        ])
    }
}

/// A decoded button-event notification.
public struct CompanionButtonEvent: Equatable, Sendable {
    /// **Check this.** Both apps sharing a phone's single link to the device
    /// receive every notification; an event for another session is not yours.
    public let sessionId: UInt8
    public let button: CompanionButton
    /// `true` on the release notification. Never rely on it alone to detect
    /// release — a disconnect mid-hold means it may never arrive. Treat "no
    /// repeat for noticeably longer than 100 ms" as an implicit release too.
    public let isFinal: Bool
    /// Elapsed hold in 100 ms ticks since the initial press; 0 on press-down.
    public let holdTicks: UInt16
    /// The content-id most recently pushed for this session, echoed verbatim.
    /// Empty if none was pushed.
    public let contentId: Data

    public var holdDuration: TimeInterval { TimeInterval(holdTicks) * 0.1 }

    public init?(_ data: Data) {
        guard let sessionId = data.byte(0),
              let header = data.byte(1),
              let duration = data.uint16LE(at: 2),
              let button = CompanionButton(rawValue: header & 0x0F) else { return nil }
        // bits 6-4 are the event type; only ButtonPress (0x1) exists today, and
        // an unknown type is a future event kind sharing this characteristic
        // rather than a corrupt packet — ignore it rather than misreport it.
        guard (header >> 4) & 0x07 == 0x01 else { return nil }

        self.sessionId = sessionId
        self.button = button
        self.isFinal = header & 0x80 != 0
        self.holdTicks = duration
        self.contentId = data.slice(4 ..< data.count) ?? Data()
    }
}
