import XCTest
@testable import CompanionKit

final class CapabilitiesTests: XCTestCase {
    /// The capability block an X3 in Companion Mode should produce.
    private func sampleBlock() -> Data {
        var data = Data()
        data.append(6)                       // 0  version
        data.append(48)                      // 1  screen width, chars
        data.append(24)                      // 2  screen height, chars
        data.appendUInt16LE(4096)            // 3..4 max text field
        data.append(0x0F)                    // 5  all four features
        data.appendUInt32LE(131_072)         // 6..9 max image field
        data.append(4)                       // 10 max sessions
        data.append(64)                      // 11 icon width
        data.append(64)                      // 12 icon height
        data.append(contentsOf: [0xA1, 0xB2, 0xC3, 0xD4])  // 13..16 deviceId
        data.appendUInt16LE(800)             // 17..18 screen pixels wide
        data.appendUInt16LE(480)             // 19..20 screen pixels high
        data.append(32)                      // 21 max content-id
        data.append(4)                       // 22 grey levels
        return data
    }

    func testParsesEveryField() {
        guard let capabilities = CompanionCapabilities(sampleBlock()) else {
            return XCTFail("expected a parse")
        }
        XCTAssertEqual(capabilities.protocolVersion, 6)
        XCTAssertEqual(capabilities.screenCharacterWidth, 48)
        XCTAssertEqual(capabilities.screenCharacterHeight, 24)
        XCTAssertEqual(capabilities.maxTextFieldLength, 4096)
        XCTAssertEqual(capabilities.maxImageFieldLength, 131_072)
        XCTAssertEqual(capabilities.maxConcurrentSessions, 4)
        XCTAssertEqual(capabilities.iconPixelWidth, 64)
        XCTAssertEqual(capabilities.iconPixelHeight, 64)
        XCTAssertEqual(capabilities.iconByteCount, 512)
        XCTAssertEqual(capabilities.deviceIdHex, "a1b2c3d4")
        XCTAssertEqual(capabilities.screenPixelWidth, 800)
        XCTAssertEqual(capabilities.screenPixelHeight, 480)
        XCTAssertEqual(capabilities.maxContentIdLength, 32)
        XCTAssertEqual(capabilities.imageGrayLevels, 4)
        XCTAssertEqual(capabilities.imageQuantizationLevels, [0, 85, 170, 255])
    }

    func testFeatureFlags() {
        var block = sampleBlock()
        block[5] = 0x0A                      // button map + sessions only
        let capabilities = CompanionCapabilities(block)!
        XCTAssertFalse(capabilities.supportsImage)
        XCTAssertTrue(capabilities.supportsButtonMap)
        XCTAssertFalse(capabilities.supportsIcons)
        XCTAssertTrue(capabilities.supportsSessions)
    }

    func testShortBlockIsRejected() {
        // A v5 device answers with 5 bytes. Parsing must fail rather than
        // inventing plausible-looking values for fields that do not exist.
        XCTAssertNil(CompanionCapabilities(Data([5, 48, 24, 0x00, 0x10])))
    }
}

final class ButtonMapTests: XCTestCase {
    func testEncodedBodyLayout() {
        let map = ButtonMap([
            ButtonMapEntry(.left, .localPagePrevious, label: "<"),
            ButtonMapEntry(.confirm, .remote, label: "Shutter")
        ])
        let body = map.encodedBody()

        XCTAssertEqual(body[0], 2)                                   // entry count
        XCTAssertEqual(Array(body[1 ..< 4]), [0x02, 0x02, 0x01])     // LEFT, LOCAL_PAGE_PREV, len 1
        XCTAssertEqual(body[4], UInt8(ascii: "<"))
        XCTAssertEqual(Array(body[5 ..< 8]), [0x01, 0x01, 0x07])     // CONFIRM, REMOTE, len 7
        XCTAssertEqual(String(data: body[8 ..< 15], encoding: .utf8), "Shutter")
    }

    func testPowerEntriesAreDropped() {
        // POWER is firmware-owned. Encoding it would be silently ignored on the
        // device, which would make the tag disagree with what is stored.
        let map = ButtonMap([
            ButtonMapEntry(.power, .remote, label: "Nope"),
            ButtonMapEntry(.back, .remote, label: "Back")
        ])
        XCTAssertEqual(map.entries.count, 1)
        XCTAssertEqual(map.encodedBody()[0], 1)
    }

    func testEncodedAssetPrefixesTheTag() {
        let map = ButtonMap.readerDefault()
        let asset = map.encodedAsset()
        XCTAssertEqual(Data(asset.prefix(4)), map.tag.bytes)
        XCTAssertEqual(Data(asset.dropFirst(4)), map.encodedBody())
        XCTAssertEqual(map.tag, AssetTag.contentHash(of: map.encodedBody()))
    }

    func testTagChangesWithTheScheme() {
        let a = ButtonMap([ButtonMapEntry(.confirm, .remote, label: "Save")])
        let b = ButtonMap([ButtonMapEntry(.confirm, .remote, label: "Queue")])
        XCTAssertNotEqual(a.tag, b.tag, "an app update that relabels a button must re-push")
    }
}

final class ButtonEventTests: XCTestCase {
    func testDecodesPressWithContentId() {
        // session 2, CONFIRM, not final, 15 ticks, content-id "abc"
        let data = Data([0x02, 0x11, 0x0F, 0x00]) + Data("abc".utf8)
        guard let event = CompanionButtonEvent(data) else { return XCTFail("expected a decode") }

        XCTAssertEqual(event.sessionId, 2)
        XCTAssertEqual(event.button, .confirm)
        XCTAssertFalse(event.isFinal)
        XCTAssertEqual(event.holdTicks, 15)
        XCTAssertEqual(event.holdDuration, 1.5, accuracy: 0.0001)
        XCTAssertEqual(String(data: event.contentId, encoding: .utf8), "abc")
    }

    func testDecodesFinalReleaseWithNoContentId() {
        let event = CompanionButtonEvent(Data([0x01, 0x94, 0x00, 0x00]))
        XCTAssertEqual(event?.button, .up)
        XCTAssertEqual(event?.isFinal, true)
        XCTAssertEqual(event?.contentId, Data())
    }

    func testRejectsUnknownEventType() {
        // bits 6-4 = 2: a future event kind sharing the characteristic. Reporting
        // it as a button press would invent input the user never gave.
        XCTAssertNil(CompanionButtonEvent(Data([0x01, 0x21, 0x00, 0x00])))
    }

    func testRejectsTruncatedPayload() {
        XCTAssertNil(CompanionButtonEvent(Data([0x01, 0x11, 0x00])))
    }
}

final class IdentityTests: XCTestCase {
    func testInstallIdIsMintedOnceAndReused() {
        let defaults = UserDefaults(suiteName: "CompanionKitTests.identity")!
        defaults.removePersistentDomain(forName: "CompanionKitTests.identity")

        let appId = UUID()
        let first = CompanionIdentity(appId: appId, displayName: "Test", defaults: defaults)
        let second = CompanionIdentity(appId: appId, displayName: "Test", defaults: defaults)

        XCTAssertEqual(first.installId.count, 16)
        XCTAssertEqual(first.installId, second.installId, "a new installId means a new peer and a re-pair")
        XCTAssertEqual(first.peerKey, second.peerKey)
        XCTAssertEqual(first.peerKey.count, 8)
    }

    func testTokenStoreRoundTripsPerDevice() {
        let store = InMemoryTokenStore()
        store.setToken(Data(repeating: 1, count: 16), forDeviceId: "a1b2c3d4")
        store.setToken(Data(repeating: 2, count: 16), forDeviceId: "eeff0011")

        XCTAssertEqual(store.token(forDeviceId: "a1b2c3d4"), Data(repeating: 1, count: 16))
        XCTAssertEqual(store.token(forDeviceId: "eeff0011"), Data(repeating: 2, count: 16))
        XCTAssertNil(store.token(forDeviceId: "unknown"))
    }
}
