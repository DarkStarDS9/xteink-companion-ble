import CryptoKit
import Foundation

enum CompanionHash {
    static func sha256(_ data: Data) -> Data {
        Data(CryptoKit.SHA256.hash(data: data))
    }
}
