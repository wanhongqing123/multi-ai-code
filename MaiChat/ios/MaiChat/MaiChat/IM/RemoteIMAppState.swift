// 文件作用：维护 IM 登录、会话、消息和连接状态，向界面发布可显示的数据。
// 阅读边界：网络请求由客户端执行，历史消息由本地存储保存。
// 连接账号、IM SDK 和本地历史，统一维护联系人、未读和消息状态。
import Foundation
import MaiChatCore
import UIKit
import UniformTypeIdentifiers

nonisolated func makeRemoteIMImageFile(
    data: Data,
    contentTypes: [UTType],
    stem: String = "remote-im-image-\(UUID().uuidString)"
) throws -> RemoteIMImageFile {
    let contentType = contentTypes.first(where: { $0.conforms(to: .image) })
    let fileExtension = contentType?.preferredFilenameExtension ?? "jpg"
    let fileURL = RemoteIMMediaStorage.fileURL(
        category: .outgoingImages,
        stem: stem,
        pathExtension: fileExtension
    )
    try data.write(to: fileURL, options: .atomic)

    let image = UIImage(data: data)
    let width = image.map { Int($0.size.width * $0.scale) }
    let height = image.map { Int($0.size.height * $0.scale) }
    return RemoteIMImageFile(fileURL: fileURL, width: width,
                             height: height, sizeBytes: data.count)
}

struct RemoteIMBroadcastResult: Equatable {
    let total: Int
    let failedUserIDs: [String]
}

@MainActor
final class RemoteIMAppState: ObservableObject, RemoteDiagnosticsContextProvider {
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
    @Published var newContactUserID = ""
    @Published var newContactRelation: RemoteIMContactRelation = .friend
    @Published var errorMessage: String?
    @Published var agentVideoPresentation: MaiFfplayPresentation?
    @Published var connectionState: ConnectionState = .disconnected
    // 仅本机用户主动发送入队时发布；历史回灌和投递状态变化不触发此标识。
    @Published private(set) var locallyQueuedMessageID: UUID?
    @Published var chatState: MasterChatState
    @Published var hasCompletedInitialLogin = false
    @Published private(set) var hasUnifiedAccount = false
    @Published var presenceStatusByUserID: [String: RemoteIMPresenceStatus] = [:]
    @Published private(set) var unreadCountByUserID: [String: Int] = [:] {
        didSet {
            client.updateApplicationBadgeCount(
                RemoteIMNewMessageNotificationPolicy.systemBadgeCount(
                    totalUnreadCount: totalUnreadCount
                )
            )
        }
    }
    @Published private(set) var userProfileByUserID: [String: RemoteIMUserProfile] = [:]
    @Published private(set) var downloadingVideoKeys = Set<String>()
    @Published private(set) var mediaFileRevision: UInt64 = 0
    @Published private(set) var activityByUserID: [String: RemoteIMActivitySignal] = [:]

    let remoteDesktop: RemoteDesktopSession
    let draft = RemoteIMDraftState()
    lazy var remoteDiagnostics = RemoteDiagnosticsCoordinator(appState: self)

    private let settingsStore: LocalSettingsStore
    private let accountAuth = MaiChatAuthClient.shared
    private let historyPersistence: LocalChatHistoryPersistence
    private let client: RemoteIMClient
    private let shouldConnectOnLaunch: Bool
    private var reconnectOnLaunch: Bool
    private var connectionAttemptGate = RemoteIMConnectionAttemptGate()
    private var didHandleLaunchAutoConnect = false
    private var visibleConversationUserID: String?
    private var conversationHistoryStateByUserID: [String: ConversationHistoryState] = [:]
    // 内存中的完整历史供搜索和引用使用；界面只接收已开放的最近消息页。
    private var visibleMessageLimitByUserID: [String: Int] = [:]
    // 搜索与引用可定位旧消息，但只给界面一个有界的目标窗口。
    private var focusedMessageIDByUserID: [String: UUID] = [:]
    private var conversationHistoryLoadGenerationByUserID: [String: UInt64] = [:]
    private var historyAccountGeneration: UInt64 = 0
    private var accountRebuildRequestGeneration: UInt64 = 0
    private var chatHistorySDKAppID: Int?
    private var pendingHistoryMutations: [LocalChatHistoryMutation] = []
    private var historySaveTask: Task<Void, Never>?
    private var pendingIncomingRemoteIDs = Set<String>()
    private var replyPreparationTask: Task<Void, Never>?
    private var videoMetadataChecks: [String: UInt64] = [:]
    private var videoMetadataSequence: UInt64 = 0
    private var profileRefreshUserIDsInFlight = Set<String>()
    private var profileFetchedForConnection = Set<String>()
    private var profileFetchGeneration = 0
    private var activityExpiryTasks: [String: Task<Void, Never>] = [:]
    private var outgoingTypingTasks: [String: Task<Void, Never>] = [:]
    private var outgoingTypingIDs: [String: String] = [:]
    private var typingSequence: [String: Int] = [:]
    private var typingLastEdit: [String: Date] = [:]
    private var closedActivityIDs: [String] = []
    private var activitySequence: [String: Int] = [:]
    private var transientErrorTask: Task<Void, Never>?
    private let messagePageSize = 20

    init(
        settingsStore: LocalSettingsStore = LocalSettingsStore(),
        historyStore: LocalChatHistoryStore = LocalChatHistoryStore(),
        client: RemoteIMClient = TencentIMClient(),
        loadedSettings: StoredRemoteIMSettings
    ) {
        self.settingsStore = settingsStore
        self.client = client
        self.remoteDesktop = RemoteDesktopSession(client: client)

        var settings = loadedSettings
        let debugRequestedAutoConnect = Self.applyDebugLaunchOverrides(settings: &settings)
        let hasAccountSession = !KeychainSecretStore(
            account: "maichat-account-refresh-token"
        ).readSecretKey().isEmpty
        let hasRestorableAccount = RemoteIMLoginCredentialPolicy.shouldRestoreSavedSession(
            userID: settings.masterUserID
        ) && hasAccountSession
        self.shouldConnectOnLaunch = hasAccountSession && (debugRequestedAutoConnect ||
            RemoteIMLoginCredentialPolicy.shouldAutoConnectSavedSession(
                userID: settings.masterUserID,
                reconnectOnLaunch: settings.reconnectOnLaunch
            ))
        self.reconnectOnLaunch = settings.reconnectOnLaunch
        self.hasCompletedInitialLogin = hasRestorableAccount
        self.hasUnifiedAccount = hasAccountSession
        self.sdkAppIDText = settings.sdkAppID.map(String.init) ?? ""
        self.masterUserID = settings.masterUserID
        self.chatHistorySDKAppID = settings.sdkAppID
        if hasAccountSession {
            // 升级时只把旧 AI 数据库归属给当前已验证的手机号账号。
            try? AIAssistantWorkspaceLocator.claimLegacyWorkspace(for: settings.masterUserID)
        }

        let loadedState = MasterChatState(
            ownerUserID: settings.masterUserID,
            contacts: Self.contacts(from: settings),
            contactGroups: settings.contactGroups,
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
        self.client.updateApplicationBadgeCount(
            RemoteIMNewMessageNotificationPolicy.systemBadgeCount(
                totalUnreadCount: self.totalUnreadCount
            )
        )
        self.client.onIncomingText = { [weak self] event in
            Task { @MainActor in
                await self?.receive(event)
            }
        }
        self.client.onIncomingActivity = { [weak self] event in
            self?.receive(event)
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
        AppDiagnosticLog.shared.setPerformanceAccount(remoteDiagnosticsAccountTag)
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

    func showTransientError(_ message: String, duration: Duration = .seconds(3)) {
        transientErrorTask?.cancel()
        errorMessage = message
        transientErrorTask = Task { @MainActor [weak self] in
            try? await Task.sleep(for: duration)
            guard !Task.isCancelled, let self, self.errorMessage == message else { return }
            self.errorMessage = nil
            self.transientErrorTask = nil
        }
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

    func setSelfAvatar(url: String) async -> Bool {
        let cleanURL = url.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let parsed = URL(string: cleanURL),
              ["https", "http"].contains(parsed.scheme?.lowercased() ?? ""),
              parsed.host != nil else {
            showTransientError("IM 返回的头像地址无效")
            return false
        }
        guard connectionState == .connected else {
            showTransientError("请先连接 IM")
            return false
        }
        let identity = remoteDiagnosticsIdentity
        let userID = masterUserID
        do {
            try await client.setSelfAvatar(url: cleanURL)
            guard remoteDiagnosticsIdentity == identity else { return false }
            let current = profile(for: userID)
            userProfileByUserID[userID] = RemoteIMUserProfile(
                userID: userID, displayName: current.displayName, avatarURL: cleanURL
            )
            return true
        } catch {
            guard remoteDiagnosticsIdentity == identity else { return false }
            showTransientError(error.localizedDescription)
            return false
        }
    }

    func uploadSelfAvatar(fileURL: URL) async -> Bool {
        guard connectionState == .connected else {
            showTransientError("请先连接 IM")
            return false
        }
        let identity = remoteDiagnosticsIdentity
        do {
            let url = try await client.uploadAvatar(fileURL: fileURL)
            guard remoteDiagnosticsIdentity == identity else { return false }
            if let data = try? Data(contentsOf: fileURL) {
                RemoteIMAvatarImageStore.shared.store(data, for: url)
            }
            return await setSelfAvatar(url: url)
        } catch {
            guard remoteDiagnosticsIdentity == identity else { return false }
            showTransientError(error.localizedDescription)
            return false
        }
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

    @discardableResult
    func saveSettings() async -> Bool {
        do {
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

    func requestAccountCode(
        phone: String, purpose: String = "login", ageConfirmed: Bool
    ) async throws -> MaiChatAuthChallenge {
        try await accountAuth.requestCode(
            phone: phone, purpose: purpose, ageConfirmed: ageConfirmed
        )
    }

    func loginWithAccountCode(
        phone: String, challengeId: String, code: String, ageConfirmed: Bool
    ) async throws {
        let session = try await accountAuth.verifyCode(
            phone: phone, challengeId: challengeId, code: code,
            ageConfirmed: ageConfirmed
        )
        try await finishAccountLogin(session)
    }

    func loginWithAccountPassword(
        phone: String, password: String, ageConfirmed: Bool
    ) async throws {
        let session = try await accountAuth.loginWithPassword(
            phone: phone, password: password, ageConfirmed: ageConfirmed
        )
        try await finishAccountLogin(session)
    }

    func setAccountPassword(_ password: String) async throws {
        try await accountAuth.setPassword(password)
    }

    @discardableResult
    func switchPhoneAccount() async -> Bool {
        // 先落盘旧账号消息，再撤销本机登录。这样进程中途结束也不会自动回到旧账号。
        guard await flushHistoryPersistence() else {
            showTransientError("保存当前账号消息失败，请稍后重试")
            return false
        }
        guard settingsStore.saveAccountSnapshot(currentStoredSettings()) else {
            showTransientError("保存当前账号设置失败，请稍后重试")
            return false
        }
        do {
            try AIAssistantWorkspaceLocator.claimLegacyWorkspace(for: masterUserID)
        } catch {
            showTransientError("保存创作中心账号归属失败，请稍后重试")
            return false
        }
        let oldToken = await accountAuth.discardLocalSession()
        AICloudCredentialSync.invalidateTemporaryModelCredentials()
        AIAssistantModel.resetForAccountSwitch()
        await disconnect()
        masterUserID = ""
        sdkAppIDText = ""
        hasUnifiedAccount = false
        hasCompletedInitialLogin = false
        resetAccountPresentation(userID: "", saved: nil)
        _ = settingsStore.saveImmediately(currentStoredSettings())
        errorMessage = nil
        if let oldToken {
            Task { await accountAuth.revokeServerSession(oldToken) }
        }
        return true
    }

    private func finishAccountLogin(_ session: MaiChatAuthSession) async throws {
        let credential = try await accountAuth.credentialIfAvailable(
            expectedUserID: session.user.imUserId
        )
        guard let credential, credential.sdkAppId > 0 else {
            throw MaiChatAuthError.invalidServerResponse
        }
        let previousUserID = masterUserID
        let wasConnected = connectionState == .connected
        if previousUserID != session.user.imUserId {
            AICloudCredentialSync.invalidateTemporaryModelCredentials()
        }
        if wasConnected && previousUserID != session.user.imUserId {
            await disconnect()
        }
        if previousUserID != session.user.imUserId {
            // 只恢复新手机号自己的联系人和未读数，不能沿用前一个 IM 账号的界面状态。
            if !previousUserID.isEmpty {
                _ = settingsStore.saveAccountSnapshot(currentStoredSettings())
            }
            let saved = settingsStore.loadAccountSnapshot(session.user.imUserId)
            resetAccountPresentation(userID: session.user.imUserId, saved: saved)
        }
        masterUserID = session.user.imUserId
        sdkAppIDText = String(credential.sdkAppId)
        hasUnifiedAccount = true
        // 手机号验证已完成；IM 暂时连不上时仍进入账号页，保留重连入口。
        hasCompletedInitialLogin = true
        guard settingsStore.saveImmediately(currentStoredSettings()) else {
            throw NSError(domain: "MaiChatSettings", code: 1, userInfo: [
                NSLocalizedDescriptionKey: "保存登录账号失败，请重试"
            ])
        }
        if wasConnected && previousUserID == session.user.imUserId {
            return
        }
        await requestConnection()
    }

    private func resetAccountPresentation(userID: String, saved: StoredRemoteIMSettings?) {
        accountRebuildRequestGeneration &+= 1
        historyAccountGeneration &+= 1
        profileFetchGeneration += 1
        profileRefreshUserIDsInFlight.removeAll()
        profileFetchedForConnection.removeAll()
        chatHistorySDKAppID = nil
        conversationHistoryStateByUserID = [:]
        visibleMessageLimitByUserID = [:]
        focusedMessageIDByUserID = [:]
        conversationHistoryLoadGenerationByUserID = [:]
        pendingIncomingRemoteIDs = []
        downloadingVideoKeys = []
        presenceStatusByUserID = [:]
        let contacts = saved.map(Self.contacts(from:)) ?? []
        chatState = MasterChatState(
            ownerUserID: userID, contacts: contacts,
            contactGroups: saved?.contactGroups ?? [], messages: []
        )
        unreadCountByUserID = (saved?.unreadCountByUserID ?? [:]).filter { userID, count in
            count > 0 && contacts.contains(where: { $0.userID == userID })
        }
        userProfileByUserID = Dictionary(uniqueKeysWithValues: contacts.map { contact in
            (contact.userID, RemoteIMUserProfile(
                userID: contact.userID, displayName: contact.displayName,
                avatarURL: contact.avatarURL
            ))
        })
    }

    /// 网络连接开始前先记录用户的连接意图。首次登录失败后，即使进程重启也必须可重试；
    /// 不能变成既不显示登录页、又不会自动重连的离线账号。
    func requestConnection() async {
        guard connectionState != .connecting else { return }
        guard hasUnifiedAccount else {
            errorMessage = "请使用手机号登录"
            return
        }
        if let validationError = RemoteIMLoginCredentialPolicy.validationError(userID: masterUserID) {
            errorMessage = validationError
            return
        }
        persistReconnectOnLaunchIntent(
            RemoteIMConnectionIntentPolicy.afterUserRequestedConnection()
        )
        await connect()
    }

    /// 建立当前手机号账号对应的 IM 连接并恢复会话状态。
    /// 鉴权、SDK 登录、联系人和历史同步分阶段进行；失败保留本地可读历史。
    func connect() async {
        guard connectionAttemptGate.begin() else {
            logIM(level: .info, event: "connect-blocked", fields: ["reason": "attempt-in-flight"])
            return
        }
        connectionState = .connecting
        defer { connectionAttemptGate.end() }

        guard await saveSettings() else {
            connectionState = .failed
            return
        }
        guard let sdkAppID = Int(sdkAppIDText.trimmingCharacters(in: .whitespacesAndNewlines)),
              sdkAppID > 0
        else {
            logIM(
                level: .warning,
                event: "connect-blocked",
                fields: ["reason": "invalid-sdk-app-id"]
            )
            errorMessage = "IM 应用配置无效"
            connectionState = .failed
            return
        }
        let cleanMasterUserID = masterUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        let startedAt = ProcessInfo.processInfo.systemUptime

        do {
            logIM(
                level: .info,
                event: "connect-start",
                fields: ["sdk_app_id": String(sdkAppID)],
                userID: cleanMasterUserID
            )
            let identity = remoteDiagnosticsIdentity
            guard let credential = try await accountAuth.credentialIfAvailable(
                expectedUserID: cleanMasterUserID
            ) else { throw MaiChatAuthError.sessionExpired }
            guard credential.sdkAppId == sdkAppID else {
                throw MaiChatAuthError.invalidServerResponse
            }
            guard remoteDiagnosticsIdentity == identity else { return }
            try await client.connect(
                sdkAppID: sdkAppID,
                userID: cleanMasterUserID,
                userSig: credential.userSig
            )
            connectionState = .connected
            profileFetchGeneration += 1
            profileRefreshUserIDsInFlight.removeAll()
            profileFetchedForConnection.removeAll()
            RemoteIMAvatarImageStore.shared.beginConnection()
            persistReconnectOnLaunchIntent(
                RemoteIMConnectionIntentPolicy.afterUserRequestedConnection()
            )
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
            RemoteIMAvatarImageStore.shared.prefetch(
                userProfileByUserID.values.compactMap(\.avatarURL)
                    + chatState.contacts.compactMap(\.avatarURL)
            )
            await refreshPresenceForCurrentContacts()
            errorMessage = nil
        } catch {
            connectionState = .failed
            if case MaiChatAuthError.sessionExpired = error {
                hasUnifiedAccount = false
                hasCompletedInitialLogin = false
            } else if case MaiChatAuthError.identityMismatch = error {
                hasUnifiedAccount = false
                hasCompletedInitialLogin = false
            }
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

    func connectOnLaunchIfNeeded() async {
        guard shouldConnectOnLaunch, !didHandleLaunchAutoConnect else { return }
        didHandleLaunchAutoConnect = true
        await connect()
    }

    func disconnect() async {
        let startedAt = ProcessInfo.processInfo.systemUptime
        // 在异步断开前先保存用户意图；即使 iOS 此刻终止进程，
        // 下次启动也应保持断开，而不是撤销用户刚点击的操作。
        persistReconnectOnLaunchIntent(
            RemoteIMConnectionIntentPolicy.afterUserDisconnected()
        )
        logIM(level: .info, event: "disconnect-start", userID: masterUserID)
        await remoteDesktop.stop(cause: "im-disconnect")
        stopAllActivity()
        await client.disconnect()
        presenceStatusByUserID = [:]
        downloadingVideoKeys = []
        connectionState = .disconnected
        logIM(
            level: .info,
            event: "disconnect-finished",
            fields: ["duration_ms": elapsedMilliseconds(since: startedAt)],
            userID: masterUserID
        )
    }

    func activity(with userID: String) -> RemoteIMActivitySignal? {
        activityByUserID[userID]
    }

    func updateHumanTyping(_ active: Bool, to userID: String) {
        let peer = userID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !peer.isEmpty else { return }
        if active {
            guard connectionState == .connected else { return }
            typingLastEdit[peer] = Date()
            guard outgoingTypingTasks[peer] == nil else { return }
            let activityID = "typing:\(UUID().uuidString.lowercased())"
            outgoingTypingIDs[peer] = activityID
            typingSequence[peer] = 0
            outgoingTypingTasks[peer] = Task { @MainActor [weak self] in
                var nextSend = Date.distantPast
                while !Task.isCancelled {
                    guard let self, self.outgoingTypingIDs[peer] == activityID else { return }
                    if Date().timeIntervalSince(self.typingLastEdit[peer] ?? .distantPast) >= 4 {
                        self.updateHumanTyping(false, to: peer)
                        return
                    }
                    if Date() >= nextSend {
                        self.typingSequence[peer, default: 0] += 1
                        if let signal = RemoteIMActivitySignal(
                            activityID: activityID, kind: .humanTyping, active: true,
                            ttlMilliseconds: 12_000, sequence: self.typingSequence[peer] ?? 1
                        ) {
                            try? await self.client.sendActivity(to: peer, signal: signal)
                        }
                        nextSend = Date().addingTimeInterval(3)
                    }
                    do { try await Task.sleep(nanoseconds: 500_000_000) }
                    catch { return }
                }
            }
            return
        }
        let activityID = outgoingTypingIDs.removeValue(forKey: peer)
        outgoingTypingTasks.removeValue(forKey: peer)?.cancel()
        typingLastEdit.removeValue(forKey: peer)
        let sequence = (typingSequence.removeValue(forKey: peer) ?? 0) + 1
        guard let activityID,
              let signal = RemoteIMActivitySignal(
                activityID: activityID, kind: .humanTyping, active: false,
                ttlMilliseconds: 1_000, sequence: sequence
              ) else { return }
        let account = remoteDiagnosticsIdentity
        Task { [weak self] in
            guard let self, self.remoteDiagnosticsIdentity == account else { return }
            try? await self.client.sendActivity(to: peer, signal: signal)
        }
    }

    /// 接收 SDK 推送的消息并合并进当前账号状态。
    /// 先保存可恢复消息，再通知界面刷新，避免前台切换时出现短暂空页。
    private func receive(_ event: IncomingRemoteIMActivity) {
        guard shouldAcceptIncomingSender(event.fromUserID, kind: "activity") else { return }
        guard chatState.contacts.contains(where: { $0.userID == event.fromUserID }) else { return }
        let userID = event.fromUserID
        let identity = userID + "|" + event.signal.activityID
        guard !closedActivityIDs.contains(identity) else { return }
        guard event.signal.sequence > (activitySequence[identity] ?? -1) else { return }
        activitySequence[identity] = event.signal.sequence
        if activitySequence.count > 512 { activitySequence = [identity: event.signal.sequence] }
        if !event.signal.active {
            rememberClosedActivity(identity)
            if activityByUserID[userID]?.activityID == event.signal.activityID {
                clearActivity(from: userID)
            }
            return
        }
        activityExpiryTasks[userID]?.cancel()
        if let old = activityByUserID[userID], old.activityID != event.signal.activityID {
            rememberClosedActivity(userID + "|" + old.activityID)
        }
        let old = activityByUserID[userID]
        if old?.activityID != event.signal.activityID || old?.kind != event.signal.kind {
            activityByUserID[userID] = event.signal
        }
        let activityID = event.signal.activityID
        let delay = UInt64(event.signal.ttlMilliseconds) * 1_000_000
        activityExpiryTasks[userID] = Task { @MainActor [weak self] in
            do { try await Task.sleep(nanoseconds: delay) } catch { return }
            guard self?.activityByUserID[userID]?.activityID == activityID else { return }
            self?.activityByUserID.removeValue(forKey: userID)
            self?.activityExpiryTasks[userID] = nil
        }
    }

    private func rememberClosedActivity(_ identity: String) {
        if !closedActivityIDs.contains(identity) { closedActivityIDs.append(identity) }
        if closedActivityIDs.count > 256 { closedActivityIDs.removeFirst() }
    }

    private func clearActivity(from userID: String) {
        activityExpiryTasks.removeValue(forKey: userID)?.cancel()
        if let old = activityByUserID[userID] {
            rememberClosedActivity(userID + "|" + old.activityID)
            activityByUserID.removeValue(forKey: userID)
        }
    }

    private func stopAllActivity() {
        for task in activityExpiryTasks.values { task.cancel() }
        for task in outgoingTypingTasks.values { task.cancel() }
        activityExpiryTasks.removeAll()
        outgoingTypingTasks.removeAll()
        outgoingTypingIDs.removeAll()
        typingSequence.removeAll()
        typingLastEdit.removeAll()
        closedActivityIDs.removeAll()
        activitySequence.removeAll()
        activityByUserID.removeAll()
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

    @discardableResult
    func createContactGroup(name: String) -> Bool {
        guard chatState.createContactGroup(name: name) else { return false }
        settingsStore.save(currentStoredSettings())
        return true
    }

    @discardableResult
    func renameContactGroup(from: String, to: String) -> Bool {
        guard chatState.renameContactGroup(from: from, to: to) else { return false }
        settingsStore.save(currentStoredSettings())
        return true
    }

    @discardableResult
    func deleteContactGroup(name: String) -> Bool {
        guard chatState.deleteContactGroup(name: name) else { return false }
        settingsStore.save(currentStoredSettings())
        return true
    }

    @discardableResult
    func setContactGroup(userID: String, groupName: String) -> Bool {
        guard chatState.setContactGroup(userID: userID, groupName: groupName) else { return false }
        settingsStore.save(currentStoredSettings())
        return true
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
            visibleMessageLimitByUserID[cleanUserID] = nil
            focusedMessageIDByUserID[cleanUserID] = nil
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

    func selectContact(_ contact: RemoteIMContact,
                       preservingMessageFocus: Bool = false) {
        // 普通进入恢复最近一页；搜索结果进入详情时保留已经设置的目标窗口。
        if !preservingMessageFocus {
            focusedMessageIDByUserID[contact.userID] = nil
            visibleMessageLimitByUserID[contact.userID] = messagePageSize
        }
        if chatState.selectedPeerID != contact.userID {
            replyPreparationTask?.cancel()
            replyPreparationTask = nil
            draft.selectConversation(ownerUserID: chatState.ownerUserID, peerUserID: contact.userID)
            chatState.selectPeer(userID: contact.userID)
        } else {
            draft.selectConversation(ownerUserID: chatState.ownerUserID, peerUserID: contact.userID)
        }
        if unreadCountByUserID.removeValue(forKey: contact.userID) != nil {
            settingsStore.save(currentStoredSettings())
        }
        clearSystemNotification(for: contact.userID)
    }

    func clearSystemNotification(for userID: String) {
        RemoteIMSystemNotificationCenter.shared.clear(
            peerUserID: userID,
            badgeCount: RemoteIMNewMessageNotificationPolicy.systemBadgeCount(
                totalUnreadCount: totalUnreadCount
            )
        )
    }

    func synchronizeSystemNotificationBadge() {
        let badgeCount = RemoteIMNewMessageNotificationPolicy.systemBadgeCount(
            totalUnreadCount: totalUnreadCount
        )
        client.updateApplicationBadgeCount(badgeCount)
        RemoteIMSystemNotificationCenter.shared.updateBadgeCount(badgeCount)
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

    func isVideoDownloading(remoteID: String?, localPath: String) -> Bool {
        downloadingVideoKeys.contains(
            Self.videoDownloadKey(remoteID: remoteID, localPath: localPath)
        )
    }

    func visibleMessages(with userID: String) -> [RemoteIMMessage] {
        let messages = chatState.messages(with: userID)
        if let focusedID = focusedMessageIDByUserID[userID],
           let index = messages.firstIndex(where: { $0.id == focusedID }) {
            // 命中项及其前面至多九条已经在内存中的消息组成一个连续窗口。
            // 缺失的中间历史不占高度，也不会把数千条后续消息一起排版。
            return Array(messages[max(0, index - 9)...index])
        }
        let limit = visibleMessageLimitByUserID[userID] ?? messagePageSize
        return Array(messages.suffix(limit))
    }

    /// 搜索或引用定位旧消息时建立有界窗口，不展开目标之后的全部历史。
    private func revealMessage(_ id: UUID, with userID: String) {
        let messages = chatState.messages(with: userID)
        guard let index = messages.firstIndex(where: { $0.id == id }) else { return }
        let recentLimit = visibleMessageLimitByUserID[userID] ?? messagePageSize
        focusedMessageIDByUserID[userID] = index >= messages.count - recentLimit ? nil : id
        objectWillChange.send()
    }

    func hasNewerMessages(with userID: String) -> Bool {
        focusedMessageIDByUserID[userID] != nil
    }

    func showLatestMessages(with userID: String) {
        guard focusedMessageIDByUserID[userID] != nil else { return }
        focusedMessageIDByUserID[userID] = nil
        visibleMessageLimitByUserID[userID] = messagePageSize
        objectWillChange.send()
    }

    func beginReply(to message: RemoteIMMessage) {
        cancelReply()
        let identity = remoteDiagnosticsIdentity
        let peer = chatState.selectedPeerID
        replyPreparationTask = Task { [weak self] in
            guard
                let quote = try? await RemoteIMBackgroundWork.parse({ RemoteIMMessageQuotePolicy.quote(for: message) }),
                let self, !Task.isCancelled, self.remoteDiagnosticsIdentity == identity,
                  self.chatState.selectedPeerID == peer else { return }
            self.draft.quote = quote
        }
    }

    func cancelReply() {
        replyPreparationTask?.cancel()
        replyPreparationTask = nil
        draft.quote = nil
    }

    func openQuotedMessage(_ quote: RemoteIMQuote, peerUserID: String) async -> UUID? {
        let remoteID = quote.messageID.trimmingCharacters(in: .whitespacesAndNewlines)
        let peer = peerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !remoteID.isEmpty, !peer.isEmpty else { return nil }
        if let cached = chatState.message(remoteID: remoteID) {
            revealMessage(cached.id, with: peer)
            return cached.id
        }
        _ = await flushHistoryPersistence()
        do {
            guard let stored = try await historyPersistence.message(
                remoteID: remoteID,
                sdkAppID: chatHistorySDKAppID,
                ownerUserID: chatState.ownerUserID,
                peerUserID: peer
            ) else { return nil }
            chatState.mergeMessages([stored])
            revealMessage(stored.id, with: peer)
            return stored.id
        } catch {
            recordHistoryLoadFailure(error, operation: "quoted-message")
            return nil
        }
    }

    func searchMessages(_ query: String, limit: Int = 100) async -> [LocalChatHistorySearchHit] {
        let cleanQuery = query.trimmingCharacters(in: .whitespacesAndNewlines)
        let account = LocalChatHistoryAccount(
            sdkAppID: chatHistorySDKAppID,
            ownerUserID: chatState.ownerUserID
        )
        let accountGeneration = historyAccountGeneration
        guard !cleanQuery.isEmpty, !account.ownerUserID.isEmpty else { return [] }

        // 搜索也要包含刚接收、尚未批量落入 SQLite 的消息。
        _ = await flushHistoryPersistence()
        guard historyAccountGeneration == accountGeneration,
              chatState.ownerUserID == account.ownerUserID,
              chatHistorySDKAppID == account.sdkAppID
        else { return [] }

        do {
            let hits = try await historyPersistence.searchMessages(
                sdkAppID: account.sdkAppID,
                ownerUserID: account.ownerUserID,
                query: cleanQuery,
                limit: limit
            )
            guard historyAccountGeneration == accountGeneration,
                  chatState.ownerUserID == account.ownerUserID,
                  chatHistorySDKAppID == account.sdkAppID
            else { return [] }
            logIM(
                level: .info,
                event: "message-search-finished",
                fields: [
                    "query_characters": String(cleanQuery.count),
                    "results": String(hits.count),
                ]
            )
            return hits
        } catch {
            recordHistoryLoadFailure(error, operation: "message-search")
            return []
        }
    }

    func messagesForHostTool(peerUserID: String, limit: Int) async -> [RemoteIMMessage] {
        let peer = peerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        let safeLimit = min(max(limit, 1), 200)
        let account = LocalChatHistoryAccount(
            sdkAppID: chatHistorySDKAppID,
            ownerUserID: chatState.ownerUserID
        )
        let accountGeneration = historyAccountGeneration
        guard !peer.isEmpty,
              !account.ownerUserID.isEmpty,
              chatState.contacts.contains(where: { $0.userID == peer })
        else { return [] }

        _ = await flushHistoryPersistence()
        guard historyAccountGeneration == accountGeneration,
              chatState.ownerUserID == account.ownerUserID,
              chatHistorySDKAppID == account.sdkAppID
        else { return [] }

        do {
            let page = try await historyPersistence.loadConversationPage(
                sdkAppID: account.sdkAppID,
                ownerUserID: account.ownerUserID,
                peerUserID: peer,
                before: nil,
                limit: safeLimit
            )
            guard historyAccountGeneration == accountGeneration,
                  chatState.ownerUserID == account.ownerUserID,
                  chatHistorySDKAppID == account.sdkAppID
            else { return [] }
            chatState.mergeMessages(page.messages)
            return page.messages
        } catch {
            recordHistoryLoadFailure(error, operation: "host-tool-messages")
            return []
        }
    }

    func messageForHostTool(peerUserID: String, messageID: String) async -> RemoteIMMessage? {
        let peer = peerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        let identifier = messageID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !peer.isEmpty, !identifier.isEmpty else { return nil }
        if let uuid = UUID(uuidString: identifier),
           let cached = chatState.message(id: uuid),
           cached.fromUserID == peer || cached.toUserID == peer {
            return cached
        }
        if let cached = chatState.message(remoteID: identifier),
           cached.fromUserID == peer || cached.toUserID == peer {
            return cached
        }
        let quote = RemoteIMQuote(
            messageID: identifier,
            senderID: "",
            digest: "host tool lookup",
            kind: "text"
        )
        guard let id = await openQuotedMessage(quote, peerUserID: peer) else { return nil }
        return chatState.message(id: id)
    }

    @discardableResult
    func openMessageSearchHit(_ hit: LocalChatHistorySearchHit) -> RemoteIMContact? {
        guard let contact = chatState.contacts.first(where: { $0.userID == hit.peerUserID })
        else { return nil }
        // 搜索覆盖整库；旧命中项可能不在当前 20 条可见页中。
        // 打开前先合并数据库中的权威消息行，让滚动目标确实存在。
        chatState.mergeMessages([hit.message])
        selectContact(contact)
        revealMessage(hit.message.id, with: hit.peerUserID)
        return contact
    }

    func isLoadingInitialMessages(with userID: String) -> Bool {
        guard let state = conversationHistoryStateByUserID[userID] else { return false }
        return state.isLoading && !state.hasLoadedInitialPage
    }

    func hasEarlierMessages(with userID: String) -> Bool {
        if focusedMessageIDByUserID[userID] != nil { return false }
        let cachedCount = chatState.messages(with: userID).count
        let visibleLimit = visibleMessageLimitByUserID[userID] ?? messagePageSize
        return cachedCount > visibleLimit ||
            (conversationHistoryStateByUserID[userID]?.hasEarlierMessages ?? false)
    }

    /// 为指定联系人读取首批已持久化和远端消息。
    /// 首屏完成前保留原会话内容，读取成功后按稳定标识合并。
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
            let historyReadStart = ProcessInfo.processInfo.systemUptime
            let page = try await historyPersistence.loadConversationPage(
                sdkAppID: account.sdkAppID,
                ownerUserID: account.ownerUserID,
                peerUserID: cleanUserID,
                before: nil,
                limit: messagePageSize
            )
            let historyReadDuration = elapsedMilliseconds(since: historyReadStart)
            try await MarkdownPreparation.prepare(page.messages.map(\.text))
            guard isCurrentHistoryLoad(
                account: account,
                accountGeneration: accountGeneration,
                peerUserID: cleanUserID,
                loadGeneration: loadGeneration
            )
            else { return }
            logIM(level: .info, event: "history-load-completed", fields: [
                "peer": DiagnosticLogPrivacy.stableTag(cleanUserID, prefix: "u"),
                "duration_ms": historyReadDuration,
                "message_count": String(page.messages.count), "operation": "page-await"
            ])
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
            objectWillChange.send()
            if error is CancellationError { return }
            recordHistoryLoadFailure(error, operation: "initial-page")
        }
    }

    /// 按游标加载当前会话更早的一页消息。
    /// 未取回的页不占用可滚动高度，加载完成后维持用户原有阅读锚点。
    func loadEarlierMessages(with userID: String) async {
        let cleanUserID = userID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanUserID.isEmpty else { return }
        let cachedCount = chatState.messages(with: cleanUserID).count
        let visibleLimit = visibleMessageLimitByUserID[cleanUserID] ?? messagePageSize
        if cachedCount > visibleLimit {
            // 先开放已在内存中的上一页；无需再次请求数据库或预留空白高度。
            visibleMessageLimitByUserID[cleanUserID] = min(cachedCount,
                visibleLimit + messagePageSize)
            objectWillChange.send()
            return
        }
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
            let historyReadStart = ProcessInfo.processInfo.systemUptime
            let page = try await historyPersistence.loadConversationPage(
                sdkAppID: account.sdkAppID,
                ownerUserID: account.ownerUserID,
                peerUserID: cleanUserID,
                before: oldestLoadedCursor,
                limit: messagePageSize
            )
            let historyReadDuration = elapsedMilliseconds(since: historyReadStart)
            try await MarkdownPreparation.prepare(page.messages.map(\.text))
            guard isCurrentHistoryLoad(
                account: account,
                accountGeneration: accountGeneration,
                peerUserID: cleanUserID,
                loadGeneration: loadGeneration
            )
            else { return }
            logIM(level: .info, event: "history-load-completed", fields: [
                "peer": DiagnosticLogPrivacy.stableTag(cleanUserID, prefix: "u"),
                "duration_ms": historyReadDuration,
                "message_count": String(page.messages.count), "operation": "page-await"
            ])
            chatState.mergeMessages(page.messages)
            visibleMessageLimitByUserID[cleanUserID] = visibleLimit + page.messages.count
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
            if error is CancellationError { return }
            recordHistoryLoadFailure(error, operation: "earlier-page")
        }
    }

    func sendDraft(to recipient: String? = nil) async {
        guard canSend else { return }
        let text = draft.text
        let quote = draft.quote
        draft.text = ""
        cancelReply()
        await sendText(text, quote: quote, to: recipient)
    }

    var remoteDiagnosticsIdentity: String {
        """
        \(chatState.ownerUserID)|\(historyAccountGeneration)|\(accountRebuildRequestGeneration)\
        |\(chatHistorySDKAppID ?? 0)
        """
    }

    func remoteDiagnosticsLogs(peer: String, since: Date) -> [RemoteDiagnosticsLogEntry] {
        AppDiagnosticLog.shared.remoteMetadata(peerUserID: peer, accountTag: remoteDiagnosticsAccountTag, since: since)
    }

    var remoteDiagnosticsAccountTag: String {
        guard let sdkAppID = chatHistorySDKAppID else { return "" }
        return RemoteDiagnosticsProtocol.accountTag(sdkAppID: sdkAppID, ownerUserID: chatState.ownerUserID)
    }

    func remoteDiagnosticsMayContinue(identity: String, peer: String, recipient: String) -> Bool {
        connectionState == .connected && remoteDiagnosticsIdentity == identity &&
            chatState.contacts.contains(where: { $0.userID == peer }) &&
            chatState.contacts.contains(where: { $0.userID == recipient }) &&
            recipient != chatState.ownerUserID
    }

    func sendRemoteDiagnosticsText(_ text: String, to peerID: String, identity: String) async -> Bool {
        guard remoteDiagnosticsMayContinue(identity: identity, peer: peerID, recipient: peerID) else { return false }
        var messageID: UUID?
        do {
            let message = try chatState.queueOutgoingText(to: peerID, text: text)
            messageID = message.id
            enqueueHistoryUpsert(message)
            let receipt = try await client.sendText(to: peerID, text: text, origin: .human, quote: nil)
            guard remoteDiagnosticsIdentity == identity else { return false }
            try chatState.updateMessageDelivery(
                id: message.id, remoteID: receipt.remoteID, createdAt: receipt.createdAt)
            enqueueCurrentMessage(id: message.id)
            return true
        } catch {
            if remoteDiagnosticsIdentity == identity, let messageID {
                try? chatState.updateMessageStatus(id: messageID, status: .failed)
                enqueueCurrentMessage(id: messageID)
            }
            return false
        }
    }

    func sendRemoteDiagnosticsReport(_ file: URL, to recipient: RemoteIMContact, identity: String) async -> Bool {
        guard remoteDiagnosticsMayContinue(identity: identity, peer: recipient.userID, recipient: recipient.userID)
        else { return false }
        let source = RemoteIMMessage(fromUserID: chatState.ownerUserID, toUserID: recipient.userID,
            text: "请协助分析这份远程排障报告。",
            fileAttachment: RemoteIMFileAttachment(
                localFilePath: file.path, fileName: file.lastPathComponent, mimeType: "text/markdown"),
            direction: .outgoing, status: .pending, createdAt: Date())
        var messageID: UUID?
        do {
            // 明确锁定目标会话，不再读取随后可能切换的聊天选中项。
            let queued = try chatState.queueForwardedMessage(source, to: recipient.userID)
            messageID = queued.id
            enqueueHistoryUpsert(queued)
            let receipt = try await client.sendFile(to: recipient.userID,
                file: RemoteIMFile(
                    fileURL: file, fileName: file.lastPathComponent, mimeType: "text/markdown", sizeBytes: nil))
            guard remoteDiagnosticsIdentity == identity else { return false }
            try chatState.updateMessageDelivery(id: queued.id, remoteID: receipt.remoteID, createdAt: receipt.createdAt)
            enqueueCurrentMessage(id: queued.id)
            return true
        } catch {
            if remoteDiagnosticsIdentity == identity, let messageID {
                try? chatState.updateMessageStatus(id: messageID, status: .failed)
                enqueueCurrentMessage(id: messageID)
            }
            return false
        }
    }

    /// 直接发一段文本（语音识别结果走这里）。与 sendDraft 共用同一套排队/落库/回执逻辑，
    /// 避免语音输入这条路径漏掉其中任何一步。
    @discardableResult
    func sendText(_ text: String, quote: RemoteIMQuote? = nil, to recipient: String? = nil) async -> Bool {
        await sendQueuedText(text, quote: quote, to: recipient) { [client] userID, queuedText in
            try await client.sendText(
                to: userID,
                text: queuedText,
                origin: .human,
                quote: quote
            )
        }
    }

    func broadcastText(to rawUserIDs: [String], text: String) async -> RemoteIMBroadcastResult {
        let recipients = RemoteIMBroadcastSelectionPolicy.uniqueRecipientIDs(rawUserIDs)
        let cleanText = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !recipients.isEmpty, !cleanText.isEmpty, connectionState == .connected else {
            return RemoteIMBroadcastResult(total: 0, failedUserIDs: recipients)
        }

        // 先把每一条独立私聊消息全部写进模型和持久化队列；当前打开的会话不参与寻址。
        var queued: [(userID: String, messageID: UUID)] = []
        do {
            for userID in recipients {
                let message = try chatState.queueOutgoingText(to: userID, text: cleanText)
                enqueueHistoryUpsert(message)
                queued.append((userID, message.id))
            }
        } catch {
            errorMessage = error.localizedDescription
            return RemoteIMBroadcastResult(total: 0, failedUserIDs: recipients)
        }

        var delivery = RemoteIMBroadcastDeliveryTracker(total: queued.count)
        for item in queued {
            do {
                let receipt = try await client.sendText(to: item.userID, text: cleanText)
                try chatState.updateMessageDelivery(
                    id: item.messageID,
                    remoteID: receipt.remoteID,
                    createdAt: receipt.createdAt
                )
            } catch {
                try? chatState.updateMessageStatus(id: item.messageID, status: .failed)
                delivery.record(userID: item.userID, succeeded: false)
            }
            enqueueCurrentMessage(id: item.messageID)
        }
        // 群发失败统一由调用方最后报一次，不逐条覆盖全局 error toast。
        errorMessage = nil
        return RemoteIMBroadcastResult(total: delivery.total, failedUserIDs: delivery.failedUserIDs)
    }

    @discardableResult
    func forwardMessage(_ source: RemoteIMMessage, to contact: RemoteIMContact) async -> Bool {
        let targetUserID = contact.userID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard connectionState == .connected,
              !targetUserID.isEmpty,
              targetUserID != chatState.ownerUserID
        else {
            errorMessage = "当前无法转发消息"
            return false
        }
        let identity = remoteDiagnosticsIdentity
        let attachmentReady =
            (try? await RemoteIMBackgroundWork.metadata { Self.forwardAttachmentIsReady(source) }) ?? false
        guard identity == remoteDiagnosticsIdentity, !Task.isCancelled else { return false }
        guard attachmentReady else {
            errorMessage = "附件尚未下载完成，暂时无法转发"
            return false
        }

        var queuedMessageID: UUID?
        do {
            // 转发是新的出站消息，不继承原消息的 remoteID、引用或审批语义。
            let queued = try chatState.queueForwardedMessage(source, to: targetUserID)
            queuedMessageID = queued.id
            locallyQueuedMessageID = queued.id
            enqueueHistoryUpsert(queued)
            let receipt = try await deliverForwardedMessage(source, to: targetUserID)
            try chatState.updateMessageDelivery(
                id: queued.id,
                remoteID: receipt.remoteID,
                createdAt: receipt.createdAt
            )
            enqueueCurrentMessage(id: queued.id)
            errorMessage = nil
            return true
        } catch {
            if let queuedMessageID {
                try? chatState.updateMessageStatus(id: queuedMessageID, status: .failed)
                enqueueCurrentMessage(id: queuedMessageID)
            }
            errorMessage = error.localizedDescription
            return false
        }
    }

    nonisolated private static func forwardAttachmentIsReady(_ message: RemoteIMMessage) -> Bool {
        let manager = FileManager.default
        if let attachment = message.imageAttachment {
            return manager.fileExists(atPath: attachment.localFilePath)
        }
        if let attachment = message.fileAttachment {
            return manager.fileExists(atPath: attachment.localFilePath)
        }
        if let attachment = message.voiceAttachment {
            return manager.fileExists(atPath: attachment.localFilePath)
        }
        if let attachment = message.videoAttachment {
            guard manager.fileExists(atPath: attachment.localPath),
                  let coverPath = attachment.coverPath,
                  manager.fileExists(atPath: coverPath)
            else { return false }
        }
        return !message.text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    /// 按媒体类型把转发内容交付给目标联系人。
    /// 发送失败应保留来源与目标信息供重试，不把未确认发送当作成功。
    private func deliverForwardedMessage(
        _ message: RemoteIMMessage,
        to targetUserID: String
    ) async throws -> RemoteIMSendReceipt {
        if let attachment = message.imageAttachment {
            return try await client.sendImage(
                to: targetUserID,
                image: RemoteIMImageFile(
                    fileURL: URL(fileURLWithPath: attachment.localFilePath),
                    width: attachment.width,
                    height: attachment.height,
                    sizeBytes: attachment.sizeBytes
                )
            )
        }
        if let attachment = message.fileAttachment {
            return try await client.sendFile(
                to: targetUserID,
                file: RemoteIMFile(
                    fileURL: URL(fileURLWithPath: attachment.localFilePath),
                    fileName: attachment.fileName,
                    mimeType: attachment.mimeType,
                    sizeBytes: attachment.sizeBytes
                )
            )
        }
        if let attachment = message.voiceAttachment {
            return try await client.sendVoice(
                to: targetUserID,
                recording: RemoteIMVoiceRecording(
                    fileURL: URL(fileURLWithPath: attachment.localFilePath),
                    durationSeconds: attachment.durationSeconds
                )
            )
        }
        if let attachment = message.videoAttachment,
           let coverPath = attachment.coverPath
        {
            return try await client.sendVideo(
                to: targetUserID,
                video: RemoteIMVideoFile(
                    fileURL: URL(fileURLWithPath: attachment.localPath),
                    coverFileURL: URL(fileURLWithPath: coverPath),
                    fileType: URL(fileURLWithPath: attachment.localPath).pathExtension,
                    durationSeconds: attachment.durationSeconds,
                    width: attachment.width,
                    height: attachment.height,
                    sizeBytes: attachment.sizeBytes
                )
            )
        }
        return try await client.sendText(
            to: targetUserID,
            text: message.text.trimmingCharacters(in: .whitespacesAndNewlines)
        )
    }

    @discardableResult
    func sendApprovalDecision(
        _ action: RemoteIMApprovalAction,
        for request: RemoteIMApprovalRequest
    ) async -> Bool {
        guard request.allows(action) else {
            errorMessage = "该审批请求不允许此操作"
            return false
        }
        let decision = RemoteIMApprovalDecision(token: request.token, action: action)
        return await sendQueuedText(
            action.decisionDisplayText,
            approvalDecision: decision
        ) { [client] userID, _ in
            try await client.sendApprovalDecision(
                to: userID,
                token: request.token,
                action: action
            )
        }
    }

    private func sendQueuedText(
        _ text: String,
        approvalDecision: RemoteIMApprovalDecision? = nil,
        quote: RemoteIMQuote? = nil,
        to recipient: String? = nil,
        deliver: (String, String) async throws -> RemoteIMSendReceipt
    ) async -> Bool {
        guard let peerID = outgoingRecipient(recipient) else { return false }   // 连接 + 已选联系人；正文非空由调用方保证
        let identity = remoteDiagnosticsIdentity
        var queuedMessageID: UUID?
        do {
            let message = if let approvalDecision {
                try chatState.queueOutgoingApprovalDecision(
                    token: approvalDecision.token,
                    action: approvalDecision.action
                )
            } else {
                try chatState.queueOutgoingText(to: peerID, text: text, quote: quote)
            }
            queuedMessageID = message.id
            enqueueHistoryUpsert(message)
            try await MarkdownPreparation.prepare([message.text])
            guard remoteDiagnosticsIdentity == identity else { return false }
            locallyQueuedMessageID = message.id
            let receipt = try await deliver(message.toUserID, message.text)
            guard remoteDiagnosticsIdentity == identity else { return false }
            try chatState.updateMessageDelivery(
                id: message.id,
                remoteID: receipt.remoteID,
                createdAt: receipt.createdAt
            )
            enqueueCurrentMessage(id: message.id)
            errorMessage = nil
            return true
        } catch {
            guard remoteDiagnosticsIdentity == identity else { return false }
            if let queuedMessageID {
                try? chatState.updateMessageStatus(id: queuedMessageID, status: .failed)
                enqueueCurrentMessage(id: queuedMessageID)
            }
            errorMessage = error.localizedDescription
            return false
        }
    }

    private func outgoingRecipient(_ explicit: String?) -> String? {
        guard connectionState == .connected,
              let id = (explicit ?? selectedContact?.userID)?.trimmingCharacters(in: .whitespacesAndNewlines),
              chatState.contacts.contains(where: { $0.userID == id }) else { return nil }
        return id
    }

    @discardableResult
    func sendVoiceRecording(_ recording: RemoteIMVoiceRecording, to recipient: String? = nil) async -> Bool {
        guard let peerID = outgoingRecipient(recipient) else { return false }

        let identity = remoteDiagnosticsIdentity
        var queuedMessageID: UUID?
        do {
            let message = try chatState.queueOutgoingVoice(
                filePath: recording.fileURL.path,
                durationSeconds: recording.durationSeconds,
                to: peerID
            )
            queuedMessageID = message.id
            locallyQueuedMessageID = message.id
            enqueueHistoryUpsert(message)
            let receipt = try await client.sendVoice(to: message.toUserID, recording: recording)
            guard remoteDiagnosticsIdentity == identity else { return false }
            try chatState.updateMessageDelivery(
                id: message.id,
                remoteID: receipt.remoteID,
                createdAt: receipt.createdAt
            )
            enqueueCurrentMessage(id: message.id)
            errorMessage = nil
            return true
        } catch {
            guard remoteDiagnosticsIdentity == identity else { return false }
            if let queuedMessageID {
                try? chatState.updateMessageStatus(id: queuedMessageID, status: .failed)
                enqueueCurrentMessage(id: queuedMessageID)
            }
            errorMessage = error.localizedDescription
            return false
        }
    }

    @discardableResult
    func sendImageFile(_ image: RemoteIMImageFile, to recipient: String? = nil) async -> Bool {
        guard let peerID = outgoingRecipient(recipient) else { return false }

        let identity = remoteDiagnosticsIdentity
        var queuedMessageID: UUID?
        do {
            let message = try chatState.queueOutgoingImage(
                filePath: image.fileURL.path,
                width: image.width,
                height: image.height,
                sizeBytes: image.sizeBytes,
                to: peerID
            )
            queuedMessageID = message.id
            locallyQueuedMessageID = message.id
            enqueueHistoryUpsert(message)
            let receipt = try await client.sendImage(to: message.toUserID, image: image)
            guard remoteDiagnosticsIdentity == identity else { return false }
            try chatState.updateMessageDelivery(
                id: message.id,
                remoteID: receipt.remoteID,
                createdAt: receipt.createdAt
            )
            enqueueCurrentMessage(id: message.id)
            errorMessage = nil
            return true
        } catch {
            guard remoteDiagnosticsIdentity == identity else { return false }
            if let queuedMessageID {
                try? chatState.updateMessageStatus(id: queuedMessageID, status: .failed)
                enqueueCurrentMessage(id: queuedMessageID)
            }
            errorMessage = error.localizedDescription
            return false
        }
    }

    @discardableResult
    func sendVideoFile(_ video: RemoteIMVideoFile, to recipient: String? = nil) async -> Bool {
        guard let peerID = outgoingRecipient(recipient) else { return false }

        let identity = remoteDiagnosticsIdentity
        var queuedMessageID: UUID?
        do {
            let message = try chatState.queueOutgoingVideo(
                filePath: video.fileURL.path,
                coverPath: video.coverFileURL.path,
                durationSeconds: video.durationSeconds,
                width: video.width,
                height: video.height,
                sizeBytes: video.sizeBytes,
                to: peerID
            )
            queuedMessageID = message.id
            locallyQueuedMessageID = message.id
            enqueueHistoryUpsert(message)
            let receipt = try await client.sendVideo(to: message.toUserID, video: video)
            guard remoteDiagnosticsIdentity == identity else { return false }
            try chatState.updateMessageDelivery(
                id: message.id,
                remoteID: receipt.remoteID,
                createdAt: receipt.createdAt
            )
            enqueueCurrentMessage(id: message.id)
            errorMessage = nil
            return true
        } catch {
            guard remoteDiagnosticsIdentity == identity else { return false }
            if let queuedMessageID {
                try? chatState.updateMessageStatus(id: queuedMessageID, status: .failed)
                enqueueCurrentMessage(id: queuedMessageID)
            }
            errorMessage = error.localizedDescription
            return false
        }
    }

    @discardableResult
    func sendFile(_ file: RemoteIMFile, to recipient: String? = nil) async -> Bool {
        guard let peerID = outgoingRecipient(recipient) else { return false }

        let identity = remoteDiagnosticsIdentity
        var queuedMessageID: UUID?
        do {
            let message = try chatState.queueOutgoingFile(
                filePath: file.fileURL.path,
                fileName: file.fileName,
                mimeType: file.mimeType,
                sizeBytes: file.sizeBytes,
                to: peerID
            )
            queuedMessageID = message.id
            locallyQueuedMessageID = message.id
            enqueueHistoryUpsert(message)
            let receipt = try await client.sendFile(to: message.toUserID, file: file)
            guard remoteDiagnosticsIdentity == identity else { return false }
            try chatState.updateMessageDelivery(
                id: message.id,
                remoteID: receipt.remoteID,
                createdAt: receipt.createdAt
            )
            enqueueCurrentMessage(id: message.id)
            errorMessage = nil
            return true
        } catch {
            guard remoteDiagnosticsIdentity == identity else { return false }
            if let queuedMessageID {
                try? chatState.updateMessageStatus(id: queuedMessageID, status: .failed)
                enqueueCurrentMessage(id: queuedMessageID)
            }
            errorMessage = error.localizedDescription
            return false
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
            let identity = remoteDiagnosticsIdentity
            guard let credential = try await accountAuth.credentialIfAvailable(
                expectedUserID: localUserID
            ) else { throw MaiChatAuthError.sessionExpired }
            guard credential.sdkAppId == sdkAppID else {
                throw MaiChatAuthError.invalidServerResponse
            }
            guard remoteDiagnosticsIdentity == identity else { return }
            await remoteDesktop.requestView(
                peerUserID: contact.userID,
                sdkAppID: sdkAppID,
                localUserID: localUserID,
                userSig: credential.userSig
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
        guard shouldAcceptIncomingSender(event.fromUserID, kind: "text") else { return }
        let receivingIdentity = remoteDiagnosticsIdentity
        if await remoteDesktop.handleIncomingText(from: event.fromUserID, text: event.text,
            shouldApply: { [weak self] in self?.remoteDiagnosticsIdentity == receivingIdentity }) {
            return
        }
        guard remoteDiagnosticsIdentity == receivingIdentity,
              await shouldAcceptIncomingMessage(remoteID: event.remoteID) else { return }
        guard remoteDiagnosticsIdentity == receivingIdentity else { return }
        let identity = receivingIdentity
        do { try await MarkdownPreparation.prepare([event.text]) }
        catch { return }
        guard remoteDiagnosticsIdentity == identity else { return }
        let previousCount = chatState.messages.count
        let message = chatState.receiveText(
            event.text,
            fromUserID: event.fromUserID,
            remoteID: event.remoteID,
            approvalRequest: event.approvalRequest,
            approvalDecision: event.approvalDecision,
            quote: event.quote,
            now: event.createdAt
        )
        let wasInserted = chatState.messages.count > previousCount
        logMessageIngested(
            kind: "text",
            userID: event.fromUserID,
            remoteID: event.remoteID,
            wasInserted: wasInserted,
            fields: [
                "content_bytes": String(event.text.lengthOfBytes(using: .utf8)),
                "interaction": event.approvalRequest != nil
                    ? "approval-request"
                    : event.approvalDecision != nil ? "approval-resolved" : "none",
            ]
        )
        updateUnreadAfterReceiving(from: event.fromUserID, wasInserted: wasInserted)
        await postSystemNotificationIfNeeded(
            message: message,
            from: event.fromUserID,
            wasInserted: wasInserted
        )
        refreshProfileIfNeeded(userID: event.fromUserID)
        if wasInserted {
            enqueueHistoryUpsert(message)
            settingsStore.save(currentStoredSettings())
        }
    }

    private func receive(_ event: IncomingRemoteIMVoice) async {
        guard shouldAcceptIncomingSender(event.fromUserID, kind: "voice") else { return }
        guard await shouldAcceptIncomingMessage(remoteID: event.remoteID) else { return }
        let previousCount = chatState.messages.count
        let message = chatState.receiveVoice(
            filePath: event.fileURL.path,
            durationSeconds: event.durationSeconds,
            fromUserID: event.fromUserID,
            remoteID: event.remoteID,
            quote: event.quote,
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
        await postSystemNotificationIfNeeded(
            message: message,
            from: event.fromUserID,
            wasInserted: wasInserted
        )
        refreshProfileIfNeeded(userID: event.fromUserID)
        if wasInserted {
            enqueueHistoryUpsert(message)
            settingsStore.save(currentStoredSettings())
        }
    }

    private func receive(_ event: IncomingRemoteIMImage) async {
        mediaFileRevision &+= 1
        guard shouldAcceptIncomingSender(event.fromUserID, kind: "image") else { return }
        guard await shouldAcceptIncomingMessage(remoteID: event.remoteID) else { return }
        let previousCount = chatState.messages.count
        let message = chatState.receiveImage(
            filePath: event.fileURL.path,
            fromUserID: event.fromUserID,
            remoteID: event.remoteID,
            width: event.width,
            height: event.height,
            sizeBytes: event.sizeBytes,
            caption: event.caption,
            captionAbove: event.captionAbove,
            quote: event.quote,
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
        await postSystemNotificationIfNeeded(
            message: message,
            from: event.fromUserID,
            wasInserted: wasInserted
        )
        refreshProfileIfNeeded(userID: event.fromUserID)
        if wasInserted {
            enqueueHistoryUpsert(message)
            settingsStore.save(currentStoredSettings())
        }
    }

    private func receive(_ event: IncomingRemoteIMFile) async {
        mediaFileRevision &+= 1
        guard !remoteDiagnosticsAccountTag.isEmpty, event.accountTag == remoteDiagnosticsAccountTag else { return }
        guard shouldAcceptIncomingSender(event.fromUserID, kind: "file") else { return }
        guard await shouldAcceptIncomingMessage(remoteID: event.remoteID) else { return }
        // 历史查询会挂起；等待期间若账号切换，旧账号下载结果不能进入新账号存储。
        guard event.accountTag == remoteDiagnosticsAccountTag else { return }
        let previousCount = chatState.messages.count
        let message = chatState.receiveFile(
            filePath: event.fileURL.path,
            fromUserID: event.fromUserID,
            fileName: event.fileName,
            mimeType: event.mimeType,
            remoteID: event.remoteID,
            sizeBytes: event.sizeBytes,
            caption: event.caption,
            captionAbove: event.captionAbove,
            quote: event.quote,
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
        await postSystemNotificationIfNeeded(
            message: message,
            from: event.fromUserID,
            wasInserted: wasInserted
        )
        refreshProfileIfNeeded(userID: event.fromUserID)
        if wasInserted {
            enqueueHistoryUpsert(message)
            settingsStore.save(currentStoredSettings())
        }
    }

    /// 接收视频消息并与同一远端消息标识的旧记录合并。
    /// 先校验发送者和去重状态，再更新附件、保存历史和触发媒体视图刷新；
    /// 下载或封面尚未完成时保留消息行，避免整个聊天页重新布局。
    private func receive(_ event: IncomingRemoteIMVideo) async {
        mediaFileRevision &+= 1
        guard shouldAcceptIncomingSender(event.fromUserID, kind: "video") else { return }
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
            caption: event.caption,
            captionAbove: event.captionAbove,
            quote: event.quote,
            now: event.createdAt
        )
        let wasInserted = chatState.messages.count > previousCount
        let wasUpdated = !wasInserted && previousAttachment != message.videoAttachment
        updateVideoDownloadState(for: event)
        logMessageIngested(
            kind: "video",
            userID: event.fromUserID,
            remoteID: event.remoteID,
            wasInserted: wasInserted,
            fields: [
                "stage": event.stage.rawValue,
                "downloading": isVideoDownloading(
                    remoteID: event.remoteID,
                    localPath: event.videoFileURL.path
                ) ? "1" : "0",
                "updated": wasUpdated ? "1" : "0",
                "duration_seconds": String(event.durationSeconds),
                "width": String(event.width),
                "height": String(event.height),
                "bytes": String(event.sizeBytes),
            ]
        )
        updateUnreadAfterReceiving(from: event.fromUserID, wasInserted: wasInserted)
        await postSystemNotificationIfNeeded(
            message: message,
            from: event.fromUserID,
            wasInserted: wasInserted
        )
        refreshProfileIfNeeded(userID: event.fromUserID)
        enqueueHistoryUpsert(message)
        if wasInserted {
            settingsStore.save(currentStoredSettings())
        }
        // 附件从元数据阶段就使用稳定路径；后续下载只让同一路径出现真实文件，
        // 因此需明确刷新气泡和播放器，而不是重建消息。
        objectWillChange.send()
    }

    private func updateVideoDownloadState(for event: IncomingRemoteIMVideo) {
        let key = Self.videoDownloadKey(remoteID: event.remoteID, localPath: event.videoFileURL.path)
        let next = RemoteIMVideoDownloadTrackingPolicy.updatedKeys(current: downloadingVideoKeys,
            key: key, stage: event.stage, fileIsUsable: false)
        if next != downloadingVideoKeys { downloadingVideoKeys = next }
        if event.stage == .videoReady || event.stage == .videoFailed {
            videoMetadataChecks[key] = nil
        } else if event.stage == .metadata {
            videoMetadataSequence &+= 1
            let sequence = videoMetadataSequence
            videoMetadataChecks[key] = sequence
            let url = event.videoFileURL
            let identity = remoteDiagnosticsIdentity
            Task { [weak self] in
                let usable = (try? await RemoteIMBackgroundWork.metadata { Self.isUsableLocalFile(url) }) ?? false
                guard let self, self.remoteDiagnosticsIdentity == identity,
                      self.videoMetadataChecks[key] == sequence else { return }
                self.videoMetadataChecks[key] = nil
                let next = RemoteIMVideoDownloadTrackingPolicy.updatedKeys(current: self.downloadingVideoKeys,
                    key: key, stage: .metadata, fileIsUsable: usable)
                if next != self.downloadingVideoKeys { self.downloadingVideoKeys = next }
            }
        }
    }

    private static func videoDownloadKey(remoteID: String?, localPath: String) -> String {
        let cleanRemoteID = remoteID?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return cleanRemoteID.isEmpty
            ? URL(fileURLWithPath: localPath).standardizedFileURL.path
            : cleanRemoteID
    }

    nonisolated private static func isUsableLocalFile(_ url: URL) -> Bool {
        guard let values = try? url.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey]) else {
            return false
        }
        return values.isRegularFile == true && (values.fileSize ?? 0) > 0
    }

    private func shouldAcceptIncomingSender(_ userID: String, kind: String) -> Bool {
        let cleanUserID = userID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard RemoteIMPeerPolicy.isValidPeer(
            userID: cleanUserID,
            ownerUserID: chatState.ownerUserID
        ) else {
            logIM(
                level: .warning,
                event: "message-dropped",
                fields: [
                    "kind": kind,
                    "reason": cleanUserID.isEmpty ? "empty-sender" : "sender-is-current-user",
                ],
                userID: cleanUserID
            )
            return false
        }
        return true
    }

    private func updateUnreadAfterReceiving(from userID: String, wasInserted: Bool) {
        guard wasInserted else { return }
        if visibleConversationUserID == userID {
            unreadCountByUserID[userID] = nil
        } else {
            unreadCountByUserID[userID, default: 0] += 1
        }
    }

    private func postSystemNotificationIfNeeded(
        message: RemoteIMMessage,
        from userID: String,
        wasInserted: Bool
    ) async {
        let isApplicationActive = UIApplication.shared.applicationState == .active
        guard RemoteIMNewMessageNotificationPolicy.shouldNotify(
            wasInserted: wasInserted,
            isApplicationActive: isApplicationActive,
            incomingUserID: userID
        ) else {
            logIM(
                level: .debug,
                event: "notification-suppressed",
                fields: [
                    "reason": wasInserted ? "app-foreground" : "duplicate-or-history",
                ],
                userID: userID
            )
            return
        }

        let profile = profile(for: userID)
        let badgeCount = RemoteIMNewMessageNotificationPolicy.systemBadgeCount(
            totalUnreadCount: totalUnreadCount
        )
        let pendingCount = unreadCountByUserID[userID] ?? 1
        let identity = remoteDiagnosticsIdentity
        guard let body = try? await RemoteIMBackgroundWork.parse({
            RemoteIMNewMessageNotificationPolicy.aggregatedPreview(for: message, pendingCount: pendingCount)
        }), remoteDiagnosticsIdentity == identity else { return }
        let posted = await RemoteIMSystemNotificationCenter.shared.post(
            peerUserID: userID,
            title: profile.displayName,
            body: body,
            badgeCount: badgeCount
        )
        logIM(
            level: posted ? .info : .warning,
            event: posted ? "notification-requested" : "notification-failed",
            fields: [
                "badge_count": String(badgeCount),
                "unread_count": String(totalUnreadCount),
            ],
            userID: userID
        )
    }

    private func currentStoredSettings() -> StoredRemoteIMSettings {
        StoredRemoteIMSettings(
            sdkAppID: Int(sdkAppIDText.trimmingCharacters(in: .whitespacesAndNewlines)),
            masterUserID: masterUserID.trimmingCharacters(in: .whitespacesAndNewlines),
            friendUserIDs: chatState.contacts
                .map(\.userID),
            slaveUserIDs: [],
            contacts: chatState.contacts,
            contactGroups: chatState.contactGroups,
            unreadCountByUserID: unreadCountByUserID,
            reconnectOnLaunch: reconnectOnLaunch
        )
    }

    private func persistReconnectOnLaunchIntent(_ value: Bool) {
        let cleanMasterUserID = masterUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanMasterUserID.isEmpty else {
            logIM(
                level: .warning,
                event: "connection-intent-not-persisted",
                fields: ["reason": "empty-user-id"]
            )
            return
        }
        reconnectOnLaunch = value
        var settings = settingsStore.load()
        // 账号切换过程中的部分写入也要可恢复：
        // 若完整重建前进程退出，不能让 B 账号继续使用 A 账号的联系人和未读数。
        if settings.masterUserID.trimmingCharacters(in: .whitespacesAndNewlines) != cleanMasterUserID {
            settings.friendUserIDs = []
            settings.slaveUserIDs = []
            settings.contacts = []
            settings.contactGroups = []
            settings.unreadCountByUserID = [:]
        }
        settings.sdkAppID = currentSDKAppID()
        settings.masterUserID = cleanMasterUserID
        settings.reconnectOnLaunch = value
        settingsStore.save(settings)
    }

    /// 账号切换后重建联系人、消息和未读状态。
    /// 隔离不同手机号的数据，完成状态恢复后再开放对应的会话入口。
    private func rebuildChatStateForCurrentAccount() async -> Bool {
        stopAllActivity()
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
        downloadingVideoKeys = []
        historyAccountGeneration &+= 1
        chatHistorySDKAppID = nextSDKAppID
        conversationHistoryStateByUserID = [:]
        visibleMessageLimitByUserID = [:]
        focusedMessageIDByUserID = [:]
        conversationHistoryLoadGenerationByUserID = [:]
        pendingIncomingRemoteIDs = []
        let nextState = MasterChatState(
            ownerUserID: cleanMasterUserID,
            contacts: chatState.contacts,
            contactGroups: chatState.contactGroups,
            messages: currentSummaries,
            selectedPeerID: chatState.selectedPeerID
        )
        chatState = nextState
        AppDiagnosticLog.shared.setPerformanceAccount(remoteDiagnosticsAccountTag)
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

    /// 批量提交等待写盘的 IM 历史变更。
    /// 按到达顺序落库，失败时保留队列以免丢失尚未持久化的消息。
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
                if result.durationMilliseconds >= 50, mutations.allSatisfy({
                    $0.account.sdkAppID == chatHistorySDKAppID && $0.account.ownerUserID == chatState.ownerUserID
                }) {
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

    /// 刷新当前联系人在线状态并发布必要的界面变化。
    /// 接口部分失败时不清空已有联系人或消息列表。
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

    /// 批量更新联系人资料、头像与显示名称。
    /// 网络结果只覆盖有变化的字段，避免因资料刷新重建整个会话页。
    private func refreshProfiles(userIDs: [String]) async {
        guard connectionState == .connected else { return }
        let generation = profileFetchGeneration
        let requestedUserIDs = Set(userIDs.lazy.map {
            $0.trimmingCharacters(in: .whitespacesAndNewlines)
        }.filter { !$0.isEmpty })
        let pendingUserIDs = requestedUserIDs.subtracting(profileRefreshUserIDsInFlight)
            .subtracting(profileFetchedForConnection)
        guard !pendingUserIDs.isEmpty else { return }
        profileRefreshUserIDsInFlight.formUnion(pendingUserIDs)
        profileFetchedForConnection.formUnion(pendingUserIDs)
        defer {
            if generation == profileFetchGeneration {
                profileRefreshUserIDsInFlight.subtract(pendingUserIDs)
            }
        }

        let pendingUserIDList = pendingUserIDs.sorted()
        let startedAt = ProcessInfo.processInfo.systemUptime
        do {
            let profiles = try await client.refreshUserProfiles(userIDs: pendingUserIDList)
            guard generation == profileFetchGeneration, connectionState == .connected else { return }
            var nextProfiles = userProfileByUserID
            var nextChatState = chatState
            for profile in profiles {
                nextProfiles[profile.userID] = profile
                if let contact = nextChatState.contacts.first(where: { $0.userID == profile.userID }) {
                    try nextChatState.upsertContact(
                        userID: profile.userID,
                        relation: contact.relation,
                        displayName: profile.displayName,
                        avatarURL: profile.avatarURL
                    )
                }
            }
            if nextProfiles != userProfileByUserID {
                userProfileByUserID = nextProfiles
            }
            RemoteIMAvatarImageStore.shared.prefetch(profiles.compactMap(\.avatarURL))
            if nextChatState != chatState {
                chatState = nextChatState
            }
            settingsStore.save(currentStoredSettings())
            logIM(
                level: .info,
                event: "profile-refresh",
                fields: [
                    "result": "ok",
                    "requested": String(pendingUserIDList.count),
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
                    "requested": String(pendingUserIDList.count),
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
        if resolvedUserID == chatState.ownerUserID { values["account"] = remoteDiagnosticsAccountTag }
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

    private static func applyDebugLaunchOverrides(settings: inout StoredRemoteIMSettings) -> Bool {
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
