import Foundation

/// The device's self-description, read once per connection from the capability
/// characteristic before anything else happens.
///
/// Read this rather than hardcoding anything about the hardware — an image
/// pusher in particular must size its canvas from ``screenPixelWidth`` /
/// ``screenPixelHeight`` and quantize to ``imageGrayLevels``.
public struct CompanionCapabilities: Equatable, Sendable {
    public let protocolVersion: UInt8
    public let screenCharacterWidth: Int
    public let screenCharacterHeight: Int
    public let maxTextFieldLength: Int
    public let featureFlags: UInt8
    public let maxImageFieldLength: Int
    public let maxConcurrentSessions: Int
    public let iconPixelWidth: Int
    public let iconPixelHeight: Int
    /// The 4-byte eFuse MAC tail. Key your per-device storage (tokens, user
    /// settings) on ``deviceIdHex``, not on the CoreBluetooth peripheral
    /// identifier, which is not stable across reinstalls.
    public let deviceId: Data
    public let screenPixelWidth: Int
    public let screenPixelHeight: Int
    public let maxContentIdLength: Int
    public let imageGrayLevels: Int

    public var supportsImage: Bool { featureFlags & 0x01 != 0 }
    public var supportsButtonMap: Bool { featureFlags & 0x02 != 0 }
    public var supportsIcons: Bool { featureFlags & 0x04 != 0 }
    public var supportsSessions: Bool { featureFlags & 0x08 != 0 }

    public var deviceIdHex: String { deviceId.hexString }

    /// Bytes in a well-formed icon asset body, excluding its 4-byte tag.
    public var iconByteCount: Int { (iconPixelWidth * iconPixelHeight) / 8 }

    /// The four 8-bit grayscale values a client-side dither must quantize to.
    /// Anything else re-quantizes unpredictably at the bucket boundaries — the
    /// device buckets by `gray / 85`.
    public var imageQuantizationLevels: [UInt8] { [0, 85, 170, 255] }

    public static let byteCount = 23

    public init?(_ data: Data) {
        guard data.count >= Self.byteCount,
              let version = data.byte(0),
              let charWidth = data.byte(1),
              let charHeight = data.byte(2),
              let maxText = data.uint16LE(at: 3),
              let flags = data.byte(5),
              let imageLow = data.uint16LE(at: 6),
              let imageHigh = data.uint16LE(at: 8),
              let sessions = data.byte(10),
              let iconW = data.byte(11),
              let iconH = data.byte(12),
              let deviceId = data.slice(13 ..< 17),
              let pixelWidth = data.uint16LE(at: 17),
              let pixelHeight = data.uint16LE(at: 19),
              let contentIdCap = data.byte(21),
              let grayLevels = data.byte(22) else { return nil }

        self.protocolVersion = version
        self.screenCharacterWidth = Int(charWidth)
        self.screenCharacterHeight = Int(charHeight)
        self.maxTextFieldLength = Int(maxText)
        self.featureFlags = flags
        self.maxImageFieldLength = Int(UInt32(imageLow) | (UInt32(imageHigh) << 16))
        self.maxConcurrentSessions = Int(sessions)
        self.iconPixelWidth = Int(iconW)
        self.iconPixelHeight = Int(iconH)
        self.deviceId = deviceId
        self.screenPixelWidth = Int(pixelWidth)
        self.screenPixelHeight = Int(pixelHeight)
        self.maxContentIdLength = Int(contentIdCap)
        self.imageGrayLevels = Int(grayLevels)
    }
}
