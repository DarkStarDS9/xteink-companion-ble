import XCTest
@testable import CompanionKit

final class SessionCodecTests: XCTestCase {
    private let appId = Data((0 ..< 16).map { UInt8($0) })
    private let installId = Data((0 ..< 16).map { UInt8(0xF0 &+ $0) })

    // MARK: HELLO

    func testHelloWithoutTokenLayout() {
        let hello = SessionCodec.encodeHello(helloTag: 0xBEEF,
                                             appId: appId,
                                             installId: installId,
                                             token: nil,
                                             displayName: "Snap2Ink")

        XCTAssertEqual(hello[0], 0x01)
        XCTAssertEqual(Array(hello[1 ..< 3]), [0xEF, 0xBE])          // helloTag, uint16 LE
        XCTAssertEqual(Data(hello[3 ..< 19]), appId)
        XCTAssertEqual(Data(hello[19 ..< 35]), installId)
        XCTAssertEqual(hello[35], 0)                                  // tokenLen
        XCTAssertEqual(hello[36], 8)                                  // nameLen
        XCTAssertEqual(String(data: hello[37 ..< 45], encoding: .utf8), "Snap2Ink")
        XCTAssertEqual(hello[45], 0)                                  // userNameLen (defaults empty)
    }

    func testHelloWithTokenLayout() {
        let token = Data(repeating: 0x5A, count: 16)
        let hello = SessionCodec.encodeHello(helloTag: 1,
                                             appId: appId,
                                             installId: installId,
                                             token: token,
                                             displayName: "")

        XCTAssertEqual(hello[35], 16)
        XCTAssertEqual(Data(hello[36 ..< 52]), token)
        XCTAssertEqual(hello[52], 0)                                  // empty name
        XCTAssertEqual(hello[53], 0)                                  // empty userName
        XCTAssertEqual(hello.count, 54)
    }

    func testWrongLengthTokenIsSentAsNoToken() {
        // A truncated token would be rejected anyway; sending none gets the
        // pairing prompt, which is the recoverable path.
        let hello = SessionCodec.encodeHello(helloTag: 1,
                                             appId: appId,
                                             installId: installId,
                                             token: Data([1, 2, 3]),
                                             displayName: "x")
        XCTAssertEqual(hello[35], 0)
    }

    func testDisplayNameIsTruncatedOnAUTF8Boundary() {
        let name = String(repeating: "é", count: 20)               // 40 UTF-8 bytes
        let hello = SessionCodec.encodeHello(helloTag: 1,
                                             appId: appId,
                                             installId: installId,
                                             token: nil,
                                             displayName: name)
        let nameLen = Int(hello[36])
        XCTAssertLessThanOrEqual(nameLen, SessionCodec.maxNameBytes)
        XCTAssertNotNil(String(data: hello[37...], encoding: .utf8), "truncation must not split a codepoint")
    }

    func testUserNameIsAppendedAfterDisplayName() {
        let hello = SessionCodec.encodeHello(helloTag: 1,
                                             appId: appId,
                                             installId: installId,
                                             token: nil,
                                             displayName: "Snap2Ink",
                                             userName: "Rainer's iPad")

        XCTAssertEqual(hello[36], 8)                                  // nameLen
        XCTAssertEqual(String(data: hello[37 ..< 45], encoding: .utf8), "Snap2Ink")
        XCTAssertEqual(hello[45], 13)                                 // userNameLen
        XCTAssertEqual(String(data: hello[46...], encoding: .utf8), "Rainer's iPad")
    }

    // MARK: Simple opcodes

    func testSimpleOpcodes() {
        XCTAssertEqual(Array(SessionCodec.encodeBye(sessionId: 2)), [0x02, 2])
        XCTAssertEqual(Array(SessionCodec.encodeAcquire(sessionId: 3)), [0x03, 3])
        XCTAssertEqual(Array(SessionCodec.encodeRelease(sessionId: 4)), [0x04, 4])
    }

    // MARK: Notifications

    func testDecodeHelloOKWithAssetDigests() {
        var payload = Data([0x81, 0x34, 0x12, 0x02])                // opcode, helloTag LE, sessionId
        payload.append(Data(repeating: 0xAA, count: 16))            // token
        payload.append(2)                                            // assetCount
        payload.append(contentsOf: [0x05, 0xDE, 0xAD, 0xBE, 0xEF])  // button map
        payload.append(contentsOf: [0x06, 0x00, 0x00, 0x00, 0x00])  // icon: none stored

        guard case let .helloOK(tag, sessionId, token, tags) = SessionCodec.decode(payload) else {
            return XCTFail("expected helloOK")
        }
        XCTAssertEqual(tag, 0x1234)
        XCTAssertEqual(sessionId, 2)
        XCTAssertEqual(token, Data(repeating: 0xAA, count: 16))
        XCTAssertEqual(tags[0x05], AssetTag(Data([0xDE, 0xAD, 0xBE, 0xEF])))
        XCTAssertEqual(tags[0x06], .empty)
        XCTAssertTrue(tags[0x06]!.isEmpty)
    }

    func testDecodeHelloOKWithNoAssets() {
        var payload = Data([0x81, 0x01, 0x00, 0x01])
        payload.append(Data(repeating: 0x11, count: 16))
        payload.append(0)

        guard case let .helloOK(_, _, _, tags) = SessionCodec.decode(payload) else {
            return XCTFail("expected helloOK")
        }
        XCTAssertTrue(tags.isEmpty)
    }

    func testDecodeTruncatedHelloOKFails() {
        var payload = Data([0x81, 0x01, 0x00, 0x01])
        payload.append(Data(repeating: 0x11, count: 16))
        payload.append(2)                                            // claims two assets
        payload.append(contentsOf: [0x05, 0xDE, 0xAD])               // but is cut short
        XCTAssertNil(SessionCodec.decode(payload))
    }

    func testDecodeHelloPendingAndDenied() {
        XCTAssertEqual(SessionCodec.decode(Data([0x82, 0x01, 0x00])), .helloPending(helloTag: 1))
        XCTAssertEqual(SessionCodec.decode(Data([0x83, 0x02, 0x00, 0x01])),
                       .helloDenied(helloTag: 2, reason: .timeout))
    }

    func testDecodeForegroundBackgroundAcquireDenied() {
        XCTAssertEqual(SessionCodec.decode(Data([0x84, 0x01])), .foreground(sessionId: 1))
        XCTAssertEqual(SessionCodec.decode(Data([0x85, 0x01, 0x00])),
                       .background(sessionId: 1, reason: .preempted))
        XCTAssertEqual(SessionCodec.decode(Data([0x86, 0x01, 0x00])),
                       .acquireDenied(sessionId: 1, reason: .noUiDeclaration))
    }

    func testDecodeAssetAckAndRenderStatus() {
        XCTAssertEqual(SessionCodec.decode(Data([0x87, 0x01, 0x05, 0x00, 0xDE, 0xAD, 0xBE, 0xEF])),
                       .assetAck(sessionId: 1,
                                 assetId: 0x05,
                                 result: .stored,
                                 tag: AssetTag(Data([0xDE, 0xAD, 0xBE, 0xEF]))))
        // v11: RENDER_STATUS gained a trailing byte over the v10 3-byte
        // IMAGE_STATUS payload — see "Session characteristic" in
        // docs/companion-display-protocol.md. That byte is the pushing
        // client's own chosen `pushId`, echoed verbatim (not a `field` id —
        // an earlier shape of this same day's v11 landing used one, but no
        // consumer app had adopted it yet, so it was replaced before ever
        // shipping).
        XCTAssertEqual(SessionCodec.decode(Data([0x88, 0x01, 0x01, 0x04])),
                       .renderStatus(sessionId: 1, result: .decodeFailed, pushId: 0x04))
        XCTAssertEqual(SessionCodec.decode(Data([0x88, 0x01, 0x00, 0x02])),
                       .renderStatus(sessionId: 1, result: .displayed, pushId: 0x02))
        // A pre-v11 3-byte payload (no trailing byte) is now malformed and
        // decodes to nil rather than being silently misparsed.
        XCTAssertNil(SessionCodec.decode(Data([0x88, 0x01, 0x01])))
    }

    func testDecodeFieldSeqGap() {
        // v10: sent when a title/body CHUNK sequence number skips ahead of
        // what the device expected — see "Title/body fields" in
        // docs/companion-display-protocol.md.
        XCTAssertEqual(SessionCodec.decode(Data([0x8A, 0x01, 0x02])),
                       .fieldSeqGap(sessionId: 1, field: 0x02))
    }

    func testUnknownReasonCodesDecodeAsUnknownRatherThanFailing() {
        // A newer device may add a reason. Losing the detail is fine; dropping
        // the whole message would strand the handshake.
        XCTAssertEqual(SessionCodec.decode(Data([0x83, 0x02, 0x00, 0x7E])),
                       .helloDenied(helloTag: 2, reason: .unknown))
    }

    func testUnknownOpcodeAndEmptyDataDecodeToNil() {
        XCTAssertNil(SessionCodec.decode(Data([0xF0, 0x01])))
        XCTAssertNil(SessionCodec.decode(Data()))
    }

    // MARK: Asset tags

    func testContentHashIsStableAndContentSensitive() {
        let a = AssetTag.contentHash(of: Data("one".utf8))
        XCTAssertEqual(a, AssetTag.contentHash(of: Data("one".utf8)))
        XCTAssertNotEqual(a, AssetTag.contentHash(of: Data("two".utf8)))
        XCTAssertFalse(a.isEmpty)
        XCTAssertEqual(a.bytes.count, 4)
    }
}
