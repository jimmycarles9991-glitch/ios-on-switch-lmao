import CommonCrypto
import CryptoKit
import Foundation
import HWTestCore
import LocalAuthentication
import Security

enum CryptoData {
    static let size = 32 << 20
    static let payload = Data((0..<size).map { UInt8(truncatingIfNeeded: $0 &* 131 &+ $0 >> 11) })
}

struct CryptoKitTest: HWTest {
    let id = "crypto.cryptokit"
    let name = "CryptoKit AES-GCM and SHA-256"
    let subsystem = Subsystem.cpu

    func run(_ context: TestContext) async throws -> Outcome {
        let data = CryptoData.payload
        let key = SymmetricKey(size: .bits256)
        var start = CFAbsoluteTimeGetCurrent()
        let sealed = try AES.GCM.seal(data, using: key)
        let sealSeconds = CFAbsoluteTimeGetCurrent() - start
        start = CFAbsoluteTimeGetCurrent()
        let opened = try AES.GCM.open(sealed, using: key)
        let openSeconds = CFAbsoluteTimeGetCurrent() - start
        start = CFAbsoluteTimeGetCurrent()
        let digest = Data(SHA256.hash(data: data))
        let hashSeconds = CFAbsoluteTimeGetCurrent() - start
        var common = [UInt8](repeating: 0, count: Int(CC_SHA256_DIGEST_LENGTH))
        data.withUnsafeBytes { _ = CC_SHA256($0.baseAddress, CC_LONG(data.count), &common) }
        var metrics = Metrics()
        metrics.set("gcm_seal_mbps", Stats.megabytesPerSecond(bytes: data.count, seconds: sealSeconds), digits: 1)
        metrics.set("gcm_open_mbps", Stats.megabytesPerSecond(bytes: data.count, seconds: openSeconds), digits: 1)
        metrics.set("sha256_mbps", Stats.megabytesPerSecond(bytes: data.count, seconds: hashSeconds), digits: 1)
        return .judged(
            [
                ("AES-GCM round trip", opened == data),
                ("CryptoKit and CommonCrypto agree on SHA-256", digest == Data(common)),
            ],
            metrics)
    }
}

struct CommonCryptoTest: HWTest {
    let id = "crypto.commoncrypto"
    let name = "CommonCrypto AES throughput"
    let subsystem = Subsystem.cpu

    func crypt(_ operation: CCOperation, _ input: Data, key: [UInt8], iv: [UInt8]) throws -> (Data, Double) {
        var output = Data(count: input.count + kCCBlockSizeAES128)
        var moved = 0
        let capacity = output.count
        let start = CFAbsoluteTimeGetCurrent()
        let status = output.withUnsafeMutableBytes { out in
            input.withUnsafeBytes { inp in
                CCCrypt(
                    operation, CCAlgorithm(kCCAlgorithmAES), CCOptions(kCCOptionPKCS7Padding), key, key.count, iv,
                    inp.baseAddress, input.count, out.baseAddress, capacity, &moved)
            }
        }
        guard status == kCCSuccess else { throw TestFailure("CCCrypt: \(status)") }
        return (output.prefix(moved), CFAbsoluteTimeGetCurrent() - start)
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let key = (0..<32).map { UInt8($0 * 7) }
        let iv = (0..<16).map { UInt8($0 * 3) }
        let (encrypted, encryptSeconds) = try crypt(CCOperation(kCCEncrypt), CryptoData.payload, key: key, iv: iv)
        let (decrypted, decryptSeconds) = try crypt(CCOperation(kCCDecrypt), encrypted, key: key, iv: iv)
        var metrics = Metrics()
        metrics.set(
            "cbc_encrypt_mbps", Stats.megabytesPerSecond(bytes: CryptoData.size, seconds: encryptSeconds), digits: 1)
        metrics.set(
            "cbc_decrypt_mbps", Stats.megabytesPerSecond(bytes: CryptoData.size, seconds: decryptSeconds), digits: 1)
        return .judged([("AES-256-CBC round trip", decrypted == CryptoData.payload)], metrics)
    }
}

struct KeychainTest: HWTest {
    let id = "crypto.keychain"
    let name = "Keychain round trip"
    let subsystem = Subsystem.cpu

    func run(_ context: TestContext) async throws -> Outcome {
        let account = "hwtest-\(UUID().uuidString)"
        let secret = Data((0..<48).map { _ in UInt8.random(in: 0...255) })
        let base: [CFString: Any] = [
            kSecClass: kSecClassGenericPassword, kSecAttrService: "com.brunostjohn.switch.hwtest",
            kSecAttrAccount: account,
        ]
        defer { SecItemDelete(base as CFDictionary) }
        var add = base
        add[kSecValueData] = secret
        add[kSecAttrAccessible] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        let added = SecItemAdd(add as CFDictionary, nil)
        guard added == errSecSuccess else {
            return .fail("SecItemAdd: \(added) \(SecCopyErrorMessageString(added, nil) as String? ?? "")")
        }
        var query = base
        query[kSecReturnData] = true
        var result: CFTypeRef?
        let copied = SecItemCopyMatching(query as CFDictionary, &result)
        let deleted = SecItemDelete(base as CFDictionary)
        var metrics = Metrics()
        metrics.set("copy_status", Int(copied))
        metrics.set("delete_status", Int(deleted))
        return .judged(
            [
                ("reads back the secret", copied == errSecSuccess && (result as? Data) == secret),
                ("deletes it", deleted == errSecSuccess),
            ],
            metrics)
    }
}

struct LocalAuthenticationTest: HWTest {
    let id = "crypto.local_authentication"
    let name = "LocalAuthentication availability"
    let subsystem = Subsystem.cpu

    func run(_ context: TestContext) async throws -> Outcome {
        let authentication = LAContext()
        var error: NSError?
        let available = authentication.canEvaluatePolicy(.deviceOwnerAuthentication, error: &error)
        var metrics = Metrics()
        metrics.set("available", available)
        metrics.set("biometry", Int(authentication.biometryType.rawValue))
        metrics.set("error", error?.code ?? 0)
        // No passcode set is a valid device state; anything else means the policy machinery itself is broken.
        if available || error?.code == LAError.passcodeNotSet.rawValue { return .pass(metrics) }
        return .fail("canEvaluatePolicy: \(error.map { "\($0.code) \($0.localizedDescription)" } ?? "false")", metrics)
    }
}
