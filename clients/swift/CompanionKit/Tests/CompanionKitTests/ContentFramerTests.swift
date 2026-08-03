import XCTest
@testable import CompanionKit

final class ContentFramerTests: XCTestCase {
    func testStartPacketLayout() {
        let framer = ContentFramer(field: .body,
                                   sessionId: 3,
                                   payload: Data(repeating: 0xAB, count: 300),
                                   isFinal: false,
                                   maxChunkPayload: 100)
        let start = Array(framer)[0]

        XCTAssertEqual(start.count, 7)
        XCTAssertEqual(start[0], 0x01)
        XCTAssertEqual(start[1], 0x02)          // field, no final flag
        XCTAssertEqual(start[2], 3)             // sessionId
        XCTAssertEqual(Array(start[3 ..< 7]), [0x2C, 0x01, 0x00, 0x00])  // 300, uint32 LE
    }

    func testFinalFlagIsSetOnTheFieldByte() {
        let framer = ContentFramer(field: .contentId,
                                   sessionId: 1,
                                   payload: Data([0x01]),
                                   isFinal: true,
                                   maxChunkPayload: 16)
        XCTAssertEqual(Array(framer)[0][1], 0x03 | 0x80)
    }

    func testChunksCarrySessionIdAndSplitAtTheLimit() {
        // .contentId rather than .body: this is the plain (no sequence
        // number) framing shared by content-id/UI declaration/icon/tag
        // state — see testTitleBodyChunksCarryALittleEndianSequenceNumber
        // for title/body's own (v10+) framing.
        let payload = Data((0 ..< 250).map { UInt8($0 % 256) })
        let framer = ContentFramer(field: .contentId, sessionId: 2, payload: payload, maxChunkPayload: 100)
        let packets = Array(framer)

        XCTAssertEqual(packets.count, 5)        // START + 3 chunks + END
        XCTAssertEqual(packets.count, framer.packetCount)

        let chunks = packets[1 ..< 4]
        XCTAssertEqual(chunks.map(\.count), [102, 102, 52])
        for chunk in chunks {
            XCTAssertEqual(chunk[chunk.startIndex], 0x02)
            XCTAssertEqual(chunk[chunk.startIndex + 1], 2)
        }

        let reassembled = chunks.reduce(into: Data()) { $0.append($1.dropFirst(2)) }
        XCTAssertEqual(reassembled, payload)
    }

    func testEndPacket() {
        // v11: END grew a required third byte, pushId. 0 (the default here)
        // means "not awaiting a RENDER_STATUS for this push" — see the type's
        // doc comment.
        let framer = ContentFramer(field: .title, sessionId: 4, payload: Data([1, 2, 3]), maxChunkPayload: 16)
        XCTAssertEqual(Array(Array(framer).last!), [0x03, 4, 0])
    }

    func testEndPacketCarriesTheChosenPushId() {
        let framer = ContentFramer(field: .title, sessionId: 4, payload: Data([1, 2, 3]), pushId: 42, maxChunkPayload: 16)
        XCTAssertEqual(Array(Array(framer).last!), [0x03, 4, 42])
    }

    func testEmptyPayloadStillProducesStartAndEnd() {
        let framer = ContentFramer(field: .title, sessionId: 1, payload: Data(), maxChunkPayload: 16)
        let packets = Array(framer)

        XCTAssertEqual(packets.count, 2)
        XCTAssertEqual(packets.count, framer.packetCount)
        XCTAssertEqual(Array(packets[0][3 ..< 7]), [0, 0, 0, 0])
    }

    func testPayloadLargerThanUInt16UsesTheWidenedLengthField() {
        // The whole reason v6 widened START's length from uint16 to uint32: a
        // real dithered 800x480 PNG lands near the old 65,535-byte ceiling.
        let payload = Data(repeating: 0x55, count: 70_000)
        let framer = ContentFramer(field: .image, sessionId: 1, payload: payload, maxChunkPayload: 180)
        let start = Array(framer.prefix(1))[0]

        XCTAssertEqual(Array(start[3 ..< 7]), [0x70, 0x11, 0x01, 0x00])  // 70000 LE
    }

    func testChunkPayloadSizeLeavesRoomForFraming() {
        XCTAssertEqual(ContentFramer.chunkPayloadSize(forATTPayload: 182), 180)
        XCTAssertEqual(ContentFramer.chunkPayloadSize(forATTPayload: 1), 1)
        // image/title/body CHUNKs carry 2 extra bytes (sequence number) — see
        // "Image field" (v9) and "Title/body fields" (v10) in
        // docs/companion-display-protocol.md.
        XCTAssertEqual(ContentFramer.chunkPayloadSize(forATTPayload: 182, field: .image), 178)
        XCTAssertEqual(ContentFramer.chunkPayloadSize(forATTPayload: 182, field: .title), 178)
        XCTAssertEqual(ContentFramer.chunkPayloadSize(forATTPayload: 182, field: .body), 178)
        // content-id (and UI declaration/icon/tag state) are unaffected.
        XCTAssertEqual(ContentFramer.chunkPayloadSize(forATTPayload: 182, field: .contentId), 180)
    }

    func testImageChunksCarryALittleEndianSequenceNumber() {
        // v9: the image field's CHUNK gains the 2-byte seq — title/body get
        // the same treatment in v10 (tested below); every other field's
        // framing (tested above) is untouched.
        let payload = Data((0 ..< 250).map { UInt8($0 % 256) })
        let framer = ContentFramer(field: .image, sessionId: 5, payload: payload, maxChunkPayload: 100)
        let packets = Array(framer)

        XCTAssertEqual(packets.count, 5)  // START + 3 chunks + END
        let chunks = packets[1 ..< 4]
        XCTAssertEqual(chunks.map(\.count), [104, 104, 54])  // +2 seq bytes per chunk vs. the non-image test
        for (index, chunk) in chunks.enumerated() {
            XCTAssertEqual(chunk[chunk.startIndex], 0x02)
            XCTAssertEqual(chunk[chunk.startIndex + 1], 5)
            let seq = UInt16(chunk[chunk.startIndex + 2]) | (UInt16(chunk[chunk.startIndex + 3]) << 8)
            XCTAssertEqual(seq, UInt16(index))
        }

        let reassembled = chunks.reduce(into: Data()) { $0.append($1.dropFirst(4)) }
        XCTAssertEqual(reassembled, payload)
    }

    func testTitleBodyChunksCarryALittleEndianSequenceNumber() {
        // v10: title/body join the image field in carrying the 2-byte seq,
        // pushed over Write Without Response — see "Title/body fields" in
        // docs/companion-display-protocol.md.
        for field: CompanionField in [.title, .body] {
            let payload = Data((0 ..< 250).map { UInt8($0 % 256) })
            let framer = ContentFramer(field: field, sessionId: 5, payload: payload, maxChunkPayload: 100)
            let packets = Array(framer)

            XCTAssertEqual(packets.count, 5)  // START + 3 chunks + END
            let chunks = packets[1 ..< 4]
            XCTAssertEqual(chunks.map(\.count), [104, 104, 54])
            for (index, chunk) in chunks.enumerated() {
                XCTAssertEqual(chunk[chunk.startIndex], 0x02)
                XCTAssertEqual(chunk[chunk.startIndex + 1], 5)
                let seq = UInt16(chunk[chunk.startIndex + 2]) | (UInt16(chunk[chunk.startIndex + 3]) << 8)
                XCTAssertEqual(seq, UInt16(index))
            }

            let reassembled = chunks.reduce(into: Data()) { $0.append($1.dropFirst(4)) }
            XCTAssertEqual(reassembled, payload)
        }
    }

    func testFramerIsLazyAndRepeatable() {
        let framer = ContentFramer(field: .body, sessionId: 1, payload: Data(repeating: 7, count: 10), maxChunkPayload: 4)
        XCTAssertEqual(Array(framer).count, Array(framer).count)
    }
}
