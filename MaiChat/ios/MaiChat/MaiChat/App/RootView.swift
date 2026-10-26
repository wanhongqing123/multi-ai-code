// 文件作用：组织登录后的主导航，并协调 IM、创作中心、联系人、远程桌面和设置页。
// 创作中心首次打开后保留页面实例；切离再返回时继续使用原会话与滚动状态。
// Agent 打开视频时，这里把播放器放在上方或横屏侧边，并把剩余空间交给原页面。
// 登录、消息和模型业务状态分别来自 RemoteIMAppState 与 AIAssistantModel。
import MaiChatCore
import SwiftUI
import UIKit

enum AppTab {
    case messages
    case cloudDrive
    case assistant
    case contacts
    case remote
    case me
}

struct RootView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @Environment(\.scenePhase) private var scenePhase
    @ObservedObject private var systemNotificationCenter = RemoteIMSystemNotificationCenter.shared
    @ObservedObject private var videoCircle = MaiVideoCircleModel.shared
    @State private var selectedTab: AppTab = {
        #if DEBUG && targetEnvironment(simulator)
        if ProcessInfo.processInfo.arguments.contains(
            "--cloud-drive-production-canary-normal") { return .me }
        #endif
        return .messages
    }()
    @State private var previousMainTab: AppTab = .messages
    @State private var hasOpenedAssistant = false
    @State private var assistantOpenGeneration = 0
    @State private var activeChatContact: RemoteIMContact?
    @State private var activeChatGroup: RemoteIMGroup?
    @State private var isShowingCreateGroup = false
    @State private var pendingCreatedGroup: RemoteIMGroup?
    @State private var pendingAddFriendAfterGroup = false
    @State private var isShowingAddContact = false
    @State private var isShowingQRScanner = false
    @State private var pendingScannedFriendID: String?
    @State private var movingContact: RemoteIMContact?
    @State private var contactGroupActionName: String?
    @State private var renamingContactGroup: String?
    @State private var contactGroupRenameDraft = ""
    @State private var deletingContactGroup: String?
    @State private var isShowingNotificationPermissionPrompt = false
    @State private var isVideoExpanded = false
    @State private var galleryImage: MaiMediaSequence?
    @State private var galleryDirection = 0
    @State private var keyboardVisible = false
    @AppStorage("maichat.notification-preprompt-dismissed")
    private var notificationPermissionPromptDismissed = false

    var body: some View {
        GeometryReader { viewport in
            content(viewportSize: viewport.size)
        }
    }

    /// 按窗口尺寸组装主界面，并在同一页面实例上为 Agent 视频留出分屏空间。
    /// 登录页、主标签、创作中心和顶层弹层各有独立层级；播放器只覆盖自己的分区。
    /// 键盘与横竖屏变化只改变提议尺寸，不清理任何消息或重建创作中心状态。
    private func content(viewportSize: CGSize) -> some View {
        let landscape = viewportSize.width > viewportSize.height && viewportSize.width >= 600
        let hasDock = appState.agentVideoPresentation != nil && !isVideoExpanded
        // 外层窗口尺寸直接决定分屏，不把子视图测量值写回状态，避免旋转时反复重排。
        let dockHeight: CGFloat = hasDock && !landscape
            ? (keyboardVisible ? 240 : min(max(viewportSize.height * 0.62, 320), 560)) : 0
        let dockWidth: CGFloat = hasDock && landscape
            ? min(viewportSize.width * (keyboardVisible ? 0.42 : 0.52), viewportSize.width - 340) : 0
        return ZStack {
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
                                activeGroup: $activeChatGroup,
                                showRemoteDesktop: {
                                    activeChatContact = nil
                                    activeChatGroup = nil
                                    selectedTab = .remote
                                },
                                showCreateGroup: { isShowingCreateGroup = true },
                                showAddContact: {
                                    appState.newContactUserID = ""
                                    isShowingAddContact = true
                                },
                                showScanQRCode: {
                                    isShowingQRScanner = true
                                },
                                bottomBar: AnyView(RootTabBar(
                                    selectedTab: $selectedTab,
                                    remoteDesktop: appState.remoteDesktop,
                                    openAssistant: openAssistant
                                ))
                            )
                        case .cloudDrive:
                            NavigationStack {
                                MaiCloudDriveView()
                            }
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

                    // 消息页的底栏位于其导航根内；若改由外层条件插入，
                    // 返回手势期间出现时机会滞后，并在清除选中项时改变两页尺寸。
                    if selectedTab != .messages && selectedTab != .assistant &&
                       activeChatContact == nil && activeChatGroup == nil {
                        RootTabBar(
                            selectedTab: $selectedTab,
                            remoteDesktop: appState.remoteDesktop,
                            openAssistant: openAssistant
                        )
                    }
                }
            }

            // 创作中心第一次打开后继续留在视图树中；切换主标签只控制可见性。
            // 保留同一页面实例可避免回来时重新加载历史或遗失当前输入。
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

            if let galleryImage {
                MaiGalleryImageScreen(sequence: galleryImage,
                                      entryDirection: galleryDirection,
                                      navigate: { direction in
                    guard let next = galleryImage.adjacent(direction) else { return false }
                    galleryDirection = direction
                    if next.current.kind == .video {
                        self.galleryImage = nil
                        appState.agentVideoPresentation = MaiFfplayPresentation(
                            sequence: next, entryDirection: direction)
                    } else {
                        self.galleryImage = next
                    }
                    return true
                }, close: { self.galleryImage = nil })
                    .id(galleryImage.current.path)
                    .zIndex(1000)
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
        // 内容区先得到扣除播放器后的尺寸，再把播放器单独叠在窗口上方。
        .padding(.top, dockHeight)
        .padding(.leading, dockWidth)
        .background(Color(red: 0.966, green: 0.976, blue: 0.988).ignoresSafeArea())
        // 竖屏上下分区，横屏左右分区；聊天或创作中心始终保留可触摸的输入区。
        // 全屏切换只改变同一个播放器的尺寸，不重新打开文件或重置播放位置。
        .overlay {
            if let presentation = appState.agentVideoPresentation {
                GeometryReader { geometry in
                    MaiFfplayVideoScreen(path: presentation.path,
                                         startSeconds: presentation.startSeconds,
                                         entryDirection: presentation.entryDirection,
                                         navigate: { direction in
                        guard let next = presentation.sequence.adjacent(direction) else {
                            return false
                        }
                        if next.current.kind == .video {
                            appState.agentVideoPresentation = MaiFfplayPresentation(
                                sequence: next, entryDirection: direction)
                        } else {
                            appState.agentVideoPresentation = nil
                            galleryDirection = direction
                            galleryImage = next
                        }
                        return true
                    }, vod: presentation.vod) {
                        appState.agentVideoPresentation = nil
                    }
                    .id(presentation.id)
                    .frame(width: isVideoExpanded ? geometry.size.width :
                           (landscape ? dockWidth : geometry.size.width),
                           height: isVideoExpanded ? geometry.size.height :
                           (landscape ? geometry.size.height : dockHeight))
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
                        .accessibilityLabel(isVideoExpanded ? "返回分屏" : "放大视频")
                        .accessibilityIdentifier("ffplay-expand")
                        .padding(.top, isVideoExpanded ? max(geometry.safeAreaInsets.top, 58) + 12 : 12)
                        .padding(.trailing, 12)
                    }
                    .overlay(alignment: .top) {
                        if presentation.playlist.count > 1 {
                            Text("\(presentation.index + 1)/\(presentation.playlist.count) · " +
                                 URL(fileURLWithPath: presentation.path).lastPathComponent)
                                .font(.system(size: 12, weight: .medium))
                                .lineLimit(1)
                                .foregroundStyle(.white)
                                .padding(.horizontal, 10)
                                .padding(.vertical, 6)
                                .background(.black.opacity(0.55), in: Capsule())
                                .padding(.horizontal, 76)
                                .padding(.top, 22)
                                .allowsHitTesting(false)
                        }
                    }
                    .position(x: isVideoExpanded || !landscape ?
                              geometry.size.width / 2 : dockWidth / 2,
                              y: isVideoExpanded || landscape ?
                              geometry.size.height / 2 : dockHeight / 2)
                    .zIndex(100)
                }
                .ignoresSafeArea(edges: isVideoExpanded ? .all : [])
            }
        }
        .statusBarHidden(galleryImage != nil ||
                         (isVideoExpanded && appState.agentVideoPresentation != nil))
        .onChange(of: appState.agentVideoPresentation?.groupID) { _ in
            isVideoExpanded = false
        }
        .onReceive(NotificationCenter.default.publisher(for: UIResponder.keyboardWillShowNotification)) { _ in
            keyboardVisible = true
        }
        .onReceive(NotificationCenter.default.publisher(for: UIResponder.keyboardWillHideNotification)) { _ in
            keyboardVisible = false
        }
        .task {
            if !appState.shouldShowInitialLogin {
                await appState.connectOnLaunchIfNeeded()
                await offerNotificationPermissionIfNeeded()
            }
            appState.synchronizeSystemNotificationBadge()
            schedulePendingNotificationRoute(sceneIsActive: scenePhase == .active)
        }
        .task(id: scenePhase == .active && appState.hasUnifiedAccount &&
              !appState.shouldShowInitialLogin) {
            guard scenePhase == .active, appState.hasUnifiedAccount,
                  !appState.shouldShowInitialLogin else { return }
            appState.refreshAvatarImagesForCurrentUsers()
            // 进入前台立即检查；前台每 30 秒复查。切到后台时 SwiftUI 取消该任务。
            while !Task.isCancelled && appState.hasUnifiedAccount &&
                  !appState.shouldShowInitialLogin {
                await appState.validateCurrentAccountSession()
                do {
                    try await Task.sleep(for: .seconds(30))
                } catch {
                    return
                }
            }
        }
        .onChange(of: appState.shouldShowInitialLogin) { needsLogin in
            if needsLogin {
                activeChatContact = nil
                activeChatGroup = nil
                selectedTab = .messages
                previousMainTab = .messages
                hasOpenedAssistant = false
                return
            }
            Task {
                await offerNotificationPermissionIfNeeded()
            }
        }
        .onChange(of: appState.masterUserID) { _ in
            clearSelectedChatForAccountChange()
        }
        .onChange(of: appState.maiChatAccountID) { _ in
            clearSelectedChatForAccountChange()
        }
        .onChange(of: appState.groups.map(\.groupID)) { joinedGroupIDs in
            if let activeChatGroup,
               !joinedGroupIDs.contains(activeChatGroup.groupID) {
                self.activeChatGroup = nil
            }
            if let pendingCreatedGroup,
               !joinedGroupIDs.contains(pendingCreatedGroup.groupID) {
                self.pendingCreatedGroup = nil
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
                    .lineLimit(2)
                    .truncationMode(.tail)
                    .accessibilityLabel(errorMessage)
                    .padding(.horizontal, 14)
                    .padding(.vertical, 9)
                    .background(.red, in: Capsule())
                    // Keep transient IM errors below the 54-point page header.
                    // A long error previously covered every top-bar action.
                    .padding(.top, 64)
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
        .sheet(isPresented: $isShowingCreateGroup, onDismiss: {
            if let pendingCreatedGroup {
                activeChatContact = nil
                activeChatGroup = pendingCreatedGroup
                self.pendingCreatedGroup = nil
            } else if pendingAddFriendAfterGroup {
                pendingAddFriendAfterGroup = false
                appState.newContactUserID = ""
                isShowingAddContact = true
            }
        }) {
            GroupCreationView(
                friends: appState.chatState.contacts.filter {
                    appState.acceptedFriendIMIDs.contains($0.userID)
                },
                recoveryGroup: appState.groupCreationRecovery,
                refreshFriends: { await appState.refreshAcceptedFriends() },
                create: { name, memberUserIDs in
                    guard let group = await appState.createGroup(
                        name: name, memberUserIDs: memberUserIDs
                    ) else { return false }
                    pendingCreatedGroup = group
                    return true
                },
                addFriend: {
                    pendingAddFriendAfterGroup = true
                    isShowingCreateGroup = false
                },
                openRecoveredGroup: {
                    guard let group = appState.groupCreationRecovery else { return }
                    appState.selectGroup(groupID: group.groupID)
                    pendingCreatedGroup = group
                    isShowingCreateGroup = false
                }
            )
        }
        .sheet(isPresented: $isShowingQRScanner, onDismiss: {
            guard let pendingScannedFriendID else { return }
            appState.newContactUserID = pendingScannedFriendID
            self.pendingScannedFriendID = nil
            isShowingAddContact = true
        }) {
            MaiChatQRScanSheet { userID in
                pendingScannedFriendID = userID
                isShowingQRScanner = false
            }
        }
        .sheet(item: $videoCircle.presentation) { presentation in
            switch presentation {
            case .circle:
                MaiVideoCircleView { videoCircle.presentation = nil }
            case let .publish(source):
                MaiVideoCirclePublishView(source: source,
                                          close: { videoCircle.presentation = nil },
                                          published: { videoCircle.presentation = .circle })
            }
        }
    }

    private func openAssistant() {
        if hasOpenedAssistant {
            logAIHistoryEvent(
                """
                open reused selected=\(AIAssistantModel.shared.selected) \
                last=\(AIAssistantModel.shared.messages.last?.id ?? "none")
                """
            )
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
            logAIHistoryEvent(
                """
                open first visible selected=\(AIAssistantModel.shared.selected) \
                count=\(AIAssistantModel.shared.messages.count) \
                last=\(AIAssistantModel.shared.messages.last?.id ?? "none")
                """
            )
            selectedTab = .assistant
        }
    }

    private func offerNotificationPermissionIfNeeded() async {
#if targetEnvironment(simulator)
        if ProcessInfo.processInfo.arguments.contains("--im-group-ui-test") ||
            ProcessInfo.processInfo.arguments.contains("--im-group-activation-fail-ui-test") {
            return
        }
#endif
        await systemNotificationCenter.refreshAuthorizationStatus()
        guard systemNotificationCenter.authorizationStatus == .notDetermined,
              !notificationPermissionPromptDismissed
        else { return }
        isShowingNotificationPermissionPrompt = true
    }

    private func clearSelectedChatForAccountChange() {
        activeChatContact = nil
        activeChatGroup = nil
        pendingCreatedGroup = nil
        isShowingCreateGroup = false
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
        // 通知点击到达时场景可能仍处于 inactive；推迟到主队列下一轮，
        // 避免在 UIKit 状态恢复事务中直接跳转会话。
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
        activeChatGroup = nil
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
                title: "云盘",
                imageName: "",
                accent: RemoteIMStyle.blue,
                selected: selectedTab == .cloudDrive,
                systemImageName: "icloud"
            ) {
                selectedTab = .cloudDrive
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
                title: "好友列表",
                imageName: "NavContacts",
                accent: RemoteIMStyle.navContacts,
                selected: selectedTab == .contacts
            ) {
                selectedTab = .contacts
            }
            TabButton(
                title: "我的",
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
    var systemImageName: String? = nil
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            ZStack(alignment: .topTrailing) {
                if let systemImageName {
                    Image(systemName: selected ? systemImageName + ".fill" : systemImageName)
                        .font(.system(size: 23))
                        .frame(width: 23, height: 23)
                } else {
                    Image(selected ? imageName + "Filled" : imageName)
                        .renderingMode(.template)
                        .resizable()
                        .scaledToFit()
                        .frame(width: 23, height: 23)
                }
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
