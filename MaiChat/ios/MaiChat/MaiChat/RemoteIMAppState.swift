import Foundation
import MaiChatCore

@MainActor
final class RemoteIMAppState: ObservableObject {
    enum ConnectionState: String {
        case disconnected = "未连接"
        case connecting = "连接中"
        case connected = "已连接"
        case failed = "连接失败"
    }

    @Published var sdkAppIDText = ""
    @Published var masterUserID = ""
    @Published var secretKey = ""
    @Published var newContactUserID = ""
    @Published var newContactRelation: RemoteIMContactRelation = .friend
    @Published var draftText = ""
    @Published var errorMessage: String?
    @Published var connectionState: ConnectionState = .disconnected
    @Published var chatState: MasterChatState
    @Published var hasCompletedInitialLogin = false
    @Published var presenceStatusByUserID: [String: RemoteIMPresenceStatus] = [:]
    @Published private(set) var unreadCountByUserID: [String: Int] = [:]
    @Published private(set) var userProfileByUserID: [String: RemoteIMUserProfile] = [:]

    let remoteDesktop: RemoteDesktopSession

    private let settingsStore: LocalSettingsStore
    private let secretStore: KeychainSecretStore
    private let historyStore: LocalChatHistoryStore
    private let client: RemoteIMClient
    private let autoConnectOnLaunch: Bool
    private var didHandleLaunchAutoConnect = false
    private var visibleConversationUserID: String?
    private var visibleMessageLimitByUserID: [String: Int] = [:]
    private let messagePageSize = 50

    init(
        settingsStore: LocalSettingsStore = LocalSettingsStore(),
        secretStore: KeychainSecretStore = KeychainSecretStore(),
        historyStore: LocalChatHistoryStore = LocalChatHistoryStore(),
        client: RemoteIMClient = TencentIMClient()
    ) {
        self.settingsStore = settingsStore
        self.secretStore = secretStore
        self.historyStore = historyStore
        self.client = client
        self.remoteDesktop = RemoteDesktopSession(client: client)

        var settings = settingsStore.load()
        var loadedSecretKey = secretStore.readSecretKey()
        Self.applyCredentialDefaults(settings: &settings, secretKey: &loadedSecretKey)
        self.autoConnectOnLaunch = Self.applyDebugLaunchOverrides(
            settings: &settings,
            secretKey: &loadedSecretKey
        )
        self.sdkAppIDText = settings.sdkAppID.map(String.init) ?? ""
        self.masterUserID = settings.masterUserID
        self.secretKey = loadedSecretKey

        let loadedState = MasterChatState(
            ownerUserID: settings.masterUserID,
            contacts: Self.contacts(from: settings),
            messages: historyStore.load(
                sdkAppID: settings.sdkAppID,
                ownerUserID: settings.masterUserID
            )
        )
        self.chatState = loadedState
        self.unreadCountByUserID = settings.unreadCountByUserID.filter { userID, count in
            count > 0 && loadedState.contacts.contains(where: { $0.userID == userID })
        }
        self.userProfileByUserID = Dictionary(
            uniqueKeysWithValues: loadedState.contacts.map { contact in
                (
                    contact.userID,
                    RemoteIMUserProfile(
                        userID: contact.userID,
                        displayName: contact.displayName,
                        avatarURL: contact.avatarURL
                    )
                )
            }
        )
        self.client.onIncomingText = { [weak self] event in
            Task { @MainActor in
                self?.receive(event)
            }
        }
        self.client.onIncomingVoice = { [weak self] event in
            Task { @MainActor in
                self?.receive(event)
            }
        }
        self.client.onIncomingImage = { [weak self] event in
            Task { @MainActor in
                self?.receive(event)
            }
        }
        self.client.onIncomingFile = { [weak self] event in
            Task { @MainActor in
                self?.receive(event)
            }
        }
        self.client.onPresenceStatusChanged = { [weak self] updates in
            Task { @MainActor in
                self?.applyPresenceStatusUpdates(updates)
            }
        }
    }

    var selectedContact: RemoteIMContact? {
        guard let selectedPeerID = chatState.selectedPeerID else { return nil }
        return chatState.contacts.first(where: { $0.userID == selectedPeerID })
    }

    var totalUnreadCount: Int {
        unreadCountByUserID.values.reduce(0, +)
    }

    func profile(for userID: String) -> RemoteIMUserProfile {
        if let profile = userProfileByUserID[userID] {
            return profile
        }
        if let contact = chatState.contacts.first(where: { $0.userID == userID }) {
            return RemoteIMUserProfile(
                userID: contact.userID,
                displayName: contact.displayName,
                avatarURL: contact.avatarURL
            )
        }
        return RemoteIMUserProfile(userID: userID, displayName: userID, avatarURL: nil)
    }

    var shouldShowInitialLogin: Bool {
        !hasCompletedInitialLogin
    }

    var canSend: Bool {
        connectionState == .connected &&
        selectedContact != nil &&
        !draftText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    var canSendVoice: Bool {
        connectionState == .connected && selectedContact != nil
    }

    var canSendImage: Bool {
        connectionState == .connected && selectedContact != nil
    }

    var canSendFile: Bool {
        connectionState == .connected && selectedContact != nil
    }

    func presenceStatus(for contact: RemoteIMContact) -> RemoteIMPresenceStatus {
        presenceStatusByUserID[contact.userID] ?? .unknown
    }

    static func hasCompleteLoginCredential(
        sdkAppIDText: String,
        userID: String,
        secretKey: String
    ) -> Bool {
        RemoteIMLoginCredentialPolicy.isComplete(userID: userID)
    }

    func saveSettings() {
        do {
            applyFixedCredential()
            try secretStore.saveSecretKey(secretKey)
            settingsStore.save(currentStoredSettings())
            rebuildChatStateForCurrentMaster()
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func submitInitialLogin() async {
        if let validationError = RemoteIMLoginCredentialPolicy.validationError(userID: masterUserID) {
            errorMessage = validationError
            return
        }
        await connect()
        if connectionState == .connected {
            hasCompletedInitialLogin = true
        }
    }

    func connect() async {
        saveSettings()
        guard let sdkAppID = Int(sdkAppIDText.trimmingCharacters(in: .whitespacesAndNewlines)),
              sdkAppID > 0
        else {
            errorMessage = "IM 应用配置无效"
            return
        }
        let cleanMasterUserID = masterUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        let cleanSecretKey = secretKey.trimmingCharacters(in: .whitespacesAndNewlines)

        do {
            connectionState = .connecting
            let userSig = try TencentUserSigGenerator.generate(
                sdkAppID: sdkAppID,
                userID: cleanMasterUserID,
                secretKey: cleanSecretKey
            )
            try await client.connect(
                sdkAppID: sdkAppID,
                userID: cleanMasterUserID,
                userSig: userSig
            )
            connectionState = .connected
            await refreshProfilesForCurrentUsers()
            await refreshPresenceForCurrentContacts()
            errorMessage = nil
        } catch {
            connectionState = .failed
            errorMessage = error.localizedDescription
        }
    }

    func connectIfRequestedByLaunchEnvironment() async {
        guard autoConnectOnLaunch, !didHandleLaunchAutoConnect else { return }
        didHandleLaunchAutoConnect = true
        await connect()
    }

    func disconnect() async {
        await remoteDesktop.stop()
        await client.disconnect()
        presenceStatusByUserID = [:]
        connectionState = .disconnected
    }

    func addContact() {
        do {
            let addedUserID = newContactUserID.trimmingCharacters(in: .whitespacesAndNewlines)
            try chatState.upsertContact(userID: newContactUserID, relation: newContactRelation)
            newContactUserID = ""
            settingsStore.save(currentStoredSettings())
            errorMessage = nil
            Task {
                await refreshProfiles(userIDs: [addedUserID])
                await refreshPresenceForCurrentContacts()
            }
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func deleteContact(_ contact: RemoteIMContact) async -> Bool {
        await deleteContact(userID: contact.userID)
    }

    func deleteContact(userID: String) async -> Bool {
        let cleanUserID = userID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanUserID.isEmpty else { return false }
        do {
            try await client.deleteContact(userID: cleanUserID)
        } catch {
            errorMessage = error.localizedDescription
            return false
        }
        chatState.removeContactAndMessages(userID: cleanUserID)
        if newContactUserID.trimmingCharacters(in: .whitespacesAndNewlines) == cleanUserID {
            newContactUserID = ""
        }
        persistCurrentHistory()
        presenceStatusByUserID = RemoteIMPresenceStatusPolicy.merged(
            current: presenceStatusByUserID,
            updates: [:],
            contactUserIDs: chatState.contacts.map(\.userID)
        )
        unreadCountByUserID[cleanUserID] = nil
        visibleMessageLimitByUserID[cleanUserID] = nil
        if visibleConversationUserID == cleanUserID {
            visibleConversationUserID = nil
        }
        userProfileByUserID[cleanUserID] = nil
        settingsStore.save(currentStoredSettings())
        Task {
            await refreshPresenceForCurrentContacts()
        }
        errorMessage = nil
        return true
    }

    func clearHistory(with userID: String) async -> Bool {
        let cleanUserID = userID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanUserID.isEmpty else { return false }
        do {
            try await client.clearHistory(userID: cleanUserID)
            chatState.removeMessages(with: cleanUserID)
            unreadCountByUserID[cleanUserID] = nil
            visibleMessageLimitByUserID[cleanUserID] = messagePageSize
            persistCurrentHistory()
            settingsStore.save(currentStoredSettings())
            errorMessage = nil
            return true
        } catch {
            errorMessage = error.localizedDescription
            return false
        }
    }

    func selectContact(_ contact: RemoteIMContact) {
        if chatState.selectedPeerID != contact.userID {
            chatState.selectPeer(userID: contact.userID)
        }
        if unreadCountByUserID.removeValue(forKey: contact.userID) != nil {
            settingsStore.save(currentStoredSettings())
        }
    }

    func setConversationVisible(userID: String, visible: Bool) {
        let cleanUserID = userID.trimmingCharacters(in: .whitespacesAndNewlines)
        var shouldPersistSettings = false
        if visible {
            visibleConversationUserID = cleanUserID
            shouldPersistSettings = unreadCountByUserID.removeValue(forKey: cleanUserID) != nil
        } else if visibleConversationUserID == cleanUserID {
            visibleConversationUserID = nil
        }
        if shouldPersistSettings {
            settingsStore.save(currentStoredSettings())
        }
    }

    func unreadCount(for userID: String) -> Int {
        unreadCountByUserID[userID] ?? 0
    }

    func visibleMessages(with userID: String) -> [RemoteIMMessage] {
        let messages = chatState.messages(with: userID)
        let limit = visibleMessageLimitByUserID[userID] ?? messagePageSize
        return Array(messages.suffix(limit))
    }

    func hasEarlierMessages(with userID: String) -> Bool {
        let limit = visibleMessageLimitByUserID[userID] ?? messagePageSize
        return chatState.messageCount(with: userID) > limit
    }

    func loadEarlierMessages(with userID: String) {
        let currentLimit = visibleMessageLimitByUserID[userID] ?? messagePageSize
        visibleMessageLimitByUserID[userID] = currentLimit + messagePageSize
        objectWillChange.send()
    }

    func sendDraft() async {
        guard canSend else { return }

        var queuedMessageID: UUID?
        do {
            let message = try chatState.queueOutgoingText(draftText)
            queuedMessageID = message.id
            let textToSend = message.text
            draftText = ""
            persistCurrentHistory()
            let receipt = try await client.sendText(to: message.toUserID, text: textToSend)
            try chatState.updateMessageDelivery(
                id: message.id,
                remoteID: receipt.remoteID,
                createdAt: receipt.createdAt
            )
            persistCurrentHistory()
            errorMessage = nil
        } catch {
            if let queuedMessageID {
                try? chatState.updateMessageStatus(id: queuedMessageID, status: .failed)
                persistCurrentHistory()
            }
            errorMessage = error.localizedDescription
        }
    }

    func sendVoiceRecording(_ recording: RemoteIMVoiceRecording) async {
        guard canSendVoice else { return }

        var queuedMessageID: UUID?
        do {
            let message = try chatState.queueOutgoingVoice(
                filePath: recording.fileURL.path,
                durationSeconds: recording.durationSeconds
            )
            queuedMessageID = message.id
            persistCurrentHistory()
            let receipt = try await client.sendVoice(to: message.toUserID, recording: recording)
            try chatState.updateMessageDelivery(
                id: message.id,
                remoteID: receipt.remoteID,
                createdAt: receipt.createdAt
            )
            persistCurrentHistory()
            errorMessage = nil
        } catch {
            if let queuedMessageID {
                try? chatState.updateMessageStatus(id: queuedMessageID, status: .failed)
                persistCurrentHistory()
            }
            errorMessage = error.localizedDescription
        }
    }

    func sendImageFile(_ image: RemoteIMImageFile) async {
        guard canSendImage else { return }

        var queuedMessageID: UUID?
        do {
            let message = try chatState.queueOutgoingImage(
                filePath: image.fileURL.path,
                width: image.width,
                height: image.height,
                sizeBytes: image.sizeBytes
            )
            queuedMessageID = message.id
            persistCurrentHistory()
            let receipt = try await client.sendImage(to: message.toUserID, image: image)
            try chatState.updateMessageDelivery(
                id: message.id,
                remoteID: receipt.remoteID,
                createdAt: receipt.createdAt
            )
            persistCurrentHistory()
            errorMessage = nil
        } catch {
            if let queuedMessageID {
                try? chatState.updateMessageStatus(id: queuedMessageID, status: .failed)
                persistCurrentHistory()
            }
            errorMessage = error.localizedDescription
        }
    }

    func sendFile(_ file: RemoteIMFile) async {
        guard canSendFile else { return }

        var queuedMessageID: UUID?
        do {
            let message = try chatState.queueOutgoingFile(
                filePath: file.fileURL.path,
                fileName: file.fileName,
                mimeType: file.mimeType,
                sizeBytes: file.sizeBytes
            )
            queuedMessageID = message.id
            persistCurrentHistory()
            let receipt = try await client.sendFile(to: message.toUserID, file: file)
            try chatState.updateMessageDelivery(
                id: message.id,
                remoteID: receipt.remoteID,
                createdAt: receipt.createdAt
            )
            persistCurrentHistory()
            errorMessage = nil
        } catch {
            if let queuedMessageID {
                try? chatState.updateMessageStatus(id: queuedMessageID, status: .failed)
                persistCurrentHistory()
            }
            errorMessage = error.localizedDescription
        }
    }

    func requestRemoteDesktopView(of contact: RemoteIMContact) async {
        guard connectionState == .connected else {
            errorMessage = "请先连接 IM"
            return
        }
        guard let sdkAppID = currentSDKAppID(), sdkAppID > 0 else {
            errorMessage = "IM 应用配置无效"
            return
        }
        let localUserID = masterUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        do {
            let userSig = try TencentUserSigGenerator.generate(
                sdkAppID: sdkAppID,
                userID: localUserID,
                secretKey: secretKey.trimmingCharacters(in: .whitespacesAndNewlines)
            )
            await remoteDesktop.requestView(
                peerUserID: contact.userID,
                sdkAppID: sdkAppID,
                localUserID: localUserID,
                userSig: userSig
            )
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func stopRemoteDesktopView() async {
        await remoteDesktop.stop()
    }

    private func receive(_ event: IncomingRemoteIMText) {
        if remoteDesktop.handleIncomingText(from: event.fromUserID, text: event.text) {
            return
        }
        let previousCount = chatState.messages.count
        _ = chatState.receiveText(
            event.text,
            fromUserID: event.fromUserID,
            remoteID: event.remoteID,
            now: event.createdAt
        )
        updateUnreadAfterReceiving(from: event.fromUserID, wasInserted: chatState.messages.count > previousCount)
        refreshProfileIfNeeded(userID: event.fromUserID)
        persistCurrentHistory()
        settingsStore.save(currentStoredSettings())
    }

    private func receive(_ event: IncomingRemoteIMVoice) {
        let previousCount = chatState.messages.count
        _ = chatState.receiveVoice(
            filePath: event.fileURL.path,
            durationSeconds: event.durationSeconds,
            fromUserID: event.fromUserID,
            remoteID: event.remoteID,
            now: event.createdAt
        )
        updateUnreadAfterReceiving(from: event.fromUserID, wasInserted: chatState.messages.count > previousCount)
        refreshProfileIfNeeded(userID: event.fromUserID)
        persistCurrentHistory()
        settingsStore.save(currentStoredSettings())
    }

    private func receive(_ event: IncomingRemoteIMImage) {
        let previousCount = chatState.messages.count
        _ = chatState.receiveImage(
            filePath: event.fileURL.path,
            fromUserID: event.fromUserID,
            remoteID: event.remoteID,
            width: event.width,
            height: event.height,
            sizeBytes: event.sizeBytes,
            now: event.createdAt
        )
        updateUnreadAfterReceiving(from: event.fromUserID, wasInserted: chatState.messages.count > previousCount)
        refreshProfileIfNeeded(userID: event.fromUserID)
        persistCurrentHistory()
        settingsStore.save(currentStoredSettings())
    }

    private func receive(_ event: IncomingRemoteIMFile) {
        let previousCount = chatState.messages.count
        _ = chatState.receiveFile(
            filePath: event.fileURL.path,
            fromUserID: event.fromUserID,
            fileName: event.fileName,
            mimeType: event.mimeType,
            remoteID: event.remoteID,
            sizeBytes: event.sizeBytes,
            now: event.createdAt
        )
        updateUnreadAfterReceiving(from: event.fromUserID, wasInserted: chatState.messages.count > previousCount)
        refreshProfileIfNeeded(userID: event.fromUserID)
        persistCurrentHistory()
        settingsStore.save(currentStoredSettings())
    }

    private func updateUnreadAfterReceiving(from userID: String, wasInserted: Bool) {
        guard wasInserted else { return }
        if visibleConversationUserID == userID {
            unreadCountByUserID[userID] = nil
        } else {
            unreadCountByUserID[userID, default: 0] += 1
        }
    }

    private func currentStoredSettings() -> StoredRemoteIMSettings {
        StoredRemoteIMSettings(
            sdkAppID: Int(sdkAppIDText.trimmingCharacters(in: .whitespacesAndNewlines)),
            masterUserID: masterUserID.trimmingCharacters(in: .whitespacesAndNewlines),
            friendUserIDs: chatState.contacts
                .map(\.userID),
            slaveUserIDs: [],
            contacts: chatState.contacts,
            unreadCountByUserID: unreadCountByUserID
        )
    }

    private func rebuildChatStateForCurrentMaster() {
        let cleanMasterUserID = masterUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard chatState.ownerUserID != cleanMasterUserID else { return }
        persistCurrentHistory()
        presenceStatusByUserID = [:]
        let nextState = MasterChatState(
            ownerUserID: cleanMasterUserID,
            contacts: chatState.contacts,
            messages: historyStore.load(
                sdkAppID: currentSDKAppID(),
                ownerUserID: cleanMasterUserID
            ),
            selectedPeerID: chatState.selectedPeerID
        )
        chatState = nextState
        Task {
            await refreshPresenceForCurrentContacts()
        }
    }

    private func persistCurrentHistory() {
        do {
            try historyStore.save(
                messages: chatState.messages,
                sdkAppID: currentSDKAppID(),
                ownerUserID: chatState.ownerUserID
            )
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func refreshPresenceForCurrentContacts() async {
        guard connectionState == .connected else { return }
        let contactUserIDs = chatState.contacts.map(\.userID)
        guard !contactUserIDs.isEmpty else {
            presenceStatusByUserID = [:]
            return
        }

        do {
            let updates = try await client.refreshPresenceStatuses(userIDs: contactUserIDs)
            applyPresenceStatusUpdates(updates)
        } catch {
            presenceStatusByUserID = RemoteIMPresenceStatusPolicy.merged(
                current: presenceStatusByUserID,
                updates: [:],
                contactUserIDs: contactUserIDs
            )
            #if DEBUG
            print("RemoteIM presence refresh failed: \(error.localizedDescription)")
            #endif
        }

        do {
            try await client.subscribePresenceStatuses(userIDs: contactUserIDs)
        } catch {
            #if DEBUG
            print("RemoteIM presence subscribe failed: \(error.localizedDescription)")
            #endif
        }
    }

    private func refreshProfilesForCurrentUsers() async {
        await refreshProfiles(
            userIDs: [chatState.ownerUserID] + chatState.contacts.map(\.userID)
        )
    }

    private func refreshProfiles(userIDs: [String]) async {
        guard connectionState == .connected else { return }
        do {
            let profiles = try await client.refreshUserProfiles(userIDs: userIDs)
            for profile in profiles {
                userProfileByUserID[profile.userID] = profile
                if let contact = chatState.contacts.first(where: { $0.userID == profile.userID }) {
                    try chatState.upsertContact(
                        userID: profile.userID,
                        relation: contact.relation,
                        displayName: profile.displayName,
                        avatarURL: profile.avatarURL
                    )
                }
            }
            settingsStore.save(currentStoredSettings())
        } catch {
            #if DEBUG
            print("RemoteIM profile refresh failed: \(error.localizedDescription)")
            #endif
        }
    }

    private func refreshProfileIfNeeded(userID: String) {
        let profile = profile(for: userID)
        guard profile.displayName == userID && profile.avatarURL == nil else { return }
        Task { await refreshProfiles(userIDs: [userID]) }
    }

    private func applyPresenceStatusUpdates(_ updates: [String: RemoteIMPresenceStatus]) {
        presenceStatusByUserID = RemoteIMPresenceStatusPolicy.merged(
            current: presenceStatusByUserID,
            updates: updates,
            contactUserIDs: chatState.contacts.map(\.userID)
        )
    }

    private func currentSDKAppID() -> Int? {
        Int(sdkAppIDText.trimmingCharacters(in: .whitespacesAndNewlines))
    }

    private static func contacts(from settings: StoredRemoteIMSettings) -> [RemoteIMContact] {
        var contacts = settings.contacts
        for userID in settings.friendUserIDs + settings.slaveUserIDs
        where !contacts.contains(where: { $0.userID == userID }) {
            contacts.append(RemoteIMContact(userID: userID, displayName: userID, relation: .friend))
        }
        return contacts
    }

    private func applyFixedCredential() {
        let credential = RemoteIMCredentialDefaults.resolvedCredential(
            sdkAppID: Int(sdkAppIDText.trimmingCharacters(in: .whitespacesAndNewlines)),
            secretKey: secretKey
        )
        sdkAppIDText = String(credential.sdkAppID)
        secretKey = credential.userSigSecretKey
    }

    private static func applyCredentialDefaults(
        settings: inout StoredRemoteIMSettings,
        secretKey: inout String
    ) {
        let credential = RemoteIMCredentialDefaults.resolvedCredential(
            sdkAppID: settings.sdkAppID,
            secretKey: secretKey
        )
        settings.sdkAppID = credential.sdkAppID
        secretKey = credential.userSigSecretKey
    }

    private static func applyDebugLaunchOverrides(
        settings: inout StoredRemoteIMSettings,
        secretKey: inout String
    ) -> Bool {
        #if DEBUG
        let environment = ProcessInfo.processInfo.environment
        if let rawSDKAppID = environment["MAICHAT_SDK_APP_ID"],
           let sdkAppID = Int(rawSDKAppID.trimmingCharacters(in: .whitespacesAndNewlines)),
           sdkAppID > 0
        {
            settings.sdkAppID = sdkAppID
        }
        if let masterUserID = cleanEnvironmentValue(
            environment["MAICHAT_MASTER_USER_ID"]
        ) {
            settings.masterUserID = masterUserID
        }
        if let injectedSecretKey = cleanEnvironmentValue(
            environment["MAICHAT_SECRET_KEY"]
        ) {
            secretKey = injectedSecretKey
        }
        if let rawSlaveUserIDs = environment["MAICHAT_SLAVE_USER_IDS"] {
            settings.slaveUserIDs = rawSlaveUserIDs
                .split(separator: ",")
                .map { String($0).trimmingCharacters(in: .whitespacesAndNewlines) }
                .filter { !$0.isEmpty }
        }
        if let rawFriendUserIDs = environment["MAICHAT_FRIEND_USER_IDS"] {
            settings.friendUserIDs = rawFriendUserIDs
                .split(separator: ",")
                .map { String($0).trimmingCharacters(in: .whitespacesAndNewlines) }
                .filter { !$0.isEmpty }
        }
        return environment["MAICHAT_AUTO_CONNECT"] == "1"
        #else
        return false
        #endif
    }

    private static func cleanEnvironmentValue(_ value: String?) -> String? {
        guard let cleanValue = value?.trimmingCharacters(in: .whitespacesAndNewlines),
              !cleanValue.isEmpty
        else {
            return nil
        }
        return cleanValue
    }
}
