import Foundation
#if os(iOS)
import UIKit
#endif

/// Who this app is, to the device.
///
/// `appId` is a constant of the app — the same 16 bytes in every install, chosen
/// once by the author and never regenerated. It is what groups sleep-screen
/// icons, so two phones running your app are one tile.
///
/// `installId` identifies this copy on this phone. It is generated randomly on
/// first run and **must be persisted** — regenerating it makes a brand new peer
/// on the device, which means a new pairing prompt and a fresh set of assets.
///
/// It is stored in `UserDefaults` (mirrored into iCloud key-value storage — see
/// `loadOrCreateInstallId(...)`) rather than the Keychain, deliberately.
/// Keychain items survive app deletion, so a delete-and-reinstall would silently
/// re-attach to the peer directory of an install that no longer exists,
/// inheriting assets and a token it never pushed. A fresh install *should* be a
/// fresh peer *on a device with no iCloud account* — with one, a reinstall
/// deliberately recovers the same install (and so the same peer directory and
/// gallery on the companion device), the same mechanism that treats an iPhone
/// and an iPad as one. (The pairing token is a related but separate case —
/// see ``CompanionTokenStore`` and ``KeychainTokenStore``'s `synchronizable`.)
public struct CompanionIdentity: Equatable, Sendable {
    public let appId: Data
    public let installId: Data
    public let displayName: String

    /// A user-facing label for *this install*, distinct from `displayName`
    /// (the app's own name, identical across every install). It's what tells
    /// two of the user's own devices apart on the device's gallery picker —
    /// same icon, same app name, different `userName` underneath. Purely
    /// presentational: never compared, matched, or used to derive `peerKey`.
    /// Defaults to the device name on iOS; may be empty, in which case the
    /// device falls back to `displayName`.
    public let userName: String

    public init(appId: Data, installId: Data, displayName: String, userName: String = "") {
        precondition(appId.count == 16, "appId must be 16 bytes")
        precondition(installId.count == 16, "installId must be 16 bytes")
        self.appId = appId
        self.installId = installId
        self.displayName = displayName
        self.userName = userName
    }

    /// The convenient form: a hardcoded `UUID` literal for the app, and an
    /// install id loaded from (or minted into) iCloud key-value storage, with
    /// a `UserDefaults`-backed fallback — see `loadOrCreateInstallId(...)`.
    ///
    /// ```swift
    /// let identity = CompanionIdentity(
    ///     appId: UUID(uuidString: "6E7E0C2A-...")!,
    ///     displayName: "Snap2Ink")
    /// ```
    public init(appId: UUID,
                displayName: String,
                userName: String = CompanionIdentity.defaultUserName,
                defaults: UserDefaults = .standard,
                installIdKey: String = "CompanionKit.installId") {
        self.init(appId: Data(appId.uuidBytes),
                  installId: Self.loadOrCreateInstallId(defaults: defaults, key: installIdKey),
                  displayName: displayName,
                  userName: userName)
    }

    /// The device's directory name for this peer: the first 8 hex chars of
    /// SHA-256 over `appId || installId`. Not needed to speak the protocol —
    /// useful when reading device logs or an SD card during bring-up.
    public var peerKey: String {
        var input = appId
        input.append(installId)
        return String(CompanionHash.sha256(input).hexString.prefix(8))
    }

    #if os(iOS)
    public static var defaultUserName: String { UIDevice.current.name }
    #else
    public static var defaultUserName: String { ProcessInfo.processInfo.hostName }
    #endif

    /// Loads (or mints) this install's id, preferring one shared across the
    /// user's own devices via iCloud key-value storage over a purely local
    /// one — the common iOS pattern for "treat my iPhone and iPad as one"
    /// without an explicit sign-in flow. `NSUbiquitousKeyValueStore` degrades
    /// silently (no crash, just no sync) when the consuming app hasn't
    /// enabled the iCloud Key-Value Storage capability, so this falls back to
    /// today's local-only behaviour automatically when that's the case.
    ///
    /// **A device only ever adopts an incoming iCloud value before it has
    /// minted one of its own.** Once a device has a local id — whether it
    /// minted it or adopted one on a previous run — it keeps it. Without this
    /// rule, a device could switch identity mid-flight (e.g. on a late
    /// `didChangeExternallyNotification` after two devices raced on first
    /// install) and orphan its own already-issued pairing token and peer
    /// directory on the companion device. A device with no local id yet just
    /// writes whatever it settles on back to the iCloud store, so a second
    /// device converges on the same one.
    static func loadOrCreateInstallId(defaults: UserDefaults, key: String) -> Data {
        if let existing = defaults.data(forKey: key), existing.count == 16 {
            return existing
        }

        let cloudStore = NSUbiquitousKeyValueStore.default
        cloudStore.synchronize()
        if let fromCloud = cloudStore.data(forKey: key), fromCloud.count == 16 {
            defaults.set(fromCloud, forKey: key)
            return fromCloud
        }

        let fresh = Data((0 ..< 16).map { _ in UInt8.random(in: 0 ... 255) })
        defaults.set(fresh, forKey: key)
        cloudStore.set(fresh, forKey: key)
        return fresh
    }
}

extension UUID {
    var uuidBytes: [UInt8] {
        let u = uuid
        return [u.0, u.1, u.2, u.3, u.4, u.5, u.6, u.7, u.8, u.9, u.10, u.11, u.12, u.13, u.14, u.15]
    }
}

/// Where the per-device pairing token lives between launches.
///
/// The token is what makes reconnects silent — without it the user gets a
/// pairing prompt on the device every single time. Keyed by the device's
/// `deviceId` (the eFuse MAC tail from the capability characteristic), because
/// one install can be paired to several displays.
public protocol CompanionTokenStore: AnyObject {
    func token(forDeviceId deviceId: String) -> Data?
    func setToken(_ token: Data?, forDeviceId deviceId: String)
}

public extension CompanionTokenStore {
    /// Drops the stored token for one device, so the next connect goes through
    /// the on-screen pairing prompt again. Backs a "forget this reader" control.
    ///
    /// Note this is only half of an unpairing: the device keeps its own peer
    /// directory, and unpairing there is on-device only — the protocol has no
    /// opcode for it. The user will be asked to confirm again, which is the
    /// visible effect they expect.
    func forget(deviceId: String) {
        setToken(nil, forDeviceId: deviceId)
    }
}

/// Keychain-backed storage. The default, and the right choice on iOS: it
/// survives app updates and is not in a plist a backup will scatter around.
public final class KeychainTokenStore: CompanionTokenStore {
    private let service: String

    /// Whether the token also follows the user's iCloud Keychain
    /// (`kSecAttrSynchronizable`), so a second device that converges on the
    /// same `installId` (see `CompanionIdentity.loadOrCreateInstallId`) skips
    /// the on-device pairing prompt too, instead of hitting it once for an
    /// "unrecognized token" even though the companion device already knows
    /// this `peerKey`. Defaults on, matching `installId`'s default iCloud
    /// behaviour; set false to keep tokens strictly per-device.
    private let synchronizable: Bool

    public init(service: String = "CompanionKit.pairingToken", synchronizable: Bool = true) {
        self.service = service
        self.synchronizable = synchronizable
    }

    public func token(forDeviceId deviceId: String) -> Data? {
        var query = baseQuery(deviceId)
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var result: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess else { return nil }
        return result as? Data
    }

    public func setToken(_ token: Data?, forDeviceId deviceId: String) {
        let query = baseQuery(deviceId)
        SecItemDelete(query as CFDictionary)
        guard let token else { return }
        var insert = query
        insert[kSecValueData as String] = token
        insert[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlock
        SecItemAdd(insert as CFDictionary, nil)
    }

    private func baseQuery(_ deviceId: String) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: service,
         kSecAttrAccount as String: deviceId,
         kSecAttrSynchronizable as String: synchronizable]
    }
}

/// `UserDefaults`-backed storage, for a macOS dev harness or a test target where
/// a Keychain entitlement is more trouble than it is worth. The token is not a
/// secret in the cryptographic sense (the link is unencrypted and the token is
/// sniffable by design) — but it is still a capability, so prefer the Keychain
/// on a shipping app.
public final class UserDefaultsTokenStore: CompanionTokenStore {
    private let defaults: UserDefaults
    private let prefix: String

    public init(defaults: UserDefaults = .standard, prefix: String = "CompanionKit.token.") {
        self.defaults = defaults
        self.prefix = prefix
    }

    public func token(forDeviceId deviceId: String) -> Data? {
        defaults.data(forKey: prefix + deviceId)
    }

    public func setToken(_ token: Data?, forDeviceId deviceId: String) {
        if let token {
            defaults.set(token, forKey: prefix + deviceId)
        } else {
            defaults.removeObject(forKey: prefix + deviceId)
        }
    }
}

public final class InMemoryTokenStore: CompanionTokenStore {
    private var tokens: [String: Data] = [:]
    private let lock = NSLock()

    public init() {}

    public func token(forDeviceId deviceId: String) -> Data? {
        lock.lock(); defer { lock.unlock() }
        return tokens[deviceId]
    }

    public func setToken(_ token: Data?, forDeviceId deviceId: String) {
        lock.lock(); defer { lock.unlock() }
        tokens[deviceId] = token
    }
}
