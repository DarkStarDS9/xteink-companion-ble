import Foundation

/// A parsed device -> phone message from the Session characteristic.
public enum SessionMessage: Equatable, Sendable {
    case helloOK(helloTag: UInt16, sessionId: UInt8, token: Data, assetTags: [UInt8: AssetTag])
    case helloPending(helloTag: UInt16)
    case helloDenied(helloTag: UInt16, reason: HelloDeniedReason)
    case foreground(sessionId: UInt8)
    case background(sessionId: UInt8, reason: BackgroundReason)
    case acquireDenied(sessionId: UInt8, reason: AcquireDeniedReason)
    case assetAck(sessionId: UInt8, assetId: UInt8, result: AssetResult, tag: AssetTag)
    case imageStatus(sessionId: UInt8, result: ImageResult)
    /// v9: progress marker during an in-flight image push — see
    /// ``SessionNotification/imageChunkAck``. `seq` is the highest
    /// contiguous CHUNK sequence number the device has processed.
    case imageChunkAck(sessionId: UInt8, seq: UInt16)
    /// v10: a title/body push's CHUNK sequence number skipped ahead of what
    /// the device expected — see ``SessionNotification/fieldSeqGap``. The
    /// field named by `field` was dropped; recovering means re-pushing it
    /// from a fresh START.
    case fieldSeqGap(sessionId: UInt8, field: UInt8)
}

/// Encoders and decoders for the Session characteristic. Pure functions over
/// bytes — no CoreBluetooth, so this is where the handshake is unit-tested.
public enum SessionCodec {
    /// Longest display name the device stores. Longer names are truncated here
    /// rather than on-device, so what the app shows and what the device shows
    /// agree.
    public static let maxNameBytes = 24

    public static func encodeHello(helloTag: UInt16,
                                   appId: Data,
                                   installId: Data,
                                   token: Data?,
                                   displayName: String,
                                   userName: String = "") -> Data {
        precondition(appId.count == 16, "appId must be 16 bytes")
        precondition(installId.count == 16, "installId must be 16 bytes")

        var out = Data(capacity: 101)
        out.append(SessionOpcode.hello.rawValue)
        out.appendUInt16LE(helloTag)
        out.append(appId)
        out.append(installId)

        let tokenBytes = (token?.count == 16) ? token! : Data()
        out.append(UInt8(tokenBytes.count))
        out.append(tokenBytes)

        let nameBytes = truncateUTF8(displayName, toByteCount: maxNameBytes)
        out.append(UInt8(nameBytes.count))
        out.append(nameBytes)

        // userName is a separate, user-facing label for this install (which of
        // the user's own devices/accounts this is), distinct from displayName
        // (the app's own name). See CompanionIdentity.userName.
        let userNameBytes = truncateUTF8(userName, toByteCount: maxNameBytes)
        out.append(UInt8(userNameBytes.count))
        out.append(userNameBytes)
        return out
    }

    public static func encodeBye(sessionId: UInt8) -> Data {
        Data([SessionOpcode.bye.rawValue, sessionId])
    }

    public static func encodeAcquire(sessionId: UInt8) -> Data {
        Data([SessionOpcode.acquire.rawValue, sessionId])
    }

    public static func encodeRelease(sessionId: UInt8) -> Data {
        Data([SessionOpcode.release.rawValue, sessionId])
    }

    /// Returns `nil` for anything unparseable or for an opcode this package does
    /// not know. Unknown opcodes are deliberately not an error: the device may
    /// be newer, and the right response is to ignore the message.
    public static func decode(_ data: Data) -> SessionMessage? {
        guard let opcode = data.byte(0), let kind = SessionNotification(rawValue: opcode) else { return nil }

        switch kind {
        case .helloOK:
            guard let tag = data.uint16LE(at: 1),
                  let sessionId = data.byte(3),
                  let token = data.slice(4 ..< 20),
                  let assetCount = data.byte(20) else { return nil }
            var tags: [UInt8: AssetTag] = [:]
            var offset = 21
            for _ in 0 ..< Int(assetCount) {
                guard let assetId = data.byte(offset),
                      let tagBytes = data.slice(offset + 1 ..< offset + 5) else { return nil }
                tags[assetId] = AssetTag(tagBytes)
                offset += 5
            }
            return .helloOK(helloTag: tag, sessionId: sessionId, token: token, assetTags: tags)

        case .helloPending:
            guard let tag = data.uint16LE(at: 1) else { return nil }
            return .helloPending(helloTag: tag)

        case .helloDenied:
            guard let tag = data.uint16LE(at: 1), let reason = data.byte(3) else { return nil }
            return .helloDenied(helloTag: tag, reason: HelloDeniedReason(wire: reason))

        case .foreground:
            guard let sessionId = data.byte(1) else { return nil }
            return .foreground(sessionId: sessionId)

        case .background:
            guard let sessionId = data.byte(1), let reason = data.byte(2) else { return nil }
            return .background(sessionId: sessionId, reason: BackgroundReason(wire: reason))

        case .acquireDenied:
            guard let sessionId = data.byte(1), let reason = data.byte(2) else { return nil }
            return .acquireDenied(sessionId: sessionId, reason: AcquireDeniedReason(wire: reason))

        case .assetAck:
            guard let sessionId = data.byte(1),
                  let assetId = data.byte(2),
                  let result = data.byte(3),
                  let tag = data.slice(4 ..< 8) else { return nil }
            return .assetAck(sessionId: sessionId,
                             assetId: assetId,
                             result: AssetResult(wire: result),
                             tag: AssetTag(tag))

        case .imageStatus:
            guard let sessionId = data.byte(1), let result = data.byte(2) else { return nil }
            return .imageStatus(sessionId: sessionId, result: ImageResult(wire: result))

        case .imageChunkAck:
            guard let sessionId = data.byte(1), let seq = data.uint16LE(at: 2) else { return nil }
            return .imageChunkAck(sessionId: sessionId, seq: seq)

        case .fieldSeqGap:
            guard let sessionId = data.byte(1), let field = data.byte(2) else { return nil }
            return .fieldSeqGap(sessionId: sessionId, field: field)
        }
    }

    /// Truncates on a UTF-8 boundary, so a clipped name is never invalid UTF-8.
    static func truncateUTF8(_ string: String, toByteCount limit: Int) -> Data {
        var candidate = string
        while candidate.utf8.count > limit {
            candidate.removeLast()
        }
        return Data(candidate.utf8)
    }
}

/// The opaque 4-byte per-asset version tag.
///
/// The device stores whatever bytes it is handed and reads them back — it never
/// hashes or compares. Deciding an asset is stale is the app's job, which is why
/// this type is on the client side of the wire.
public struct AssetTag: Equatable, Hashable, Sendable, CustomStringConvertible {
    public let bytes: Data

    public init(_ bytes: Data) {
        precondition(bytes.count == 4, "an asset tag is exactly 4 bytes")
        self.bytes = Data(bytes)
    }

    /// The device reports four zero bytes when it holds no copy of an asset.
    public static let empty = AssetTag(Data([0, 0, 0, 0]))

    public var isEmpty: Bool { self == .empty }

    /// Content-derived tag: the first 4 bytes of SHA-256 over the asset body.
    ///
    /// Prefer this to a counter. A counter breaks on app downgrade — the device
    /// holds a tag the older build will never produce again, so it never
    /// re-pushes — while a hash is correct in both directions. Collides with the
    /// reserved all-zero value once in 2^32 pushes, which is handled by nudging
    /// to a non-zero value rather than by pretending it cannot happen.
    public static func contentHash(of body: Data) -> AssetTag {
        let tag = AssetTag(CompanionHash.sha256(body).prefix(4))
        return tag.isEmpty ? AssetTag(Data([0, 0, 0, 1])) : tag
    }

    public var description: String { bytes.hexString }
}
