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
        XCTAssertTrue(capabilities.supportsUiDeclaration)
        XCTAssertFalse(capabilities.supportsIcons)
        XCTAssertTrue(capabilities.supportsSessions)
    }

    func testShortBlockIsRejected() {
        // A v5 device answers with 5 bytes. Parsing must fail rather than
        // inventing plausible-looking values for fields that do not exist.
        XCTAssertNil(CompanionCapabilities(Data([5, 48, 24, 0x00, 0x10])))
    }
}

final class UiDeclarationTests: XCTestCase {
    func testEncodedBodyLayout() {
        let declaration = UiDeclaration(buttons: [
            ButtonMapEntry(.left, .localPagePrevious, label: "<"),
            ButtonMapEntry(.confirm, .remote, label: "Shutter")
        ])
        let body = declaration.encodedBody()

        XCTAssertEqual(body[0], 2)                                   // button count
        XCTAssertEqual(Array(body[1 ..< 4]), [0x02, 0x02, 0x01])     // LEFT, LOCAL_PAGE_PREV, len 1
        XCTAssertEqual(body[4], UInt8(ascii: "<"))
        XCTAssertEqual(Array(body[5 ..< 8]), [0x01, 0x01, 0x07])     // CONFIRM, REMOTE, len 7
        XCTAssertEqual(String(data: body[8 ..< 15], encoding: .utf8), "Shutter")
        XCTAssertEqual(body[15], 0, "a declaration with no tags still emits a zero count")
        XCTAssertEqual(body[16], 0, "bordered is the default render style")
        XCTAssertEqual(body[17], 0, "no capabilities by default")
        XCTAssertEqual(body.count, 18)
    }

    func testTagSectionLayout() {
        let declaration = UiDeclaration(
            buttons: [ButtonMapEntry(.confirm, .remote, label: "S")],
            tags: [TagDeclaration(id: 7, label: "Saved"), TagDeclaration(id: 9, label: "New")]
        )
        let body = declaration.encodedBody()
        // 1 count + (1+1+1+1) button = 5 bytes, then the tag section.
        XCTAssertEqual(body[5], 2)                                   // tag count
        XCTAssertEqual(Array(body[6 ..< 8]), [7, 5])                 // id 7, label len 5
        XCTAssertEqual(String(data: body[8 ..< 13], encoding: .utf8), "Saved")
        XCTAssertEqual(Array(body[13 ..< 15]), [9, 3])
        XCTAssertEqual(String(data: body[15 ..< 18], encoding: .utf8), "New")
        XCTAssertEqual(body[18], 0, "bordered is the default render style")
        XCTAssertEqual(body[19], 0, "no capabilities by default")
        XCTAssertEqual(body.count, 20)
    }

    func testTagRenderStyleByteIsTrailingAndOptIn() {
        let bordered = UiDeclaration(buttons: [], tags: [TagDeclaration(id: 0, label: "Saved")])
        let borderedBody = bordered.encodedBody()
        XCTAssertEqual(borderedBody[borderedBody.count - 2], TagRenderStyle.bordered.rawValue)

        let plain = UiDeclaration(buttons: [], tags: [TagDeclaration(id: 0, label: "Saved")], tagRenderStyle: .plain)
        let plainBody = plain.encodedBody()
        XCTAssertEqual(plainBody[plainBody.count - 2], TagRenderStyle.plain.rawValue)
        XCTAssertEqual(plainBody.count, borderedBody.count,
                       "the style byte replaces nothing else in the body -- same shape, different trailing value")
    }

    func testCapabilitiesByteIsTrailingAfterStyle() {
        let plain = UiDeclaration(buttons: [], tags: [TagDeclaration(id: 0, label: "Saved")])
        XCTAssertEqual(plain.encodedBody().last, 0, "no capabilities by default")

        let gallery = UiDeclaration(buttons: [], tags: [TagDeclaration(id: 0, label: "Saved")],
                                    capabilities: .imageGallery)
        let body = gallery.encodedBody()
        XCTAssertEqual(body.last, PeerCapabilities.imageGallery.rawValue)
        XCTAssertEqual(body.count, plain.encodedBody().count,
                       "the capabilities byte replaces nothing else in the body -- same shape, different trailing value")
    }

    func testTagLabelsAreTruncatedOnAUTF8Boundary() {
        let declaration = UiDeclaration(buttons: [],
                                        tags: [TagDeclaration(id: 0, label: String(repeating: "é", count: 10))])
        let body = declaration.encodedBody()
        let labelLength = Int(body[3])
        XCTAssertLessThanOrEqual(labelLength, CompanionTagLimits.maxLabelBytes)
        XCTAssertNotNil(String(data: body[4 ..< (4 + labelLength)], encoding: .utf8))
    }

    func testTagsPastTheCapAreDropped() {
        let many = (0 ..< 10).map { TagDeclaration(id: UInt8($0), label: "t\($0)") }
        XCTAssertEqual(UiDeclaration(buttons: [], tags: many).tags.count, CompanionTagLimits.maxTags)
    }

    func testPowerEntriesAreDropped() {
        // POWER is firmware-owned. Encoding it would be silently ignored on the
        // device, which would make our digest disagree with what is stored.
        let declaration = UiDeclaration(buttons: [
            ButtonMapEntry(.power, .remote, label: "Nope"),
            ButtonMapEntry(.back, .remote, label: "Back")
        ])
        XCTAssertEqual(declaration.buttons.count, 1)
        XCTAssertEqual(declaration.encodedBody()[0], 1)
    }

    func testEncodedAssetPrefixesTheDigest() {
        let declaration = UiDeclaration.readerDefault()
        let asset = declaration.encodedAsset()
        XCTAssertEqual(Data(asset.prefix(4)), declaration.tag.bytes)
        XCTAssertEqual(Data(asset.dropFirst(4)), declaration.encodedBody())
        XCTAssertEqual(declaration.tag, AssetTag.contentHash(of: declaration.encodedBody()))
    }

    func testDigestChangesWithButtonsOrTags() {
        let a = UiDeclaration(buttons: [ButtonMapEntry(.confirm, .remote, label: "Save")])
        let b = UiDeclaration(buttons: [ButtonMapEntry(.confirm, .remote, label: "Queue")])
        XCTAssertNotEqual(a.tag, b.tag, "relabelling a button must re-push")

        let c = UiDeclaration(buttons: a.buttons, tags: [TagDeclaration(id: 0, label: "Saved")])
        XCTAssertNotEqual(a.tag, c.tag, "adding a tag must re-push")

        let d = UiDeclaration(buttons: a.buttons, tags: c.tags, tagRenderStyle: .plain)
        XCTAssertNotEqual(c.tag, d.tag, "switching render style must re-push")
    }
}

final class TagStateTests: XCTestCase {
    func testEncodingIsCountThenPairs() {
        let update = TagStateUpdate([3: .filled, 1: .hidden])
        XCTAssertEqual(Array(update.encoded()), [2, 1, 0, 3, 2], "ordered by id, count first")
    }

    func testEmptyUpdateIsJustAZeroCount() {
        XCTAssertEqual(Array(TagStateUpdate([:]).encoded()), [0])
    }

    func testUpdateIsCappedAtTheDeviceLimit() {
        var states: [UInt8: TagState] = [:]
        for id in 0 ..< 10 { states[UInt8(id)] = .outline }
        XCTAssertEqual(Int(TagStateUpdate(states).encoded()[0]), CompanionTagLimits.maxTags)
    }

    func testTagStateRidesTheContentFraming() {
        // The point of field 0x07: it is a content field, so it can carry the
        // final flag and commit with title/body in one redraw.
        let framer = ContentFramer(field: .tagState,
                                   sessionId: 1,
                                   payload: TagStateUpdate([0: .filled]).encoded(),
                                   isFinal: true,
                                   maxChunkPayload: 64)
        XCTAssertEqual(Array(framer)[0][1], 0x07 | 0x80)
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

    func testUserNameDefaultsAndCanBeOverridden() {
        // The iCloud key-value tier itself isn't exercised here — there's no
        // iCloud container in a test executable, so loadOrCreateInstallId()
        // always takes its local UserDefaults fallback, same as before this
        // feature. This only covers userName's plumbing, not the sync path.
        let defaults = UserDefaults(suiteName: "CompanionKitTests.identity.userName")!
        defaults.removePersistentDomain(forName: "CompanionKitTests.identity.userName")

        let overridden = CompanionIdentity(appId: UUID(), displayName: "Test", userName: "Rainer's iPad",
                                           defaults: defaults)
        XCTAssertEqual(overridden.userName, "Rainer's iPad")

        let explicit = CompanionIdentity(appId: Data(repeating: 1, count: 16),
                                         installId: Data(repeating: 2, count: 16),
                                         displayName: "Test")
        XCTAssertEqual(explicit.userName, "", "userName defaults to empty on the byte-level initializer")
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
