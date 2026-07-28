import Foundation

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
/// It is stored in `UserDefaults` rather than the Keychain, deliberately.
/// Keychain items survive app deletion, so a delete-and-reinstall would silently
/// re-attach to the peer directory of an install that no longer exists,
/// inheriting assets and a token it never pushed. A fresh install *should* be a
/// fresh peer. (The pairing token is the opposite case and does belong in the
/// Keychain — see ``CompanionTokenStore``.)
public struct CompanionIdentity: Equatable, Sendable {
    public let appId: Data
    public let installId: Data
    public let displayName: String

    public init(appId: Data, installId: Data, displayName: String) {
        precondition(appId.count == 16, "appId must be 16 bytes")
        precondition(installId.count == 16, "installId must be 16 bytes")
        self.appId = appId
        self.installId = installId
        self.displayName = displayName
    }

    /// The convenient form: a hardcoded `UUID` literal for the app, and an
    /// install id loaded from (or minted into) `UserDefaults`.
    ///
    /// ```swift
    /// let identity = CompanionIdentity(
    ///     appId: UUID(uuidString: "6E7E0C2A-...")!,
    ///     displayName: "Snap2Ink")
    /// ```
    public init(appId: UUID,
                displayName: String,
                defaults: UserDefaults = .standard,
                installIdKey: String = "CompanionKit.installId") {
        self.init(appId: Data(appId.uuidBytes),
                  installId: Self.loadOrCreateInstallId(defaults: defaults, key: installIdKey),
                  displayName: displayName)
    }

    /// The device's directory name for this peer: the first 8 hex chars of
    /// SHA-256 over `appId || installId`. Not needed to speak the protocol —
    /// useful when reading device logs or an SD card during bring-up.
    public var peerKey: String {
        var input = appId
        input.append(installId)
        return String(CompanionHash.sha256(input).hexString.prefix(8))
    }

    static func loadOrCreateInstallId(defaults: UserDefaults, key: String) -> Data {
        if let existing = defaults.data(forKey: key), existing.count == 16 {
            return existing
        }
        let fresh = Data((0 ..< 16).map { _ in UInt8.random(in: 0 ... 255) })
        defaults.set(fresh, forKey: key)
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

    public init(service: String = "CompanionKit.pairingToken") {
        self.service = service
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
         kSecAttrAccount as String: deviceId]
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
