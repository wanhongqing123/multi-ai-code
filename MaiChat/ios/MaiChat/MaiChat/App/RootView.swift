import MaiChatCore
import SwiftUI
import UIKit

enum AppTab {
    case messages
    case assistant
    case contacts
    case remote
    case me
}

struct RootView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @Environment(\.scenePhase) private var scenePhase
    @ObservedObject private var systemNotificationCenter = RemoteIMSystemNotificationCenter.shared
    @State private var selectedTab: AppTab = .messages
    @State private var previousMainTab: AppTab = .messages
    @State private var hasOpenedAssistant = false
    @State private var assistantOpenGeneration = 0
    @State private var activeChatContact: RemoteIMContact?
    @State private var isShowingAddContact = false
    @State private var movingContact: RemoteIMContact?
    @State private var contactGroupActionName: String?
    @State private var renamingContactGroup: String?
    @State private var contactGroupRenameDraft = ""
    @State private var deletingContactGroup: String?
    @State private var isShowingNotificationPermissionPrompt = false
    @State private var isVideoExpanded = false
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
                                },
                                bottomBar: AnyView(RootTabBar(
                                    selectedTab: $selectedTab,
                                    remoteDesktop: appState.remoteDesktop,
                                    openAssistant: openAssistant
                                ))
                            )
                        case .assistant:
                            Color.clear
                        case .contacts:
                            ContactsView(
                                selectedTab: $selectedTab,
                                activeContact: $activeChatContact,
                                movingContact: $movingContact,
                                groupActionName: $contactGroupActionName,
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

                    // Messages owns its bar inside the navigation root. An
                    // outer conditional bar appears too late during back swipes
                    // and resizes both pages as soon as selection is cleared.
                    if selectedTab != .messages && selectedTab != .assistant &&
                       activeChatContact == nil {
                        RootTabBar(
                            selectedTab: $selectedTab,
                            remoteDesktop: appState.remoteDesktop,
                            openAssistant: openAssistant
                        )
                    }
                }
            }

            if !appState.shouldShowInitialLogin &&
                (selectedTab == .assistant || hasOpenedAssistant) {
                AIAssistantView(
                    isActive: selectedTab == .assistant,
                    onExit: { selectedTab = previousMainTab }
                )
                .opacity(selectedTab == .assistant ? 1 : 0)
                .allowsHitTesting(selectedTab == .assistant)
                .accessibilityHidden(selectedTab != .assistant)
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
        .onChange(of: selectedTab) { tab in
            if tab != .assistant { assistantOpenGeneration += 1 }
            if tab == .assistant { hasOpenedAssistant = true }
            if tab != .assistant { previousMainTab = tab }
        }
        .animation(.easeOut(duration: 0.18), value: isShowingAddContact)
        .animation(.easeOut(duration: 0.18), value: movingContact?.userID)
        .animation(.easeOut(duration: 0.18), value: isShowingNotificationPermissionPrompt)
        .animation(.easeOut(duration: 0.18), value: contactGroupActionName)
        .animation(.easeOut(duration: 0.18), value: renamingContactGroup)
        .animation(.easeOut(duration: 0.18), value: deletingContactGroup)
        .background(Color(red: 0.966, green: 0.976, blue: 0.988).ignoresSafeArea())
        // 视频和创作中心共用一个窗口：缩小时保留输入区，AI 才能继续控制正在播放的实例。
        // 放大只改变同一个播放器的尺寸，不重新打开文件或重置播放位置。
        .overlay {
            GeometryReader { geometry in
                if let presentation = appState.agentVideoPresentation {
                    let width = min(geometry.size.width - 24, 340)
                    let height: CGFloat = 260
                    MaiFfplayVideoScreen(path: presentation.path) {
                        appState.agentVideoPresentation = nil
                    }
                    .frame(width: isVideoExpanded ? geometry.size.width : width,
                           height: isVideoExpanded ? geometry.size.height : height)
                    .overlay(alignment: .topTrailing) {
                        Button {
                            isVideoExpanded.toggle()
                        } label: {
                            Image(systemName: isVideoExpanded ? "arrow.down.right.and.arrow.up.left" :
                                  "arrow.up.left.and.arrow.down.right")
                                .font(.system(size: 17, weight: .semibold))
                                .foregroundStyle(.white)
                                .frame(width: 44, height: 44)
                                .background(.black.opacity(0.55), in: Circle())
                        }
                        .accessibilityLabel(isVideoExpanded ? "缩小视频，返回对话" : "放大视频")
                        .accessibilityIdentifier("ffplay-expand")
                        .padding(.top, isVideoExpanded ? max(geometry.safeAreaInsets.top, 58) + 12 : 12)
                        .padding(.trailing, 12)
                    }
                    .clipShape(RoundedRectangle(cornerRadius: isVideoExpanded ? 0 : 18))
                    .shadow(color: .black.opacity(isVideoExpanded ? 0 : 0.3), radius: 16)
                    .position(x: isVideoExpanded ? geometry.size.width / 2 :
                              geometry.size.width - width / 2 - 12,
                              y: isVideoExpanded ? geometry.size.height / 2 :
                              max(geometry.safeAreaInsets.top, 58) + height / 2 + 8)
                    .zIndex(100)
                }
            }
            .ignoresSafeArea()
        }
        .statusBarHidden(isVideoExpanded && appState.agentVideoPresentation != nil)
        .onChange(of: appState.agentVideoPresentation?.id) { _ in
            isVideoExpanded = false
        }
        .task {
            if !appState.shouldShowInitialLogin {
                await appState.connectOnLaunchIfNeeded()
                await offerNotificationPermissionIfNeeded()
            }
            appState.synchronizeSystemNotificationBadge()
            schedulePendingNotificationRoute(sceneIsActive: scenePhase == .active)
        }
        .onChange(of: appState.shouldShowInitialLogin) { needsLogin in
            if needsLogin {
                activeChatContact = nil
                selectedTab = .messages
                previousMainTab = .messages
                hasOpenedAssistant = false
                return
            }
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
                    .onTapGesture {
                        if appState.errorMessage == errorMessage {
                            appState.errorMessage = nil
                        }
                    }
                    .transition(.move(edge: .top).combined(with: .opacity))
            }
        }
        .animation(.easeOut(duration: 0.18), value: appState.errorMessage)
    }

    private func openAssistant() {
        if hasOpenedAssistant {
            logAIHistoryEvent("open reused selected=\(AIAssistantModel.shared.selected) last=\(AIAssistantModel.shared.messages.last?.id ?? "none")")
            selectedTab = .assistant
            return
        }
        logAIHistoryEvent("open first begin tab=\(selectedTab)")
        assistantOpenGeneration += 1
        let generation = assistantOpenGeneration
        let origin = selectedTab
        Task {
            await AIAssistantModel.shared.prepareFirstPage()
            guard assistantOpenGeneration == generation,
                  selectedTab == origin,
                  !appState.shouldShowInitialLogin else { return }
            logAIHistoryEvent("open first visible selected=\(AIAssistantModel.shared.selected) count=\(AIAssistantModel.shared.messages.count) last=\(AIAssistantModel.shared.messages.last?.id ?? "none")")
            selectedTab = .assistant
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
    let openAssistant: () -> Void

    var body: some View {
        if selectedTab != .remote || !remoteDesktop.state.isActive {
            CompactTabBar(selectedTab: $selectedTab, openAssistant: openAssistant)
        }
    }
}

private struct InitialLoginView: View {
    var body: some View {
        PhoneAccountLoginView()
    }
}

private struct CompactTabBar: View {
    @Binding var selectedTab: AppTab
    let openAssistant: () -> Void
    @EnvironmentObject private var appState: RemoteIMAppState

    var body: some View {
        HStack(spacing: 8) {
            TabButton(
                title: "消息",
                imageName: "NavMessages",
                accent: RemoteIMStyle.navMessages,
                selected: selectedTab == .messages,
                badgeCount: appState.totalUnreadCount
            ) {
                selectedTab = .messages
            }
            TabButton(
                title: "创作中心",
                imageName: "NavAssistant",
                accent: RemoteIMStyle.navAssistant,
                selected: selectedTab == .assistant
            ) {
                openAssistant()
            }
            TabButton(
                title: "通讯录",
                imageName: "NavContacts",
                accent: RemoteIMStyle.navContacts,
                selected: selectedTab == .contacts
            ) {
                selectedTab = .contacts
            }
            TabButton(
                title: "我",
                imageName: "NavProfile",
                accent: RemoteIMStyle.navProfile,
                selected: selectedTab == .me
            ) {
                selectedTab = .me
            }
        }
        .padding(.horizontal, 16)
        .padding(.top, 10)
        .padding(.bottom, 12)
        .frame(maxWidth: .infinity)
        .background(Color.white.ignoresSafeArea(.container, edges: .bottom))
        .overlay(alignment: .top) {
            Divider().background(Color(red: 0.855, green: 0.894, blue: 0.941))
        }
    }
}

private struct TabButton: View {
    let title: String
    let imageName: String
    let accent: Color
    let selected: Bool
    var badgeCount = 0
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            ZStack(alignment: .topTrailing) {
                Image(selected ? imageName + "Filled" : imageName)
                    .renderingMode(.template)
                    .resizable()
                    .scaledToFit()
                    .frame(width: 23, height: 23)
                if badgeCount > 0 {
                    Circle()
                        .fill(Color(red: 1.0, green: 0.235, blue: 0.188))
                        .frame(width: 8, height: 8)
                        .offset(x: 6, y: -2)
                }
            }
            .frame(width: 48, height: 40)
            .frame(maxWidth: .infinity)
        }
        .buttonStyle(.plain)
        .foregroundStyle(selected ? accent : RemoteIMStyle.textSecondary.opacity(0.78))
        .accessibilityLabel(title)
        .accessibilityValue(accessibilityValue)
    }

    private var accessibilityValue: String {
        [selected ? "已选择" : nil, badgeCount > 0 ? "\(badgeCount) 条未读" : nil]
            .compactMap { $0 }
            .joined(separator: "，")
    }
}
