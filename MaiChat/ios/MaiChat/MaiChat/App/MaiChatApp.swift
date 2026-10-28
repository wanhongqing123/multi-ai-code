// 文件作用：创建 iOS App 场景，初始化日志、通知、账号状态与全局共享服务。
// 窗口最外层承载 IM 和创作中心共用的语音转写悬浮窗；悬浮窗只负责覆盖显示。
// 启动、后台和返回前台的状态流在此接线，具体业务页面由 RootView 组织。
// 模拟器专用入口用于不触发付费服务的真实界面验收，不进入真机发布路径。
import MaiChatCore
import SwiftUI
import UserNotifications
import UIKit

@MainActor
final class IOSBackgroundActivityKeeper {
    static let shared = IOSBackgroundActivityKeeper()

    private var taskIdentifier: UIBackgroundTaskIdentifier = .invalid

    private init() {
    }

    /// 请求 iOS 授予普通 App 的有限后台执行窗口。
    /// 不播放静音音频，也不虚报定位或 VoIP 模式；这些都不是通用 IM 的合法保活方式，
    /// 进程仍会被 iOS 结束。
    func beginIfNeeded() {
        guard taskIdentifier == .invalid else { return }
        taskIdentifier = UIApplication.shared.beginBackgroundTask(
            withName: "MaiChat.IMBackgroundKeepAlive"
        ) { [weak self] in
            Task { @MainActor in
                self?.end(cause: "system-expired")
            }
        }
        let started = taskIdentifier != .invalid
        AppDiagnosticLog.shared.record(
            level: started ? .info : .warning,
            category: "app",
            event: started ? "background-keepalive-started" : "background-keepalive-unavailable",
            fields: [
                "remaining_seconds": remainingSecondsText(),
            ]
        )
    }

    func end(cause: String) {
        guard taskIdentifier != .invalid else { return }
        let identifier = taskIdentifier
        taskIdentifier = .invalid
        UIApplication.shared.endBackgroundTask(identifier)
        AppDiagnosticLog.shared.record(
            level: .info,
            category: "app",
            event: "background-keepalive-ended",
            fields: ["cause": cause]
        )
    }

    private func remainingSecondsText() -> String {
        let remaining = UIApplication.shared.backgroundTimeRemaining
        guard remaining.isFinite, remaining >= 0, remaining <= Double(Int.max) else {
            return "unknown"
        }
        return String(Int(remaining.rounded(.down)))
    }
}

@MainActor
final class RemoteIMSystemNotificationCenter: NSObject, ObservableObject, UNUserNotificationCenterDelegate {
    static let shared = RemoteIMSystemNotificationCenter()

    private let center = UNUserNotificationCenter.current()
    @Published private(set) var pendingRouteRevision: UInt64 = 0
    @Published private(set) var authorizationStatus: UNAuthorizationStatus = .notDetermined
    private(set) var pendingPeerUserID: String?

    func install() {
        center.delegate = self
    }

    func requestAuthorizationIfNeeded() async {
        let settings = await center.notificationSettings()
        authorizationStatus = settings.authorizationStatus
        guard settings.authorizationStatus == .notDetermined else { return }
        _ = try? await center.requestAuthorization(options: [.alert, .sound, .badge])
        await refreshAuthorizationStatus()
    }

    func refreshAuthorizationStatus() async {
        authorizationStatus = await center.notificationSettings().authorizationStatus
    }

    func post(peerUserID: String, title: String, body: String, badgeCount: Int) async -> Bool {
        let settings = await center.notificationSettings()
        guard settings.authorizationStatus == .authorized
            || settings.authorizationStatus == .provisional
            || settings.authorizationStatus == .ephemeral else { return false }
        let identifier = notificationIdentifier(peerUserID)
        center.removePendingNotificationRequests(withIdentifiers: [identifier])
        center.removeDeliveredNotifications(withIdentifiers: [identifier])

        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        content.sound = .default
        content.badge = NSNumber(value: RemoteIMNewMessageNotificationPolicy.systemBadgeCount(
            totalUnreadCount: badgeCount
        ))
        content.threadIdentifier = "remote-im-\(peerUserID)"
        content.userInfo = ["peerUserID": peerUserID]
        do {
            try await center.add(UNNotificationRequest(
                identifier: identifier,
                content: content,
                trigger: nil
            ))
            return true
        } catch {
            return false
        }
    }

    func clear(peerUserID: String, badgeCount: Int) {
        let identifier = notificationIdentifier(peerUserID)
        center.removePendingNotificationRequests(withIdentifiers: [identifier])
        center.removeDeliveredNotifications(withIdentifiers: [identifier])
        if #available(iOS 16.0, *) {
            center.setBadgeCount(RemoteIMNewMessageNotificationPolicy.systemBadgeCount(
                totalUnreadCount: badgeCount
            ))
        }
    }

    func updateBadgeCount(_ badgeCount: Int) {
        if #available(iOS 16.0, *) {
            center.setBadgeCount(RemoteIMNewMessageNotificationPolicy.systemBadgeCount(
                totalUnreadCount: badgeCount
            ))
        }
    }

    func consumePendingPeerUserID() -> String? {
        defer { pendingPeerUserID = nil }
        return pendingPeerUserID
    }

    nonisolated func userNotificationCenter(
        _ center: UNUserNotificationCenter,
        willPresent notification: UNNotification
    ) async -> UNNotificationPresentationOptions {
        // 这个回调只会在 App 位于前台时触发。前台已经有会话列表和未读红点，
        // 不再叠一层系统横幅；退到后台后由系统按通知内容正常展示。
        []
    }

    nonisolated func userNotificationCenter(
        _ center: UNUserNotificationCenter,
        didReceive response: UNNotificationResponse,
        withCompletionHandler completionHandler: @escaping () -> Void
    ) {
        guard let peerUserID = response.notification.request.content.userInfo["peerUserID"] as? String,
              !peerUserID.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
        else {
            completionHandler()
            return
        }
        completionHandler()
        // 真机上的异步 UNUserNotificationCenterDelegate 入口运行在协作队列。
        // 从自动生成的异步桥接中调用 UIKit，曾在进入 MainActor 方法前就崩溃。
        // 改由带完成回调的入口显式切到 GCD 主队列，越过真实线程边界后再断言 actor 隔离。
        DispatchQueue.main.async { [weak self] in
            MainActor.assumeIsolated {
                self?.queueNotificationConversation(peerUserID)
            }
        }
    }

    private func queueNotificationConversation(_ peerUserID: String) {
        pendingPeerUserID = peerUserID
        pendingRouteRevision &+= 1
        AppDiagnosticLog.shared.record(
            level: .info,
            category: "remote-im",
            event: "notification-route-queued",
            fields: [
                "peer": DiagnosticLogPrivacy.stableTag(peerUserID, prefix: "u"),
                "main_thread": Thread.isMainThread ? "true" : "false",
                "revision": String(pendingRouteRevision),
            ]
        )
    }

    private func notificationIdentifier(_ peerUserID: String) -> String {
        "remote-im-message-\(peerUserID)"
    }
}

@MainActor
private final class MaiChatBootstrap: ObservableObject {
    @Published private(set) var appState: RemoteIMAppState?
    @Published private(set) var errorMessage: String?
    private var loading = false
    func load() async {
        guard appState == nil, !loading else { return }
        loading = true
        errorMessage = nil
        defer { loading = false }
        do {
            let loaded = try await RemoteIMBackgroundWork.file {
                _ = RemoteIMBackgroundWork.audioQueue
                try RemoteIMMediaStorage.prepareDirectories()
                let settings = LocalSettingsStore()
                // 旧版曾在本机保存 IM 签名密钥；新登录只保留服务端会话刷新令牌。
                try? KeychainSecretStore().saveSecretKey("")
                return (settings, settings.load())
            }
            guard !Task.isCancelled else { return }
            let state = RemoteIMAppState(
                settingsStore: loaded.0,
                loadedSettings: loaded.1
            )
            AIMobileHostToolProvider.shared.appState = state
            appState = state
            if state.hasUnifiedAccount {
                await MainActor.run {
                    AIAssistantModel.shared.startSessionSyncForAccount()
                }
            }
        } catch {
            if !Task.isCancelled { errorMessage = error.localizedDescription }
        }
    }
}

@main
struct MaiChatApp: App {
    @StateObject private var bootstrap = MaiChatBootstrap()
    @StateObject private var voiceOverlay = VoiceTranscriptionOverlayState.shared

    init() {
        AppDiagnosticLog.shared.install()
        MaiCloudDriveAgentPlaybackCache.shared.cleanupOrphans()
        Task.detached(priority: .utility) {
            // 账号会话已取代手填共享令牌；启动时删除此前保存的连接信息。
            UserDefaults.standard.removeObject(forKey: "oss-media-signer-url")
            UserDefaults.standard.removeObject(forKey: "cloud-credentials-last-sync")
            UserDefaults.standard.removeObject(forKey: "cloud-credentials-synced-providers")
            try? KeychainSecretStore(account: "oss-media-signer-token").saveSecretKey("")
            if let documents = FileManager.default.urls(for: .documentDirectory,
                                                         in: .userDomainMask).first {
                try? FileManager.default.removeItem(
                    at: documents.appendingPathComponent("cloud-service-setup.json"))
            }
            _ = try? await AICloudCredentialSync.refreshAccountModelKeysIfAvailable()
            try? await AICloudCredentialSync.refreshSeedanceIfNeeded()
            try? await AICloudCredentialSync.refreshBailianIfNeeded()
        }
        RemoteIMSystemNotificationCenter.shared.install()
    }

    var body: some Scene {
        WindowGroup {
            Group {
                #if targetEnvironment(simulator)
                if ProcessInfo.processInfo.arguments.contains("--ai-ui-test") {
                    if ProcessInfo.processInfo.arguments.contains("--im-curl-download-ui-test") {
                        IMCurlDownloadUITestRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--im-group-ui-test") ||
                                ProcessInfo.processInfo.arguments.contains(
                                    "--im-group-activation-fail-ui-test") {
                        GroupChatUITestRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--video-circle-ui-test") {
                        AIVideoCircleUITestRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--cloud-drive-image-pager-test") {
                        MaiCloudImagePagerUITestRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--cloud-drive-ui-test") {
                        MaiCloudDriveUITestRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--ai-calendar-ui-test") {
                        AICalendarUITestRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--im-add-friend-menu-ui-test") {
                        ChatAddFriendMenuUITestRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--permission-card-ui-test") {
                        AIPermissionUITestRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--ai-image-fullscreen-ui-test") {
                        AIImageFullscreenUITestRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--ai-image-mixed-ui-test") {
                        AIImageMixedUITestRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--im-image-fullscreen-ui-test") {
                        RemoteIMImageFullscreenUITestRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--text-media-ui-test") {
                        AITextMediaUITestRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--video-bubble-ui-test") {
                        AIVideoBubbleUITestRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--transcript-ui-test") {
                        AITranscriptUITestRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--ai-history-ui-test") {
                        AIHistoryUITestRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--ai-welcome-ui-test") {
                        AIWelcomeUITestRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--ai-session-switch-ui-test") {
                        AISessionSwitchUITestRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--ai-browser-ui-test") {
                        #if DEBUG
                        AIBrowserUITestRoot()
                        #else
                        AIAssistantView()
                        #endif
                    } else if ProcessInfo.processInfo.arguments.contains("--ai-media-draft-ui-test") {
                        AIMediaDraftUITestRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--ai-local-music-ui-test") {
                        AILocalMusicUITestRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--ai-stream-typing-ui-test") {
                        AIStreamTypingUITestRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--voice-conversation-preview") {
                        VoiceTranscriptionConversationPreviewRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--ffplay-ui-test") {
                        MaiFfplayUITestRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--ffplay-swipe-ui-test") {
                        MaiFfplaySwipeUITestRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--mixed-gallery-ui-test") {
                        MaiMixedGalleryUITestRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--ffplay-seek-ui-test") {
                        MaiFfplaySeekUITestRoot(startSeconds: 0)
                    } else if ProcessInfo.processInfo.arguments.contains("--ffplay-start-ui-test") {
                        MaiFfplaySeekUITestRoot(startSeconds: 6)
                    } else if ProcessInfo.processInfo.arguments.contains("--ffplay-agent-dock-ui-test") {
                        #if DEBUG
                        MaiFfplayAgentDockUITestRoot()
                        #else
                        AIAssistantView()
                        #endif
                    } else if ProcessInfo.processInfo.arguments.contains("--voice-docked-ui-test") {
                        VoiceTranscriptionDockUITestRoot()
                    } else if ProcessInfo.processInfo.arguments.contains("--im-landscape-ui-test") ||
                                ProcessInfo.processInfo.arguments.contains("--im-large-history-ui-test") ||
                                ProcessInfo.processInfo.arguments.contains("--im-search-focus-ui-test") {
                        ChatLandscapeUITestRoot()
                    } else {
                        AIAssistantUITestRoot()
                    }
                } else {
                    appContent
                }
                #elseif DEBUG
                if ProcessInfo.processInfo.arguments.contains("--ffplay-agent-dock-ui-test") ||
                    ProcessInfo.processInfo.arguments.contains("--ffplay-agent-real-video-test") {
                    MaiFfplayAgentDockUITestRoot()
                } else {
                    appContent
                }
                #else
                appContent
                #endif
            }
            .overlay {
                // IM 和创作中心共用的语音悬浮层放在窗口最外侧，始终盖过视频播放器。
                if let presentation = voiceOverlay.activePresentation {
                    VoiceTranscriptionHighlightHost(presentation: presentation)
                        .ignoresSafeArea()
                }
            }
            .task {
                #if targetEnvironment(simulator)
                if ProcessInfo.processInfo.arguments.contains("--ai-ui-test") { return }
                #endif
                #if DEBUG
                if ProcessInfo.processInfo.arguments.contains("--ffplay-agent-dock-ui-test") ||
                    ProcessInfo.processInfo.arguments.contains("--ffplay-agent-real-video-test") {
                    return
                }
                #endif
                await bootstrap.load()
                #if DEBUG && targetEnvironment(simulator)
                if ProcessInfo.processInfo.arguments.contains(
                    "--cloud-drive-production-canary-normal"),
                   let appState = bootstrap.appState {
                    do {
                        let account = try MaiCloudDriveProductionCanaryCredential.consume()
                        try await appState.loginWithAccountPassword(
                            account: account.account,
                            password: account.password,
                            ageConfirmed: true)
                    } catch {
                        AppDiagnosticLog.shared.record(
                            level: .error, category: "cloud-drive-canary",
                            event: "account-login-failed")
                    }
                    return
                }
                // 仅本机正常 App 的交互验收使用；测试账号由本地模拟短信服务创建，
                // 不进入真机和发布包，也不触发任何付费模型请求。
                let environment = ProcessInfo.processInfo.environment
                if let phone = environment["MAICHAT_LOCAL_UI_PHONE"],
                   let password = environment["MAICHAT_LOCAL_UI_PASSWORD"],
                   !password.isEmpty,
                   let appState = bootstrap.appState {
                    do {
                        try await appState.loginWithAccountPassword(
                            account: phone, password: password, ageConfirmed: true)
                    } catch {
                        AppDiagnosticLog.shared.record(
                            level: .error, category: "local-ui",
                            event: "local-account-login-failed",
                            fields: ["error": error.localizedDescription])
                    }
                }
                #endif
            }
        }
    }

    @ViewBuilder private var appContent: some View {
        if let appState = bootstrap.appState {
            RootView().environmentObject(appState)
        } else if let error = bootstrap.errorMessage {
            VStack(spacing: 12) {
                Text("加载失败")
                Text(error).font(.footnote)
                Button("重试") { Task { await bootstrap.load() } }
            }.padding()
        } else {
            ProgressView("正在加载")
        }
    }
}

#if targetEnvironment(simulator)
private struct AIAssistantUITestRoot: View {
    @StateObject private var appState = RemoteIMAppState(loadedSettings: .empty)

    var body: some View {
        AIAssistantView().environmentObject(appState)
    }
}

/// Local TLS integration route for the production iOS downloader and C bridge.
/// It is reachable only from a simulator launch carrying an explicit test flag.
private struct IMCurlDownloadUITestRoot: View {
    @State private var result = "running"

    var body: some View {
        Text(result)
            .accessibilityIdentifier(result == "passed" ? "im-media-all-ok" : "im-media-result")
            .task { await run() }
    }

    private func run() async {
        guard let encoded = ProcessInfo.processInfo.environment["MAICHAT_MEDIA_TEST_CA_BASE64"],
              let certificate = Data(base64Encoded: encoded) else {
            result = "missing-local-certificate"
            return
        }
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("im-media-\(UUID().uuidString)", isDirectory: true)
        do { try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true) }
        catch { result = "create-directory-failed"; return }
        defer { try? FileManager.default.removeItem(at: directory) }
        let downloader = MaiChatAttachmentDownloader()
        let image = directory.appendingPathComponent("image.png")
        let imageResult = await transfer(
            downloader, "https://127.0.0.1:18442/image", image, 1024 * 1024, certificate
        )
        guard imageResult.succeeded,
              let imageData = try? Data(contentsOf: image),
              imageData.starts(with: [137, 80, 78, 71]) else {
            result = "image-failed-\(imageResult.status)-\(imageResult.curlCode)"
            return
        }
        let file = directory.appendingPathComponent("file.bin")
        let fileResult = await transfer(
            downloader, "https://127.0.0.1:18442/file", file, 1024 * 1024, certificate
        )
        guard fileResult.succeeded,
              let fileData = try? Data(contentsOf: file),
              String(data: fileData, encoding: .utf8) == "MaiChat local TLS attachment fixture\n" else {
            result = "file-failed-\(fileResult.status)-\(fileResult.curlCode)"
            return
        }
        let held = directory.appendingPathComponent("held.bin")
        let oldAccountGeneration = downloader.generation
        let canceled = await withCheckedContinuation {
            (continuation: CheckedContinuation<MaiChatAttachmentDownloadResult, Never>) in
            downloader.download(
                signedURL: "https://127.0.0.1:18442/held", targetURL: held,
                maximumBytes: 1024 * 1024, certificatePEM: certificate
            ) { response in continuation.resume(returning: response) }
            Task {
                try? await Task.sleep(for: .milliseconds(200))
                downloader.cancelAll()
            }
        }
        guard canceled.status == Int32(MAI_DOWNLOAD_CANCELED.rawValue),
              !FileManager.default.fileExists(atPath: held.path) else {
            result = "cancel-failed-\(canceled.status)-\(canceled.curlCode)"
            return
        }
        let staleTarget = directory.appendingPathComponent("stale.bin")
        let staleResult = await withCheckedContinuation {
            (continuation: CheckedContinuation<MaiChatAttachmentDownloadResult, Never>) in
            downloader.download(
                signedURL: "https://127.0.0.1:18442/file", targetURL: staleTarget,
                maximumBytes: 1024 * 1024, generation: oldAccountGeneration,
                certificatePEM: certificate
            ) { response in continuation.resume(returning: response) }
        }
        guard staleResult.status == Int32(MAI_DOWNLOAD_CANCELED.rawValue),
              !FileManager.default.fileExists(atPath: staleTarget.path) else {
            result = "stale-account-failed-\(staleResult.status)"
            return
        }
        result = "passed"
    }

    private func transfer(
        _ downloader: MaiChatAttachmentDownloader,
        _ signedURL: String, _ target: URL, _ maximumBytes: UInt64, _ certificate: Data
    ) async -> MaiChatAttachmentDownloadResult {
        await withCheckedContinuation { continuation in
            downloader.download(
                signedURL: signedURL, targetURL: target,
                maximumBytes: maximumBytes, certificatePEM: certificate
            ) { response in continuation.resume(returning: response) }
        }
    }
}
#endif

#if DEBUG && targetEnvironment(simulator)
private struct AICalendarUITestRoot: View {
    @State private var result = "尚未创建日程"

    var body: some View {
        VStack(spacing: 20) {
            Button("创建系统日历测试事件") {
                Task {
                    let start = Int64(Date().timeIntervalSince1970 * 1_000) + 3_600_000
                    let response = await AIMobileHostToolProvider.shared.createCalendarEvent([
                        "title": "MaiChat 日历隔离测试",
                        "notes": "仅在隔离模拟器中创建",
                        "session_id": "ses_calendar_fixture",
                        "starts_at_utc_ms": start,
                        "ends_at_utc_ms": start + 1_800_000
                    ])
                    result = response.errorMessage ?? response.output ?? "日历没有返回结果"
                }
            }
            .accessibilityIdentifier("ai-calendar-create-sample")
            Text(result).accessibilityIdentifier("ai-calendar-result")
        }
    }
}
#endif
