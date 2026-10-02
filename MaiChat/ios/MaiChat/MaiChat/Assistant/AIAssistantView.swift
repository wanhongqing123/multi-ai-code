import AVFoundation
import CoreTransferable
import MaiChatCore
import Photos
import PhotosUI
import SwiftUI
import UIKit
import UniformTypeIdentifiers

struct AIAssistantView: View {
    let onExit: (() -> Void)?
    @ObservedObject private var model = AIAssistantModel.shared
    @Environment(\.scenePhase) private var scenePhase
    @State private var showSessions = false
    @State private var showActions = false
    @State private var confirmClear = false
    @State private var followsBottom = true
    @State private var sessionDrawerOffset: CGFloat = 0
    @State private var sessionDrawerWidth: CGFloat = 320
    @State private var isAttachmentPanelPresented = false
    @State private var composerFocusController = AIComposerFocusController()
    @State private var transcriptionPresentation = VoiceTranscriptionPresentation()

    init(onExit: (() -> Void)? = nil) {
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
                    ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
                } else {
                    GeometryReader { _ in
                        ScrollView {
                            VStack(alignment: .leading, spacing: 24) {
                                if model.messages.isEmpty {
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
                                    }.frame(maxWidth: .infinity).padding(.vertical, 70)
                                }
                                if !model.messages.isEmpty {
                                    LazyVStack(alignment: .leading, spacing: 24) {
                                        ForEach(model.messages) { message in
                                            AIMessageRow(message: message,
                                                         workspacePath: model.workspacePath)
                                        }
                                    }
                                }
                                ForEach(model.permissions) { permission in AIPermissionCard(permission: permission, model: model) }
                                ForEach(model.questions) { question in AIQuestionCard(question: question, model: model) }
                                Color.clear.frame(height: 1).id("bottom")
                                    .background(MessageScrollPositionReader(
                                        restoreInitialScrollableHistory: !model.messages.isEmpty,
                                        allowsBottomFollowing: followsBottom,
                                        onUserScroll: {
                                            followsBottom = false
                                        }
                                    ) { nearBottom in
                                        if nearBottom { followsBottom = true }
                                    })
                            }.padding(18)
                        }
                        .contentShape(Rectangle())
                        .onTapGesture {
                            composerFocusController.dismiss()
                            isAttachmentPanelPresented = false
                        }
                        .onChange(of: model.scrollRequest) { _ in followsBottom = true }
                        .onChange(of: model.selected) { _ in followsBottom = true }
                    }
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
                AIImagePreviewOverlay(filePath: preview.filePath) {
                    model.previewImage = nil
                }
                .id(preview.id)
                .zIndex(30)
            }
        }
        .simultaneousGesture(sessionDrawerOpenGesture)
        .confirmationDialog("清空当前对话的所有消息？", isPresented: $confirmClear, titleVisibility: .visible) {
            Button("清空消息", role: .destructive) { Task { await model.action("clear") } }
        }
        .onAppear {
            if !ProcessInfo.processInfo.arguments.contains("--ai-history-ui-test") {
                model.appear()
            }
        }
        .onDisappear {
            if !ProcessInfo.processInfo.arguments.contains("--ai-history-ui-test") {
                model.disappear()
            }
        }
        .onChange(of: scenePhase) { phase in
            if ProcessInfo.processInfo.arguments.contains("--ai-history-ui-test") { return }
            if phase == .active { model.appear() } else { model.disappear() }
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
                                Text(session.displayTitle).font(.system(size: 15, weight: .medium)).lineLimit(2)
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
    @State private var pdfPreview: AIPDFPreviewItem?
    @State private var pdfPreviewError = false
    @State private var videoPreview: AIVideoPreviewItem?
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            if message.role == "user" {
                HStack {
                    Spacer(minLength: 30)
                    VStack(alignment: .leading, spacing: 10) {
                        Text(message.text)
                            .font(AssistantMessageFont.body)
                            .lineSpacing(6)
                            .textSelection(.enabled)
                        ForEach(Array(imagePaths.enumerated()), id: \.offset) { _, path in
                            AIWorkspaceImage(filePath: path)
                        }
                        ForEach(videoPaths, id: \.self) { path in
                            AIWorkspaceVideoCard(filePath: path) {
                                videoPreview = AIVideoPreviewItem(path: path)
                            }
                        }
                    }
                    .padding(14)
                    .background(Color(uiColor: .secondarySystemBackground), in: RoundedRectangle(cornerRadius: 18))
                }
            } else {
                ForEach(message.parts) { part in
                    if part.kind == "text", let text = part.text, !text.isEmpty {
                        MarkdownLikeText(text, retainsPreviousWhilePreparing: true,
                                         bodyFont: AssistantMessageFont.body, assistantTypography: true)
                    } else if part.kind == "reasoning", part.id == reasoningParts.first?.id,
                              !reasoningText.isEmpty {
                        AIExpandableBlock(title: "思考过程", systemImage: "brain") {
                            Text(reasoningText)
                                .font(AssistantMessageFont.detail)
                                .lineSpacing(5)
                                .textSelection(.enabled)
                        }
                    } else if part.kind == "tool", part.id == toolParts.first?.id {
                        AIExpandableBlock(title: "工具调用 \(toolParts.count) 次 · \(toolGroupStatus)",
                                          subtitle: toolSummary, systemImage: "terminal") {
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
                }
                if message.active {
                    TimelineView(.periodic(from: .now, by: 1)) { context in
                        HStack(spacing: 8) {
                            ProgressView().controlSize(.small)
                            Text("正在思考 · \(max(0, Int(context.date.timeIntervalSince1970) - Int(message.created / 1000))) 秒")
                        }.font(AssistantMessageFont.metadata).foregroundStyle(.secondary)
                    }
                } else {
                    HStack {
                        Text(message.completed == 0 ? "已中断" : "用时 \(max(0, (message.completed - message.created) / 1000)) 秒")
                            .font(AssistantMessageFont.metadata)
                        Button { RemoteIMClipboard.writeText(message.text) } label: {
                            Image(systemName: "doc.on.doc")
                        }
                            .accessibilityLabel("复制回复")
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
    }
    private var reasoningParts: [AIPart] {
        message.parts.filter { $0.kind == "reasoning" }
    }
    private var reasoningText: String {
        var seen = Set<String>()
        return reasoningParts.compactMap { part -> String? in
            guard let text = part.text?.trimmingCharacters(in: .whitespacesAndNewlines),
                  !text.isEmpty, seen.insert(text).inserted else { return nil }
            return text
        }.joined(separator: "\n\n")
    }
    private var toolParts: [AIPart] {
        message.parts.filter { $0.kind == "tool" }
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

    var body: some View {
        RemoteIMAsyncImage(filePath: filePath,
                           maximumPointSize: CGSize(width: 240, height: 200)) { image in
                Image(uiImage: image).resizable().scaledToFit()
                    .frame(width: 240, height: 200)
                    .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
                    .accessibilityLabel("AI 助手图片附件")
        } placeholder: { failed in
            if failed {
                Label("图片无法显示", systemImage: "photo")
                    .foregroundStyle(Color.secondary)
                    .frame(width: 180, height: 120)
            } else {
                Color.clear.frame(width: 240, height: 200)
            }
        }
    }
}

private struct AIWorkspaceVideoCard: View {
    let filePath: String
    let open: () -> Void
    @State private var cover: UIImage?
    @State private var videoSize: CGSize?
    @State private var durationSeconds = 0

    private var exists: Bool { FileManager.default.fileExists(atPath: filePath) }

    var body: some View {
        Button(action: open) {
            VStack(alignment: .leading, spacing: 6) {
                ZStack {
                    if let cover {
                        Image(uiImage: cover).resizable().scaledToFit()
                    } else {
                        LinearGradient(colors: [Color(red: 0.10, green: 0.17, blue: 0.27),
                                                Color(red: 0.18, green: 0.32, blue: 0.47)],
                                       startPoint: .topLeading, endPoint: .bottomTrailing)
                    }
                    Image(systemName: exists ? "play.fill" : "exclamationmark.triangle.fill")
                        .font(.system(size: 20, weight: .bold))
                        .foregroundStyle(.white)
                        .frame(width: 52, height: 52)
                        .background(.black.opacity(0.5), in: Circle())
                    Text(String(format: "%d:%02d", durationSeconds / 60,
                                durationSeconds % 60))
                        .font(.system(size: 11, weight: .semibold, design: .monospaced))
                        .foregroundStyle(.white)
                        .padding(.horizontal, 7)
                        .frame(height: 22)
                        .background(.black.opacity(0.56), in: Capsule())
                        .frame(maxWidth: .infinity, maxHeight: .infinity,
                               alignment: .bottomTrailing)
                        .padding(8)
                }
                .frame(width: previewSize.width, height: previewSize.height)
                .clipped()
                .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
                Text(exists ? URL(fileURLWithPath: filePath).lastPathComponent : "视频文件已丢失")
                    .font(AssistantMessageFont.detail).lineLimit(1).truncationMode(.middle)
                    .frame(width: previewSize.width, alignment: .leading)
            }
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
               let transform = try? await track.load(.preferredTransform) {
                let displayed = CGRect(origin: .zero, size: naturalSize).applying(transform)
                videoSize = CGSize(width: abs(displayed.width), height: abs(displayed.height))
            }
            if let duration = try? await asset.load(.duration),
               duration.seconds.isFinite {
                durationSeconds = max(0, Int(duration.seconds))
            }
            let generator = AVAssetImageGenerator(asset: asset)
            generator.appliesPreferredTrackTransform = true
            generator.maximumSize = CGSize(width: 440, height: 480)
            let time = CMTime(seconds: durationSeconds > 1 ? 0.5 : 0,
                              preferredTimescale: 600)
            if let image = try? await generator.image(at: time), !Task.isCancelled {
                videoSize = CGSize(width: image.image.width, height: image.image.height)
                cover = UIImage(cgImage: image.image)
            }
        }
    }

    private var previewSize: CGSize {
        VideoBubbleSize.fitted(videoSize ?? .zero)
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
                         workspacePath: video.deletingLastPathComponent().path)
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
        AIMessageRow(message: message, workspacePath: "")
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
    let filePath: String
    let close: () -> Void
    @State private var failed = false

    var body: some View {
        GeometryReader { geometry in
            ZStack(alignment: .topTrailing) {
                Color.black.ignoresSafeArea()
                if !failed {
                    MaiGraphicsImageSurface(filePath: filePath) { failed = true }
                        .frame(width: geometry.size.width, height: geometry.size.height)
                        .accessibilityLabel("Agent 处理后的图片预览")
                } else {
                    Label("图片无法显示", systemImage: "photo")
                        .foregroundStyle(.white)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
                Button(action: close) {
                    Image(systemName: "xmark")
                        .font(.system(size: 17, weight: .bold))
                        .foregroundStyle(.white)
                        .frame(width: 44, height: 44)
                        .background(.black.opacity(0.5), in: Circle())
                }
                .accessibilityLabel("关闭图片预览")
                .padding(20)
            }
        }
        .onChange(of: filePath) { _ in failed = false }
    }
}

private struct AIExpandableBlock<Content: View>: View {
    let title: String
    let subtitle: String?
    let systemImage: String
    let content: Content
    @State private var expanded = false

    init(title: String, subtitle: String? = nil, systemImage: String,
         @ViewBuilder content: () -> Content) {
        self.title = title
        self.subtitle = subtitle
        self.systemImage = systemImage
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Button { withAnimation(.easeOut(duration: 0.16)) { expanded.toggle() } } label: {
                HStack(spacing: 8) {
                    Image(systemName: systemImage).frame(width: 16)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(title).font(AssistantMessageFont.detail.weight(.medium)).lineLimit(1)
                        if let subtitle, !subtitle.isEmpty {
                            Text(subtitle).font(AssistantMessageFont.metadata).lineLimit(1)
                        }
                    }
                    Spacer()
                    Image(systemName: "chevron.right").rotationEffect(.degrees(expanded ? 90 : 0))
                }.foregroundStyle(Color.secondary)
                    .padding(.horizontal, 10).frame(minHeight: subtitle == nil ? 38 : 52)
                    .background(Color(uiColor: .secondarySystemBackground), in: RoundedRectangle(cornerRadius: 9))
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
                                .font(.system(size: 14, weight: isPressingVoice ? .semibold : .regular))
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
        let videoPrompt = videos.isEmpty ? "" :
            (videos.count == 1 ? "请查看这个视频。" : "请查看这些视频。")
        let imagePrompt = images.isEmpty ? "" :
            (images.count == 1 ? "请查看这张图片。" : "请查看这些图片。")
        let prompt = [existingDraft, videoPrompt, imagePrompt]
            .filter { !$0.isEmpty }.joined(separator: "\n")
        if await model.send(prompt, images: images, videos: videos,
                            expectedSession: targetSession) {
            if existingDraft.isEmpty || draft == prompt { draft = "" }
            return
        }

        // 导入已经完成但发送条件在异步读取期间改变时，保留媒体和提示，
        // 让用户之后按回车重试，不能丢掉刚选中的内容。
        for file in images + videos { model.addAttachment(file, to: targetSession) }
        if targetSession == model.selected {
            if draft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty { draft = prompt }
        } else if model.drafts[targetSession, default: ""]
            .trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            model.drafts[targetSession] = prompt
        }
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
            Label(isSendText ? "确认发送消息" : "确认工具操作", systemImage: "hand.raised")
                .font(.headline)
                .foregroundStyle(Color.primary)
            if isSendText, let peer = fields["peer_id"] as? String,
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
            if isSendText {
                DisclosureGroup("查看完整参数", isExpanded: $showsRawInput) {
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
                action("拒绝", "denied")
                action(approveTitle, "approved")
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
        .onAppear { settings = model.settings }
        .accessibilityIdentifier("ai-model-settings")
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
