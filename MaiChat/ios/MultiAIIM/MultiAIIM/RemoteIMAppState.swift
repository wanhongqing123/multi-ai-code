import Foundation
import MultiAIIMCore

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

    private let settingsStore: LocalSettingsStore
    private let secretStore: KeychainSecretStore
    private let historyStore: LocalChatHistoryStore
    private let client: RemoteIMClient
    private let autoConnectOnLaunch: Bool
    private var didHandleLaunchAutoConnect = false

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
        await client.disconnect()
        presenceStatusByUserID = [:]
        connectionState = .disconnected
    }

    func addContact() {
        do {
            try chatState.upsertContact(userID: newContactUserID, relation: newContactRelation)
            newContactUserID = ""
            settingsStore.save(currentStoredSettings())
            errorMessage = nil
            Task {
                await refreshPresenceForCurrentContacts()
            }
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func deleteContact(_ contact: RemoteIMContact) {
        deleteContact(userID: contact.userID)
    }

    func deleteContact(userID: String) {
        let cleanUserID = userID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanUserID.isEmpty else { return }
        chatState.removeContactAndMessages(userID: cleanUserID)
        if newContactUserID.trimmingCharacters(in: .whitespacesAndNewlines) == cleanUserID {
            newContactUserID = ""
        }
        persistCurrentHistory()
        settingsStore.save(currentStoredSettings())
        presenceStatusByUserID = RemoteIMPresenceStatusPolicy.merged(
            current: presenceStatusByUserID,
            updates: [:],
            contactUserIDs: chatState.contacts.map(\.userID)
        )
        Task {
            await refreshPresenceForCurrentContacts()
        }
        errorMessage = nil
    }

    func selectContact(_ contact: RemoteIMContact) {
        chatState.selectPeer(userID: contact.userID)
    }

    func sendDraft() async {
        guard canSend else { return }

        do {
            let message = try chatState.queueOutgoingText(draftText)
            let textToSend = message.text
            draftText = ""
            persistCurrentHistory()
            try await client.sendText(to: message.toUserID, text: textToSend)
            try chatState.updateMessageStatus(id: message.id, status: .sent)
            persistCurrentHistory()
            errorMessage = nil
        } catch {
            if let lastMessage = chatState.messages.last, lastMessage.status == .pending {
                try? chatState.updateMessageStatus(id: lastMessage.id, status: .failed)
                persistCurrentHistory()
            }
            errorMessage = error.localizedDescription
        }
    }

    func sendVoiceRecording(_ recording: RemoteIMVoiceRecording) async {
        guard canSendVoice else { return }

        do {
            let message = try chatState.queueOutgoingVoice(
                filePath: recording.fileURL.path,
                durationSeconds: recording.durationSeconds
            )
            persistCurrentHistory()
            try await client.sendVoice(to: message.toUserID, recording: recording)
            try chatState.updateMessageStatus(id: message.id, status: .sent)
            persistCurrentHistory()
            errorMessage = nil
        } catch {
            if let lastMessage = chatState.messages.last, lastMessage.status == .pending {
                try? chatState.updateMessageStatus(id: lastMessage.id, status: .failed)
                persistCurrentHistory()
            }
            errorMessage = error.localizedDescription
        }
    }

    func sendImageFile(_ image: RemoteIMImageFile) async {
        guard canSendImage else { return }

        do {
            let message = try chatState.queueOutgoingImage(
                filePath: image.fileURL.path,
                width: image.width,
                height: image.height,
                sizeBytes: image.sizeBytes
            )
            persistCurrentHistory()
            try await client.sendImage(to: message.toUserID, image: image)
            try chatState.updateMessageStatus(id: message.id, status: .sent)
            persistCurrentHistory()
            errorMessage = nil
        } catch {
            if let lastMessage = chatState.messages.last, lastMessage.status == .pending {
                try? chatState.updateMessageStatus(id: lastMessage.id, status: .failed)
                persistCurrentHistory()
            }
            errorMessage = error.localizedDescription
        }
    }

    private func receive(_ event: IncomingRemoteIMText) {
        _ = chatState.receiveText(event.text, fromUserID: event.fromUserID)
        persistCurrentHistory()
        settingsStore.save(currentStoredSettings())
    }

    private func receive(_ event: IncomingRemoteIMVoice) {
        _ = chatState.receiveVoice(
            filePath: event.fileURL.path,
            durationSeconds: event.durationSeconds,
            fromUserID: event.fromUserID,
            remoteID: event.remoteID
        )
        persistCurrentHistory()
        settingsStore.save(currentStoredSettings())
    }

    private func receive(_ event: IncomingRemoteIMImage) {
        _ = chatState.receiveImage(
            filePath: event.fileURL.path,
            fromUserID: event.fromUserID,
            remoteID: event.remoteID,
            width: event.width,
            height: event.height,
            sizeBytes: event.sizeBytes
        )
        persistCurrentHistory()
        settingsStore.save(currentStoredSettings())
    }

    private func receive(_ event: IncomingRemoteIMFile) {
        _ = chatState.receiveFile(
            filePath: event.fileURL.path,
            fromUserID: event.fromUserID,
            fileName: event.fileName,
            mimeType: event.mimeType,
            remoteID: event.remoteID,
            sizeBytes: event.sizeBytes
        )
        persistCurrentHistory()
        settingsStore.save(currentStoredSettings())
    }

    private func currentStoredSettings() -> StoredRemoteIMSettings {
        StoredRemoteIMSettings(
            sdkAppID: Int(sdkAppIDText.trimmingCharacters(in: .whitespacesAndNewlines)),
            masterUserID: masterUserID.trimmingCharacters(in: .whitespacesAndNewlines),
            friendUserIDs: chatState.contacts
                .map(\.userID),
            slaveUserIDs: []
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
        (settings.friendUserIDs + settings.slaveUserIDs).map { userID in
            RemoteIMContact(userID: userID, displayName: userID, relation: .friend)
        }
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
        if let rawSDKAppID = environment["MULTI_AI_IM_SDK_APP_ID"],
           let sdkAppID = Int(rawSDKAppID.trimmingCharacters(in: .whitespacesAndNewlines)),
           sdkAppID > 0
        {
            settings.sdkAppID = sdkAppID
        }
        if let masterUserID = cleanEnvironmentValue(
            environment["MULTI_AI_IM_MASTER_USER_ID"]
        ) {
            settings.masterUserID = masterUserID
        }
        if let injectedSecretKey = cleanEnvironmentValue(
            environment["MULTI_AI_IM_SECRET_KEY"]
        ) {
            secretKey = injectedSecretKey
        }
        if let rawSlaveUserIDs = environment["MULTI_AI_IM_SLAVE_USER_IDS"] {
            settings.slaveUserIDs = rawSlaveUserIDs
                .split(separator: ",")
                .map { String($0).trimmingCharacters(in: .whitespacesAndNewlines) }
                .filter { !$0.isEmpty }
        }
        if let rawFriendUserIDs = environment["MULTI_AI_IM_FRIEND_USER_IDS"] {
            settings.friendUserIDs = rawFriendUserIDs
                .split(separator: ",")
                .map { String($0).trimmingCharacters(in: .whitespacesAndNewlines) }
                .filter { !$0.isEmpty }
        }
        return environment["MULTI_AI_IM_AUTO_CONNECT"] == "1"
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
