import Foundation
import MaiChatCore

struct StoredRemoteIMSettings: Codable, Equatable, Sendable {
    var sdkAppID: Int?
    var masterUserID: String
    var friendUserIDs: [String]
    var slaveUserIDs: [String]
    var contacts: [RemoteIMContact]
    var contactGroups: [RemoteIMContactGroup]
    var unreadCountByUserID: [String: Int]
    var reconnectOnLaunch: Bool

    static let empty = StoredRemoteIMSettings(
        sdkAppID: nil,
        masterUserID: "",
        friendUserIDs: [],
        slaveUserIDs: [],
        contacts: [],
        contactGroups: [],
        unreadCountByUserID: [:],
        reconnectOnLaunch: false
    )

    init(
        sdkAppID: Int?,
        masterUserID: String,
        friendUserIDs: [String] = [],
        slaveUserIDs: [String] = [],
        contacts: [RemoteIMContact] = [],
        contactGroups: [RemoteIMContactGroup] = [],
        unreadCountByUserID: [String: Int] = [:],
        reconnectOnLaunch: Bool
    ) {
        self.sdkAppID = sdkAppID
        self.masterUserID = masterUserID
        self.friendUserIDs = friendUserIDs
        self.slaveUserIDs = slaveUserIDs
        self.contacts = contacts
        self.contactGroups = contactGroups
        self.unreadCountByUserID = unreadCountByUserID
        self.reconnectOnLaunch = reconnectOnLaunch
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        self.sdkAppID = try container.decodeIfPresent(Int.self, forKey: .sdkAppID)
        self.masterUserID = try container.decodeIfPresent(String.self, forKey: .masterUserID) ?? ""
        self.friendUserIDs = try container.decodeIfPresent([String].self, forKey: .friendUserIDs) ?? []
        self.slaveUserIDs = try container.decodeIfPresent([String].self, forKey: .slaveUserIDs) ?? []
        self.contacts = try container.decodeIfPresent([RemoteIMContact].self, forKey: .contacts) ?? []
        self.contactGroups = try container.decodeIfPresent(
            [RemoteIMContactGroup].self,
            forKey: .contactGroups
        ) ?? []
        self.unreadCountByUserID = try container.decodeIfPresent(
            [String: Int].self,
            forKey: .unreadCountByUserID
        ) ?? [:]
        self.reconnectOnLaunch = RemoteIMLoginCredentialPolicy.migratedReconnectOnLaunch(
            storedValue: try container.decodeIfPresent(Bool.self, forKey: .reconnectOnLaunch),
            userID: masterUserID
        )
    }
}

final class LocalSettingsStore: @unchecked Sendable {
    private static let writes = DispatchQueue(label: "MaiChat.SettingsWrites", qos: .utility)
    private let defaults: UserDefaults
    private let key = "maichat_settings"
    private let lock = NSLock()
    private var cached: StoredRemoteIMSettings?
    private var revision = 0

    init(defaults: UserDefaults = .standard) { self.defaults = defaults }

    func load() -> StoredRemoteIMSettings {
        lock.lock(); let existing = cached; lock.unlock()
        if let existing { return existing }
        let loaded = defaults.data(forKey: key).flatMap {
            try? JSONDecoder().decode(StoredRemoteIMSettings.self, from: $0)
        } ?? .empty
        lock.lock()
        if cached == nil { cached = loaded }
        let result = cached ?? loaded
        lock.unlock()
        return result
    }

    func save(_ settings: StoredRemoteIMSettings) {
        lock.lock(); cached = settings; revision &+= 1; let version = revision; lock.unlock()
        Self.writes.async { [self] in
            lock.lock(); let current = revision; lock.unlock()
            guard current == version else { return }
            autoreleasepool {
                guard let data = try? JSONEncoder().encode(settings) else { return }
                defaults.set(data, forKey: key)
            }
        }
    }
}
