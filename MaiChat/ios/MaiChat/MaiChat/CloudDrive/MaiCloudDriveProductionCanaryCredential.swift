#if DEBUG && targetEnvironment(simulator)
import Foundation

/// Only the explicitly launched simulator canary reads this one-time file.
/// Neither the password nor a bearer token is passed through XCTest actions.
enum MaiCloudDriveProductionCanaryCredential {
    struct Account: Decodable {
        let account: String
        let password: String
    }

    static func consume() throws -> Account {
        let documents = FileManager.default.urls(for: .documentDirectory,
                                                 in: .userDomainMask)[0]
        let file = documents.appendingPathComponent("cloud-drive-production-canary.json")
        let attributes = try FileManager.default.attributesOfItem(atPath: file.path)
        guard (attributes[.posixPermissions] as? NSNumber)?.intValue == 0o600 else {
            throw CocoaError(.fileReadNoPermission)
        }
        let data = try Data(contentsOf: file)
        try FileManager.default.removeItem(at: file)
        let account = try JSONDecoder().decode(Account.self, from: data)
        guard !account.account.isEmpty, !account.password.isEmpty else {
            throw CocoaError(.fileReadCorruptFile)
        }
        return account
    }
}
#endif
