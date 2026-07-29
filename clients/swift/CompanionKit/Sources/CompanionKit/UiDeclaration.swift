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

/// One tag the app declares: a short labelled chip the device draws beside the
/// title and the app switches on and off at runtime.
///
/// The device defines no tags of its own — no built-in "saved", no fixed slots,
/// no reserved ids. `id` means whatever you say it means and is scoped to your
/// app, so two apps both using `0` never collide.
public struct TagDeclaration: Equatable, Sendable {
    public let id: UInt8
    /// Drawn verbatim. Truncated to 12 bytes on a UTF-8 boundary.
    public let label: String

    public init(id: UInt8, label: String) {
        self.id = id
        self.label = label
    }
}

/// An app's complete on-device UI declaration: what its buttons do and what its
/// tags are called. **Mandatory** — the device refuses `ACQUIRE` from a peer with
/// no stored declaration, so an app cannot reach the screen without having said
/// what its buttons do.
///
/// Buttons and tags are one asset with one digest because they are the same kind
/// of thing (near-static strings the device renders without understanding) and
/// change on the same cadence, at app update. Ship a new declaration whenever the
/// scheme changes; the digest changes with the content, the device picks it up on
/// the next connect, and the user does not re-pair.
public struct UiDeclaration: Equatable, Sendable {
    public let buttons: [ButtonMapEntry]
    public let tags: [TagDeclaration]
    /// Whole-row look for `tags`. A per-peer choice, not per-tag.
    public let tagRenderStyle: TagRenderStyle

    public init(buttons: [ButtonMapEntry], tags: [TagDeclaration] = [], tagRenderStyle: TagRenderStyle = .bordered) {
        // POWER is firmware-owned; encoding it would be ignored on the device,
        // which would make our digest disagree with what is stored.
        self.buttons = buttons.filter { $0.button != .power }
        self.tags = Array(tags.prefix(CompanionTagLimits.maxTags))
        self.tagRenderStyle = tagRenderStyle
    }

    /// The asset body, without its 4-byte digest prefix.
    public func encodedBody() -> Data {
        var out = Data()
        out.append(UInt8(min(buttons.count, 255)))
        for entry in buttons.prefix(255) {
            let label = SessionCodec.truncateUTF8(entry.label, toByteCount: 255)
            out.append(entry.button.rawValue)
            out.append(entry.routing.rawValue)
            out.append(UInt8(label.count))
            out.append(label)
        }
        // The tag count byte is optional on the wire, but always emitted here.
        // "Zero tags" and "section omitted" mean the same thing to the device,
        // so emitting one shape only keeps the digest a function of the
        // declaration rather than of how it happened to be serialized.
        out.append(UInt8(tags.count))
        for tag in tags {
            let label = SessionCodec.truncateUTF8(tag.label, toByteCount: CompanionTagLimits.maxLabelBytes)
            out.append(tag.id)
            out.append(UInt8(label.count))
            out.append(label)
        }
        // Trailing and optional on the wire (absent means Bordered), but
        // always emitted here for the same reason as the tag count above:
        // the digest is a function of the declaration, not of how it was
        // serialized.
        out.append(tagRenderStyle.rawValue)
        return out
    }

    /// Content-derived digest over the encoded body — what gets compared against
    /// the digest reported in `HELLO_OK`.
    public var tag: AssetTag { AssetTag.contentHash(of: encodedBody()) }

    /// The full field `0x05` payload: digest, then body.
    public func encodedAsset() -> Data {
        var out = tag.bytes
        out.append(encodedBody())
        return out
    }

    /// A reasonable starting point for a text-reading app: page locally with the
    /// bottom page buttons, forward the rest. Provided as a convenience, not as
    /// a default — there is no implicit declaration on the device.
    public static func readerDefault(confirmLabel: String = "Save",
                                     tags: [TagDeclaration] = [],
                                     tagRenderStyle: TagRenderStyle = .bordered) -> UiDeclaration {
        UiDeclaration(buttons: [
            ButtonMapEntry(.left, .localPagePrevious, label: "<"),
            ButtonMapEntry(.right, .localPageNext, label: ">"),
            ButtonMapEntry(.confirm, .remote, label: confirmLabel),
            ButtonMapEntry(.back, .remote, label: "Back"),
            ButtonMapEntry(.up, .remote),
            ButtonMapEntry(.down, .remote)
        ], tags: tags, tagRenderStyle: tagRenderStyle)
    }
}

/// A set of tag state changes, encoded as content field `0x07`.
///
/// Push this inside the same atomic batch as title/body and the content and its
/// tags commit in one redraw — which is the whole reason the field exists. Tags
/// are **not** reset by a content push, so an app that changes content without
/// saying what its tags should now be leaves the previous content's tags on
/// screen.
public struct TagStateUpdate: Equatable, Sendable {
    public let states: [UInt8: TagState]

    public init(_ states: [UInt8: TagState]) {
        self.states = states
    }

    public func encoded() -> Data {
        var out = Data()
        let ordered = states.sorted { $0.key < $1.key }.prefix(CompanionTagLimits.maxTags)
        out.append(UInt8(ordered.count))
        for (id, state) in ordered {
            out.append(id)
            out.append(state.rawValue)
        }
        return out
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
