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
    @Published var newContactRelation: RemoteIMContactRelation = .slave
    @Published var draftText = ""
    @Published var errorMessage: String?
    @Published var connectionState: ConnectionState = .disconnected
    @Published var chatState: MasterChatState

    private let settingsStore: LocalSettingsStore
    private let secretStore: KeychainSecretStore
    private let client: RemoteIMClient
    private let autoConnectOnLaunch: Bool
    private var didHandleLaunchAutoConnect = false

    init(
        settingsStore: LocalSettingsStore = LocalSettingsStore(),
        secretStore: KeychainSecretStore = KeychainSecretStore(),
        client: RemoteIMClient = TencentIMClient()
    ) {
        self.settingsStore = settingsStore
        self.secretStore = secretStore
        self.client = client

        var settings = settingsStore.load()
        var loadedSecretKey = secretStore.readSecretKey()
        self.autoConnectOnLaunch = Self.applyDebugLaunchOverrides(
            settings: &settings,
            secretKey: &loadedSecretKey
        )
        self.sdkAppIDText = settings.sdkAppID.map(String.init) ?? ""
        self.masterUserID = settings.masterUserID
        self.secretKey = loadedSecretKey

        var loadedState = MasterChatState(ownerUserID: settings.masterUserID)
        for friendUserID in settings.friendUserIDs {
            try? loadedState.upsertFriend(userID: friendUserID)
        }
        for slaveUserID in settings.slaveUserIDs {
            try? loadedState.upsertSlave(userID: slaveUserID)
        }
        self.chatState = loadedState

        self.client.onIncomingText = { [weak self] event in
            Task { @MainActor in
                self?.receive(event)
            }
        }
    }

    var selectedContact: RemoteIMContact? {
        guard let selectedPeerID = chatState.selectedPeerID else { return nil }
        return chatState.contacts.first(where: { $0.userID == selectedPeerID })
    }

    var canSend: Bool {
        connectionState == .connected &&
        selectedContact != nil &&
        !draftText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    func saveSettings() {
        do {
            try secretStore.saveSecretKey(secretKey)
            settingsStore.save(currentStoredSettings())
            rebuildChatStateForCurrentMaster()
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func connect() async {
        saveSettings()
        guard let sdkAppID = Int(sdkAppIDText.trimmingCharacters(in: .whitespacesAndNewlines)),
              sdkAppID > 0
        else {
            errorMessage = "请填写有效 SDKAppID"
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
        connectionState = .disconnected
    }

    func addContact() {
        do {
            try chatState.upsertContact(userID: newContactUserID, relation: newContactRelation)
            newContactUserID = ""
            settingsStore.save(currentStoredSettings())
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
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
            try await client.sendText(to: message.toUserID, text: textToSend)
            try chatState.updateMessageStatus(id: message.id, status: .sent)
            errorMessage = nil
        } catch {
            if let lastMessage = chatState.messages.last, lastMessage.status == .pending {
                try? chatState.updateMessageStatus(id: lastMessage.id, status: .failed)
            }
            errorMessage = error.localizedDescription
        }
    }

    private func receive(_ event: IncomingRemoteIMText) {
        _ = chatState.receiveText(event.text, fromUserID: event.fromUserID)
        settingsStore.save(currentStoredSettings())
    }

    private func currentStoredSettings() -> StoredRemoteIMSettings {
        StoredRemoteIMSettings(
            sdkAppID: Int(sdkAppIDText.trimmingCharacters(in: .whitespacesAndNewlines)),
            masterUserID: masterUserID.trimmingCharacters(in: .whitespacesAndNewlines),
            friendUserIDs: chatState.contacts
                .filter { $0.relation == .friend }
                .map(\.userID),
            slaveUserIDs: chatState.contacts
                .filter { $0.relation == .slave }
                .map(\.userID)
        )
    }

    private func rebuildChatStateForCurrentMaster() {
        let cleanMasterUserID = masterUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard chatState.ownerUserID != cleanMasterUserID else { return }
        var nextState = MasterChatState(ownerUserID: cleanMasterUserID)
        for contact in chatState.contacts {
            try? nextState.upsertContact(
                userID: contact.userID,
                relation: contact.relation,
                displayName: contact.displayName
            )
        }
        chatState = nextState
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
