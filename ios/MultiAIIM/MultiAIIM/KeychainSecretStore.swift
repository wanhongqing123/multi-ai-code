import Foundation
import Security

enum KeychainSecretStoreError: Error, LocalizedError {
    case unexpectedStatus(OSStatus)

    var errorDescription: String? {
        switch self {
        case let .unexpectedStatus(status):
            return "Keychain 操作失败：\(status)"
        }
    }
}

final class KeychainSecretStore {
    private let service = "com.multiaicode.remoteim"
    private let account = "tencent-user-sig-secret-key"

    func readSecretKey() -> String {
        var query = baseQuery()
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne

        var result: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        guard status == errSecSuccess,
              let data = result as? Data,
              let value = String(data: data, encoding: .utf8)
        else {
            return ""
        }
        return value
    }

    func saveSecretKey(_ secretKey: String) throws {
        let cleanSecretKey = secretKey.trimmingCharacters(in: .whitespacesAndNewlines)
        SecItemDelete(baseQuery() as CFDictionary)
        guard !cleanSecretKey.isEmpty else { return }

        var item = baseQuery()
        item[kSecValueData as String] = Data(cleanSecretKey.utf8)
        item[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        let status = SecItemAdd(item as CFDictionary, nil)
        guard status == errSecSuccess else {
            throw KeychainSecretStoreError.unexpectedStatus(status)
        }
    }

    private func baseQuery() -> [String: Any] {
        [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account
        ]
    }
}
