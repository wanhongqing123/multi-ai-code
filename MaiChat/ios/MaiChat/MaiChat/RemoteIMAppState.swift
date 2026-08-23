import Foundation
import MaiChatCore

@MainActor
final class RemoteIMDraftState: ObservableObject {
    @Published var text = ""
}

@MainActor
final class RemoteIMAppState: ObservableObject {
    private struct ConversationHistoryState {
        var hasLoadedInitialPage = false
        var isLoading = false
        var hasEarlierMessages = false
        var oldestLoadedCursor: LocalChatHistoryCursor?
    }

    enum ConnectionState: String {
        case disconnected = "未连接"
        case connecting = "连接中"
        case connected = "已连接"
        case failed = "连接失败"

        var diagnosticName: String {
            switch self {
            case .disconnected: return "disconnected"
            case .connecting: return "connecting"
            case .connected: return "connected"
            case .failed: return "failed"
            }
        }
    }

    @Published var sdkAppIDText = ""
    @Published var masterUserID = ""
    @Published var secretKey = ""
    @Published var newContactUserID = ""
    @Published var newContactRelation: RemoteIMContactRelation = .friend
    @Published var errorMessage: String?
    @Published var connectionState: ConnectionState = .disconnected
    @Published var chatState: MasterChatState
    @Published var hasCompletedInitialLogin = false
    @Published var presenceStatusByUserID: [String: RemoteIMPresenceStatus] = [:]
    @Published private(set) var unreadCountByUserID: [String: Int] = [:]
    @Published private(set) var userProfileByUserID: [String: RemoteIMUserProfile] = [:]

    let remoteDesktop: RemoteDesktopSession
    let draft = RemoteIMDraftState()

    private let settingsStore: LocalSettingsStore
    private let secretStore: KeychainSecretStore
    private let historyPersistence: LocalChatHistoryPersistence
    private let client: RemoteIMClient
    private let autoConnectOnLaunch: Bool
    private var didHandleLaunchAutoConnect = false
    private var visibleConversationUserID: String?
    private var conversationHistoryStateByUserID: [String: ConversationHistoryState] = [:]
    private var conversationHistoryLoadGenerationByUserID: [String: UInt64] = [:]
    private var historyAccountGeneration: UInt64 = 0
    private var accountRebuildRequestGeneration: UInt64 = 0
    private var chatHistorySDKAppID: Int?
    private var pendingHistoryMutations: [LocalChatHistoryMutation] = []
    private var historySaveTask: Task<Void, Never>?
    private var pendingIncomingRemoteIDs = Set<String>()
    private let messagePageSize = 50

    init(
        settingsStore: LocalSettingsStore = LocalSettingsStore(),
        secretStore: KeychainSecretStore = KeychainSecretStore(),
        historyStore: LocalChatHistoryStore = LocalChatHistoryStore(),
        client: RemoteIMClient = TencentIMClient()
    ) {
        self.settingsStore = settingsStore
        self.secretStore = secretStore
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
        self.chatHistorySDKAppID = settings.sdkAppID

        let loadedState = MasterChatState(
            ownerUserID: settings.masterUserID,
            contacts: Self.contacts(from: settings),
            messages: []
        )
        self.chatState = loadedState
        self.historyPersistence = historyStore.makePersistence()
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
                await self?.receive(event)
            }
        }
        self.client.onIncomingVoice = { [weak self] event in
            Task { @MainActor in
                await self?.receive(event)
            }
        }
        self.client.onIncomingImage = { [weak self] event in
            Task { @MainActor in
                await self?.receive(event)
            }
        }
        self.client.onIncomingFile = { [weak self] event in
            Task { @MainActor in
                await self?.receive(event)
            }
        }
        self.client.onIncomingVideo = { [weak self] event in
            Task { @MainActor in
                await self?.receive(event)
            }
        }
        self.client.onPresenceStatusChanged = { [weak self] updates in
            Task { @MainActor in
                self?.applyPresenceStatusUpdates(updates)
            }
        }
        Task { [weak self] in
            await self?.loadConversationSummariesForCurrentAccount()
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
        !draft.text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    var canSendVoice: Bool {
        connectionState == .connected && selectedContact != nil
    }

    var canSendImage: Bool {
        connectionState == .connected && selectedContact != nil
    }

    var canSendVideo: Bool {
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

    @discardableResult
    func saveSettings() async -> Bool {
        do {
            applyFixedCredential()
            try secretStore.saveSecretKey(secretKey)
            guard await rebuildChatStateForCurrentAccount() else { return false }
            settingsStore.save(currentStoredSettings())
            errorMessage = nil
            return true
        } catch {
            errorMessage = error.localizedDescription
            let errorValue = error as NSError
            logIM(
                level: .error,
                event: "settings-save-failed",
                fields: [
                    "error_domain": errorValue.domain,
                    "error_code": String(errorValue.code),
                ]
            )
            return false
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
        guard await saveSettings() else { return }
        guard let sdkAppID = Int(sdkAppIDText.trimmingCharacters(in: .whitespacesAndNewlines)),
              sdkAppID > 0
        else {
            logIM(
                level: .warning,
                event: "connect-blocked",
                fields: ["reason": "invalid-sdk-app-id"]
            )
            errorMessage = "IM 应用配置无效"
            return
        }
        let cleanMasterUserID = masterUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        let cleanSecretKey = secretKey.trimmingCharacters(in: .whitespacesAndNewlines)
        let startedAt = ProcessInfo.processInfo.systemUptime

        do {
            connectionState = .connecting
            logIM(
                level: .info,
                event: "connect-start",
                fields: ["sdk_app_id": String(sdkAppID)],
                userID: cleanMasterUserID
            )
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
            logIM(
                level: .info,
                event: "connect-finished",
                fields: [
                    "result": "ok",
                    "duration_ms": elapsedMilliseconds(since: startedAt),
                ],
                userID: cleanMasterUserID
            )
            await refreshProfilesForCurrentUsers()
            await refreshPresenceForCurrentContacts()
            errorMessage = nil
        } catch {
            connectionState = .failed
            let errorValue = error as NSError
            logIM(
                level: .error,
                event: "connect-finished",
                fields: [
                    "result": "failed",
                    "error_domain": errorValue.domain,
                    "error_code": String(errorValue.code),
                    "duration_ms": elapsedMilliseconds(since: startedAt),
                ],
                userID: cleanMasterUserID
            )
            errorMessage = error.localizedDescription
        }
    }

    func connectIfRequestedByLaunchEnvironment() async {
        guard autoConnectOnLaunch, !didHandleLaunchAutoConnect else { return }
        didHandleLaunchAutoConnect = true
        await connect()
    }

    func disconnect() async {
        let startedAt = ProcessInfo.processInfo.systemUptime
        logIM(level: .info, event: "disconnect-start", userID: masterUserID)
        await remoteDesktop.stop(cause: "im-disconnect")
        await client.disconnect()
        presenceStatusByUserID = [:]
        connectionState = .disconnected
        logIM(
            level: .info,
            event: "disconnect-finished",
            fields: ["duration_ms": elapsedMilliseconds(since: startedAt)],
            userID: masterUserID
        )
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
            let errorValue = error as NSError
            logIM(
                level: .error,
                event: "contact-add-failed",
                fields: [
                    "error_domain": errorValue.domain,
                    "error_code": String(errorValue.code),
                ]
            )
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
        invalidateConversationHistoryLoad(for: cleanUserID)
        if newContactUserID.trimmingCharacters(in: .whitespacesAndNewlines) == cleanUserID {
            newContactUserID = ""
        }
        enqueueHistoryMutation(.removeConversation(
            peerUserID: cleanUserID,
            sdkAppID: chatHistorySDKAppID,
            ownerUserID: chatState.ownerUserID
        ))
        presenceStatusByUserID = RemoteIMPresenceStatusPolicy.merged(
            current: presenceStatusByUserID,
            updates: [:],
            contactUserIDs: chatState.contacts.map(\.userID)
        )
        unreadCountByUserID[cleanUserID] = nil
        if visibleConversationUserID == cleanUserID {
            visibleConversationUserID = nil
        }
        userProfileByUserID[cleanUserID] = nil
        settingsStore.save(currentStoredSettings())
        guard await flushHistoryPersistence() else {
            errorMessage = "删除好友记录已生效，但本地消息清理尚未保存，请稍后重试"
            return false
        }
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
            invalidateConversationHistoryLoad(for: cleanUserID)
            unreadCountByUserID[cleanUserID] = nil
            conversationHistoryStateByUserID[cleanUserID] = ConversationHistoryState(
                hasLoadedInitialPage: true,
                isLoading: false,
                hasEarlierMessages: false,
                oldestLoadedCursor: nil
            )
            enqueueHistoryMutation(.removeConversation(
                peerUserID: cleanUserID,
                sdkAppID: chatHistorySDKAppID,
                ownerUserID: chatState.ownerUserID
            ))
            settingsStore.save(currentStoredSettings())
            guard await flushHistoryPersistence() else {
                errorMessage = "聊天记录已清空，但本地清理尚未保存，请稍后重试"
                return false
            }
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
        chatState.messages(with: userID)
    }

    func hasEarlierMessages(with userID: String) -> Bool {
        conversationHistoryStateByUserID[userID]?.hasEarlierMessages ?? false
    }

    func loadInitialMessages(with userID: String) async {
        let cleanUserID = userID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanUserID.isEmpty else { return }
        let account = LocalChatHistoryAccount(
            sdkAppID: chatHistorySDKAppID,
            ownerUserID: chatState.ownerUserID
        )
        let accountGeneration = historyAccountGeneration
        let loadGeneration = conversationHistoryLoadGeneration(for: cleanUserID)
        guard chatState.contacts.contains(where: { $0.userID == cleanUserID }) else { return }
        let current = conversationHistoryStateByUserID[cleanUserID] ?? ConversationHistoryState()
        guard !current.hasLoadedInitialPage, !current.isLoading else { return }
        conversationHistoryStateByUserID[cleanUserID] = ConversationHistoryState(
            hasLoadedInitialPage: false,
            isLoading: true,
            hasEarlierMessages: current.hasEarlierMessages,
            oldestLoadedCursor: current.oldestLoadedCursor
        )
        objectWillChange.send()

        do {
            let page = try await historyPersistence.loadConversationPage(
                sdkAppID: account.sdkAppID,
                ownerUserID: account.ownerUserID,
                peerUserID: cleanUserID,
                before: nil,
                limit: messagePageSize
            )
            guard isCurrentHistoryLoad(
                account: account,
                accountGeneration: accountGeneration,
                peerUserID: cleanUserID,
                loadGeneration: loadGeneration
            )
            else { return }
            chatState.mergeMessages(page.messages)
            conversationHistoryStateByUserID[cleanUserID] = ConversationHistoryState(
                hasLoadedInitialPage: true,
                isLoading: false,
                hasEarlierMessages: page.hasEarlierMessages,
                oldestLoadedCursor: page.messages.first.map {
                    LocalChatHistoryCursor(createdAt: $0.createdAt, messageID: $0.id)
                }
            )
            objectWillChange.send()
        } catch {
            guard isCurrentHistoryLoad(
                account: account,
                accountGeneration: accountGeneration,
                peerUserID: cleanUserID,
                loadGeneration: loadGeneration
            ) else { return }
            conversationHistoryStateByUserID[cleanUserID] = current
            recordHistoryLoadFailure(error, operation: "initial-page")
        }
    }

    func loadEarlierMessages(with userID: String) async {
        let cleanUserID = userID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanUserID.isEmpty else { return }
        let account = LocalChatHistoryAccount(
            sdkAppID: chatHistorySDKAppID,
            ownerUserID: chatState.ownerUserID
        )
        let accountGeneration = historyAccountGeneration
        let loadGeneration = conversationHistoryLoadGeneration(for: cleanUserID)
        if conversationHistoryStateByUserID[cleanUserID]?.hasLoadedInitialPage != true {
            await loadInitialMessages(with: cleanUserID)
            return
        }
        guard var state = conversationHistoryStateByUserID[cleanUserID],
              state.hasEarlierMessages,
              !state.isLoading,
              let oldestLoadedCursor = state.oldestLoadedCursor
        else { return }
        state.isLoading = true
        conversationHistoryStateByUserID[cleanUserID] = state
        objectWillChange.send()

        do {
            let page = try await historyPersistence.loadConversationPage(
                sdkAppID: account.sdkAppID,
                ownerUserID: account.ownerUserID,
                peerUserID: cleanUserID,
                before: oldestLoadedCursor,
                limit: messagePageSize
            )
            guard isCurrentHistoryLoad(
                account: account,
                accountGeneration: accountGeneration,
                peerUserID: cleanUserID,
                loadGeneration: loadGeneration
            )
            else { return }
            chatState.mergeMessages(page.messages)
            state.isLoading = false
            state.hasEarlierMessages = page.hasEarlierMessages
            if let oldestMessage = page.messages.first {
                state.oldestLoadedCursor = LocalChatHistoryCursor(
                    createdAt: oldestMessage.createdAt,
                    messageID: oldestMessage.id
                )
            }
            conversationHistoryStateByUserID[cleanUserID] = state
            objectWillChange.send()
        } catch {
            guard isCurrentHistoryLoad(
                account: account,
                accountGeneration: accountGeneration,
                peerUserID: cleanUserID,
                loadGeneration: loadGeneration
            ) else { return }
            state.isLoading = false
            conversationHistoryStateByUserID[cleanUserID] = state
            recordHistoryLoadFailure(error, operation: "earlier-page")
        }
    }

    func sendDraft() async {
        guard canSend else { return }
        let text = draft.text
        draft.text = ""
        await sendText(text)
    }

    /// 直接发一段文本（语音识别结果走这里）。与 sendDraft 共用同一套排队/落库/回执逻辑，
    /// 避免语音输入这条路径漏掉其中任何一步。
    func sendText(_ text: String) async {
        guard canSendVoice else { return }   // 连接 + 已选联系人；正文非空由调用方保证
        var queuedMessageID: UUID?
        do {
            let message = try chatState.queueOutgoingText(text)
            queuedMessageID = message.id
            let textToSend = message.text
            enqueueHistoryUpsert(message)
            let receipt = try await client.sendText(to: message.toUserID, text: textToSend)
            try chatState.updateMessageDelivery(
                id: message.id,
                remoteID: receipt.remoteID,
                createdAt: receipt.createdAt
            )
            enqueueCurrentMessage(id: message.id)
            errorMessage = nil
        } catch {
            if let queuedMessageID {
                try? chatState.updateMessageStatus(id: queuedMessageID, status: .failed)
                enqueueCurrentMessage(id: queuedMessageID)
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
            enqueueHistoryUpsert(message)
            let receipt = try await client.sendVoice(to: message.toUserID, recording: recording)
            try chatState.updateMessageDelivery(
                id: message.id,
                remoteID: receipt.remoteID,
                createdAt: receipt.createdAt
            )
            enqueueCurrentMessage(id: message.id)
            errorMessage = nil
        } catch {
            if let queuedMessageID {
                try? chatState.updateMessageStatus(id: queuedMessageID, status: .failed)
                enqueueCurrentMessage(id: queuedMessageID)
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
            enqueueHistoryUpsert(message)
            let receipt = try await client.sendImage(to: message.toUserID, image: image)
            try chatState.updateMessageDelivery(
                id: message.id,
                remoteID: receipt.remoteID,
                createdAt: receipt.createdAt
            )
            enqueueCurrentMessage(id: message.id)
            errorMessage = nil
        } catch {
            if let queuedMessageID {
                try? chatState.updateMessageStatus(id: queuedMessageID, status: .failed)
                enqueueCurrentMessage(id: queuedMessageID)
            }
            errorMessage = error.localizedDescription
        }
    }

    func sendVideoFile(_ video: RemoteIMVideoFile) async {
        guard canSendVideo else { return }

        var queuedMessageID: UUID?
        do {
            let message = try chatState.queueOutgoingVideo(
                filePath: video.fileURL.path,
                coverPath: video.coverFileURL.path,
                durationSeconds: video.durationSeconds,
                width: video.width,
                height: video.height,
                sizeBytes: video.sizeBytes
            )
            queuedMessageID = message.id
            enqueueHistoryUpsert(message)
            let receipt = try await client.sendVideo(to: message.toUserID, video: video)
            try chatState.updateMessageDelivery(
                id: message.id,
                remoteID: receipt.remoteID,
                createdAt: receipt.createdAt
            )
            enqueueCurrentMessage(id: message.id)
            errorMessage = nil
        } catch {
            if let queuedMessageID {
                try? chatState.updateMessageStatus(id: queuedMessageID, status: .failed)
                enqueueCurrentMessage(id: queuedMessageID)
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
            enqueueHistoryUpsert(message)
            let receipt = try await client.sendFile(to: message.toUserID, file: file)
            try chatState.updateMessageDelivery(
                id: message.id,
                remoteID: receipt.remoteID,
                createdAt: receipt.createdAt
            )
            enqueueCurrentMessage(id: message.id)
            errorMessage = nil
        } catch {
            if let queuedMessageID {
                try? chatState.updateMessageStatus(id: queuedMessageID, status: .failed)
                enqueueCurrentMessage(id: queuedMessageID)
            }
            errorMessage = error.localizedDescription
        }
    }

    func requestRemoteDesktopView(of contact: RemoteIMContact) async {
        guard connectionState == .connected else {
            logIM(
                level: .warning,
                event: "remote-desktop-request-blocked",
                fields: ["reason": "im-not-connected"],
                userID: contact.userID
            )
            errorMessage = "请先连接 IM"
            return
        }
        guard let sdkAppID = currentSDKAppID(), sdkAppID > 0 else {
            logIM(
                level: .warning,
                event: "remote-desktop-request-blocked",
                fields: ["reason": "invalid-sdk-app-id"],
                userID: contact.userID
            )
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

    func stopRemoteDesktopView(cause: String = "user") async {
        await remoteDesktop.stop(cause: cause)
    }

    private func receive(_ event: IncomingRemoteIMText) async {
        if remoteDesktop.handleIncomingText(from: event.fromUserID, text: event.text) {
            return
        }
        guard await shouldAcceptIncomingMessage(remoteID: event.remoteID) else { return }
        let previousCount = chatState.messages.count
        let message = chatState.receiveText(
            event.text,
            fromUserID: event.fromUserID,
            remoteID: event.remoteID,
            now: event.createdAt
        )
        let wasInserted = chatState.messages.count > previousCount
        logMessageIngested(
            kind: "text",
            userID: event.fromUserID,
            remoteID: event.remoteID,
            wasInserted: wasInserted,
            fields: ["content_bytes": String(event.text.lengthOfBytes(using: .utf8))]
        )
        updateUnreadAfterReceiving(from: event.fromUserID, wasInserted: wasInserted)
        refreshProfileIfNeeded(userID: event.fromUserID)
        if wasInserted {
            enqueueHistoryUpsert(message)
            settingsStore.save(currentStoredSettings())
        }
    }

    private func receive(_ event: IncomingRemoteIMVoice) async {
        guard await shouldAcceptIncomingMessage(remoteID: event.remoteID) else { return }
        let previousCount = chatState.messages.count
        let message = chatState.receiveVoice(
            filePath: event.fileURL.path,
            durationSeconds: event.durationSeconds,
            fromUserID: event.fromUserID,
            remoteID: event.remoteID,
            now: event.createdAt
        )
        let wasInserted = chatState.messages.count > previousCount
        logMessageIngested(
            kind: "voice",
            userID: event.fromUserID,
            remoteID: event.remoteID,
            wasInserted: wasInserted,
            fields: ["duration_seconds": String(event.durationSeconds)]
        )
        updateUnreadAfterReceiving(from: event.fromUserID, wasInserted: wasInserted)
        refreshProfileIfNeeded(userID: event.fromUserID)
        if wasInserted {
            enqueueHistoryUpsert(message)
            settingsStore.save(currentStoredSettings())
        }
    }

    private func receive(_ event: IncomingRemoteIMImage) async {
        guard await shouldAcceptIncomingMessage(remoteID: event.remoteID) else { return }
        let previousCount = chatState.messages.count
        let message = chatState.receiveImage(
            filePath: event.fileURL.path,
            fromUserID: event.fromUserID,
            remoteID: event.remoteID,
            width: event.width,
            height: event.height,
            sizeBytes: event.sizeBytes,
            now: event.createdAt
        )
        let wasInserted = chatState.messages.count > previousCount
        logMessageIngested(
            kind: "image",
            userID: event.fromUserID,
            remoteID: event.remoteID,
            wasInserted: wasInserted,
            fields: [
                "width": event.width.map(String.init) ?? "unknown",
                "height": event.height.map(String.init) ?? "unknown",
                "bytes": event.sizeBytes.map(String.init) ?? "unknown",
            ]
        )
        updateUnreadAfterReceiving(from: event.fromUserID, wasInserted: wasInserted)
        refreshProfileIfNeeded(userID: event.fromUserID)
        if wasInserted {
            enqueueHistoryUpsert(message)
            settingsStore.save(currentStoredSettings())
        }
    }

    private func receive(_ event: IncomingRemoteIMFile) async {
        guard await shouldAcceptIncomingMessage(remoteID: event.remoteID) else { return }
        let previousCount = chatState.messages.count
        let message = chatState.receiveFile(
            filePath: event.fileURL.path,
            fromUserID: event.fromUserID,
            fileName: event.fileName,
            mimeType: event.mimeType,
            remoteID: event.remoteID,
            sizeBytes: event.sizeBytes,
            now: event.createdAt
        )
        let wasInserted = chatState.messages.count > previousCount
        logMessageIngested(
            kind: "file",
            userID: event.fromUserID,
            remoteID: event.remoteID,
            wasInserted: wasInserted,
            fields: [
                "bytes": event.sizeBytes.map(String.init) ?? "unknown",
                "mime_type": event.mimeType,
            ]
        )
        updateUnreadAfterReceiving(from: event.fromUserID, wasInserted: wasInserted)
        refreshProfileIfNeeded(userID: event.fromUserID)
        if wasInserted {
            enqueueHistoryUpsert(message)
            settingsStore.save(currentStoredSettings())
        }
    }

    private func receive(_ event: IncomingRemoteIMVideo) async {
        let existingMessage = chatState.message(remoteID: event.remoteID)
        if existingMessage == nil {
            guard await shouldAcceptIncomingMessage(remoteID: event.remoteID) else { return }
        }

        let previousCount = chatState.messages.count
        let previousAttachment = existingMessage?.videoAttachment
        let message = chatState.receiveVideo(
            filePath: event.videoFileURL.path,
            coverFilePath: event.coverFileURL?.path,
            durationSeconds: event.durationSeconds,
            width: event.width,
            height: event.height,
            sizeBytes: event.sizeBytes,
            fromUserID: event.fromUserID,
            remoteID: event.remoteID,
            now: event.createdAt
        )
        let wasInserted = chatState.messages.count > previousCount
        let wasUpdated = !wasInserted && previousAttachment != message.videoAttachment
        logMessageIngested(
            kind: "video",
            userID: event.fromUserID,
            remoteID: event.remoteID,
            wasInserted: wasInserted,
            fields: [
                "stage": event.stage.rawValue,
                "updated": wasUpdated ? "1" : "0",
                "duration_seconds": String(event.durationSeconds),
                "width": String(event.width),
                "height": String(event.height),
                "bytes": String(event.sizeBytes),
            ]
        )
        updateUnreadAfterReceiving(from: event.fromUserID, wasInserted: wasInserted)
        refreshProfileIfNeeded(userID: event.fromUserID)
        enqueueHistoryUpsert(message)
        if wasInserted {
            settingsStore.save(currentStoredSettings())
        }
        // The attachment paths are stable from the metadata stage onward. A later download
        // makes the file appear at the same path, so explicitly refresh the bubble/player.
        objectWillChange.send()
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

    private func rebuildChatStateForCurrentAccount() async -> Bool {
        accountRebuildRequestGeneration &+= 1
        let requestGeneration = accountRebuildRequestGeneration
        let cleanMasterUserID = masterUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        let nextSDKAppID = currentSDKAppID()
        guard chatState.ownerUserID != cleanMasterUserID || chatHistorySDKAppID != nextSDKAppID else {
            return true
        }
        guard await flushHistoryPersistence() else {
            guard isCurrentAccountRebuildRequest(
                generation: requestGeneration,
                sdkAppID: nextSDKAppID,
                ownerUserID: cleanMasterUserID
            ) else { return false }
            errorMessage = "保存当前账号消息失败，请稍后重试"
            logIM(
                level: .error,
                event: "account-switch-blocked",
                fields: ["reason": "history-flush-failed"]
            )
            return false
        }
        guard isCurrentAccountRebuildRequest(
            generation: requestGeneration,
            sdkAppID: nextSDKAppID,
            ownerUserID: cleanMasterUserID
        ) else { return false }
        let summaries: [RemoteIMMessage]
        do {
            summaries = try await historyPersistence.loadConversationSummaries(
                sdkAppID: nextSDKAppID,
                ownerUserID: cleanMasterUserID,
                peerUserIDs: chatState.contacts.map(\.userID)
            )
        } catch {
            guard isCurrentAccountRebuildRequest(
                generation: requestGeneration,
                sdkAppID: nextSDKAppID,
                ownerUserID: cleanMasterUserID
            ) else { return false }
            recordHistoryLoadFailure(error, operation: "account-summaries")
            return false
        }
        guard isCurrentAccountRebuildRequest(
            generation: requestGeneration,
            sdkAppID: nextSDKAppID,
            ownerUserID: cleanMasterUserID
        ) else { return false }

        let currentPeerUserIDs = Set(chatState.contacts.map(\.userID))
        let currentSummaries = summaries.filter { message in
            currentPeerUserIDs.contains(
                historyPeerUserID(for: message, ownerUserID: cleanMasterUserID)
            )
        }
        presenceStatusByUserID = [:]
        historyAccountGeneration &+= 1
        chatHistorySDKAppID = nextSDKAppID
        conversationHistoryStateByUserID = [:]
        conversationHistoryLoadGenerationByUserID = [:]
        pendingIncomingRemoteIDs = []
        let nextState = MasterChatState(
            ownerUserID: cleanMasterUserID,
            contacts: chatState.contacts,
            messages: currentSummaries,
            selectedPeerID: chatState.selectedPeerID
        )
        chatState = nextState
        Task {
            await refreshPresenceForCurrentContacts()
        }
        return true
    }

    private func isCurrentAccountRebuildRequest(
        generation: UInt64,
        sdkAppID: Int?,
        ownerUserID: String
    ) -> Bool {
        accountRebuildRequestGeneration == generation &&
            currentSDKAppID() == sdkAppID &&
            masterUserID.trimmingCharacters(in: .whitespacesAndNewlines) == ownerUserID
    }

    private func loadConversationSummariesForCurrentAccount() async {
        let account = LocalChatHistoryAccount(
            sdkAppID: chatHistorySDKAppID,
            ownerUserID: chatState.ownerUserID
        )
        let accountGeneration = historyAccountGeneration
        let requestedPeerGenerations = Dictionary(uniqueKeysWithValues: chatState.contacts.map {
            ($0.userID, conversationHistoryLoadGeneration(for: $0.userID))
        })
        guard !account.ownerUserID.isEmpty else { return }
        do {
            let summaries = try await historyPersistence.loadConversationSummaries(
                sdkAppID: account.sdkAppID,
                ownerUserID: account.ownerUserID,
                peerUserIDs: chatState.contacts.map(\.userID)
            )
            guard historyAccountGeneration == accountGeneration,
                  chatHistorySDKAppID == account.sdkAppID,
                  chatState.ownerUserID == account.ownerUserID
            else { return }
            let currentPeerUserIDs = Set(chatState.contacts.map(\.userID))
            let currentSummaries = summaries.filter { message in
                let peerUserID = historyPeerUserID(for: message, ownerUserID: account.ownerUserID)
                return currentPeerUserIDs.contains(peerUserID) &&
                    requestedPeerGenerations[peerUserID] == conversationHistoryLoadGeneration(for: peerUserID)
            }
            chatState.mergeMessages(currentSummaries)
        } catch {
            guard historyAccountGeneration == accountGeneration,
                  chatHistorySDKAppID == account.sdkAppID,
                  chatState.ownerUserID == account.ownerUserID
            else { return }
            recordHistoryLoadFailure(error, operation: "conversation-summaries")
        }
    }

    private func enqueueHistoryUpsert(_ message: RemoteIMMessage) {
        enqueueHistoryMutation(.upsert(
            [message],
            sdkAppID: chatHistorySDKAppID,
            ownerUserID: chatState.ownerUserID
        ))
    }

    private func conversationHistoryLoadGeneration(for userID: String) -> UInt64 {
        conversationHistoryLoadGenerationByUserID[userID] ?? 0
    }

    private func invalidateConversationHistoryLoad(for userID: String) {
        conversationHistoryLoadGenerationByUserID[userID] =
            conversationHistoryLoadGeneration(for: userID) &+ 1
        conversationHistoryStateByUserID[userID] = nil
    }

    private func isCurrentHistoryLoad(
        account: LocalChatHistoryAccount,
        accountGeneration: UInt64,
        peerUserID: String,
        loadGeneration: UInt64
    ) -> Bool {
        historyAccountGeneration == accountGeneration &&
            chatHistorySDKAppID == account.sdkAppID &&
            chatState.ownerUserID == account.ownerUserID &&
            conversationHistoryLoadGeneration(for: peerUserID) == loadGeneration &&
            chatState.contacts.contains(where: { $0.userID == peerUserID })
    }

    private func historyPeerUserID(
        for message: RemoteIMMessage,
        ownerUserID: String
    ) -> String {
        message.fromUserID == ownerUserID ? message.toUserID : message.fromUserID
    }

    private func enqueueCurrentMessage(id: UUID) {
        guard let message = chatState.message(id: id) else { return }
        enqueueHistoryUpsert(message)
    }

    private func enqueueHistoryMutation(_ mutation: LocalChatHistoryMutation) {
        pendingHistoryMutations.append(mutation)
        startHistorySaveTaskIfNeeded()
    }

    private func startHistorySaveTaskIfNeeded() {
        guard !pendingHistoryMutations.isEmpty else { return }
        guard historySaveTask == nil else { return }
        historySaveTask = Task { [weak self] in
            await self?.drainPendingHistoryMutations()
        }
    }

    @discardableResult
    func flushHistoryPersistence() async -> Bool {
        startHistorySaveTaskIfNeeded()
        while let task = historySaveTask {
            await task.value
        }
        return pendingHistoryMutations.isEmpty
    }

    private func drainPendingHistoryMutations() async {
        while !pendingHistoryMutations.isEmpty {
            let mutations = pendingHistoryMutations
            pendingHistoryMutations.removeAll(keepingCapacity: true)
            var saveResult: LocalChatHistorySaveResult?
            var saveError: Error?
            for attempt in 1 ... 3 {
                do {
                    saveResult = try await historyPersistence.persist(mutations: mutations)
                    saveError = nil
                    break
                } catch {
                    saveError = error
                    if attempt < 3 {
                        try? await Task.sleep(for: .milliseconds(50 * attempt))
                    }
                }
            }

            if let result = saveResult {
                if result.durationMilliseconds >= 50 {
                    logIM(
                        level: .warning,
                        event: "history-save-slow",
                        fields: [
                            "duration_ms": String(result.durationMilliseconds),
                            "mutation_count": String(mutations.count),
                            "upserted_count": String(result.upsertedCount),
                            "removed_count": String(result.removedCount),
                        ]
                    )
                }
            } else if let error = saveError {
                pendingHistoryMutations.insert(contentsOf: mutations, at: 0)
                errorMessage = error.localizedDescription
                let errorValue = error as NSError
                logIM(
                    level: .error,
                    event: "history-save-failed",
                    fields: [
                        "error_domain": errorValue.domain,
                        "error_code": String(errorValue.code),
                        "mutation_count": String(mutations.count),
                    ]
                )
                break
            }
        }
        historySaveTask = nil
    }

    private func shouldAcceptIncomingMessage(remoteID: String?) async -> Bool {
        guard let remoteID, !remoteID.isEmpty else { return true }
        guard pendingIncomingRemoteIDs.insert(remoteID).inserted else { return false }
        defer { pendingIncomingRemoteIDs.remove(remoteID) }
        let account = LocalChatHistoryAccount(
            sdkAppID: chatHistorySDKAppID,
            ownerUserID: chatState.ownerUserID
        )
        do {
            let exists = try await historyPersistence.containsMessage(
                remoteID: remoteID,
                sdkAppID: account.sdkAppID,
                ownerUserID: account.ownerUserID
            )
            guard chatHistorySDKAppID == account.sdkAppID,
                  chatState.ownerUserID == account.ownerUserID
            else { return false }
            return !exists
        } catch {
            recordHistoryLoadFailure(error, operation: "remote-id-lookup")
            return true
        }
    }

    private func recordHistoryLoadFailure(_ error: Error, operation: String) {
        let errorValue = error as NSError
        errorMessage = error.localizedDescription
        logIM(
            level: .error,
            event: "history-load-failed",
            fields: [
                "operation": operation,
                "error_domain": errorValue.domain,
                "error_code": String(errorValue.code),
            ]
        )
    }

    private func refreshPresenceForCurrentContacts() async {
        guard connectionState == .connected else { return }
        let contactUserIDs = chatState.contacts.map(\.userID)
        guard !contactUserIDs.isEmpty else {
            presenceStatusByUserID = [:]
            return
        }

        do {
            let startedAt = ProcessInfo.processInfo.systemUptime
            let updates = try await client.refreshPresenceStatuses(userIDs: contactUserIDs)
            applyPresenceStatusUpdates(updates)
            logIM(
                level: .info,
                event: "presence-refresh",
                fields: [
                    "result": "ok",
                    "requested": String(contactUserIDs.count),
                    "received": String(updates.count),
                    "duration_ms": elapsedMilliseconds(since: startedAt),
                ]
            )
        } catch {
            presenceStatusByUserID = RemoteIMPresenceStatusPolicy.merged(
                current: presenceStatusByUserID,
                updates: [:],
                contactUserIDs: contactUserIDs
            )
            let errorValue = error as NSError
            logIM(
                level: .warning,
                event: "presence-refresh",
                fields: [
                    "result": "failed",
                    "requested": String(contactUserIDs.count),
                    "error_domain": errorValue.domain,
                    "error_code": String(errorValue.code),
                ]
            )
        }

        do {
            try await client.subscribePresenceStatuses(userIDs: contactUserIDs)
            logIM(
                level: .info,
                event: "presence-subscribe",
                fields: [
                    "result": "ok",
                    "requested": String(contactUserIDs.count),
                ]
            )
        } catch {
            let errorValue = error as NSError
            logIM(
                level: .warning,
                event: "presence-subscribe",
                fields: [
                    "result": "failed",
                    "requested": String(contactUserIDs.count),
                    "error_domain": errorValue.domain,
                    "error_code": String(errorValue.code),
                ]
            )
        }
    }

    private func refreshProfilesForCurrentUsers() async {
        await refreshProfiles(
            userIDs: [chatState.ownerUserID] + chatState.contacts.map(\.userID)
        )
    }

    private func refreshProfiles(userIDs: [String]) async {
        guard connectionState == .connected else { return }
        let startedAt = ProcessInfo.processInfo.systemUptime
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
            logIM(
                level: .info,
                event: "profile-refresh",
                fields: [
                    "result": "ok",
                    "requested": String(userIDs.count),
                    "received": String(profiles.count),
                    "duration_ms": elapsedMilliseconds(since: startedAt),
                ]
            )
        } catch {
            let errorValue = error as NSError
            logIM(
                level: .warning,
                event: "profile-refresh",
                fields: [
                    "result": "failed",
                    "requested": String(userIDs.count),
                    "error_domain": errorValue.domain,
                    "error_code": String(errorValue.code),
                    "duration_ms": elapsedMilliseconds(since: startedAt),
                ]
            )
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

    private func logMessageIngested(
        kind: String,
        userID: String,
        remoteID: String?,
        wasInserted: Bool,
        fields: [String: String] = [:]
    ) {
        var values = fields
        values["kind"] = kind
        values["inserted"] = wasInserted ? "true" : "false"
        if let remoteID, !remoteID.isEmpty {
            values["message"] = DiagnosticLogPrivacy.stableTag(remoteID, prefix: "m")
        } else {
            values["message"] = "none"
        }
        logIM(
            level: .info,
            event: "message-ingested",
            fields: values,
            userID: userID
        )
    }

    private func logIM(
        level: DiagnosticLogLevel,
        event: String,
        fields: [String: String] = [:],
        userID: String? = nil
    ) {
        var values = fields
        values["connection_state"] = connectionState.diagnosticName
        let resolvedUserID = userID ?? masterUserID
        if !resolvedUserID.isEmpty {
            values["user"] = DiagnosticLogPrivacy.stableTag(resolvedUserID, prefix: "u")
        }
        AppDiagnosticLog.shared.record(
            level: level,
            category: "remote-im",
            event: event,
            fields: values
        )
    }

    private func elapsedMilliseconds(since startedAt: TimeInterval) -> String {
        let elapsed = max(ProcessInfo.processInfo.systemUptime - startedAt, 0)
        return String(Int((elapsed * 1_000).rounded()))
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
