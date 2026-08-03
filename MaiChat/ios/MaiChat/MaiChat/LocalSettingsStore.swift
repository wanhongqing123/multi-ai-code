import Foundation
import MaiChatCore

struct StoredRemoteIMSettings: Codable, Equatable, Sendable {
    var sdkAppID: Int?
    var masterUserID: String
    var friendUserIDs: [String]
    var slaveUserIDs: [String]
    var contacts: [RemoteIMContact]
    var unreadCountByUserID: [String: Int]

    static let empty = StoredRemoteIMSettings(
        sdkAppID: nil,
        masterUserID: "",
        friendUserIDs: [],
        slaveUserIDs: [],
        contacts: [],
        unreadCountByUserID: [:]
    )

    init(
        sdkAppID: Int?,
        masterUserID: String,
        friendUserIDs: [String] = [],
        slaveUserIDs: [String] = [],
        contacts: [RemoteIMContact] = [],
        unreadCountByUserID: [String: Int] = [:]
    ) {
        self.sdkAppID = sdkAppID
        self.masterUserID = masterUserID
        self.friendUserIDs = friendUserIDs
        self.slaveUserIDs = slaveUserIDs
        self.contacts = contacts
        self.unreadCountByUserID = unreadCountByUserID
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        self.sdkAppID = try container.decodeIfPresent(Int.self, forKey: .sdkAppID)
        self.masterUserID = try container.decodeIfPresent(String.self, forKey: .masterUserID) ?? ""
        self.friendUserIDs = try container.decodeIfPresent([String].self, forKey: .friendUserIDs) ?? []
        self.slaveUserIDs = try container.decodeIfPresent([String].self, forKey: .slaveUserIDs) ?? []
        self.contacts = try container.decodeIfPresent([RemoteIMContact].self, forKey: .contacts) ?? []
        self.unreadCountByUserID = try container.decodeIfPresent(
            [String: Int].self,
            forKey: .unreadCountByUserID
        ) ?? [:]
    }
}

final class LocalSettingsStore {
    private let defaults: UserDefaults
    private let key = "maichat_settings"

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    func load() -> StoredRemoteIMSettings {
        guard let data = defaults.data(forKey: key),
              let settings = try? JSONDecoder().decode(StoredRemoteIMSettings.self, from: data)
        else {
            return .empty
        }
        return settings
    }

    func save(_ settings: StoredRemoteIMSettings) {
        guard let data = try? JSONEncoder().encode(settings) else { return }
        defaults.set(data, forKey: key)
    }
}
