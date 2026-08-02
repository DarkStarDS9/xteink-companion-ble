import Foundation

/// Splits one content field into START/CHUNK/END packets for the Content
/// characteristic.
///
/// A `Sequence`, not an array, so a multi-hundred-kilobyte image is chunked
/// lazily instead of being copied into a second full-size buffer of `Data`
/// slices before the first byte goes out.
///
/// ```
/// START:  0x01 | field|final | sessionId | length uint32 LE      (7 bytes)
/// CHUNK:  0x02 | sessionId   | payload...                          (other fields)
/// CHUNK:  0x02 | sessionId   | seq uint16 LE | payload...           (field 0x04, image, v9+)
/// END:    0x03 | sessionId
/// ```
///
/// The image field's CHUNK carries an extra 2-byte sequence number because it
/// is the one field pushed over Write Without Response (see
/// `docs/companion-display-protocol.md` "Image field") — without a sequence
/// number a dropped chunk would shift every byte after it and silently
/// corrupt the reassembled raw 2bpp payload instead of failing loudly.
public struct ContentFramer: Sequence {
    public let field: CompanionField
    public let sessionId: UInt8
    public let payload: Data
    /// Sets the `0x80` flag on the START packet: the device commits and redraws
    /// everything it has buffered when this field's END arrives. Use it on the
    /// last field of a title+body+content-id batch so they land together.
    public let isFinal: Bool
    /// Payload bytes per CHUNK, already netted against this field's CHUNK
    /// framing overhead — see ``ContentFramer/chunkPayloadSize(forATTPayload:field:)``.
    public let maxChunkPayload: Int

    /// `true` for ``CompanionField/image`` — every other field's CHUNK is
    /// unchanged from pre-v9.
    var includesSequenceNumber: Bool { field == .image }

    public init(field: CompanionField,
                sessionId: UInt8,
                payload: Data,
                isFinal: Bool = false,
                maxChunkPayload: Int) {
        precondition(maxChunkPayload > 0, "chunk payload must be positive")
        self.field = field
        self.sessionId = sessionId
        self.payload = payload
        self.isFinal = isFinal
        self.maxChunkPayload = maxChunkPayload
    }

    /// Usable CHUNK payload for a given ATT payload size (`maximumWriteValueLength`
    /// on iOS, which already excludes the 3-byte ATT header) and field — the
    /// image field's CHUNK carries 2 extra bytes of sequence number.
    public static func chunkPayloadSize(forATTPayload attPayload: Int, field: CompanionField) -> Int {
        Swift.max(1, attPayload - (field == .image ? 4 : 2))
    }

    /// Deprecated: assumes non-image framing overhead (2 bytes). Prefer
    /// ``chunkPayloadSize(forATTPayload:field:)``.
    public static func chunkPayloadSize(forATTPayload attPayload: Int) -> Int {
        chunkPayloadSize(forATTPayload: attPayload, field: .title)
    }

    /// Number of packets this framer will emit, START and END included.
    public var packetCount: Int {
        let chunks = payload.isEmpty ? 0 : (payload.count + maxChunkPayload - 1) / maxChunkPayload
        return chunks + 2
    }

    public func makeIterator() -> Iterator { Iterator(framer: self) }

    public struct Iterator: IteratorProtocol {
        private let framer: ContentFramer
        private var offset = 0
        private var seq: UInt16 = 0
        private var stage = Stage.start

        private enum Stage { case start, chunks, end, done }

        init(framer: ContentFramer) { self.framer = framer }

        public mutating func next() -> Data? {
            switch stage {
            case .start:
                stage = framer.payload.isEmpty ? .end : .chunks
                var packet = Data(capacity: 7)
                packet.append(CompanionProtocol.opStart)
                packet.append(framer.field.rawValue | (framer.isFinal ? CompanionProtocol.finalFieldFlag : 0))
                packet.append(framer.sessionId)
                packet.appendUInt32LE(UInt32(framer.payload.count))
                return packet

            case .chunks:
                let end = Swift.min(offset + framer.maxChunkPayload, framer.payload.count)
                let headerLen = 2 + (framer.includesSequenceNumber ? 2 : 0)
                var packet = Data(capacity: headerLen + (end - offset))
                packet.append(CompanionProtocol.opChunk)
                packet.append(framer.sessionId)
                if framer.includesSequenceNumber {
                    packet.appendUInt16LE(seq)
                    seq += 1
                }
                packet.append(framer.payload[framer.payload.startIndex + offset ..< framer.payload.startIndex + end])
                offset = end
                if offset >= framer.payload.count { stage = .end }
                return packet

            case .end:
                stage = .done
                return Data([CompanionProtocol.opEnd, framer.sessionId])

            case .done:
                return nil
            }
        }
    }
}

extension Data {
    mutating func appendUInt16LE(_ value: UInt16) {
        append(UInt8(value & 0xFF))
        append(UInt8((value >> 8) & 0xFF))
    }

    mutating func appendUInt32LE(_ value: UInt32) {
        append(UInt8(value & 0xFF))
        append(UInt8((value >> 8) & 0xFF))
        append(UInt8((value >> 16) & 0xFF))
        append(UInt8((value >> 24) & 0xFF))
    }

    /// Byte at logical index `index`, tolerating slices whose `startIndex` is
    /// not zero (which is what CoreBluetooth hands back).
    func byte(_ index: Int) -> UInt8? {
        guard index >= 0, index < count else { return nil }
        return self[startIndex + index]
    }

    func slice(_ range: Range<Int>) -> Data? {
        guard range.lowerBound >= 0, range.upperBound <= count else { return nil }
        return self[startIndex + range.lowerBound ..< startIndex + range.upperBound]
    }

    func uint16LE(at index: Int) -> UInt16? {
        guard let low = byte(index), let high = byte(index + 1) else { return nil }
        return UInt16(low) | (UInt16(high) << 8)
    }

    var hexString: String {
        map { String(format: "%02x", $0) }.joined()
    }
}
