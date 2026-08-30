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
    @State private var selectedTab: AppTab = .messages
    @State private var activeChatContact: RemoteIMContact?
    @State private var isShowingAddContact = false
    @State private var movingContact: RemoteIMContact?

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
        }
        .animation(.easeOut(duration: 0.18), value: isShowingAddContact)
        .animation(.easeOut(duration: 0.18), value: movingContact?.userID)
        .background(Color(red: 0.966, green: 0.976, blue: 0.988).ignoresSafeArea())
        .task {
            if !appState.shouldShowInitialLogin {
                await RemoteIMSystemNotificationCenter.shared.requestAuthorizationIfNeeded()
                await appState.connectOnLaunchIfNeeded()
            }
            appState.synchronizeSystemNotificationBadge()
            if let peerUserID = RemoteIMSystemNotificationCenter.shared.consumePendingPeerUserID() {
                openNotificationConversation(peerUserID)
            }
        }
        .onChange(of: appState.shouldShowInitialLogin) { needsLogin in
            guard !needsLogin else { return }
            Task {
                await RemoteIMSystemNotificationCenter.shared.requestAuthorizationIfNeeded()
            }
        }
        .onReceive(
            NotificationCenter.default.publisher(for: .remoteIMNotificationConversationSelected)
        ) { notification in
            guard let peerUserID = notification.object as? String else { return }
            openNotificationConversation(peerUserID)
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
            fields: ["peer": DiagnosticLogPrivacy.stableTag(peerUserID, prefix: "u")]
        )
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
