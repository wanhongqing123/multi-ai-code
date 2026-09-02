import MaiChatCore
import SwiftUI
import UIKit

enum AppTab {
    case messages
    case contacts
    case remote
    case me
}

struct RootView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @Environment(\.scenePhase) private var scenePhase
    @ObservedObject private var systemNotificationCenter = RemoteIMSystemNotificationCenter.shared
    @State private var selectedTab: AppTab = .messages
    @State private var activeChatContact: RemoteIMContact?
    @State private var isShowingAddContact = false
    @State private var isShowingBroadcast = false
    @State private var movingContact: RemoteIMContact?
    @State private var contactGroupActionName: String?
    @State private var renamingContactGroup: String?
    @State private var contactGroupRenameDraft = ""
    @State private var deletingContactGroup: String?
    @State private var isShowingNotificationPermissionPrompt = false
    @AppStorage("maichat.notification-preprompt-dismissed")
    private var notificationPermissionPromptDismissed = false

    var body: some View {
        ZStack {
            VStack(spacing: 0) {
                if appState.shouldShowInitialLogin {
                    InitialLoginView()
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                } else {
                    Group {
                        switch selectedTab {
                        case .messages:
                            ChatView(
                                activeContact: $activeChatContact,
                                showRemoteDesktop: {
                                    activeChatContact = nil
                                    selectedTab = .remote
                                }
                            )
                        case .contacts:
                            ContactsView(
                                selectedTab: $selectedTab,
                                activeContact: $activeChatContact,
                                movingContact: $movingContact,
                                groupActionName: $contactGroupActionName,
                                isShowingBroadcast: $isShowingBroadcast,
                                showAddContact: {
                                    appState.newContactUserID = ""
                                    isShowingAddContact = true
                                }
                            )
                        case .remote:
                            RemoteDesktopView()
                        case .me:
                            SettingsView()
                        }
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)

                    if activeChatContact == nil {
                        RootTabBar(
                            selectedTab: $selectedTab,
                            remoteDesktop: appState.remoteDesktop
                        )
                    }
                }
            }

            if isShowingAddContact, !appState.shouldShowInitialLogin {
                AddContactDialog(isPresented: $isShowingAddContact)
                    .transition(.opacity.combined(with: .scale(scale: 0.96)))
                    .zIndex(10)
            }

            if let movingContact, !appState.shouldShowInitialLogin {
                MoveContactGroupDialog(contact: movingContact) {
                    self.movingContact = nil
                }
                .transition(.opacity.combined(with: .scale(scale: 0.96)))
                .zIndex(20)
            }

            if isShowingBroadcast, !appState.shouldShowInitialLogin {
                BroadcastComposeDialog(isPresented: $isShowingBroadcast)
                    .transition(.move(edge: .trailing).combined(with: .opacity))
                    .zIndex(30)
            }

            if isShowingNotificationPermissionPrompt, !appState.shouldShowInitialLogin {
                NotificationPermissionDialog(
                    later: dismissNotificationPermissionPrompt,
                    enable: enableNotifications
                )
                .transition(.opacity.combined(with: .scale(scale: 0.96)))
                .zIndex(40)
            }

            if let contactGroupActionName, !appState.shouldShowInitialLogin {
                ContactGroupActionDialog(
                    groupName: contactGroupActionName,
                    dismiss: { self.contactGroupActionName = nil },
                    rename: {
                        contactGroupRenameDraft = contactGroupActionName
                        renamingContactGroup = contactGroupActionName
                        self.contactGroupActionName = nil
                    },
                    delete: {
                        deletingContactGroup = contactGroupActionName
                        self.contactGroupActionName = nil
                    }
                )
                .transition(.opacity.combined(with: .scale(scale: 0.96)))
                .zIndex(50)
            }

            if let renamingContactGroup, !appState.shouldShowInitialLogin {
                RenameContactGroupDialog(
                    originalName: renamingContactGroup,
                    name: $contactGroupRenameDraft,
                    cancel: { self.renamingContactGroup = nil },
                    save: saveRenamedContactGroup
                )
                .transition(.opacity.combined(with: .scale(scale: 0.96)))
                .zIndex(60)
            }

            if let deletingContactGroup, !appState.shouldShowInitialLogin {
                DeleteContactGroupDialog(
                    groupName: deletingContactGroup,
                    memberCount: appState.chatState.contacts.filter {
                        $0.groupName == deletingContactGroup
                    }.count,
                    cancel: { self.deletingContactGroup = nil },
                    delete: deleteContactGroup
                )
                .transition(.opacity.combined(with: .scale(scale: 0.96)))
                .zIndex(70)
            }
        }
        .animation(.easeOut(duration: 0.18), value: isShowingAddContact)
        .animation(.easeOut(duration: 0.18), value: movingContact?.userID)
        .animation(.easeOut(duration: 0.18), value: isShowingBroadcast)
        .animation(.easeOut(duration: 0.18), value: isShowingNotificationPermissionPrompt)
        .animation(.easeOut(duration: 0.18), value: contactGroupActionName)
        .animation(.easeOut(duration: 0.18), value: renamingContactGroup)
        .animation(.easeOut(duration: 0.18), value: deletingContactGroup)
        .background(Color(red: 0.966, green: 0.976, blue: 0.988).ignoresSafeArea())
        .task {
            if !appState.shouldShowInitialLogin {
                await appState.connectOnLaunchIfNeeded()
                await offerNotificationPermissionIfNeeded()
            }
            appState.synchronizeSystemNotificationBadge()
            schedulePendingNotificationRoute(sceneIsActive: scenePhase == .active)
        }
        .onChange(of: appState.shouldShowInitialLogin) { needsLogin in
            guard !needsLogin else { return }
            Task {
                await offerNotificationPermissionIfNeeded()
            }
        }
        .onChange(of: systemNotificationCenter.pendingRouteRevision) { _ in
            schedulePendingNotificationRoute(sceneIsActive: scenePhase == .active)
        }
        .onChange(of: scenePhase) { phase in
            AppDiagnosticLog.shared.record(
                level: .info,
                category: "app",
                event: "scene-phase-changed",
                fields: ["phase": phase.diagnosticName]
            )
            if phase == .active {
                IOSBackgroundActivityKeeper.shared.end(cause: "scene-active")
                appState.synchronizeSystemNotificationBadge()
                schedulePendingNotificationRoute(sceneIsActive: true)
                return
            }
            guard phase == .inactive || phase == .background else { return }
            if phase == .background {
                IOSBackgroundActivityKeeper.shared.beginIfNeeded()
            }
            Task { @MainActor in
                let historyFlushed = await appState.flushHistoryPersistence()
                if !historyFlushed {
                    AppDiagnosticLog.shared.record(
                        level: .error,
                        category: "remote-im",
                        event: "history-flush-failed",
                        fields: [
                            "cause": phase == .background
                                ? "scene-background"
                                : "scene-inactive",
                        ]
                    )
                }
                guard phase == .background else { return }
                await appState.stopRemoteDesktopView(cause: "scene-background")
                await AppDiagnosticLog.shared.flush()
            }
        }
        .overlay(alignment: .top) {
            if !appState.shouldShowInitialLogin, let errorMessage = appState.errorMessage {
                Text(errorMessage)
                    .font(.footnote)
                    .foregroundStyle(.white)
                    .padding(.horizontal, 14)
                    .padding(.vertical, 9)
                    .background(.red, in: Capsule())
                    .padding(.top, 8)
                    .padding(.horizontal, 16)
            }
        }
    }

    private func offerNotificationPermissionIfNeeded() async {
        await systemNotificationCenter.refreshAuthorizationStatus()
        guard systemNotificationCenter.authorizationStatus == .notDetermined,
              !notificationPermissionPromptDismissed
        else { return }
        isShowingNotificationPermissionPrompt = true
    }

    private func dismissNotificationPermissionPrompt() {
        notificationPermissionPromptDismissed = true
        isShowingNotificationPermissionPrompt = false
    }

    private func enableNotifications() {
        notificationPermissionPromptDismissed = true
        isShowingNotificationPermissionPrompt = false
        Task {
            await systemNotificationCenter.requestAuthorizationIfNeeded()
        }
    }

    private func saveRenamedContactGroup() {
        guard let originalName = renamingContactGroup else { return }
        if !appState.renameContactGroup(from: originalName, to: contactGroupRenameDraft) {
            appState.errorMessage = "分组名不能为空，且不能与已有分组重名"
        }
        renamingContactGroup = nil
    }

    private func deleteContactGroup() {
        guard let groupName = deletingContactGroup else { return }
        _ = appState.deleteContactGroup(name: groupName)
        deletingContactGroup = nil
    }

    private func schedulePendingNotificationRoute(sceneIsActive: Bool) {
        guard sceneIsActive else { return }
        // Notification taps arrive while the scene is commonly still inactive. Dispatching to the
        // next real main-queue turn avoids routing during UIKit's state-restoration transaction.
        DispatchQueue.main.async {
            MainActor.assumeIsolated {
                guard let peerUserID = systemNotificationCenter.consumePendingPeerUserID()
                else { return }
                openNotificationConversation(peerUserID)
            }
        }
    }

    private func openNotificationConversation(_ peerUserID: String) {
        guard let contact = appState.chatState.contacts.first(where: { $0.userID == peerUserID })
        else { return }
        appState.selectContact(contact)
        selectedTab = .messages
        activeChatContact = contact
        AppDiagnosticLog.shared.record(
            level: .info,
            category: "remote-im",
            event: "notification-clicked",
            fields: [
                "peer": DiagnosticLogPrivacy.stableTag(peerUserID, prefix: "u"),
                "main_thread": Thread.isMainThread ? "true" : "false",
            ]
        )
    }
}

private struct NotificationPermissionDialog: View {
    let later: () -> Void
    let enable: () -> Void

    var body: some View {
        ZStack {
            Color.black.opacity(0.28)
                .ignoresSafeArea()
                .contentShape(Rectangle())
                .onTapGesture(perform: later)

            VStack(alignment: .leading, spacing: 16) {
                HStack(spacing: 12) {
                    Image(systemName: "bell.badge.fill")
                        .font(.system(size: 19, weight: .semibold))
                        .foregroundStyle(RemoteIMStyle.blue)
                        .frame(width: 42, height: 42)
                        .background(RemoteIMStyle.blueSoft, in: Circle())
                    VStack(alignment: .leading, spacing: 3) {
                        Text("开启消息通知")
                            .font(.system(size: 20, weight: .bold))
                            .foregroundStyle(RemoteIMStyle.textPrimary)
                        Text("仅在 MaiChat 退到后台时提醒新消息")
                            .font(.system(size: 13))
                            .foregroundStyle(RemoteIMStyle.textSecondary)
                    }
                }

                HStack(spacing: 12) {
                    Button("稍后") {
                        later()
                    }
                    .buttonStyle(.plain)
                    .font(.system(size: 16, weight: .semibold))
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                    .frame(maxWidth: .infinity)
                    .frame(height: 46)
                    .background(
                        RemoteIMStyle.pageBackground,
                        in: RoundedRectangle(cornerRadius: 12, style: .continuous)
                    )

                    Button("开启通知") {
                        enable()
                    }
                    .buttonStyle(.plain)
                    .font(.system(size: 16, weight: .semibold))
                    .foregroundStyle(.white)
                    .frame(maxWidth: .infinity)
                    .frame(height: 46)
                    .background(
                        RemoteIMStyle.blue,
                        in: RoundedRectangle(cornerRadius: 12, style: .continuous)
                    )
                }
            }
            .padding(20)
            .frame(maxWidth: 360)
            .background(
                RemoteIMStyle.panelBackground,
                in: RoundedRectangle(cornerRadius: 18, style: .continuous)
            )
            .overlay {
                RoundedRectangle(cornerRadius: 18, style: .continuous)
                    .stroke(RemoteIMStyle.border, lineWidth: 1)
            }
            .shadow(color: Color.black.opacity(0.16), radius: 24, y: 10)
            .padding(.horizontal, 24)
        }
        .accessibilityElement(children: .contain)
        .accessibilityAction(.escape, later)
    }
}

private extension ScenePhase {
    var diagnosticName: String {
        switch self {
        case .active: return "active"
        case .inactive: return "inactive"
        case .background: return "background"
        @unknown default: return "unknown"
        }
    }
}

private struct RootTabBar: View {
    @Binding var selectedTab: AppTab
    @ObservedObject var remoteDesktop: RemoteDesktopSession

    var body: some View {
        if selectedTab != .remote || !remoteDesktop.state.isActive {
            CompactTabBar(selectedTab: $selectedTab)
        }
    }
}

private struct InitialLoginView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @FocusState private var isAccountFocused: Bool

    private let loginBlue = Color(
        red: 47.0 / 255.0,
        green: 129.0 / 255.0,
        blue: 247.0 / 255.0
    )

    private var isConnecting: Bool {
        appState.connectionState == .connecting
    }

    private var canSubmit: Bool {
        !isConnecting &&
            !appState.masterUserID.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    var body: some View {
        VStack(spacing: 0) {
            Spacer(minLength: 32)

            VStack(spacing: 12) {
                Image(uiImage: Self.applicationIcon)
                    .resizable()
                    .interpolation(.high)
                    .scaledToFit()
                    .frame(width: 64, height: 64)
                    .clipShape(RoundedRectangle(cornerRadius: 15, style: .continuous))

                Text("欢迎使用 MaiChat")
                    .font(.system(size: 20, weight: .heavy))
                    .foregroundStyle(Color(red: 15.0 / 255.0, green: 23.0 / 255.0, blue: 42.0 / 255.0))
            }
            .multilineTextAlignment(.center)
            .padding(.bottom, 28)

            TextField("请输入登录账号", text: $appState.masterUserID)
                .font(.system(size: 15))
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .submitLabel(.go)
                .focused($isAccountFocused)
                .padding(.horizontal, 14)
                .frame(height: 46)
                .background(Color.white, in: RoundedRectangle(cornerRadius: 10, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .stroke(
                            isAccountFocused
                                ? loginBlue
                                : Color(red: 217.0 / 255.0, green: 225.0 / 255.0, blue: 236.0 / 255.0),
                            lineWidth: isAccountFocused ? 1.5 : 1
                        )
                )
                .onSubmit {
                    guard canSubmit else { return }
                    Task { await appState.submitInitialLogin() }
                }

            if let errorMessage = appState.errorMessage {
                Text(errorMessage)
                    .font(.system(size: 13, weight: .bold))
                    .foregroundStyle(.red)
                    .multilineTextAlignment(.center)
                    .frame(maxWidth: .infinity)
                    .padding(.top, 12)
            }

            Button {
                Task { await appState.submitInitialLogin() }
            } label: {
                HStack(spacing: 8) {
                    if appState.connectionState == .connecting {
                        ProgressView()
                            .tint(.white)
                    }
                    Text(appState.connectionState == .connecting ? "登录中..." : "登录")
                        .font(.system(size: 15, weight: .heavy))
                }
                .frame(maxWidth: .infinity)
                .frame(height: 46)
                .contentShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
            }
            .buttonStyle(.plain)
            .foregroundStyle(canSubmit ? Color.white : Color(red: 170.0 / 255.0, green: 180.0 / 255.0, blue: 195.0 / 255.0))
            .background(
                canSubmit
                    ? loginBlue
                    : Color(red: 238.0 / 255.0, green: 241.0 / 255.0, blue: 245.0 / 255.0),
                in: RoundedRectangle(cornerRadius: 10, style: .continuous)
            )
            .disabled(!canSubmit)
            .padding(.top, 16)

            Spacer(minLength: 32)
        }
        .padding(.horizontal, 28)
        .frame(maxWidth: 420)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color.white.ignoresSafeArea())
    }

    private static var applicationIcon: UIImage {
        guard let icons = Bundle.main.object(forInfoDictionaryKey: "CFBundleIcons") as? [String: Any],
              let primaryIcon = icons["CFBundlePrimaryIcon"] as? [String: Any],
              let iconFiles = primaryIcon["CFBundleIconFiles"] as? [String],
              let iconFile = iconFiles.last,
              let image = UIImage(named: iconFile)
        else {
            return UIImage()
        }
        return image
    }
}

private struct CompactTabBar: View {
    @Binding var selectedTab: AppTab
    @EnvironmentObject private var appState: RemoteIMAppState

    var body: some View {
        HStack(spacing: 8) {
            TabButton(
                title: "消息",
                systemImage: selectedTab == .messages ? "bubble.left.fill" : "bubble.left",
                selected: selectedTab == .messages,
                badgeCount: appState.totalUnreadCount
            ) {
                selectedTab = .messages
            }
            TabButton(
                title: "通讯录",
                systemImage: selectedTab == .contacts ? "person.2.fill" : "person.2",
                selected: selectedTab == .contacts
            ) {
                selectedTab = .contacts
            }
            TabButton(
                title: "远程",
                // `display.fill` is not available on the iOS 16 deployment target and
                // renders as an empty image. Selection is already conveyed by tint/background.
                systemImage: "display",
                selected: selectedTab == .remote
            ) {
                selectedTab = .remote
            }
            TabButton(
                title: "我",
                systemImage: selectedTab == .me ? "person.fill" : "person",
                selected: selectedTab == .me
            ) {
                selectedTab = .me
            }
        }
        .padding(6)
        .background(Color(red: 0.962, green: 0.97, blue: 0.98), in: RoundedRectangle(cornerRadius: 14, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .stroke(Color(red: 0.855, green: 0.894, blue: 0.941), lineWidth: 1)
        )
        .padding(.horizontal, 16)
        .padding(.top, 9)
        .padding(.bottom, 10)
        .frame(maxWidth: .infinity)
        .background(Color.white)
        .overlay(alignment: .top) {
            Divider().background(Color(red: 0.855, green: 0.894, blue: 0.941))
        }
    }
}

private struct TabButton: View {
    let title: String
    let systemImage: String
    let selected: Bool
    var badgeCount = 0
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            ZStack(alignment: .topTrailing) {
                Image(systemName: systemImage)
                    .font(.system(size: 18, weight: .semibold))
                    .frame(width: 28, height: 26)
                if badgeCount > 0 {
                    Circle()
                        .fill(Color(red: 1.0, green: 0.235, blue: 0.188))
                        .frame(width: 8, height: 8)
                        .offset(x: 6, y: -2)
                }
            }
            .frame(maxWidth: .infinity)
            .frame(height: 38)
        }
        .buttonStyle(.plain)
        .foregroundStyle(selected ? Color(red: 0.035, green: 0.376, blue: 0.667) : Color(red: 0.392, green: 0.459, blue: 0.561))
        .background(
            selected
                ? Color(red: 0.882, green: 0.957, blue: 1.0)
                : Color.clear,
            in: RoundedRectangle(cornerRadius: 10, style: .continuous)
        )
        .accessibilityLabel(title)
        .accessibilityValue(accessibilityValue)
    }

    private var accessibilityValue: String {
        [selected ? "已选择" : nil, badgeCount > 0 ? "\(badgeCount) 条未读" : nil]
            .compactMap { $0 }
            .joined(separator: "，")
    }
}
