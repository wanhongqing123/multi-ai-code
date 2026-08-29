import SwiftUI
import UserNotifications
import UIKit

@MainActor
final class IOSBackgroundActivityKeeper {
    static let shared = IOSBackgroundActivityKeeper()

    private var taskIdentifier: UIBackgroundTaskIdentifier = .invalid

    private init() {
    }

    /// Requests the finite background execution window that iOS grants regular apps.
    /// This deliberately does not play silent audio or claim location/VoIP modes: those are
    /// not legitimate generic IM keep-alive mechanisms and would still be terminated by iOS.
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

extension Notification.Name {
    static let remoteIMNotificationConversationSelected = Notification.Name(
        "MaiChat.remoteIMNotificationConversationSelected"
    )
}

@MainActor
final class RemoteIMSystemNotificationCenter: NSObject, UNUserNotificationCenterDelegate {
    static let shared = RemoteIMSystemNotificationCenter()

    private let center = UNUserNotificationCenter.current()
    private(set) var pendingPeerUserID: String?

    func install() {
        center.delegate = self
    }

    func requestAuthorizationIfNeeded() async {
        let settings = await center.notificationSettings()
        guard settings.authorizationStatus == .notDetermined else { return }
        _ = try? await center.requestAuthorization(options: [.alert, .sound, .badge])
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
        content.badge = NSNumber(value: min(99, max(0, badgeCount)))
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
            center.setBadgeCount(min(99, max(0, badgeCount)))
        }
    }

    func updateBadgeCount(_ badgeCount: Int) {
        if #available(iOS 16.0, *) {
            center.setBadgeCount(min(99, max(0, badgeCount)))
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
        [.banner, .list, .sound, .badge]
    }

    nonisolated func userNotificationCenter(
        _ center: UNUserNotificationCenter,
        didReceive response: UNNotificationResponse
    ) async {
        guard let peerUserID = response.notification.request.content.userInfo["peerUserID"] as? String,
              !peerUserID.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return }
        await MainActor.run {
            pendingPeerUserID = peerUserID
            NotificationCenter.default.post(
                name: .remoteIMNotificationConversationSelected,
                object: peerUserID
            )
        }
    }

    private func notificationIdentifier(_ peerUserID: String) -> String {
        "remote-im-message-\(peerUserID)"
    }
}

@main
struct MaiChatApp: App {
    @StateObject private var appState = RemoteIMAppState()

    init() {
        AppDiagnosticLog.shared.install()
        RemoteIMSystemNotificationCenter.shared.install()
    }

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(appState)
        }
    }
}
