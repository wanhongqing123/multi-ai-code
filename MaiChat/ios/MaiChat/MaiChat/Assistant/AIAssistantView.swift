import AVFoundation
import CoreTransferable
import MaiChatCore
import Photos
import PhotosUI
import SwiftUI
import UIKit
import UniformTypeIdentifiers

private enum AIImagePreviewLayout {
    static let coordinateSpaceName = "ai-assistant-image-preview-space"
}

struct AIAssistantView: View {
    let isActive: Bool
    let onExit: (() -> Void)?
    @ObservedObject private var model = AIAssistantModel.shared
    @Environment(\.scenePhase) private var scenePhase
    @State private var showSessions = false
    @State private var showActions = false
    @State private var confirmClear = false
    @State private var sessionDrawerOffset: CGFloat = 0
    @State private var sessionDrawerWidth: CGFloat = 320
    @State private var isAttachmentPanelPresented = false
    @State private var composerFocusController = AIComposerFocusController()
    @State private var transcriptionPresentation = VoiceTranscriptionPresentation()

    init(isActive: Bool = true, onExit: (() -> Void)? = nil) {
        self.isActive = isActive
        self.onExit = onExit
    }

    var body: some View {
        ZStack(alignment: .topTrailing) {
            VStack(spacing: 0) {
                AIHeader(
                    exit: onExit == nil ? nil : {
                        composerFocusController.dismiss()
                        isAttachmentPanelPresented = false
                        showActions = false
                        showSessions = false
                        onExit?()
                    },
                    openSessions: {
                        composerFocusController.dismiss()
                        isAttachmentPanelPresented = false
                        showActions = false
                        openSessionDrawer()
                    },
                    openActions: {
                        composerFocusController.dismiss()
                        isAttachmentPanelPresented = false
                        showActions.toggle()
                    }
                )
                Divider().overlay(Color.black.opacity(0.06))
                if !model.ready {
                    Color.clear.frame(maxWidth: .infinity, maxHeight: .infinity)
                } else {
                    AIAssistantMessageList(
                        model: model,
                        focusController: composerFocusController,
                        isAttachmentPanelPresented: $isAttachmentPanelPresented,
                        isActive: isActive
                    )
                }
                if !model.error.isEmpty {
                    HStack(alignment: .top) {
                        Image(systemName: "exclamationmark.circle")
                        Text(model.error).font(.caption).textSelection(.enabled)
                        Spacer(minLength: 0)
                        Button { model.error = "" } label: { Image(systemName: "xmark") }
                    }.foregroundStyle(.red).padding(12).background(Color.red.opacity(0.05))
                }
                AIComposer(
                    model: model,
                    focusController: composerFocusController,
                    transcriptionPresentation: transcriptionPresentation,
                    isAttachmentPanelPresented: $isAttachmentPanelPresented
                )
            }
            .background(Color(uiColor: .systemBackground))
            if showActions {
                Color.black.opacity(0.001).ignoresSafeArea().contentShape(Rectangle())
                    .onTapGesture { showActions = false }
                AIActionPanel(
                    selectedModel: model.settings.model,
                    selectedPolicy: model.settings.policy,
                    canChangeSettings: model.configured && !model.busy && !model.isSubmitting,
                    canClear: !model.busy && !model.selected.isEmpty,
                    selectModel: { selected in
                        guard selected != model.settings.model else { return }
                        var next = model.settings
                        next.model = selected
                        Task { _ = await model.save(next, key: "") }
                    },
                    selectPolicy: { policy in
                        guard policy != model.settings.policy else { return }
                        var next = model.settings
                        next.policy = policy
                        Task { _ = await model.save(next, key: "") }
                    },
                    configureModel: {
                        showActions = false
                        model.showSettings = true
                    },
                    clear: { showActions = false; confirmClear = true }
                )
                .padding(.top, 54).padding(.trailing, 14)
                .transition(.scale(scale: 0.96, anchor: .topTrailing).combined(with: .opacity))
            }
            GeometryReader { geometry in
                let width = min(360, geometry.size.width * 0.86)
                let offset = sessionDrawerX(width: width)
                let progress = max(0, min(1, 1 + offset / width))
                ZStack(alignment: .leading) {
                    Color.black.opacity(0.16 * progress)
                        .ignoresSafeArea()
                        .contentShape(Rectangle())
                        .onTapGesture { closeSessionDrawer() }
                    sessionList
                        .frame(width: width)
                        .offset(x: offset)
                        .shadow(color: Color.black.opacity(0.18 * progress), radius: 22, x: 8)
                        .accessibilityHidden(!showSessions)
                }
                .onAppear { sessionDrawerWidth = width }
                .onChange(of: geometry.size.width) { _ in sessionDrawerWidth = width }
                .simultaneousGesture(sessionDrawerCloseGesture(width: width))
            }
            .allowsHitTesting(showSessions)
            .accessibilityHidden(!showSessions)
            .zIndex(3)
            if model.showSettings {
                AISettingsView(model: model) { model.showSettings = false }
                    .transition(.move(edge: .trailing).combined(with: .opacity))
                    .zIndex(5)
            }
            VoiceTranscriptionHighlightHost(presentation: transcriptionPresentation)
                .zIndex(20)
            if let preview = model.previewImage {
                AIImagePreviewOverlay(preview: preview) {
                    model.previewImage = nil
                }
                .id(preview.id)
                .zIndex(30)
            }
        }
        .coordinateSpace(name: AIImagePreviewLayout.coordinateSpaceName)
        .simultaneousGesture(sessionDrawerOpenGesture)
        .confirmationDialog("清空当前对话的所有消息？", isPresented: $confirmClear, titleVisibility: .visible) {
            Button("清空消息", role: .destructive) { Task { await model.action("clear") } }
        }
        .onAppear {
            if isActive &&
                !ProcessInfo.processInfo.arguments.contains("--ai-history-ui-test") {
                model.appear()
            }
        }
        .onChange(of: isActive) { active in
            guard !ProcessInfo.processInfo.arguments.contains("--ai-history-ui-test") else {
                return
            }
            if active {
                model.setPageActive(true)
            } else {
                composerFocusController.dismiss()
                isAttachmentPanelPresented = false
                transcriptionPresentation.onCancel?()
                model.setPageActive(false)
            }
        }
        .onDisappear {
            if !ProcessInfo.processInfo.arguments.contains("--ai-history-ui-test") {
                model.disappear()
            }
        }
        .onChange(of: scenePhase) { phase in
            if ProcessInfo.processInfo.arguments.contains("--ai-history-ui-test") {
                return
            }
            if phase == .active { model.appear() }
            else { model.disappear() }
        }
    }

    private var drawerAnimation: Animation {
        .interactiveSpring(response: 0.28, dampingFraction: 0.9)
    }

    private var sessionDrawerOpenGesture: some Gesture {
        DragGesture(minimumDistance: 12, coordinateSpace: .global)
            .onChanged { value in
                guard !showSessions,
                      value.startLocation.x <= 32,
                      value.translation.width > 0,
                      abs(value.translation.width) > abs(value.translation.height)
                else { return }
                composerFocusController.dismiss()
                showActions = false
                sessionDrawerOffset = min(sessionDrawerWidth, value.translation.width)
            }
            .onEnded { value in
                guard !showSessions, sessionDrawerOffset > 0 else { return }
                let shouldOpen = value.translation.width >= 72
                    || value.predictedEndTranslation.width >= sessionDrawerWidth * 0.45
                withAnimation(drawerAnimation) {
                    showSessions = shouldOpen
                    sessionDrawerOffset = 0
                }
            }
    }

    private func sessionDrawerCloseGesture(width: CGFloat) -> some Gesture {
        DragGesture(minimumDistance: 12, coordinateSpace: .global)
            .onChanged { value in
                guard showSessions,
                      value.translation.width < 0,
                      abs(value.translation.width) > abs(value.translation.height)
                else { return }
                sessionDrawerOffset = max(-width, value.translation.width)
            }
            .onEnded { value in
                guard showSessions, sessionDrawerOffset < 0 else { return }
                let shouldClose = value.translation.width <= -72
                    || value.predictedEndTranslation.width <= -width * 0.45
                if shouldClose {
                    closeSessionDrawer()
                } else {
                    withAnimation(drawerAnimation) { sessionDrawerOffset = 0 }
                }
            }
    }

    private func sessionDrawerX(width: CGFloat) -> CGFloat {
        if showSessions { return min(0, sessionDrawerOffset) }
        return -width + max(0, min(width, sessionDrawerOffset))
    }

    private func openSessionDrawer() {
        withAnimation(drawerAnimation) {
            showSessions = true
            sessionDrawerOffset = 0
        }
    }

    private func closeSessionDrawer() {
        withAnimation(drawerAnimation) {
            showSessions = false
            sessionDrawerOffset = 0
        }
    }

    private var sessionList: some View {
        VStack(spacing: 0) {
            HStack {
                Text("对话").font(.system(size: 20, weight: .bold))
                Spacer()
                Button("完成") { closeSessionDrawer() }
                    .font(.system(size: 14, weight: .semibold)).buttonStyle(.plain)
            }.padding(.horizontal, 20).padding(.vertical, 18)
            Divider().overlay(Color.black.opacity(0.06))
            ScrollView {
                LazyVStack(spacing: 8) {
                ForEach(model.sessions) { session in
                    HStack(spacing: 8) {
                        Button {
                            closeSessionDrawer()
                            Task { await model.select(session.id) }
                        } label: {
                            HStack {
                            Text(session.displayTitle).font(ChatTypography.conversationTitle).lineLimit(2)
                                Spacer(minLength: 12)
                                if session.busy { ProgressView() }
                                else if session.id == model.selected {
                                    Image(systemName: "checkmark").foregroundStyle(Color.blue)
                                }
                            }
                            .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        .foregroundStyle(Color.primary)
                        .accessibilityIdentifier("ai-conversation-\(session.id)")
                        if !session.busy {
                            Button {
                                Task { await model.select(session.id); await model.action("delete") }
                            } label: {
                                Image(systemName: "trash").foregroundStyle(Color.secondary)
                                    .frame(width: 44, height: 44)
                            }.buttonStyle(.plain).accessibilityLabel("删除 \(session.displayTitle)")
                        }
                    }
                    .padding(.horizontal, 14).padding(.vertical, 8)
                    .background(session.id == model.selected ? Color.blue.opacity(0.08) : Color.clear,
                                in: RoundedRectangle(cornerRadius: 12))
                }
                }.padding(.horizontal, 16)
            }
            HStack {
                Button {
                    closeSessionDrawer()
                    Task { await model.create() }
                } label: {
                    HStack(spacing: 9) {
                        Image(systemName: "square.and.pencil").font(.system(size: 14, weight: .semibold))
                        Text("新对话").font(.system(size: 14, weight: .semibold))
                    }.foregroundStyle(Color.white).padding(.horizontal, 16).frame(height: 42)
                        .background(Color.primary, in: Capsule())
                }.buttonStyle(.plain)
                Spacer()
            }.padding(16).overlay(alignment: .top) { Divider().overlay(Color.black.opacity(0.06)) }
        }
        .background(Color(uiColor: .systemBackground))
        .accessibilityIdentifier("ai-session-drawer")
    }
}

private final class AIMessageViewportLog {
    private let state = MessageScrollViewportState()
    private var session = ""
    private var visibleHeight = -1
    private var lastReportedNearBottom: Bool?
    var nearBottom: Bool { state.nearBottom }

    func record(session: String, content: CGFloat, visible: CGFloat,
                offset: CGFloat, remaining: CGFloat, nearBottom: Bool) {
        let height = Int(visible.rounded())
        state.record(visible: visible, nearBottom: nearBottom)
        guard self.session != session || visibleHeight != height ||
            lastReportedNearBottom != nearBottom else {
            return
        }
        self.session = session
        visibleHeight = height
        lastReportedNearBottom = nearBottom
        logAIHistoryEvent("viewport session=\(session) content=\(Int(content)) visible=\(height) offset=\(Int(offset)) remaining=\(Int(remaining)) nearBottom=\(nearBottom)")
    }

    func nearBottomBeforeKeyboard(opening: Bool) -> Bool {
        state.nearBottomBeforeKeyboard(opening: opening)
    }

    func finishKeyboardTransition() { state.finishKeyboardTransition() }

    func reset() {
        state.reset()
        session = ""
        visibleHeight = -1
        lastReportedNearBottom = nil
    }
}

// Owns AI message positioning. Data refresh supplies stable message IDs;
// keyboard resizing and streamed parts never replace the current scroll intent.
private struct AIAssistantMessageList: View {
    @ObservedObject var model: AIAssistantModel
    let focusController: AIComposerFocusController
    @Binding var isAttachmentPanelPresented: Bool
    let isActive: Bool
    @State private var followsLatest = true
    @State private var prependAnchorID: String?
    @State private var viewportLog = AIMessageViewportLog()
    @State private var keyboardFollowIntent: Bool?
    @State private var keyboardTransitionOpening: Bool?
    @State private var bottomCorrectionRequest = 0

    var body: some View {
        GeometryReader { geometry in
            ScrollViewReader { proxy in
                ScrollView {
                    VStack(alignment: .leading, spacing: 24) {
                        if model.messages.isEmpty && model.historyLoaded {
                            welcome
                        }
                        if !model.messages.isEmpty {
                            VStack(alignment: .leading, spacing: 24) {
                                ForEach(model.messages) { message in
                                    AIMessageRow(message: message,
                                                 workspacePath: model.workspacePath,
                                                 quote: {
                                                     model.quotedMessage = message
                                                     focusController.focus()
                                                 })
                                        .id(message.id)
                                }
                            }
                        }
                        ForEach(model.permissions) { permission in
                            AIPermissionCard(permission: permission, model: model)
                                .id(permission.id)
                        }
                        ForEach(model.questions) { question in
                            AIQuestionCard(question: question, model: model)
                                .id(question.id)
                        }
                    }
                    .padding(.horizontal, 24)
                    .padding(.top, 18)
                    .padding(.bottom, 24)
                    .frame(minHeight: geometry.size.height, alignment: .bottom)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(alignment: .bottom) {
                        MessageScrollPositionReader(
                            restoreInitialScrollableHistory: isActive && followsLatest &&
                                !model.messages.isEmpty,
                            allowsBottomFollowing: isActive && followsLatest,
                            bottomCorrectionRequest: bottomCorrectionRequest,
                            userScrollAwayThreshold: 60,
                            minimumOlderDragDistance: 24,
                            onUserScroll: {
                                logAIHistoryEvent("user-scroll session=\(model.selected)")
                                followsLatest = false
                            },
                            onUserReachedTop: {
                                guard prependAnchorID == nil,
                                      let anchor = model.messages.first?.id else { return }
                                prependAnchorID = anchor
                                Task {
                                    await model.loadOlderMessages()
                                    if model.messages.first?.id == anchor {
                                        prependAnchorID = nil
                                    }
                                }
                            },
                            onViewportMeasured: { content, viewport, offset, remaining, nearBottom in
                                viewportLog.record(session: model.selected, content: content,
                                                   visible: viewport, offset: offset,
                                                   remaining: remaining, nearBottom: nearBottom)
                            }
                        ) { nearBottom in
                            if nearBottom && keyboardFollowIntent != false {
                                followsLatest = true
                            }
                        }
                        .frame(height: 1)
                    }
                }
                .contentShape(Rectangle())
                .onTapGesture {
                    focusController.dismiss()
                    isAttachmentPanelPresented = false
                }
                .onAppear {
                    logAIHistoryEvent("message-list appear session=\(model.selected) count=\(model.messages.count) first=\(model.messages.first?.id ?? "none") last=\(model.messages.last?.id ?? "none")")
                }
                .onChange(of: model.messages.first?.id) { _ in
                    if let anchor = prependAnchorID {
                        prependAnchorID = nil
                        position(at: anchor, proxy: proxy, anchor: .top)
                    }
                }
                .onChange(of: model.scrollRequest) { _ in
                    followsLatest = true
                    bottomCorrectionRequest += 1
                    logAIHistoryEvent("sent session=\(model.selected) following latest")
                }
                .onChange(of: model.selected) { _ in
                    prependAnchorID = nil
                    followsLatest = true
                    viewportLog.reset()
                }
                .onChange(of: isActive) { active in
                    guard active else { return }
                    followsLatest = true
                }
                .onReceive(NotificationCenter.default.publisher(
                    for: UIResponder.keyboardWillShowNotification)) { _ in
                    captureKeyboardFollowIntent(opening: true)
                }
                .onReceive(NotificationCenter.default.publisher(
                    for: UIResponder.keyboardWillHideNotification)) { _ in
                    captureKeyboardFollowIntent(opening: false)
                }
                .onReceive(NotificationCenter.default.publisher(
                    for: UIResponder.keyboardDidShowNotification)) { _ in
                    if keyboardFollowIntent == true { bottomCorrectionRequest += 1 }
                    viewportLog.finishKeyboardTransition()
                    keyboardFollowIntent = nil
                    keyboardTransitionOpening = nil
                }
                .onReceive(NotificationCenter.default.publisher(
                    for: UIResponder.keyboardDidHideNotification)) { _ in
                    if keyboardFollowIntent == true { bottomCorrectionRequest += 1 }
                    viewportLog.finishKeyboardTransition()
                    keyboardFollowIntent = nil
                    keyboardTransitionOpening = nil
                }
            }
        }
    }

    private func position(at id: String, proxy: ScrollViewProxy, anchor: UnitPoint) {
        logAIHistoryEvent("scroll-request session=\(model.selected) target=\(id) first=\(model.messages.first?.id ?? "none") last=\(model.messages.last?.id ?? "none")")
        var transaction = Transaction(animation: nil)
        transaction.disablesAnimations = true
        withTransaction(transaction) { proxy.scrollTo(id, anchor: anchor) }
    }

    private func captureKeyboardFollowIntent(opening: Bool) {
        guard isActive else { return }
        if keyboardTransitionOpening == opening { return }
        keyboardTransitionOpening = opening
        let nearBottom = viewportLog.nearBottomBeforeKeyboard(opening: opening)
        keyboardFollowIntent = nearBottom
        followsLatest = nearBottom
        logAIHistoryEvent("keyboard session=\(model.selected) opening=\(opening) nearBottomBefore=\(nearBottom)")
    }

    private var welcome: some View {
        VStack(spacing: 14) {
            Image(systemName: "sparkles").font(.largeTitle).foregroundStyle(.blue)
            Text("有什么可以帮你？").font(.title3.bold())
            Text("可以聊天、分析文本和处理导入的文件。")
                .font(.subheadline).foregroundStyle(.secondary)
            if !model.configured {
                Button("配置模型", action: { model.showSettings = true })
                    .buttonStyle(.plain).font(.system(size: 14, weight: .semibold))
                    .foregroundStyle(Color.white).padding(.horizontal, 16).frame(height: 38)
                    .background(Color.blue, in: RoundedRectangle(cornerRadius: 10))
            }
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 70)
    }
}

private struct AIHeader: View {
    let exit: (() -> Void)?
    let openSessions: () -> Void
    let openActions: () -> Void

    var body: some View {
        HStack(spacing: 8) {
            if let exit {
                AIHeaderButton(systemImage: "chevron.left", label: "返回主页面", action: exit)
            }
            AIHeaderButton(systemImage: "sidebar.left", label: "对话列表", action: openSessions)
            Spacer()
            AIHeaderButton(systemImage: "ellipsis", label: "更多", action: openActions)
        }
        .overlay { Text("AI 助手").font(.system(size: 18, weight: .bold)).allowsHitTesting(false) }
        .padding(.horizontal, 16).frame(height: 54)
        .background(Color(uiColor: .systemBackground))
    }
}

private struct AIHeaderButton: View {
    let systemImage: String
    let label: String
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: systemImage).font(.system(size: 16, weight: .semibold))
                .frame(width: 34, height: 34)
                .background(Color(uiColor: .secondarySystemBackground), in: RoundedRectangle(cornerRadius: 10))
        }.buttonStyle(.plain).foregroundStyle(Color.primary).accessibilityLabel(label)
    }
}

private struct AIActionPanel: View {
    let selectedModel: String
    let selectedPolicy: String
    let canChangeSettings: Bool
    let canClear: Bool
    let selectModel: (String) -> Void
    let selectPolicy: (String) -> Void
    let configureModel: () -> Void
    let clear: () -> Void
    @State private var expandedSection: Section?

    private enum Section: Equatable {
        case model
        case policy
    }

    var body: some View {
        VStack(spacing: 0) {
            expandableAction(
                title: "模型",
                value: selectedModel.isEmpty ? "未配置" : selectedModel,
                image: "sparkles",
                section: .model
            )
            if expandedSection == .model {
                optionGroup(identifier: "ai-model-menu") {
                    modelOption("glm-5.3")
                    modelOption("glm-5.3-flash")
                }
            }
            Divider().padding(.leading, 42)
            expandableAction(
                title: "操作权限",
                value: policyTitle(selectedPolicy),
                image: "shield",
                section: .policy
            )
            if expandedSection == .policy {
                optionGroup(identifier: "ai-policy-menu") {
                    policyOption("请求批准", value: "on-request")
                    policyOption("帮我批准", value: "unless-trusted")
                    policyOption("完全访问", value: "never")
                }
            }
            Divider().padding(.leading, 42)
            action("模型配置", "slider.horizontal.3", Color.primary, configureModel)
            Divider().padding(.leading, 42)
            action("清空当前对话", "trash", canClear ? Color.red : Color.secondary, clear)
                .disabled(!canClear)
        }.frame(width: 264).background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14))
            .overlay(RoundedRectangle(cornerRadius: 14).stroke(Color.black.opacity(0.08)))
            .shadow(color: Color.black.opacity(0.14), radius: 20, y: 8)
    }

    private func expandableAction(
        title: String,
        value: String,
        image: String,
        section: Section
    ) -> some View {
        Button {
            withAnimation(.easeOut(duration: 0.14)) {
                expandedSection = expandedSection == section ? nil : section
            }
        } label: {
            HStack(spacing: 12) {
                Image(systemName: image).frame(width: 18)
                VStack(alignment: .leading, spacing: 2) {
                    Text(title).font(.system(size: 14, weight: .medium))
                    Text(value).font(.system(size: 11)).foregroundStyle(Color.secondary)
                        .lineLimit(1)
                }
                Spacer(minLength: 6)
                Image(systemName: "chevron.right")
                    .font(.system(size: 11, weight: .semibold))
                    .rotationEffect(.degrees(expandedSection == section ? 90 : 0))
            }
            .foregroundStyle(Color.primary)
            .padding(.horizontal, 14)
            .frame(height: 52)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(!canChangeSettings)
        .opacity(canChangeSettings ? 1 : 0.5)
        .accessibilityLabel(title)
    }

    private func optionGroup<Content: View>(
        identifier: String,
        @ViewBuilder content: () -> Content
    ) -> some View {
        VStack(spacing: 0) { content() }
            .padding(.vertical, 4)
            .background(Color(uiColor: .secondarySystemBackground))
            .accessibilityIdentifier(identifier)
    }

    private func modelOption(_ model: String) -> some View {
        option(model, selected: selectedModel.caseInsensitiveCompare(model) == .orderedSame) {
            selectModel(model)
        }
    }

    private func policyOption(_ title: String, value: String) -> some View {
        option(title, selected: selectedPolicy == value) { selectPolicy(value) }
    }

    private func option(_ title: String, selected: Bool, select: @escaping () -> Void) -> some View {
        Button(action: select) {
            HStack {
                Text(title).font(.system(size: 13, weight: .medium))
                Spacer()
                if selected {
                    Image(systemName: "checkmark")
                        .font(.system(size: 11, weight: .bold))
                        .foregroundStyle(Color.blue)
                }
            }
            .foregroundStyle(Color.primary)
            .padding(.horizontal, 44)
            .frame(height: 38)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    private func policyTitle(_ policy: String) -> String {
        switch policy {
        case "never": return "完全访问"
        case "unless-trusted": return "帮我批准"
        default: return "请求批准"
        }
    }

    private func action(_ title: String, _ image: String, _ color: Color,
                        _ handler: @escaping () -> Void) -> some View {
        Button(action: handler) {
            HStack(spacing: 12) {
                Image(systemName: image).frame(width: 18)
                Text(title).font(.system(size: 14, weight: .medium))
                Spacer()
            }.foregroundStyle(color).padding(.horizontal, 14).frame(height: 44)
        }.buttonStyle(.plain)
    }
}
private struct AIMessageRow: View {
    let message: AIMessage
    let workspacePath: String
    let quote: () -> Void
    @State private var pdfPreview: AIPDFPreviewItem?
    @State private var pdfPreviewError = false
    @State private var videoPreview: AIVideoPreviewItem?
    @State private var mediaSaveError: String?
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            if message.role == "user" {
                HStack {
                    Spacer(minLength: 30)
                    VStack(alignment: .leading, spacing: 10) {
                        ForEach(message.parts.filter { $0.kind == "quote" }) { part in
                            Label(part.preview ?? "已引用消息", systemImage: "arrowshape.turn.up.left")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                                .lineLimit(2)
                        }
                        if !userDisplayText.isEmpty {
                            Text(userDisplayText)
                                .font(ChatTypography.body)
                                .lineSpacing(ChatTypography.lineSpacing)
                                .foregroundStyle(RemoteIMStyle.textPrimary)
                                .textSelection(.enabled)
                                .padding(14)
                                .background(Color(uiColor: .secondarySystemBackground),
                                            in: RoundedRectangle(cornerRadius: 18))
                        }
                        ForEach(Array(imagePaths.enumerated()), id: \.offset) { _, path in
                            AIWorkspaceImage(filePath: path)
                        }
                        ForEach(videoPaths, id: \.self) { path in
                            AIWorkspaceVideoCard(filePath: path) {
                                videoPreview = AIVideoPreviewItem(path: path)
                            }
                        }
                        Button(action: quote) {
                            Label("引用", systemImage: "arrowshape.turn.up.left")
                        }
                        .buttonStyle(.plain)
                        .font(AssistantMessageFont.metadata)
                        .foregroundStyle(.secondary)
                    }
                }
            } else {
                if !reasoningText.isEmpty {
                    AIReasoningDisclosure(text: reasoningText,
                                          active: message.active,
                                          answerStarted: hasVisibleAnswerText,
                                          durationSeconds: reasoningDurationSeconds)
                }
                if !toolParts.isEmpty {
                    AIExpandableBlock(title: toolDisclosureTitle) {
                        VStack(alignment: .leading, spacing: 10) {
                            ForEach(toolParts) { tool in
                                VStack(alignment: .leading, spacing: 6) {
                                    Text("\(tool.tool ?? "工具") · \(toolStatus(tool.state))")
                                        .font(AssistantMessageFont.detail.weight(.semibold))
                                    Text(tool.input ?? "")
                                        .font(AssistantMessageFont.detailMonospaced)
                                    if let output = tool.output, !output.isEmpty {
                                        Text(output)
                                            .font(AssistantMessageFont.detailMonospaced)
                                    }
                                    if let error = tool.error, !error.isEmpty {
                                        Text(error).font(AssistantMessageFont.detail).foregroundStyle(.red)
                                    }
                                }
                                if tool.id != toolParts.last?.id { Divider() }
                            }
                        }
                        .textSelection(.enabled).padding(10)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(Color(uiColor: .secondarySystemBackground),
                                    in: RoundedRectangle(cornerRadius: 8))
                    }
                    ForEach(toolParts) { tool in
                        if let path = pdfArtifactPath(for: tool) {
                            AIPDFArtifactButton(path: path) {
                                if let url = validatedPDFURL(path: path) {
                                    pdfPreview = AIPDFPreviewItem(url: url)
                                } else {
                                    pdfPreviewError = true
                                }
                            }
                        }
                    }
                }
                ForEach(mediaArtifacts) { artifact in
                    VStack(alignment: .leading, spacing: 8) {
                        if artifact.type == "image" {
                            AIWorkspaceImage(filePath: artifact.filePath)
                        } else if artifact.type == "video" {
                            AIWorkspaceVideoCard(filePath: artifact.filePath) {
                                videoPreview = AIVideoPreviewItem(path: artifact.filePath)
                            }
                        } else {
                            AIWorkspaceAudioCard(filePath: artifact.filePath)
                        }
                        if !artifact.caption.isEmpty {
                            Text(artifact.caption)
                                .font(ChatTypography.body)
                                .foregroundStyle(RemoteIMStyle.textPrimary)
                        }
                    }
                    .contextMenu {
                        Button(action: quote) {
                            Label("引用这条消息", systemImage: "arrowshape.turn.up.left")
                        }
                        if artifact.type != "audio" {
                            Button {
                                Task { await saveMediaArtifact(artifact) }
                            } label: {
                                Label("保存到相册", systemImage: "square.and.arrow.down")
                            }
                        }
                        ShareLink(item: URL(fileURLWithPath: artifact.filePath)) {
                            Label("转发或分享", systemImage: "square.and.arrow.up")
                        }
                    }
                }
                ForEach(message.parts) { part in
                    if part.kind == "text", let text = part.text, !text.isEmpty {
                        if let failure = specialistFailureText(text) {
                            Label(failure, systemImage: "exclamationmark.triangle.fill")
                                .font(ChatTypography.body)
                                .foregroundStyle(.red)
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .padding(12)
                                .background(Color.red.opacity(0.08),
                                            in: RoundedRectangle(cornerRadius: 12))
                                .textSelection(.enabled)
                        } else {
                            MarkdownLikeText(text, retainsPreviousWhilePreparing: true,
                                             bodyFont: ChatTypography.body, assistantTypography: true)
                                .foregroundStyle(RemoteIMStyle.textPrimary)
                        }
                    }
                }
                if message.active && reasoningText.isEmpty && toolParts.isEmpty &&
                    mediaArtifacts.isEmpty &&
                    !hasVisibleAnswerText {
                    AIThinkingIndicator()
                }
                if !message.active {
                    HStack {
                        Text(message.completed == 0 ? "已中断" : "用时 \(max(0, (message.completed - message.created) / 1000)) 秒")
                            .font(AssistantMessageFont.metadata)
                        Button { RemoteIMClipboard.writeText(message.text) } label: {
                            Image(systemName: "doc.on.doc")
                        }
                            .accessibilityLabel("复制回复")
                        Button(action: quote) {
                            Label("引用", systemImage: "arrowshape.turn.up.left")
                        }
                        .accessibilityLabel("引用这条消息")
                    }.foregroundStyle(.secondary).font(AssistantMessageFont.metadata)
                }
            }
        }.frame(maxWidth: .infinity, alignment: .leading)
            .sheet(item: $pdfPreview) { item in
                AIPDFPreviewScreen(item: item) { pdfPreview = nil }
            }
            .fullScreenCover(item: $videoPreview) { item in
                MaiFfplayVideoScreen(path: item.path) { videoPreview = nil }
            }
            .alert("无法预览 PDF", isPresented: $pdfPreviewError) {
                Button("知道了", role: .cancel) {}
            } message: {
                Text("文件不存在、已移出 AI 工作区，或内容不是 PDF。")
            }
            .alert("无法保存媒体", isPresented: Binding(
                get: { mediaSaveError != nil },
                set: { if !$0 { mediaSaveError = nil } }
            )) {
                Button("知道了", role: .cancel) { mediaSaveError = nil }
            } message: {
                Text(mediaSaveError ?? "请检查相册权限与媒体文件。")
            }
    }
    private var reasoningParts: [AIPart] {
        message.parts.filter { $0.kind == "reasoning" }
    }
    private func specialistFailureText(_ text: String) -> String? {
        if text.hasPrefix("Video task failed. ") {
            return "视频任务失败：" + String(text.dropFirst("Video task failed. ".count))
        }
        if text.hasPrefix("Image task failed. ") {
            return "图片任务失败：" + String(text.dropFirst("Image task failed. ".count))
        }
        if text.hasPrefix("Video task status unavailable. ") {
            return "视频状态查询失败，云端结果未确认：" +
                String(text.dropFirst("Video task status unavailable. ".count))
        }
        if text.hasPrefix("Image task status unavailable. ") {
            return "图片状态查询失败，云端结果未确认：" +
                String(text.dropFirst("Image task status unavailable. ".count))
        }
        if text.hasPrefix("Video output unavailable. ") {
            return "视频产物获取失败：" + String(text.dropFirst("Video output unavailable. ".count))
        }
        if text.hasPrefix("Image output unavailable. ") {
            return "图片产物获取失败：" + String(text.dropFirst("Image output unavailable. ".count))
        }
        return nil
    }
    private var reasoningText: String {
        var seen = Set<String>()
        return reasoningParts.compactMap { part -> String? in
            guard let text = part.text?.trimmingCharacters(in: .whitespacesAndNewlines),
                  !text.isEmpty, seen.insert(text).inserted else { return nil }
            return text
        }.joined(separator: "\n\n")
    }
    private var reasoningDurationSeconds: Int64? {
        guard message.completed > message.created else { return nil }
        return (message.completed - message.created) / 1000
    }
    private var hasVisibleAnswerText: Bool {
        message.parts.contains { part in
            part.kind == "text" && !(part.text ?? "").isEmpty
        }
    }
    private var toolParts: [AIPart] {
        message.parts.filter { $0.kind == "tool" && mediaArtifact(for: $0) == nil }
    }
    private var mediaArtifacts: [AIAgentMediaArtifact] {
        message.parts.compactMap(mediaArtifact)
    }
    private func mediaArtifact(for part: AIPart) -> AIAgentMediaArtifact? {
        guard part.kind == "tool", part.tool == "agent_send_media",
              part.state == "completed",
              let output = part.output?.data(using: .utf8),
              let object = (try? JSONSerialization.jsonObject(with: output)) as? [String: Any],
              object["delivery"] as? String == "current_ai_session",
              let relative = object["path"] as? String,
              let type = object["type"] as? String,
              ["image", "video", "audio"].contains(type),
              let mime = object["mime_type"] as? String,
              mime.hasPrefix(type + "/"),
              let fileURL = AIAssistantPathPolicy.resolve(relative,
                                                           workspacePath: workspacePath)
        else { return nil }
        return AIAgentMediaArtifact(id: part.id, filePath: fileURL.path,
                                    type: type, caption: object["caption"] as? String ?? "")
    }
    private func saveMediaArtifact(_ artifact: AIAgentMediaArtifact) async {
        let saveRequestedAt = Date()
        let url = URL(fileURLWithPath: artifact.filePath)
        do {
            try await PHPhotoLibrary.shared().performChanges {
                if artifact.type == "image" {
                    PHAssetChangeRequest.creationRequestForAssetFromImage(atFileURL: url)?
                        .creationDate = saveRequestedAt
                } else {
                    PHAssetChangeRequest.creationRequestForAssetFromVideo(atFileURL: url)?
                        .creationDate = saveRequestedAt
                }
            }
        } catch {
            mediaSaveError = error.localizedDescription
        }
    }
    private var toolSummary: String {
        var counts: [(name: String, count: Int)] = []
        for part in toolParts {
            let name = part.tool ?? "工具"
            if let index = counts.firstIndex(where: { $0.name == name }) {
                counts[index].count += 1
            } else {
                counts.append((name, 1))
            }
        }
        return counts.map { "\($0.name) ×\($0.count)" }.joined(separator: " · ")
    }
    private var toolDisclosureTitle: String {
        let summary = "工具调用 \(toolParts.count) 次，\(toolGroupStatus)"
        return toolSummary.isEmpty ? summary : "\(summary) · \(toolSummary)"
    }
    private var toolGroupStatus: String {
        let states = toolParts.map(\.state)
        if states.contains("running") { return "运行中" }
        if states.contains("pending") { return "等待授权" }
        if states.contains("error") { return "有失败" }
        if states.allSatisfy({ $0 == "canceled" }) { return "已取消" }
        if states.contains("canceled") { return "部分取消" }
        return "已完成"
    }
    private func pdfArtifactPath(for part: AIPart) -> String? {
        guard part.tool == "generate_pdf", part.state == "completed",
              let output = part.output else { return nil }
        if let data = output.data(using: .utf8),
           let object = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
           object["mime_type"] as? String == "application/pdf",
           let path = object["path"] as? String,
           path.lowercased().hasSuffix(".pdf") { return path }
        if output.hasPrefix("Created "), output.hasSuffix(" bytes)."),
           let end = output.range(of: " (", options: .backwards)?.lowerBound {
            let start = output.index(output.startIndex, offsetBy: 8)
            if end > start {
                let path = String(output[start..<end])
                if path.lowercased().hasSuffix(".pdf") { return path }
            }
        }
        return nil
    }
    private func validatedPDFURL(path: String) -> URL? {
        guard let candidate = AIAssistantPathPolicy.resolve(path, workspacePath: workspacePath),
              candidate.pathExtension.lowercased() == "pdf",
              let attributes = try? FileManager.default.attributesOfItem(atPath: candidate.path),
              let size = attributes[.size] as? NSNumber, size.int64Value >= 8,
              size.int64Value <= 100 * 1024 * 1024,
              let handle = try? FileHandle(forReadingFrom: candidate) else { return nil }
        defer { try? handle.close() }
        guard let header = try? handle.read(upToCount: 5) else { return nil }
        return header == Data("%PDF-".utf8) ? candidate : nil
    }
    private var imagePaths: [String] {
        guard !workspacePath.isEmpty else { return [] }
        let root = URL(fileURLWithPath: workspacePath, isDirectory: true).standardizedFileURL
        let rootPrefix = root.path.hasSuffix("/") ? root.path : root.path + "/"
        let stored = message.parts.compactMap { part -> String? in
            guard part.kind == "image", part.mimeType?.hasPrefix("image/") == true else {
                return nil
            }
            return part.path
        }
        let prefixes = ["图片附件（工作区相对路径）：", "请查看文件："]
        let legacy = message.text.split(whereSeparator: \.isNewline).compactMap { line -> String? in
            let value = String(line).trimmingCharacters(in: .whitespacesAndNewlines)
            guard let prefix = prefixes.first(where: value.hasPrefix) else { return nil }
            return String(value.dropFirst(prefix.count))
                .trimmingCharacters(in: .whitespacesAndNewlines)
        }
        var seen = Set<String>()
        return (stored + legacy).compactMap { path in
            guard !(path as NSString).isAbsolutePath else { return nil }
            let candidate = root.appendingPathComponent(path).standardizedFileURL
            guard candidate.path.hasPrefix(rootPrefix), Self.isImage(candidate),
                  seen.insert(candidate.path).inserted
            else { return nil }
            return candidate.path
        }
    }
    private var userDisplayText: String {
        guard !imagePaths.isEmpty || !videoPaths.isEmpty ||
              message.parts.contains(where: { $0.kind == "image" || $0.kind == "video" }) else {
            return message.text
        }
        let automatic: Set<String> = ["请查看这张图片。", "请查看这些图片。",
                                      "请查看这个视频。", "请查看这些视频。", "请查看这些媒体。"]
        var lines = message.text.components(separatedBy: "\n")
        while let last = lines.last,
              automatic.contains(last.trimmingCharacters(in: .whitespacesAndNewlines)) {
            lines.removeLast()
        }
        return lines.joined(separator: "\n").trimmingCharacters(in: .whitespacesAndNewlines)
    }
    private var videoPaths: [String] {
        guard !workspacePath.isEmpty else { return [] }
        let root = URL(fileURLWithPath: workspacePath, isDirectory: true)
            .standardizedFileURL.resolvingSymlinksInPath()
        let prefix = root.path.hasSuffix("/") ? root.path : root.path + "/"
        var seen = Set<String>()
        return message.parts.compactMap { part in
            guard part.kind == "video", part.mimeType?.hasPrefix("video/") == true,
                  let relative = part.path, !(relative as NSString).isAbsolutePath else {
                return nil
            }
            let file = root.appendingPathComponent(relative)
                .standardizedFileURL.resolvingSymlinksInPath()
            guard file.path.hasPrefix(prefix), seen.insert(file.path).inserted else { return nil }
            return file.path
        }
    }
    private static func isImage(_ url: URL) -> Bool {
        guard let type = UTType(filenameExtension: url.pathExtension) else { return false }
        return type.conforms(to: .image)
    }
    private func toolStatus(_ state: String?) -> String {
        switch state { case "completed": return "已完成"; case "error": return "失败"; case "pending": return "等待授权"; default: return "正在运行" }
    }
}

private struct AIAgentMediaArtifact: Identifiable {
    let id: String
    let filePath: String
    let type: String
    let caption: String
}

private struct AIPDFPreviewItem: Identifiable {
    let url: URL
    var id: String { url.path }
}

private struct AIVideoPreviewItem: Identifiable {
    let path: String
    var id: String { path }
}

private struct AIPDFPreviewScreen: View {
    let item: AIPDFPreviewItem
    let close: () -> Void

    var body: some View {
        NavigationStack {
            RemoteIMQuickLookPreview(filePath: item.url.path)
                .navigationTitle(item.url.lastPathComponent)
                .navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItem(placement: .topBarLeading) {
                        ShareLink(item: item.url) { Image(systemName: "square.and.arrow.up") }
                    }
                    ToolbarItem(placement: .topBarTrailing) {
                        Button("关闭", action: close)
                    }
                }
        }
    }
}

private struct AIPDFArtifactButton: View {
    let path: String
    let open: () -> Void

    var body: some View {
        Button(action: open) {
            Label("预览 PDF · \((path as NSString).lastPathComponent)",
                  systemImage: "doc.richtext")
                .font(.system(size: 14, weight: .medium))
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(12)
                .background(Color.blue.opacity(0.07), in: RoundedRectangle(cornerRadius: 10))
        }
        .buttonStyle(.plain)
        .accessibilityIdentifier("agentPdfPreviewButton")
    }
}

private struct AIWorkspaceImage: View {
    let filePath: String
    @State private var decodedSize: CGSize?

    private var previewSize: CGSize {
        ImageBubbleSize.fitted(decodedSize ?? CGSize(width: 220, height: 180),
                               maximum: ImageBubbleSize.assistantMaximum)
    }

    var body: some View {
        GeometryReader { geometry in
            Button {
                let sourceFrame = geometry.frame(in: .named(
                    AIImagePreviewLayout.coordinateSpaceName))
                guard sourceFrame.width > 0, sourceFrame.height > 0 else { return }
                AIAssistantModel.shared.previewImage = AIImagePreview(
                    filePath: filePath,
                    imageSize: decodedSize ?? previewSize,
                    sourceFrame: sourceFrame)
            } label: {
                RemoteIMAsyncImage(filePath: filePath,
                                   maximumPointSize: previewSize) { image in
                    Image(uiImage: image).resizable().scaledToFit()
                        .task(id: image.size) {
                            if let pixels = image.cgImage {
                                decodedSize = CGSize(width: pixels.width, height: pixels.height)
                            }
                        }
                        .accessibilityLabel("AI 助手图片附件")
                } placeholder: { failed in
                    if failed {
                        Label("图片无法显示", systemImage: "photo")
                            .foregroundStyle(Color.secondary)
                            .frame(width: previewSize.width, height: previewSize.height)
                    } else {
                        Color.clear.frame(width: previewSize.width, height: previewSize.height)
                    }
                }
                .frame(width: previewSize.width, height: previewSize.height)
                .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
                .overlay(alignment: .bottomTrailing) {
                    Image(systemName: "arrow.up.left.and.arrow.down.right")
                        .font(.system(size: 11, weight: .bold))
                        .foregroundStyle(.white)
                        .frame(width: 25, height: 25)
                        .background(.black.opacity(0.52), in: Circle())
                        .padding(7)
                        .allowsHitTesting(false)
                        .accessibilityHidden(true)
                }
            }
            .buttonStyle(.plain)
            .accessibilityLabel("放大图片")
            .accessibilityIdentifier("agent-image-bubble")
        }
        .frame(width: previewSize.width, height: previewSize.height)
    }
}

private struct AIWorkspaceVideoCard: View {
    let filePath: String
    let open: () -> Void
    @Environment(\.displayScale) private var displayScale
    @State private var cover: UIImage?
    @State private var videoSize: CGSize?
    @State private var durationSeconds = 0

    private var exists: Bool { FileManager.default.fileExists(atPath: filePath) }

    var body: some View {
        Button(action: open) {
            ZStack {
                if let cover {
                    Image(uiImage: cover).resizable().scaledToFit()
                } else {
                    LinearGradient(
                        colors: [
                            Color(red: 0.10, green: 0.17, blue: 0.27),
                            Color(red: 0.18, green: 0.32, blue: 0.47),
                        ],
                        startPoint: .topLeading, endPoint: .bottomTrailing)
                }
                Image(systemName: exists ? "play.fill" : "exclamationmark.triangle.fill")
                    .font(.system(size: 20, weight: .bold))
                    .foregroundStyle(.white)
                    .frame(width: 52, height: 52)
                    .background(.black.opacity(0.5), in: Circle())
                Text(
                    String(
                        format: "%d:%02d", durationSeconds / 60,
                        durationSeconds % 60)
                )
                .font(.system(size: 11, weight: .semibold, design: .monospaced))
                .foregroundStyle(.white)
                .padding(.horizontal, 7)
                .frame(height: 22)
                .background(.black.opacity(0.56), in: Capsule())
                .frame(
                    maxWidth: .infinity, maxHeight: .infinity,
                    alignment: .bottomTrailing
                )
                .padding(8)
            }
            .frame(width: previewSize.width, height: previewSize.height)
            .clipped()
            .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
        }
        .buttonStyle(.plain)
        .disabled(!exists)
        .accessibilityLabel(exists ? "播放视频" : "视频文件已丢失")
        .accessibilityIdentifier("agent-video-bubble")
        .task(id: filePath) {
            guard exists else { return }
            let asset = AVURLAsset(url: URL(fileURLWithPath: filePath))
            if let track = try? await asset.loadTracks(withMediaType: .video).first,
                let naturalSize = try? await track.load(.naturalSize),
                let transform = try? await track.load(.preferredTransform)
            {
                let displayed = CGRect(origin: .zero, size: naturalSize).applying(transform)
                videoSize = CGSize(width: abs(displayed.width), height: abs(displayed.height))
            }
            if let duration = try? await asset.load(.duration),
                duration.seconds.isFinite
            {
                durationSeconds = max(0, Int(duration.seconds))
            }
            let generator = AVAssetImageGenerator(asset: asset)
            generator.appliesPreferredTrackTransform = true
            generator.maximumSize = CGSize(
                width: VideoBubbleSize.assistantMaximum.width * min(displayScale, 3),
                height: VideoBubbleSize.assistantMaximum.height * min(displayScale, 3))
            let time = CMTime(
                seconds: durationSeconds > 1 ? 0.5 : 0,
                preferredTimescale: 600)
            if let image = try? await generator.image(at: time), !Task.isCancelled {
                videoSize = CGSize(width: image.image.width, height: image.image.height)
                cover = UIImage(cgImage: image.image)
            }
        }
    }

    private var previewSize: CGSize {
        VideoBubbleSize.fitted(videoSize ?? .zero,
                               maximum: VideoBubbleSize.assistantMaximum)
    }
}

private struct AIWorkspaceAudioCard: View {
    let filePath: String
    @State private var player: AVPlayer?
    @State private var isPlaying = false
    @State private var durationSeconds = 0

    var body: some View {
        Button {
            if isPlaying {
                player?.pause()
                isPlaying = false
            } else {
                if player == nil {
                    player = AVPlayer(url: URL(fileURLWithPath: filePath))
                }
                player?.play()
                isPlaying = true
            }
        } label: {
            HStack(spacing: 12) {
                Image(systemName: isPlaying ? "pause.fill" : "play.fill")
                    .frame(width: 38, height: 38)
                    .background(RemoteIMStyle.blueSoft, in: Circle())
                VStack(alignment: .leading, spacing: 3) {
                    Text(URL(fileURLWithPath: filePath).lastPathComponent)
                        .lineLimit(1)
                        .truncationMode(.middle)
                    Text(String(format: "%d:%02d", durationSeconds / 60,
                                durationSeconds % 60))
                        .font(AssistantMessageFont.metadata)
                        .foregroundStyle(RemoteIMStyle.textSecondary)
                }
                Spacer(minLength: 0)
            }
            .font(ChatTypography.body)
            .foregroundStyle(RemoteIMStyle.textPrimary)
            .padding(10)
            .frame(width: 240)
            .background(Color(uiColor: .secondarySystemBackground),
                        in: RoundedRectangle(cornerRadius: 12))
        }
        .buttonStyle(.plain)
        .accessibilityLabel(isPlaying ? "暂停音频" : "播放音频")
        .task(id: filePath) {
            let asset = AVURLAsset(url: URL(fileURLWithPath: filePath))
            if let duration = try? await asset.load(.duration), duration.seconds.isFinite {
                durationSeconds = max(0, Int(duration.seconds))
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: .AVPlayerItemDidPlayToEndTime)) {
            notification in
            if notification.object as? AVPlayerItem === player?.currentItem {
                isPlaying = false
                player?.seek(to: .zero)
            }
        }
        .onDisappear { player?.pause(); isPlaying = false }
    }
}

#if targetEnvironment(simulator)
struct AIVideoBubbleUITestRoot: View {
    var body: some View {
        if let video = Bundle.main.url(forResource: "ffplay-sample", withExtension: "mp4") {
            let part = AIPart(id: "video", kind: "video", text: nil, tool: nil,
                              input: nil, output: nil, error: nil, state: nil,
                              path: video.lastPathComponent, mimeType: "video/mp4")
            let message = AIMessage(id: "video-message", role: "user", created: 0,
                                    completed: 1, active: false, parts: [part])
            AIMessageRow(message: message,
                         workspacePath: video.deletingLastPathComponent().path, quote: {})
                .padding(20)
        }
    }
}

struct AITranscriptUITestRoot: View {
    private func part(_ id: String, kind: String, text: String? = nil,
                      tool: String? = nil) -> AIPart {
        AIPart(id: id, kind: kind, text: text, tool: tool,
               input: tool == nil ? nil : "{}", output: tool == nil ? nil : "done",
               error: nil, state: tool == nil ? nil : "completed",
               path: nil, mimeType: nil)
    }

    var body: some View {
        let parts = [part("reasoning-1", kind: "reasoning", text: "检查视频"),
                     part("reasoning-2", kind: "reasoning", text: "检查视频")]
            + (0..<4).map { part("ffmpeg-\($0)", kind: "tool", tool: "ffmpeg") }
            + (0..<4).map { part("view-image-\($0)", kind: "tool", tool: "view_image") }
            + [part("answer", kind: "text", text: "处理完成，结果已经准备好。")]
        let message = AIMessage(id: "transcript", role: "assistant", created: 0,
                                completed: 1, active: false, parts: parts)
        AIMessageRow(message: message, workspacePath: "", quote: {})
            .padding(20)
    }
}

struct AIHistoryUITestRoot: View {
    var body: some View {
        AIAssistantView()
            .onAppear { AIAssistantModel.shared.installHistoryUITestFixture() }
    }
}
#endif

private struct AIImagePreviewOverlay: View {
    let preview: AIImagePreview
    let close: () -> Void
    @State private var isExpanded = false

    private var previewAnimation: Animation {
        .interactiveSpring(response: 0.38, dampingFraction: 0.86, blendDuration: 0.08)
    }

    var body: some View {
        GeometryReader { geometry in
            let fittedSize = preview.imageSize.width > 0 && preview.imageSize.height > 0
                ? aspectFitSize(imageSize: preview.imageSize, containerSize: geometry.size)
                : geometry.size
            let destinationFrame = CGRect(
                x: (geometry.size.width - fittedSize.width) / 2,
                y: (geometry.size.height - fittedSize.height) / 2,
                width: fittedSize.width,
                height: fittedSize.height)
            let imageFrame = isExpanded ? destinationFrame
                : (preview.sourceFrame ?? destinationFrame)

            ZStack(alignment: .topTrailing) {
                Color.black.opacity(isExpanded ? 1 : 0)
                    .ignoresSafeArea()
                    .onTapGesture(perform: shrinkAndClose)
                RemoteIMAsyncImage(filePath: preview.filePath,
                                   maximumPointSize: ImagePreviewZoomModifier.maximumDecodePointSize(
                                    for: geometry.size)) { image in
                    Image(uiImage: image).resizable().scaledToFit()
                        .accessibilityLabel("Agent 处理后的图片预览")
                } placeholder: { failed in
                    if failed {
                        Label("图片无法显示", systemImage: "photo")
                            .foregroundStyle(.white)
                            .frame(maxWidth: .infinity, maxHeight: .infinity)
                    } else {
                        ProgressView().tint(.white)
                    }
                }
                .frame(width: imageFrame.width, height: imageFrame.height)
                .modifier(ImagePreviewZoomModifier(enabled: isExpanded,
                                                   imageSize: fittedSize,
                                                   viewportSize: geometry.size))
                .position(x: imageFrame.midX, y: imageFrame.midY)
                .contentShape(Rectangle())
                .onTapGesture(perform: shrinkAndClose)
                .accessibilityIdentifier("agent-image-preview")

                Button(action: shrinkAndClose) {
                    Image(systemName: "xmark")
                        .font(.system(size: 17, weight: .bold))
                        .foregroundStyle(.white)
                        .frame(width: 44, height: 44)
                        .background(.black.opacity(0.5), in: Circle())
                }
                .accessibilityLabel("关闭图片预览")
                .padding(20)
                .opacity(isExpanded ? 1 : 0)
                .allowsHitTesting(isExpanded)
            }
            .frame(width: geometry.size.width, height: geometry.size.height)
        }
        .onAppear {
            DispatchQueue.main.async {
                withAnimation(previewAnimation) { isExpanded = true }
            }
        }
        .accessibilityAction(.escape, shrinkAndClose)
    }

    private func shrinkAndClose() {
        guard isExpanded else { return }
        withAnimation(previewAnimation) { isExpanded = false }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.42) {
            guard !isExpanded else { return }
            close()
        }
    }

    private func aspectFitSize(imageSize: CGSize, containerSize: CGSize) -> CGSize {
        guard imageSize.width > 0, imageSize.height > 0,
              containerSize.width > 0, containerSize.height > 0 else { return .zero }
        let scale = min(containerSize.width / imageSize.width,
                        containerSize.height / imageSize.height)
        return CGSize(width: imageSize.width * scale, height: imageSize.height * scale)
    }
}

private struct AIThinkingIndicator: View {
    var body: some View {
        TimelineView(.periodic(from: .now, by: 0.35)) { context in
            let highlighted = Int(context.date.timeIntervalSince1970 * 3) % 3
            HStack(spacing: 5) {
                ForEach(0..<3, id: \.self) { index in
                    Circle()
                        .fill(Color.secondary.opacity(index == highlighted ? 0.8 : 0.25))
                        .frame(width: 6, height: 6)
                }
            }
            .frame(height: 24)
            .accessibilityLabel("正在思考")
        }
    }
}

private struct AIReasoningDisclosure: View {
    let text: String
    let active: Bool
    let answerStarted: Bool
    let durationSeconds: Int64?
    @State private var manualExpanded: Bool?

    private var expanded: Bool { manualExpanded ?? (active && !answerStarted) }
    private var title: String {
        if active && !answerStarted { return "正在思考" }
        if let durationSeconds { return "已思考（用时 \(durationSeconds) 秒）" }
        return "已思考"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Button {
                withAnimation(.easeOut(duration: 0.16)) { manualExpanded = !expanded }
            } label: {
                HStack(spacing: 8) {
                    Text(title)
                    Image(systemName: "chevron.right")
                        .rotationEffect(.degrees(expanded ? 90 : 0))
                }
                .font(AssistantMessageFont.detail)
                .foregroundStyle(.secondary)
            }
            .buttonStyle(.plain)
            if expanded {
                Text(text)
                    .font(AssistantMessageFont.detail)
                    .lineSpacing(5)
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
                    .padding(.leading, 16)
                    .overlay(alignment: .leading) {
                        Rectangle().fill(Color.secondary.opacity(0.25)).frame(width: 2)
                    }
            }
        }
        .onChange(of: answerStarted) { if $0 { manualExpanded = nil } }
        .onChange(of: active) { if !$0 { manualExpanded = nil } }
    }
}

private struct AIExpandableBlock<Content: View>: View {
    let title: String
    let content: Content
    @State private var expanded = false

    init(title: String, @ViewBuilder content: () -> Content) {
        self.title = title
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Button { withAnimation(.easeOut(duration: 0.16)) { expanded.toggle() } } label: {
                HStack(spacing: 8) {
                    Text(title).font(AssistantMessageFont.detail).lineLimit(1)
                    Spacer()
                    Image(systemName: "chevron.right").rotationEffect(.degrees(expanded ? 90 : 0))
                }
                .foregroundStyle(.secondary)
                .frame(minHeight: 32)
            }.buttonStyle(.plain)
            if expanded { content }
        }
    }
}

@MainActor
private final class AIComposerFocusController {
    weak var textView: UITextView?

    func focus() { textView?.becomeFirstResponder() }
    func dismiss() { textView?.resignFirstResponder() }
}

private struct AIPickedVideoTransfer: Transferable, Sendable {
    let fileURL: URL

    static var transferRepresentation: some TransferRepresentation {
        FileRepresentation(contentType: .movie) { video in
            SentTransferredFile(video.fileURL)
        } importing: { received in
            let sourceURL = received.file
            let targetURL = try await RemoteIMBackgroundWork.file {
                let directory = FileManager.default.temporaryDirectory
                    .appendingPathComponent("AIAssistantImports", isDirectory: true)
                try FileManager.default.createDirectory(at: directory,
                                                        withIntermediateDirectories: true)
                let fileExtension = sourceURL.pathExtension.isEmpty
                    ? "mov" : sourceURL.pathExtension
                let target = directory.appendingPathComponent(UUID().uuidString)
                    .appendingPathExtension(fileExtension)
                try FileManager.default.copyItem(at: sourceURL, to: target)
                return target
            }
            return AIPickedVideoTransfer(fileURL: targetURL)
        }
    }
}

private struct AIComposer: View {
    @ObservedObject var model: AIAssistantModel
    let focusController: AIComposerFocusController
    let transcriptionPresentation: VoiceTranscriptionPresentation
    @Binding var isAttachmentPanelPresented: Bool
    @StateObject private var speechRecognizer = TencentRealtimeSpeechRecognizer(
        appId: TencentASRCredentials.appId,
        secretId: TencentASRCredentials.secretId,
        secretKey: TencentASRCredentials.secretKey
    )
    @State private var draft = ""
    @State private var importing = false
    @State private var isPhotoPickerPresented = false
    @State private var isCameraPresented = false
    @State private var selectedMediaItems: [PhotosPickerItem] = []
    @State private var draftSession = ""
    @State private var isVoiceMode = false
    @State private var isPressingVoice = false
    @State private var composerFocusRequestGeneration = 0
    @State private var composerEditingController = ComposerTextEditingController()
    @State private var composerEditMenuState: ComposerEditMenuState?
    @State private var voiceBaseDraft = ""
    @State private var voiceStartTask: Task<Bool, Never>?
    var body: some View {
        VStack(spacing: 0) {
            if let quoted = model.quotedMessage {
                HStack(spacing: 10) {
                    Image(systemName: "arrowshape.turn.up.left")
                        .foregroundStyle(RemoteIMStyle.blue)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(quoted.role == "user" ? "引用你的消息" : "引用 AI 消息")
                            .font(.caption.weight(.semibold))
                        Text(quoted.quotePreview)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .lineLimit(2)
                    }
                    Spacer(minLength: 4)
                    Button { model.quotedMessage = nil } label: {
                        Image(systemName: "xmark.circle.fill")
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("取消引用")
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 9)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(Color(uiColor: .secondarySystemBackground))
            }
            HStack(alignment: .bottom, spacing: 8) {
                Button {
                    composerEditMenuState = nil
                    isAttachmentPanelPresented = false
                    focusController.dismiss()
                    withAnimation(.easeOut(duration: 0.16)) { isVoiceMode.toggle() }
                } label: {
                    Image(systemName: isVoiceMode ? "keyboard" : "speaker.wave.2.fill")
                        .font(.system(size: 18, weight: .bold))
                        .frame(width: 44, height: 44)
                        .background(RemoteIMStyle.blueSoft, in: Circle())
                        .overlay(Circle().stroke(RemoteIMStyle.border, lineWidth: 1))
                }
                .buttonStyle(.plain)
                .foregroundStyle(RemoteIMStyle.blue)
                .accessibilityLabel(isVoiceMode ? "切换到键盘输入" : "切换到按住转文字")

                if isVoiceMode {
                    AITranscriptionButton(
                        isPressing: isPressingVoice,
                        isCancelling: transcriptionPresentation.target == .cancel,
                        isEnabled: canStartVoiceTranscription,
                        onChanged: { translation in
                            handleVoiceGestureChanged(translation: translation, location: nil)
                        },
                        onEnded: { translation in
                            Task {
                                await finishVoiceGesture(translation: translation, location: nil)
                            }
                        }
                    )
                } else {
                    ZStack(alignment: .topLeading) {
                        ComposerTextView(
                            text: $draft,
                            onSubmit: { submitDraft() },
                            focusRequestGeneration: composerFocusRequestGeneration,
                            editingController: composerEditingController,
                            onEditMenuRequested: { state in
                                let next = state.hasActions ? state : nil
                                guard composerEditMenuState != next else { return }
                                withAnimation(.easeOut(duration: 0.1)) {
                                    composerEditMenuState = next
                                }
                            },
                            onEditMenuDismissed: {
                                guard composerEditMenuState != nil else { return }
                                withAnimation(.easeOut(duration: 0.08)) {
                                    composerEditMenuState = nil
                                }
                            },
                            onTypingActivityChanged: { _ in },
                            voiceTranscriptionEnabled: canStartVoiceTranscription,
                            onVoiceLongPressChanged: { translation, location in
                                handleVoiceGestureChanged(
                                    translation: translation,
                                    location: location
                                )
                            },
                            onVoiceLongPressEnded: { translation, location in
                                Task {
                                    await finishVoiceGesture(
                                        translation: translation,
                                        location: location
                                    )
                                }
                            },
                            onVoiceLongPressCancelled: {
                                cancelVoiceTranscription(restoresDraft: true)
                            },
                            registerTextView: { focusController.textView = $0 },
                            accessibilityIdentifier: "ai-composer",
                            accessibilityLabel: "AI 助手输入框，可按住转文字"
                        )
                        if draft.isEmpty {
                            Text(composerPrompt)
                                .foregroundStyle(RemoteIMStyle.textSecondary)
                                .font(ChatTypography.body.weight(isPressingVoice ? .semibold : .regular))
                                .padding(.horizontal, 13)
                                .padding(.vertical, 13)
                                .allowsHitTesting(false)
                                .accessibilityHidden(true)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(Color.white, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
                    .overlay(
                        RoundedRectangle(cornerRadius: 14, style: .continuous)
                            .stroke(model.ready ? RemoteIMStyle.blue : RemoteIMStyle.border,
                                    lineWidth: model.ready ? 1.5 : 1)
                    )
                    .overlay(alignment: .topLeading) {
                        if let state = composerEditMenuState {
                            ComposerEditActionBar(
                                state: state,
                                pasteTarget: composerEditingController.textView,
                                perform: performComposerEditAction
                            )
                            .offset(x: 4, y: -50)
                            .transition(.opacity)
                            .zIndex(20)
                        }
                    }
                    .zIndex(composerEditMenuState == nil ? 0 : 20)
                }

                Button {
                    composerEditMenuState = nil
                    if model.busy {
                        Task { await model.action("stop") }
                    } else if isAttachmentPanelPresented {
                        isAttachmentPanelPresented = false
                    } else {
                        focusController.dismiss()
                        isAttachmentPanelPresented = true
                    }
                } label: {
                    Image(systemName: model.busy ? "stop.fill" : "plus")
                        .font(.system(size: model.busy ? 16 : 20, weight: .semibold))
                        .foregroundStyle(model.busy ? Color.white : RemoteIMStyle.textPrimary)
                        .frame(width: 44, height: 44)
                        .background(model.busy ? Color.primary : Color.white, in: Circle())
                        .overlay(Circle().stroke(RemoteIMStyle.border, lineWidth: model.busy ? 0 : 1))
                }
                .buttonStyle(.plain)
                .disabled(!model.ready || model.isSubmitting)
                .accessibilityLabel(
                    model.busy ? "停止" : (isAttachmentPanelPresented ? "收起更多功能" : "展开更多功能")
                )
            }
            .padding(.horizontal, 16)
            .padding(.top, 12)
            .padding(.bottom, 10)

            if isAttachmentPanelPresented {
                Divider().background(RemoteIMStyle.border)
                ComposerAttachmentPanel(
                    canSendImage: true,
                    canSendVideo: true,
                    canSendFile: true,
                    canSendVoice: false,
                    openLibrary: {
                        isAttachmentPanelPresented = false
                        Task { await openPhotoPicker() }
                    },
                    openCamera: {
                        isAttachmentPanelPresented = false
                        Task { await openCamera() }
                    },
                    openFile: {
                        isAttachmentPanelPresented = false
                        importing = true
                    },
                    openVoiceInput: {},
                    suggestReply: nil,
                    showsVoiceInput: false
                )
                .transition(.move(edge: .bottom).combined(with: .opacity))
            }
        }
            .background(RemoteIMStyle.panelBackground)
            .overlay(alignment: .top) { Divider().background(RemoteIMStyle.border) }
            .animation(.easeOut(duration: 0.18), value: isAttachmentPanelPresented)
            .photosPicker(
                isPresented: $isPhotoPickerPresented,
                selection: $selectedMediaItems,
                maxSelectionCount: 10,
                selectionBehavior: .ordered,
                matching: .any(of: [.images, .videos]),
                photoLibrary: .shared()
            )
            .fullScreenCover(isPresented: $isCameraPresented) {
                RemoteIMCameraPicker(
                    onCapture: { image in
                        isCameraPresented = false
                        Task { await importCapturedImage(image) }
                    },
                    onCancel: { isCameraPresented = false }
                )
                .ignoresSafeArea()
            }
            .fileImporter(
                isPresented: $importing,
                allowedContentTypes: [.plainText, .sourceCode, .json, .image, .movie]
            ) { result in
                if case let .success(url) = result {
                    let target = model.selected
                    Task {
                        let type = UTType(filenameExtension: url.pathExtension)
                        if type?.conforms(to: .movie) == true {
                            if let file = await model.importVideoFile(url) {
                                await sendImportedMedia([], videos: [file], to: target)
                            }
                        } else if let file = await model.importFile(url) {
                            if file.isImage {
                                await sendImportedMedia([file], to: target)
                            } else {
                                appendImportedDocument(file, to: target)
                            }
                        }
                    }
                }
            }
            .onChange(of: selectedMediaItems) { items in
                guard !items.isEmpty else { return }
                Task { await importSelectedMedia(items) }
            }
            .onAppear {
                draftSession = model.selected
                draft = model.drafts[draftSession] ?? ""
                transcriptionPresentation.onCancel = {
                    cancelVoiceTranscription(restoresDraft: true)
                }
                transcriptionPresentation.onEdit = {
                    Task {
                        await finishVoiceGesture(
                            translation: .zero,
                            location: nil,
                            explicitTarget: .edit
                        )
                    }
                }
                speechRecognizer.onLiveTextUpdate = { text, sessionID in
                    guard isPressingVoice else { return }
                    transcriptionPresentation.updateLiveText(text, sessionID: sessionID)
                }
            }
            .onDisappear {
                model.drafts[draftSession] = draft
                voiceStartTask?.cancel()
                voiceStartTask = nil
                speechRecognizer.onLiveTextUpdate = nil
                transcriptionPresentation.onCancel = nil
                transcriptionPresentation.onEdit = nil
                transcriptionPresentation.reset()
                speechRecognizer.cancel()
                isAttachmentPanelPresented = false
                composerEditMenuState = nil
                selectedMediaItems = []
            }
            .onChange(of: model.selected) { selected in
                cancelVoiceTranscription(restoresDraft: true)
                model.drafts[draftSession] = draft
                let sendingFirstMessage = model.isSubmitting && draftSession.isEmpty
                draftSession = selected
                if !sendingFirstMessage { draft = model.drafts[selected] ?? "" }
            }
    }

    private func openPhotoPicker() async {
        guard await requestPhotoLibraryPermission() else {
            model.showTransientError("没有相册权限，请在系统设置中允许访问照片")
            return
        }
        isPhotoPickerPresented = true
    }

    private func openCamera() async {
        guard UIImagePickerController.isSourceTypeAvailable(.camera) else {
            model.showTransientError("当前设备不支持拍照")
            return
        }
        guard await requestCameraPermission() else {
            model.showTransientError("没有相机权限，请在系统设置中允许 MaiChat 使用相机")
            return
        }
        isCameraPresented = true
    }

    private func requestCameraPermission() async -> Bool {
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            return true
        case .notDetermined:
            return await withCheckedContinuation { continuation in
                AVCaptureDevice.requestAccess(for: .video) { granted in
                    continuation.resume(returning: granted)
                }
            }
        case .denied, .restricted:
            return false
        @unknown default:
            return false
        }
    }

    private func requestPhotoLibraryPermission() async -> Bool {
        switch PHPhotoLibrary.authorizationStatus(for: .readWrite) {
        case .authorized, .limited:
            return true
        case .notDetermined:
            let status = await withCheckedContinuation { continuation in
                PHPhotoLibrary.requestAuthorization(for: .readWrite) { status in
                    continuation.resume(returning: status)
                }
            }
            return status == .authorized || status == .limited
        case .denied, .restricted:
            return false
        @unknown default:
            return false
        }
    }

    private func importSelectedMedia(_ items: [PhotosPickerItem]) async {
        let targetSession = model.selected
        defer { selectedMediaItems = [] }
        var images: [AIImportedFile] = []
        var videos: [AIImportedFile] = []
        var failedCount = 0
        for item in items {
            do {
                if item.supportedContentTypes.contains(where: { $0.conforms(to: .movie) }) {
                    guard let picked = try await item.loadTransferable(type: AIPickedVideoTransfer.self)
                    else { failedCount += 1; continue }
                    let temporaryURL = picked.fileURL
                    defer { Task { await Self.removeTemporaryFile(temporaryURL) } }
                    guard let file = await model.importVideoFile(temporaryURL) else {
                        failedCount += 1
                        continue
                    }
                    videos.append(file)
                    continue
                }
                guard let data = try await item.loadTransferable(type: Data.self),
                      data.count <= 20 * 1024 * 1024
                else {
                    failedCount += 1
                    continue
                }
                let type = item.supportedContentTypes.first { $0.conforms(to: .image) }
                let temporaryURL = try await Self.writeTemporaryImage(
                    data,
                    pathExtension: type?.preferredFilenameExtension ?? "jpg"
                )
                defer { Task { await Self.removeTemporaryFile(temporaryURL) } }
                guard let file = await model.importFile(temporaryURL) else {
                    failedCount += 1
                    continue
                }
                images.append(file)
            } catch {
                failedCount += 1
            }
        }
        if !images.isEmpty || !videos.isEmpty {
            await sendImportedMedia(images, videos: videos, to: targetSession)
        }
        if failedCount > 0 {
            model.showTransientError(
                !images.isEmpty || !videos.isEmpty
                    ? "已导入\(images.count + videos.count)项，另有\(failedCount)项读取失败"
                    : "所选照片或视频读取失败"
            )
        }
    }

    private func importCapturedImage(_ image: UIImage) async {
        let targetSession = model.selected
        guard let data = image.jpegData(compressionQuality: 0.9) else {
            model.showTransientError("拍摄图片处理失败")
            return
        }
        do {
            let temporaryURL = try await Self.writeTemporaryImage(data, pathExtension: "jpg")
            defer { Task { await Self.removeTemporaryFile(temporaryURL) } }
            if let file = await model.importFile(temporaryURL) {
                await sendImportedMedia([file], to: targetSession)
            }
        } catch {
            model.showTransientError("拍摄图片导入失败")
        }
    }

    private func sendImportedMedia(_ images: [AIImportedFile],
                                   videos: [AIImportedFile] = [],
                                   to targetSession: String) async {
        guard !images.isEmpty || !videos.isEmpty else { return }
        let existingDraft = draft.trimmingCharacters(in: .whitespacesAndNewlines)
        if await model.send(existingDraft, images: images, videos: videos,
                            expectedSession: targetSession) {
            if draft.trimmingCharacters(in: .whitespacesAndNewlines) == existingDraft { draft = "" }
            return
        }

        // 导入已完成但发送条件在异步读取期间改变时，保留媒体供下次发送。
        for file in images + videos { model.addAttachment(file, to: targetSession) }
        model.showTransientError("媒体已保留，请输入消息后重试")
    }

    private func appendImportedDocument(_ file: AIImportedFile, to targetSession: String) {
        model.addAttachment(file, to: targetSession)
        let reference = "请查看工作区文件：\(file.relativePath)"
        if targetSession == model.selected {
            draft += (draft.isEmpty ? "" : "\n") + reference
            focusController.focus()
        } else {
            let previous = model.drafts[targetSession, default: ""]
            model.drafts[targetSession] = previous
                + (previous.isEmpty ? "" : "\n")
                + reference
        }
    }

    nonisolated private static func writeTemporaryImage(
        _ data: Data,
        pathExtension: String
    ) async throws -> URL {
        try await RemoteIMBackgroundWork.file {
            let directory = FileManager.default.temporaryDirectory
                .appendingPathComponent("AIAssistantImports", isDirectory: true)
            try FileManager.default.createDirectory(
                at: directory,
                withIntermediateDirectories: true
            )
            let cleanExtension = pathExtension.trimmingCharacters(in: .whitespacesAndNewlines)
            let target = directory
                .appendingPathComponent(UUID().uuidString)
                .appendingPathExtension(cleanExtension.isEmpty ? "jpg" : cleanExtension)
            try data.write(to: target, options: .atomic)
            return target
        }
    }

    nonisolated private static func removeTemporaryFile(_ url: URL) async {
        _ = try? await RemoteIMBackgroundWork.file {
            try FileManager.default.removeItem(at: url)
        }
    }

    private var composerPrompt: String {
        return "可按住转文字"
    }

    private var canStartVoiceTranscription: Bool {
        isPressingVoice || (
            model.ready && !model.busy && !model.isSubmitting && draft.isEmpty
        )
    }

    private func submitDraft(_ submittedText: String? = nil) {
        guard model.ready, !model.busy, !model.isSubmitting else { return }
        let sent = submittedText ?? draft
        guard !sent.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return }
        Task {
            if await model.send(sent), draft == sent { draft = "" }
        }
    }

    private func performComposerEditAction(_ action: ComposerEditAction) {
        composerEditingController.perform(action)
        switch action {
        case .select, .selectAll:
            let next = composerEditingController.menuState()
            composerEditMenuState = next.hasActions ? next : nil
        case .paste, .cut, .copy, .readAloud, .newLine:
            composerEditMenuState = nil
        }
    }

    private func handleVoiceGestureChanged(translation: CGSize, location: CGPoint?) {
        guard canStartVoiceTranscription else { return }
        if !isPressingVoice { beginVoiceTranscription() }
        guard isPressingVoice else { return }
        let target = transcriptionTarget(for: translation, location: location)
        if transcriptionPresentation.target != target {
            transcriptionPresentation.target = target
        }
    }

    private func beginVoiceTranscription() {
        guard !isPressingVoice else { return }
        guard draft.isEmpty else { return }
        guard speechRecognizer.isAvailable else {
            model.error = "语音转文字凭证未配置"
            return
        }
        focusController.dismiss()
        isPressingVoice = true
        voiceBaseDraft = draft
        transcriptionPresentation.prepareForNewSession(
            account: "ai-assistant",
            peer: model.selected
        )
        transcriptionPresentation.target = .send
        AppDiagnosticLog.shared.record(
            level: .info,
            category: "asr",
            event: "gesture-started",
            fields: ["mode": "ai-assistant-transcription"]
                .merging(transcriptionPresentation.diagnosticFields) { _, context in context }
        )
        voiceStartTask = Task { @MainActor in
            do {
                try await speechRecognizer.start(
                    diagnosticFields: transcriptionPresentation.diagnosticFields
                )
                return true
            } catch is CancellationError {
                return false
            } catch {
                isPressingVoice = false
                transcriptionPresentation.reset()
                model.error = "语音转文字失败：\(error.localizedDescription)"
                return false
            }
        }
    }

    private func finishVoiceGesture(
        translation: CGSize,
        location: CGPoint?,
        explicitTarget: VoiceTranscriptionTarget? = nil
    ) async {
        guard isPressingVoice else { return }
        isPressingVoice = false
        let target = explicitTarget ?? transcriptionTarget(
            for: translation,
            location: location
        )
        let diagnosticFields = transcriptionPresentation.diagnosticFields
        if target == .cancel {
            AppDiagnosticLog.shared.record(
                level: .info,
                category: "asr",
                event: "gesture-ended",
                fields: ["action": "cancel"]
                    .merging(diagnosticFields) { _, context in context }
            )
            cancelVoiceTranscription(restoresDraft: true)
            return
        }

        let shouldEdit = target == .edit
        transcriptionPresentation.target = shouldEdit ? .finishingEdit : .finishingSend
        AppDiagnosticLog.shared.record(
            level: .info,
            category: "asr",
            event: "gesture-ended",
            fields: ["action": shouldEdit ? "edit" : "send"]
                .merging(diagnosticFields) { _, context in context }
        )
        let startTask = voiceStartTask
        voiceStartTask = nil
        let didStart = await startTask?.value ?? speechRecognizer.isRecognizing
        guard didStart, speechRecognizer.isRecognizing else {
            transcriptionPresentation.reset()
            return
        }
        do {
            let text = try await speechRecognizer.stop()
                .trimmingCharacters(in: .whitespacesAndNewlines)
            transcriptionPresentation.reset()
            guard !text.isEmpty else {
                model.showTransientError("没有识别到文字")
                return
            }
            if shouldEdit {
                draft = voiceText(base: voiceBaseDraft, transcript: text)
                focusController.focus()
            } else {
                _ = await model.send(text)
            }
        } catch is CancellationError {
            transcriptionPresentation.reset()
        } catch {
            transcriptionPresentation.reset()
            model.error = "语音转文字失败：\(error.localizedDescription)"
        }
    }

    private func cancelVoiceTranscription(restoresDraft: Bool) {
        let hadActiveTranscription = isPressingVoice || speechRecognizer.isRecognizing || voiceStartTask != nil
        voiceStartTask?.cancel()
        voiceStartTask = nil
        speechRecognizer.cancel()
        isPressingVoice = false
        transcriptionPresentation.reset()
        if restoresDraft, hadActiveTranscription { draft = voiceBaseDraft }
    }

    private func transcriptionTarget(
        for translation: CGSize,
        location: CGPoint?
    ) -> VoiceTranscriptionTarget {
        VoiceTranscriptionHitTest.target(
            translation: translation,
            location: location,
            cancelFrame: transcriptionPresentation.actionFrames[.cancel],
            editFrame: transcriptionPresentation.actionFrames[.edit]
        )
    }

    private func voiceText(base: String, transcript: String) -> String {
        let cleaned = transcript.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !base.isEmpty, !cleaned.isEmpty else { return base.isEmpty ? cleaned : base }
        let separator = base.last?.isWhitespace == true ? "" : " "
        return base + separator + cleaned
    }
}

private struct AITranscriptionButton: View {
    let isPressing: Bool
    let isCancelling: Bool
    let isEnabled: Bool
    let onChanged: (CGSize) -> Void
    let onEnded: (CGSize) -> Void

    var body: some View {
        Text(title)
            .font(.system(size: 15, weight: .semibold))
            .foregroundStyle(isEnabled ? RemoteIMStyle.textPrimary : RemoteIMStyle.textSecondary)
            .frame(maxWidth: .infinity, minHeight: 44)
            .background(backgroundColor, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .stroke(borderColor, lineWidth: isPressing ? 1.5 : 1)
            )
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { onChanged($0.translation) }
                    .onEnded { onEnded($0.translation) }
            )
            .allowsHitTesting(isEnabled)
            .accessibilityLabel("按住转文字")
    }

    private var title: String {
        if !isEnabled { return "清空文字后可按住转文字" }
        if isCancelling { return "松开取消" }
        return isPressing ? "松开处理" : "按住 转文字"
    }

    private var backgroundColor: Color {
        if !isEnabled { return Color(uiColor: .secondarySystemBackground) }
        if isCancelling { return Color.red.opacity(0.08) }
        return isPressing ? RemoteIMStyle.blueSoft : Color.white
    }

    private var borderColor: Color {
        if isCancelling { return .red }
        return isPressing ? RemoteIMStyle.blue : RemoteIMStyle.border
    }
}

private struct AIPermissionCard: View {
    let permission: AIPermission
    @ObservedObject var model: AIAssistantModel
    @State private var showsRawInput = false
    private var fields: [String: Any] {
        guard let data = permission.input.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data),
              let fields = object as? [String: Any] else { return [:] }
        return fields
    }
    private var isSendText: Bool { permission.tool == "maichat_send_text" }
    private var isPaidGeneration: Bool {
        ["wan_video", "wan_video_edit", "seedance_video", "seedream_image", "qwen_image",
         "glm_video", "glm_image", "kling_video", "kling_image", "minimax_video", "minimax_image"]
            .contains(permission.tool)
    }
    private var isVideoGeneration: Bool {
        ["wan_video", "wan_video_edit", "seedance_video", "glm_video", "kling_video",
         "minimax_video"].contains(permission.tool)
    }
    private var isRevision: Bool {
        (fields["action"] as? String) == "revise" ||
        ["edit", "extend"].contains((fields["mode"] as? String) ?? "") ||
        permission.tool == "wan_video_edit"
    }
    private var paidTitle: String {
        if isVideoGeneration { return isRevision ? "确认编辑视频" : "确认生成视频" }
        return isRevision ? "确认编辑图片" : "确认生成图片"
    }
    private var paidActionTitle: String { isRevision ? "确认编辑" : "确认生成" }
    private var paidProvider: String {
        switch permission.tool {
        case "wan_video", "wan_video_edit": return "万相"
        case "qwen_image": return "通义千问"
        case "seedance_video": return "Seedance"
        case "glm_video", "glm_image": return "GLM"
        case "kling_video", "kling_image": return "可灵"
        case "minimax_video", "minimax_image": return "海螺 / MiniMax"
        default: return "Seedream"
        }
    }
    private var paidRequest: String {
        ((fields["message"] as? String) ?? (fields["prompt"] as? String) ?? "")
            .trimmingCharacters(in: .whitespacesAndNewlines)
    }
    private var paidDetails: String {
        let production = fields["production"] as? [String: Any] ?? [:]
        let content = fields["content"] as? [[String: Any]] ?? []
        var details: [String] = []
        if permission.tool == "minimax_video", let model = fields["model"] as? String {
            details.append(model == "MiniMax-H3-Max" ? "H3 Max" : "H3")
        }
        if let duration = (fields["duration"] as? Int) ?? (production["duration"] as? Int) {
            details.append("\(duration) 秒")
        }
        if let resolution = (fields["resolution"] as? String) ??
            (production["resolution"] as? String) { details.append(resolution) }
        if let ratio = (fields["ratio"] as? String) ??
            (production["ratio"] as? String) {
            details.append(ratio == "adaptive" ? "按素材比例" : ratio)
        }
        if (fields["virtual_avatar_asset_id"] as? String)?.isEmpty == false {
            details.append("平台虚拟人像")
        }
        if (fields["authorized_portrait_asset_id"] as? String)?.isEmpty == false {
            details.append("已授权真人形象")
        }
        if let size = fields["size"] as? String { details.append(size) }
        let imageCount = ((fields["reference_image_paths"] as? [String])?.count ??
                          (fields["image_paths"] as? [String])?.count ?? 0)
            + (content.filter { ($0["type"] as? String) == "image_url" }.count)
            + (((fields["reference_image_path"] as? String)?.isEmpty == false) ? 1 : 0)
            + ((((fields["image_path"] as? String)?.isEmpty == false) ||
                ((fields["first_frame"] as? String)?.isEmpty == false)) ? 1 : 0)
            + ((((fields["last_frame_path"] as? String)?.isEmpty == false) ||
                ((fields["last_frame"] as? String)?.isEmpty == false)) ? 1 : 0)
        if imageCount > 0 { details.append("参考图片 \(imageCount) 张") }
        let videoCount = content.filter { ($0["type"] as? String) == "video_url" }.count
            + ((((fields["reference_video_path"] as? String)?.isEmpty == false) ||
                ((fields["reference_video"] as? String)?.isEmpty == false) ||
                ((fields["video_path"] as? String)?.isEmpty == false)) ? 1 : 0)
        if videoCount > 0 { details.append("参考视频 \(videoCount) 个") }
        let audioCount = content.filter { ($0["type"] as? String) == "audio_url" }.count
            + ((((fields["reference_audio_path"] as? String)?.isEmpty == false) ||
                ((fields["reference_audio"] as? String)?.isEmpty == false)) ? 1 : 0)
        if audioCount > 0 { details.append("参考音频 \(audioCount) 个") }
        return details.joined(separator: " · ")
    }
    private var formattedInput: String {
        guard !fields.isEmpty,
              let data = try? JSONSerialization.data(withJSONObject: fields,
                                                       options: [.prettyPrinted, .sortedKeys]),
              let result = String(data: data, encoding: .utf8) else { return permission.input }
        return result
    }
    private var approveTitle: String {
        guard permission.rememberOnApproval == true else { return "允许一次" }
        return (permission.fileCount ?? 0) > 1 ? "允许并记住这些文件" : "允许并记住此文件"
    }
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Label(isPaidGeneration ? paidTitle : (isSendText ? "确认发送消息" : "确认工具操作"),
                  systemImage: isPaidGeneration ? "sparkles" : "hand.raised")
                .font(.headline)
                .foregroundStyle(Color.primary)
            if isPaidGeneration {
                Text((fields["virtual_avatar_asset_id"] as? String)?.isEmpty == false
                     ? "将使用平台虚拟人像，不保留真实人物长相。确认后提交给\(paidProvider)，可能消耗模型额度。"
                     : (fields["authorized_portrait_asset_id"] as? String)?.isEmpty == false
                         ? "将使用已授权真人形象。确认后提交给\(paidProvider)，可能消耗模型额度。"
                         : "确认后将提交给\(paidProvider)，可能消耗模型额度。")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                if !paidRequest.isEmpty {
                    ScrollView {
                        Text(paidRequest)
                            .font(.body)
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .frame(maxHeight: 150)
                    .padding(12)
                    .background(Color(uiColor: .systemBackground),
                                in: RoundedRectangle(cornerRadius: 10))
                }
                if !paidDetails.isEmpty {
                    Text(paidDetails)
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                }
            } else if isSendText, let peer = fields["peer_id"] as? String,
               let text = fields["text"] as? String {
                Label(peer, systemImage: "person.crop.circle")
                    .font(.subheadline).foregroundStyle(.secondary)
                ScrollView {
                    Text(text)
                        .font(.body)
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxHeight: 220)
                .padding(12)
                .background(Color(uiColor: .systemBackground),
                            in: RoundedRectangle(cornerRadius: 10))
            } else {
                Text(permission.tool).font(.subheadline.bold()).foregroundStyle(.secondary)
                ScrollView {
                    Text(formattedInput)
                        .font(.system(.caption, design: .monospaced))
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxHeight: 220)
                .padding(12)
                .background(Color(uiColor: .systemBackground),
                            in: RoundedRectangle(cornerRadius: 10))
            }
            if isSendText || isPaidGeneration {
                DisclosureGroup(isPaidGeneration ? "查看完整请求" : "查看完整参数",
                                isExpanded: $showsRawInput) {
                    ScrollView {
                        Text(formattedInput)
                            .font(.system(.caption, design: .monospaced))
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .frame(maxHeight: 180)
                    .padding(.top, 6)
                }
                .font(.caption)
            }
            HStack {
                action(isPaidGeneration ? "取消" : "拒绝", "denied")
                action(isPaidGeneration ? paidActionTitle : approveTitle, "approved")
            }
            if permission.allowForSession != false && permission.rememberOnApproval != true {
                action("本会话允许", "approved_for_session")
            }
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.orange.opacity(0.06), in: RoundedRectangle(cornerRadius: 14))
        .overlay(RoundedRectangle(cornerRadius: 14).stroke(Color.orange.opacity(0.16)))
    }
    private func action(_ title: String, _ decision: String) -> some View {
        Button(title) { Task { await model.action("permission", values: ["id": permission.id, "decision": decision]) } }
            .buttonStyle(.plain).font(.system(size: 12, weight: .semibold))
            .foregroundStyle(decision == "denied" ? Color.secondary : Color.orange)
            .padding(.horizontal, 10).frame(height: 32)
            .background(Color(uiColor: .systemBackground), in: RoundedRectangle(cornerRadius: 8))
            .overlay(RoundedRectangle(cornerRadius: 8).stroke(Color.black.opacity(0.08)))
    }
}

#if targetEnvironment(simulator)
struct AIPermissionUITestRoot: View {
    private let permission = AIPermission(
        id: "preview", tool: "maichat_send_text",
        input: "{\"peer_id\":\"demo-contact\",\"text\":\"" +
            String(repeating: "A paragraph for approval.\\n", count: 60) + "\"}",
        allowForSession: false, rememberOnApproval: false, fileCount: nil
    )

    var body: some View {
        ScrollView {
            AIPermissionCard(permission: permission, model: .shared)
                .padding(16)
        }
        .background(Color(uiColor: .systemBackground))
    }
}
#endif

private struct AIQuestionCard: View {
    let question: AIQuestion
    @ObservedObject var model: AIAssistantModel
    @State private var answer = ""
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(question.question).font(.subheadline.bold())
            ForEach(question.options, id: \.self) { option in
                Button(option) { reply(option) }.buttonStyle(.plain).font(.system(size: 13, weight: .medium))
                    .foregroundStyle(Color.blue).padding(.horizontal, 12).frame(minHeight: 34)
                    .background(Color.blue.opacity(0.08), in: RoundedRectangle(cornerRadius: 9))
            }
            HStack {
                TextField("你的回答", text: $answer)
                    .padding(.horizontal, 10).frame(height: 38)
                    .background(Color(uiColor: .systemBackground), in: RoundedRectangle(cornerRadius: 9))
                Button("回复") { reply(answer) }.buttonStyle(.plain).font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(Color.white).padding(.horizontal, 12).frame(height: 38)
                    .background(Color.blue, in: RoundedRectangle(cornerRadius: 9))
                    .disabled(answer.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
            }
        }.padding().background(Color.blue.opacity(0.06), in: RoundedRectangle(cornerRadius: 12))
    }
    private func reply(_ text: String) { Task { await model.action("answer", values: ["id": question.id, "text": text]) } }
}
private struct AISettingsView: View {
    @ObservedObject var model: AIAssistantModel
    let close: () -> Void
    @State private var settings = AIModelSettings()
    @State private var apiKey = ""
    @State private var seedanceKey = ""
    @State private var seedanceConfigured = false
    @State private var seedanceMessage = ""
    @State private var wanKey = ""
    @State private var wanWorkspaceId = ""
    @State private var wanConfigured = false
    @State private var wanMessage = ""
    @State private var klingKey = ""
    @State private var klingConfigured = false
    @State private var miniMaxKey = ""
    @State private var miniMaxConfigured = false
    @State private var creativeMessage = ""
    @State private var saving = false

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Button(action: close) {
                    Image(systemName: "xmark")
                        .font(.system(size: 14, weight: .bold))
                        .frame(width: 36, height: 36)
                        .background(Color.white, in: Circle())
                        .overlay(Circle().stroke(Color.black.opacity(0.08)))
                }
                .buttonStyle(.plain)
                .disabled(saving)
                .accessibilityLabel("关闭模型配置")
                Spacer()
                Text("模型配置").font(.system(size: 18, weight: .bold))
                Spacer()
                Color.clear.frame(width: 36, height: 36)
            }
            .padding(.horizontal, 18)
            .frame(height: 64)

            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    VStack(alignment: .leading, spacing: 6) {
                        Text("连接你的模型服务").font(.system(size: 22, weight: .bold))
                        Text("支持兼容 Chat Completions 的接口，密钥只保存在本机 Keychain。")
                            .font(.system(size: 13)).foregroundStyle(.secondary)
                    }

                    settingsField("API 地址", systemImage: "link") {
                        TextField("https://example.com/v1", text: $settings.baseUrl)
                            .keyboardType(.URL)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled()
                            .textFieldStyle(.plain)
                    }
                    settingsField("模型名称", systemImage: "cpu") {
                        TextField("模型名称", text: $settings.model)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled()
                            .textFieldStyle(.plain)
                    }
                    settingsField("API Key", systemImage: "key") {
                        SecureField(model.configured ? "留空保留原密钥" : "输入 API Key", text: $apiKey)
                            .textFieldStyle(.plain)
                    }
                    settingsField("方舟创作 Key（Seedance / Seedream）", systemImage: "film") {
                        SecureField(seedanceConfigured ? "已配置，留空则保留" : "输入方舟 API Key",
                                    text: $seedanceKey)
                            .textFieldStyle(.plain)
                    }
                    Button("保存方舟创作 Key") {
                        do {
                            try KeychainSecretStore(account: "seedance-ark-api-key")
                                .saveSecretKey(seedanceKey)
                            seedanceConfigured = !seedanceKey.isEmpty
                            seedanceKey = ""
                            seedanceMessage = seedanceConfigured ? "已保存到本机 Keychain" : "已清除方舟创作 Key"
                        } catch {
                            seedanceMessage = error.localizedDescription
                        }
                    }
                    .disabled(seedanceKey.isEmpty)
                    if !seedanceMessage.isEmpty {
                        Text(seedanceMessage).font(.system(size: 12)).foregroundStyle(.secondary)
                    }
                    settingsField("百炼创作 Key（Wan / Qwen）", systemImage: "film.stack") {
                        SecureField(wanConfigured ? "已配置，留空保留原密钥" : "输入百炼 API Key",
                                    text: $wanKey)
                            .textFieldStyle(.plain)
                    }
                    settingsField("百炼 Workspace ID", systemImage: "square.stack.3d.up") {
                        TextField("ws-…", text: $wanWorkspaceId)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled()
                            .textFieldStyle(.plain)
                    }
                    Text("点击下方“保存配置”，同时保存百炼 Key 和 Workspace ID。")
                        .font(.system(size: 12)).foregroundStyle(.secondary)
                    if !wanMessage.isEmpty {
                        Text(wanMessage).font(.system(size: 12)).foregroundStyle(.secondary)
                    }
                    settingsField("可灵 API Key", systemImage: "film") {
                        SecureField(klingConfigured ? "已配置，留空保留原密钥" : "输入可灵 API Key",
                                    text: $klingKey)
                            .textFieldStyle(.plain)
                    }
                    settingsField("海螺 / MiniMax API Key", systemImage: "photo.on.rectangle") {
                        SecureField(miniMaxConfigured ? "已配置，留空保留原密钥" : "输入 MiniMax API Key",
                                    text: $miniMaxKey)
                            .textFieldStyle(.plain)
                    }
                    if !creativeMessage.isEmpty {
                        Text(creativeMessage).font(.system(size: 12)).foregroundStyle(.secondary)
                    }
                    Text("本地图片处理使用 FFmpeg（LGPLv2.1+）；完整源码随项目放在 MaiAgent/third_party/ffmpeg。")
                        .font(.system(size: 12)).foregroundStyle(.secondary)
                    if !model.error.isEmpty {
                        Text(model.error).foregroundStyle(.red).font(.system(size: 12))
                            .padding(12).frame(maxWidth: .infinity, alignment: .leading)
                            .background(Color.red.opacity(0.06), in: RoundedRectangle(cornerRadius: 10))
                    }

                    Button {
                        saving = true
                        Task {
                            guard saveWanConfiguration() else {
                                saving = false
                                return
                            }
                            guard saveCreativeKeys() else {
                                saving = false
                                return
                            }
                            if await model.save(settings, key: apiKey) { close() }
                            saving = false
                        }
                    } label: {
                        HStack(spacing: 8) {
                            if saving { ProgressView().tint(.white) }
                            Text(saving ? "保存中" : "保存配置")
                                .font(.system(size: 15, weight: .bold))
                        }
                        .foregroundStyle(Color.white)
                        .frame(maxWidth: .infinity)
                        .frame(height: 48)
                        .background(Color.blue, in: RoundedRectangle(cornerRadius: 14))
                    }
                    .buttonStyle(.plain)
                    .disabled(saving || model.sessions.contains(where: \.busy))
                }
                .padding(.horizontal, 20)
                .padding(.bottom, 28)
            }
        }
        .background(Color(red: 0.97, green: 0.98, blue: 1.0).ignoresSafeArea())
        .onAppear {
            settings = model.settings
            seedanceConfigured = !KeychainSecretStore(account: "seedance-ark-api-key")
                .readSecretKey().isEmpty
            wanConfigured = !KeychainSecretStore(account: "wan-model-studio-api-key")
                .readSecretKey().isEmpty
            wanWorkspaceId = UserDefaults.standard.string(forKey: "wan-model-studio-workspace-id") ?? ""
            klingConfigured = !KeychainSecretStore(account: "kling-creative-api-key")
                .readSecretKey().isEmpty
            miniMaxConfigured = !KeychainSecretStore(account: "minimax-creative-api-key")
                .readSecretKey().isEmpty
        }
        .accessibilityIdentifier("ai-model-settings")
    }

    private func saveWanConfiguration() -> Bool {
        let workspaceId = wanWorkspaceId.trimmingCharacters(in: .whitespacesAndNewlines)
        let key = wanKey.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !workspaceId.isEmpty || !key.isEmpty || wanConfigured else { return true }
        guard (workspaceId.hasPrefix("ws-") || workspaceId.hasPrefix("llm-")),
              workspaceId.count > 4 else {
            wanMessage = "请填写完整的百炼 Workspace ID"
            return false
        }
        guard !key.isEmpty || wanConfigured else {
            wanMessage = "请填写百炼 API Key"
            return false
        }
        do {
            if !key.isEmpty {
                try KeychainSecretStore(account: "wan-model-studio-api-key").saveSecretKey(key)
            }
            UserDefaults.standard.set(workspaceId, forKey: "wan-model-studio-workspace-id")
            wanConfigured = true
            wanKey = ""
            wanMessage = "百炼配置已保存"
            return true
        } catch {
            wanMessage = error.localizedDescription
            return false
        }
    }

    private func saveCreativeKeys() -> Bool {
        do {
            let kling = klingKey.trimmingCharacters(in: .whitespacesAndNewlines)
            let miniMax = miniMaxKey.trimmingCharacters(in: .whitespacesAndNewlines)
            if !kling.isEmpty {
                try KeychainSecretStore(account: "kling-creative-api-key").saveSecretKey(kling)
                klingConfigured = true
                klingKey = ""
            }
            if !miniMax.isEmpty {
                try KeychainSecretStore(account: "minimax-creative-api-key").saveSecretKey(miniMax)
                miniMaxConfigured = true
                miniMaxKey = ""
            }
            if !kling.isEmpty || !miniMax.isEmpty { creativeMessage = "创作模型 Key 已保存到本机 Keychain" }
            return true
        } catch {
            creativeMessage = error.localizedDescription
            return false
        }
    }

    private func settingsField<Content: View>(
        _ title: String,
        systemImage: String,
        @ViewBuilder content: () -> Content
    ) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Label(title, systemImage: systemImage)
                .font(.system(size: 13, weight: .semibold))
                .foregroundStyle(Color.secondary)
            content()
                .font(.system(size: 15))
                .padding(.horizontal, 14)
                .frame(minHeight: 48)
                .background(Color.white, in: RoundedRectangle(cornerRadius: 13))
                .overlay(RoundedRectangle(cornerRadius: 13).stroke(Color.black.opacity(0.08)))
        }
    }
}
