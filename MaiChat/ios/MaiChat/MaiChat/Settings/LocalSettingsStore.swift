// 文件作用：持久化不敏感的本机设置，并向应用提供可观察的配置状态。
// 保存非敏感界面选项、联系人配置和本地迁移状态。
// 读取和保存按账号隔离的联系人、分组与未读快照；敏感令牌交给 Keychain 管理。
import Foundation
import MaiChatCore

struct StoredRemoteIMSettings: Codable, Equatable, Sendable {
    var sdkAppID: Int?
    var masterUserID: String
    var maiChatAccountID: String
    var maiChatNickname: String
    var selfAvatarURL: String?
    var friendUserIDs: [String]
    var slaveUserIDs: [String]
    var contacts: [RemoteIMContact]
    var contactGroups: [RemoteIMContactGroup]
    var unreadCountByUserID: [String: Int]
    var reconnectOnLaunch: Bool

    static let empty = StoredRemoteIMSettings(
        sdkAppID: nil,
        masterUserID: "",
        maiChatAccountID: "",
        maiChatNickname: "",
        selfAvatarURL: nil,
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
        maiChatAccountID: String = "",
        maiChatNickname: String = "",
        selfAvatarURL: String? = nil,
        friendUserIDs: [String] = [],
        slaveUserIDs: [String] = [],
        contacts: [RemoteIMContact] = [],
        contactGroups: [RemoteIMContactGroup] = [],
        unreadCountByUserID: [String: Int] = [:],
        reconnectOnLaunch: Bool
    ) {
        self.sdkAppID = sdkAppID
        self.masterUserID = masterUserID
        self.maiChatAccountID = maiChatAccountID
        self.maiChatNickname = maiChatNickname
        self.selfAvatarURL = selfAvatarURL
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
        self.maiChatAccountID = try container.decodeIfPresent(
            String.self, forKey: .maiChatAccountID
        ) ?? ""
        self.maiChatNickname = try container.decodeIfPresent(
            String.self, forKey: .maiChatNickname
        ) ?? ""
        self.selfAvatarURL = try container.decodeIfPresent(String.self, forKey: .selfAvatarURL)
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

    private func accountKey(_ userID: String) -> String {
        "maichat_settings_account_" + userID
    }

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
                if !settings.masterUserID.isEmpty {
                    defaults.set(data, forKey: accountKey(settings.masterUserID))
                }
            }
        }
    }

    func saveAccountSnapshot(_ settings: StoredRemoteIMSettings) -> Bool {
        guard !settings.masterUserID.isEmpty,
              let data = try? JSONEncoder().encode(settings) else { return false }
        // 切换手机号前同步保存，避免紧接着写入空登录态时丢掉旧账号联系人。
        Self.writes.sync {
            defaults.set(data, forKey: accountKey(settings.masterUserID))
        }
        return true
    }

    func saveImmediately(_ settings: StoredRemoteIMSettings) -> Bool {
        guard let data = try? JSONEncoder().encode(settings) else { return false }
        lock.lock(); cached = settings; revision &+= 1; let version = revision; lock.unlock()
        return Self.writes.sync { [self] in
            lock.lock(); let current = revision; lock.unlock()
            guard current == version else { return false }
            defaults.set(data, forKey: key)
            if !settings.masterUserID.isEmpty {
                defaults.set(data, forKey: accountKey(settings.masterUserID))
            }
            return true
        }
    }

    func loadAccountSnapshot(_ userID: String) -> StoredRemoteIMSettings? {
        guard !userID.isEmpty,
              let data = defaults.data(forKey: accountKey(userID)),
              let settings = try? JSONDecoder().decode(StoredRemoteIMSettings.self, from: data),
              settings.masterUserID == userID else { return nil }
        return settings
    }
}
