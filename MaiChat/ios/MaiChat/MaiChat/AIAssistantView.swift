import AVFoundation
import ImageIO
import Photos
import PhotosUI
import SwiftUI
import UIKit
import UniformTypeIdentifiers

struct AIAssistantView: View {
    @ObservedObject private var model = AIAssistantModel.shared
    @Environment(\.scenePhase) private var scenePhase
    @State private var showSessions = false
    @State private var showActions = false
    @State private var confirmClear = false
    @State private var followsBottom = true
    @State private var userDragging = false
    @State private var latestY: CGFloat = 0
    @State private var sessionDrawerOffset: CGFloat = 0
    @State private var sessionDrawerWidth: CGFloat = 320
    @State private var composerFocusController = AIComposerFocusController()
    @State private var transcriptionPresentation = VoiceTranscriptionPresentation()

    var body: some View {
        ZStack(alignment: .topTrailing) {
            VStack(spacing: 0) {
                AIHeader(
                    openSessions: {
                        composerFocusController.dismiss()
                        showActions = false
                        openSessionDrawer()
                    },
                    openActions: { composerFocusController.dismiss(); showActions.toggle() }
                )
                Divider().overlay(Color.black.opacity(0.06))
                if !model.ready {
                    ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
                } else {
                    GeometryReader { geometry in
                        ScrollViewReader { proxy in
                            ScrollView {
                                LazyVStack(alignment: .leading, spacing: 24) {
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
                                    ForEach(model.messages) { message in
                                        AIMessageRow(message: message, workspacePath: model.workspacePath)
                                    }
                                    ForEach(model.permissions) { permission in AIPermissionCard(permission: permission, model: model) }
                                    ForEach(model.questions) { question in AIQuestionCard(question: question, model: model) }
                                    Color.clear.frame(height: 1).id("bottom")
                                        .background(GeometryReader { anchor in
                                            Color.clear.preference(key: AIBottomPreference.self,
                                                value: anchor.frame(in: .named("ai-scroll")).maxY)
                                        })
                                }.padding(18)
                            }
                            .coordinateSpace(name: "ai-scroll")
                            .contentShape(Rectangle())
                            .onTapGesture { composerFocusController.dismiss() }
                            .onPreferenceChange(AIBottomPreference.self) { y in
                                latestY = y
                                let nearBottom = y < geometry.size.height + 40
                                if followsBottom && !nearBottom { proxy.scrollTo("bottom", anchor: .bottom) }
                                else if !userDragging && nearBottom { followsBottom = true }
                            }
                            .simultaneousGesture(DragGesture().onChanged { _ in
                                userDragging = true; followsBottom = false
                            }.onEnded { _ in
                                userDragging = false
                                followsBottom = latestY < geometry.size.height + 40
                            })
                            .onChange(of: geometry.size.height) { _ in
                                if followsBottom { proxy.scrollTo("bottom", anchor: .bottom) }
                            }
                            .onChange(of: model.scrollRequest) { _ in followsBottom = true; proxy.scrollTo("bottom", anchor: .bottom) }
                            .onChange(of: model.messages) { _ in
                                if followsBottom { proxy.scrollTo("bottom", anchor: .bottom) }
                            }
                            .overlay(alignment: .bottomTrailing) {
                                if !followsBottom {
                                    Button { followsBottom = true; proxy.scrollTo("bottom", anchor: .bottom) } label: {
                                        Image(systemName: "arrow.down").padding(12).background(.regularMaterial, in: Circle())
                                    }.padding()
                                }
                            }
                        }
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
                    transcriptionPresentation: transcriptionPresentation
                )
                    .padding(.horizontal, 12).padding(.vertical, 8)
            }
            .background(Color(uiColor: .systemBackground))
            if showActions {
                Color.black.opacity(0.001).ignoresSafeArea().contentShape(Rectangle())
                    .onTapGesture { showActions = false }
                AIActionPanel(
                    canClear: !model.busy && !model.selected.isEmpty,
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
        }
        .simultaneousGesture(sessionDrawerOpenGesture)
        .confirmationDialog("清空当前对话的所有消息？", isPresented: $confirmClear, titleVisibility: .visible) {
            Button("清空消息", role: .destructive) { Task { await model.action("clear") } }
        }
        .onAppear { model.appear() }
        .onDisappear { model.disappear() }
        .onChange(of: scenePhase) { phase in
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
    let openSessions: () -> Void
    let openActions: () -> Void

    var body: some View {
        HStack {
            AIHeaderButton(systemImage: "sidebar.left", label: "对话列表", action: openSessions)
            Spacer()
            Text("AI 助手").font(.system(size: 18, weight: .bold))
            Spacer()
            AIHeaderButton(systemImage: "ellipsis", label: "更多", action: openActions)
        }.padding(.horizontal, 16).frame(height: 54).background(Color(uiColor: .systemBackground))
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
    let canClear: Bool
    let configureModel: () -> Void
    let clear: () -> Void

    var body: some View {
        VStack(spacing: 0) {
            action("模型配置", "slider.horizontal.3", Color.primary, configureModel)
            Divider().padding(.leading, 42)
            action("清空当前对话", "trash", canClear ? Color.red : Color.secondary, clear)
                .disabled(!canClear)
        }.frame(width: 176).background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14))
            .overlay(RoundedRectangle(cornerRadius: 14).stroke(Color.black.opacity(0.08)))
            .shadow(color: Color.black.opacity(0.14), radius: 20, y: 8)
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
private struct AIBottomPreference: PreferenceKey {
    static let defaultValue: CGFloat = 0
    static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) { value = nextValue() }
}
private struct AIMessageRow: View {
    let message: AIMessage
    let workspacePath: String
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            if message.role == "user" {
                HStack {
                    Spacer(minLength: 30)
                    VStack(alignment: .leading, spacing: 10) {
                        Text(message.text).font(.system(size: 14)).textSelection(.enabled)
                        ForEach(Array(imagePaths.enumerated()), id: \.offset) { _, path in
                            AIWorkspaceImage(filePath: path)
                        }
                    }
                    .padding(14)
                    .background(Color(uiColor: .secondarySystemBackground), in: RoundedRectangle(cornerRadius: 18))
                }
            } else {
                ForEach(message.parts.filter { $0.kind == "reasoning" } + message.parts.filter { $0.kind != "reasoning" }) { part in
                    if part.kind == "text", let text = part.text, !text.isEmpty {
                        MarkdownLikeText(text, retainsPreviousWhilePreparing: true)
                    } else if part.kind == "reasoning", let text = part.text, !text.isEmpty {
                        AIExpandableBlock(title: "思考过程", systemImage: "brain") {
                            Text(text).font(.caption).textSelection(.enabled)
                        }
                    } else if part.kind == "tool" {
                        AIExpandableBlock(title: "\(toolStatus(part.state)) \(part.tool ?? "")",
                                          systemImage: "terminal") {
                            VStack(alignment: .leading, spacing: 8) {
                                Text(part.input ?? "").font(.system(.caption, design: .monospaced))
                                if let output = part.output, !output.isEmpty { Text(output).font(.system(.caption, design: .monospaced)) }
                                if let error = part.error, !error.isEmpty { Text(error).foregroundStyle(.red) }
                            }.textSelection(.enabled).padding(10).frame(maxWidth: .infinity, alignment: .leading)
                                .background(Color(uiColor: .secondarySystemBackground), in: RoundedRectangle(cornerRadius: 8))
                        }
                    }
                }
                if message.active {
                    TimelineView(.periodic(from: .now, by: 1)) { context in
                        HStack(spacing: 8) {
                            ProgressView().controlSize(.small)
                            Text("正在思考 · \(max(0, Int(context.date.timeIntervalSince1970) - Int(message.created / 1000))) 秒")
                        }.font(.caption).foregroundStyle(.secondary)
                    }
                } else {
                    HStack {
                        Text(message.completed == 0 ? "已中断" : "用时 \(max(0, (message.completed - message.created) / 1000)) 秒").font(.caption2)
                        Button { RemoteIMClipboard.writeText(message.text) } label: {
                            Image(systemName: "doc.on.doc")
                        }
                            .accessibilityLabel("复制回复")
                    }.foregroundStyle(.secondary).font(.caption)
                }
            }
        }.frame(maxWidth: .infinity, alignment: .leading)
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
    private static func isImage(_ url: URL) -> Bool {
        guard let type = UTType(filenameExtension: url.pathExtension) else { return false }
        return type.conforms(to: .image)
    }
    private func toolStatus(_ state: String?) -> String {
        switch state { case "completed": return "已完成"; case "error": return "失败"; case "pending": return "等待授权"; default: return "正在运行" }
    }
}

private struct AIWorkspaceImage: View {
    let filePath: String
    @StateObject private var state = AIWorkspaceImageState()

    var body: some View {
        Group {
            if let image = state.image {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFit()
                    .frame(maxWidth: 240, maxHeight: 200)
                    .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
                    .accessibilityLabel("AI 助手图片附件")
            } else if state.hasFinished {
                Label("图片无法显示", systemImage: "photo")
                    .foregroundStyle(Color.secondary)
                    .frame(width: 180, height: 120)
            } else {
                ProgressView().frame(width: 180, height: 120)
            }
        }
        .task(id: filePath) { await state.load(filePath) }
    }
}

@MainActor
private final class AIWorkspaceImageState: ObservableObject {
    @Published private(set) var image: UIImage?
    @Published private(set) var hasFinished = false

    func load(_ path: String) async {
        image = nil
        hasFinished = false
        let decoded = await Task.detached(priority: .utility) { () -> AIWorkspaceImageBox? in
            let url = URL(fileURLWithPath: path) as CFURL
            guard let source = CGImageSourceCreateWithURL(url, nil),
                  let cgImage = CGImageSourceCreateThumbnailAtIndex(source, 0, [
                    kCGImageSourceCreateThumbnailFromImageAlways: true,
                    kCGImageSourceCreateThumbnailWithTransform: true,
                    kCGImageSourceThumbnailMaxPixelSize: 720
                  ] as CFDictionary)
            else { return nil }
            return AIWorkspaceImageBox(UIImage(cgImage: cgImage))
        }.value
        guard !Task.isCancelled else { return }
        image = decoded?.image
        hasFinished = true
    }
}

private final class AIWorkspaceImageBox: @unchecked Sendable {
    let image: UIImage
    init(_ image: UIImage) { self.image = image }
}

private struct AIExpandableBlock<Content: View>: View {
    let title: String
    let systemImage: String
    let content: Content
    @State private var expanded = false

    init(title: String, systemImage: String, @ViewBuilder content: () -> Content) {
        self.title = title
        self.systemImage = systemImage
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Button { withAnimation(.easeOut(duration: 0.16)) { expanded.toggle() } } label: {
                HStack(spacing: 8) {
                    Image(systemName: systemImage).frame(width: 16)
                    Text(title).lineLimit(1)
                    Spacer()
                    Image(systemName: "chevron.right").rotationEffect(.degrees(expanded ? 90 : 0))
                }.font(.system(size: 12, weight: .medium)).foregroundStyle(Color.secondary)
                    .padding(.horizontal, 10).frame(height: 34)
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

private struct AIComposer: View {
    @ObservedObject var model: AIAssistantModel
    let focusController: AIComposerFocusController
    let transcriptionPresentation: VoiceTranscriptionPresentation
    @StateObject private var speechRecognizer = TencentRealtimeSpeechRecognizer(
        appId: TencentASRCredentials.appId,
        secretId: TencentASRCredentials.secretId,
        secretKey: TencentASRCredentials.secretKey
    )
    @State private var draft = ""
    @State private var importing = false
    @State private var isAttachmentPanelPresented = false
    @State private var isPhotoPickerPresented = false
    @State private var isCameraPresented = false
    @State private var selectedMediaItems: [PhotosPickerItem] = []
    @State private var draftSession = ""
    @State private var isPressingVoice = false
    @State private var showPolicyMenu = false
    @State private var showModelMenu = false
    @State private var voiceBaseDraft = ""
    @State private var voiceStartTask: Task<Bool, Never>?
    var body: some View {
        VStack(spacing: 10) {
            ZStack(alignment: .topLeading) {
                AIComposerTextView(
                    text: $draft,
                    focusController: focusController,
                    voiceTranscriptionEnabled: canStartVoiceTranscription,
                    onSubmit: { submitDraft($0) },
                    onVoiceChanged: { translation, location in
                        handleVoiceGestureChanged(translation: translation, location: location)
                    },
                    onVoiceEnded: { translation, location in
                        Task {
                            await finishVoiceGesture(
                                translation: translation,
                                location: location
                            )
                        }
                    },
                    onVoiceCancelled: { cancelVoiceTranscription(restoresDraft: true) }
                )
                .onChange(of: draft) { updated in
                    guard let submitted = AIComposerSubmissionPolicy.submittedText(
                        currentText: "",
                        replacing: NSRange(location: 0, length: 0),
                        with: updated
                    ) else { return }
                    draft = submitted
                    submitDraft(submitted)
                }
                if draft.isEmpty {
                    Text(composerPrompt)
                        .foregroundStyle(Color.secondary)
                        .font(.system(size: 17, weight: isPressingVoice ? .semibold : .regular))
                        .allowsHitTesting(false)
                        .accessibilityHidden(true)
                }
            }
            HStack {
                Button {
                    focusController.dismiss()
                    withAnimation(.easeOut(duration: 0.18)) {
                        isAttachmentPanelPresented.toggle()
                    }
                } label: {
                    Image(systemName: "plus")
                        .font(.system(size: 17, weight: .medium))
                        .rotationEffect(.degrees(isAttachmentPanelPresented ? 45 : 0))
                        .frame(width: 30, height: 30)
                }
                .buttonStyle(.plain)
                .foregroundStyle(Color.blue)
                .accessibilityLabel(isAttachmentPanelPresented ? "收起更多功能" : "展开更多功能")
                Button {
                    withAnimation(.easeOut(duration: 0.14)) {
                        showModelMenu = false
                        showPolicyMenu.toggle()
                    }
                } label: {
                    HStack(spacing: 5) {
                        Image(systemName: "shield")
                        Text(model.settings.policy == "never" ? "完全访问" : model.settings.policy == "unless-trusted" ? "帮我批准" : "请求批准")
                    }.font(.system(size: 12, weight: .medium))
                        .foregroundStyle(model.settings.policy == "never" ? Color.orange : Color.secondary)
                        .padding(.horizontal, 8).frame(height: 28)
                        .background(Color(uiColor: .secondarySystemBackground), in: Capsule())
                }.buttonStyle(.plain).accessibilityLabel("操作权限")
                Spacer(minLength: 4)
                Button {
                    withAnimation(.easeOut(duration: 0.14)) {
                        showPolicyMenu = false
                        showModelMenu.toggle()
                    }
                } label: {
                    Text(model.configured ? model.settings.model : "未配置模型")
                        .font(.system(size: 12, weight: .medium)).lineLimit(1)
                        .foregroundStyle(Color.blue).padding(.horizontal, 8).frame(height: 28)
                        .background(Color.blue.opacity(0.08), in: Capsule())
                }
                .buttonStyle(.plain)
                .disabled(!model.configured || model.busy || model.isSubmitting)
                .accessibilityLabel("切换模型")
                if model.busy {
                    Button { Task { await model.action("stop") } } label: {
                        Image(systemName: "stop.fill").font(.system(size: 15, weight: .bold))
                            .foregroundStyle(.white).frame(width: 34, height: 34).background(.primary, in: Circle())
                    }
                    .disabled(!model.ready || model.isSubmitting)
                    .accessibilityLabel("停止")
                }
            }
            if isAttachmentPanelPresented {
                Divider()
                ComposerAttachmentPanel(
                    canSendImage: true,
                    canSendVideo: false,
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
                    showsVoiceInput: false
                )
                .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
                .transition(.move(edge: .bottom).combined(with: .opacity))
            }
        }.padding(14).background(Color(uiColor: .systemBackground), in: RoundedRectangle(cornerRadius: 20))
            .overlay(RoundedRectangle(cornerRadius: 20).stroke(Color.gray.opacity(0.22)))
            .overlay(alignment: .bottomLeading) {
                if showPolicyMenu {
                    AIPolicyMenu(selected: model.settings.policy) { policy in
                        showPolicyMenu = false
                        guard policy != model.settings.policy else { return }
                        var next = model.settings
                        next.policy = policy
                        Task { _ = await model.save(next, key: "") }
                    }
                    .offset(x: 34, y: -48)
                    .transition(.scale(scale: 0.96, anchor: .bottomLeading).combined(with: .opacity))
                }
            }
            .overlay(alignment: .bottomTrailing) {
                if showModelMenu {
                    AIModelMenu(selected: model.settings.model) { selected in
                        showModelMenu = false
                        guard selected != model.settings.model else { return }
                        var next = model.settings
                        next.model = selected
                        Task { _ = await model.save(next, key: "") }
                    }
                    .offset(x: -42, y: -48)
                    .transition(.scale(scale: 0.96, anchor: .bottomTrailing).combined(with: .opacity))
                }
            }
            .zIndex(showPolicyMenu || showModelMenu ? 4 : 0)
            .animation(.easeOut(duration: 0.18), value: isAttachmentPanelPresented)
            .photosPicker(
                isPresented: $isPhotoPickerPresented,
                selection: $selectedMediaItems,
                maxSelectionCount: 10,
                selectionBehavior: .ordered,
                matching: .images,
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
                allowedContentTypes: [.plainText, .sourceCode, .json, .image]
            ) { result in
                if case let .success(url) = result {
                    let target = model.selected
                    Task {
                        if let file = await model.importFile(url) {
                            if file.isImage {
                                await sendImportedImages([file], to: target)
                            } else {
                                appendImportedDocument(file, to: target)
                            }
                        }
                    }
                }
            }
            .onChange(of: selectedMediaItems) { items in
                guard !items.isEmpty else { return }
                Task { await importSelectedImages(items) }
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
                showPolicyMenu = false
                showModelMenu = false
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

    private func importSelectedImages(_ items: [PhotosPickerItem]) async {
        let targetSession = model.selected
        defer { selectedMediaItems = [] }
        var imported: [AIImportedFile] = []
        var failedCount = 0
        for item in items {
            do {
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
                imported.append(file)
            } catch {
                failedCount += 1
            }
        }
        if !imported.isEmpty {
            await sendImportedImages(imported, to: targetSession)
        }
        if failedCount > 0 {
            model.showTransientError(
                !imported.isEmpty
                    ? "已发送\(imported.count)张图片，另有\(failedCount)张失败"
                    : "所选图片读取失败"
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
                await sendImportedImages([file], to: targetSession)
            }
        } catch {
            model.showTransientError("拍摄图片导入失败")
        }
    }

    private func sendImportedImages(_ files: [AIImportedFile], to targetSession: String) async {
        guard !files.isEmpty else { return }
        let existingDraft = draft.trimmingCharacters(in: .whitespacesAndNewlines)
        let prompt = existingDraft.isEmpty
            ? (files.count == 1 ? "请查看这张图片。" : "请查看这些图片。")
            : draft
        if await model.send(prompt, images: files, expectedSession: targetSession) {
            if existingDraft.isEmpty || draft == prompt { draft = "" }
            return
        }

        // 导入已经完成但发送条件在异步读取期间改变时，保留图片和提示，
        // 让用户之后按回车重试，不能丢掉刚选中的内容。
        for file in files { model.addAttachment(file, to: targetSession) }
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

    private func handleVoiceGestureChanged(translation: CGSize, location: CGPoint) {
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

private struct AIPolicyMenu: View {
    let selected: String
    let select: (String) -> Void

    private let options = [
        ("on-request", "请求批准", "hand.raised"),
        ("unless-trusted", "帮我批准", "checkmark.shield"),
        ("never", "完全访问", "exclamationmark.shield")
    ]

    var body: some View {
        VStack(spacing: 0) {
            ForEach(options, id: \.0) { option in
                Button { select(option.0) } label: {
                    HStack(spacing: 10) {
                        Image(systemName: option.2).frame(width: 18)
                        Text(option.1).font(.system(size: 13, weight: .semibold))
                        Spacer(minLength: 12)
                        if selected == option.0 {
                            Image(systemName: "checkmark")
                                .font(.system(size: 12, weight: .bold))
                                .foregroundStyle(Color.blue)
                        }
                    }
                    .foregroundStyle(Color.primary)
                    .padding(.horizontal, 12)
                    .frame(height: 44)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                if option.0 != options.last?.0 { Divider().padding(.leading, 40) }
            }
        }
        .frame(width: 220)
        .background(Color(uiColor: .systemBackground), in: RoundedRectangle(cornerRadius: 13))
        .overlay(RoundedRectangle(cornerRadius: 13).stroke(Color.black.opacity(0.1)))
        .shadow(color: Color.black.opacity(0.16), radius: 18, y: 8)
        .accessibilityIdentifier("ai-policy-menu")
    }
}

private struct AIModelMenu: View {
    let selected: String
    let select: (String) -> Void

    private let options = ["glm-5.3", "glm-5.3-flash"]

    var body: some View {
        VStack(spacing: 0) {
            ForEach(options, id: \.self) { option in
                Button { select(option) } label: {
                    HStack(spacing: 10) {
                        Text(option).font(.system(size: 13, weight: .semibold))
                        Spacer(minLength: 12)
                        if selected.caseInsensitiveCompare(option) == .orderedSame {
                            Image(systemName: "checkmark")
                                .font(.system(size: 12, weight: .bold))
                                .foregroundStyle(Color.blue)
                        }
                    }
                    .foregroundStyle(Color.primary)
                    .padding(.horizontal, 12)
                    .frame(height: 44)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                if option != options.last { Divider().padding(.leading, 12) }
            }
        }
        .frame(width: 188)
        .background(Color(uiColor: .systemBackground), in: RoundedRectangle(cornerRadius: 13))
        .overlay(RoundedRectangle(cornerRadius: 13).stroke(Color.black.opacity(0.1)))
        .shadow(color: Color.black.opacity(0.16), radius: 18, y: 8)
        .accessibilityIdentifier("ai-model-menu")
    }
}

private struct AIComposerTextView: UIViewRepresentable {
    @Binding var text: String
    let focusController: AIComposerFocusController
    let voiceTranscriptionEnabled: Bool
    let onSubmit: (String) -> Void
    let onVoiceChanged: (CGSize, CGPoint) -> Void
    let onVoiceEnded: (CGSize, CGPoint) -> Void
    let onVoiceCancelled: () -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(parent: self)
    }

    func makeUIView(context: Context) -> UITextView {
        let textView = UITextView(usingTextLayoutManager: false)
        textView.delegate = context.coordinator
        textView.backgroundColor = .clear
        textView.font = .systemFont(ofSize: 17)
        textView.textColor = .label
        textView.tintColor = .systemBlue
        textView.returnKeyType = .send
        textView.enablesReturnKeyAutomatically = true
        textView.textContainerInset = .zero
        textView.textContainer.lineFragmentPadding = 0
        textView.isScrollEnabled = true
        textView.showsVerticalScrollIndicator = false
        textView.accessibilityIdentifier = "ai-composer"
        textView.accessibilityLabel = "AI 助手输入框，可按住转文字"
        focusController.textView = textView

        let voiceGesture = UILongPressGestureRecognizer(
            target: context.coordinator,
            action: #selector(Coordinator.handleVoiceLongPress(_:))
        )
        voiceGesture.minimumPressDuration = 0.35
        voiceGesture.allowableMovement = 400
        voiceGesture.cancelsTouchesInView = true
        voiceGesture.delegate = context.coordinator
        textView.addGestureRecognizer(voiceGesture)
        context.coordinator.voiceGesture = voiceGesture
        return textView
    }

    func updateUIView(_ textView: UITextView, context: Context) {
        context.coordinator.parent = self
        focusController.textView = textView
        if textView.text != text {
            textView.text = text
            textView.selectedRange = NSRange(location: (text as NSString).length, length: 0)
            textView.invalidateIntrinsicContentSize()
        }
    }

    static func dismantleUIView(_ textView: UITextView, coordinator: Coordinator) {
        textView.delegate = nil
        if coordinator.parent.focusController.textView === textView {
            coordinator.parent.focusController.textView = nil
        }
    }

    func sizeThatFits(
        _ proposal: ProposedViewSize,
        uiView textView: UITextView,
        context _: Context
    ) -> CGSize? {
        guard let width = proposal.width, width.isFinite, width > 0 else { return nil }
        let fitting = textView.sizeThatFits(
            CGSize(width: width, height: .greatestFiniteMagnitude)
        )
        let lineHeight = textView.font?.lineHeight ?? 20
        return CGSize(width: width, height: min(max(lineHeight, fitting.height), lineHeight * 6))
    }

    @MainActor
    final class Coordinator: NSObject, UITextViewDelegate, UIGestureRecognizerDelegate {
        var parent: AIComposerTextView
        weak var voiceGesture: UILongPressGestureRecognizer?
        private var voiceOrigin: CGPoint?

        init(parent: AIComposerTextView) {
            self.parent = parent
        }

        func textViewDidChange(_ textView: UITextView) {
            if parent.text != textView.text { parent.text = textView.text }
            textView.invalidateIntrinsicContentSize()
        }

        func textView(
            _ textView: UITextView,
            shouldChangeTextIn range: NSRange,
            replacementText: String
        ) -> Bool {
            guard textView.markedTextRange == nil,
                  let submitted = AIComposerSubmissionPolicy.submittedText(
                      currentText: textView.text,
                      replacing: range,
                      with: replacementText
                  )
            else { return true }
            parent.onSubmit(submitted)
            return false
        }

        func gestureRecognizerShouldBegin(_ gestureRecognizer: UIGestureRecognizer) -> Bool {
            guard gestureRecognizer === voiceGesture else { return true }
            return parent.voiceTranscriptionEnabled && parent.text.isEmpty
        }

        @objc func handleVoiceLongPress(_ gesture: UILongPressGestureRecognizer) {
            let location = gesture.location(in: gesture.view?.window)
            switch gesture.state {
            case .began:
                guard parent.voiceTranscriptionEnabled, parent.text.isEmpty else { return }
                voiceOrigin = location
                gesture.view?.resignFirstResponder()
                parent.onVoiceChanged(.zero, location)
            case .changed:
                guard let origin = voiceOrigin else { return }
                parent.onVoiceChanged(
                    CGSize(width: location.x - origin.x, height: location.y - origin.y),
                    location
                )
            case .ended:
                guard let origin = voiceOrigin else { return }
                voiceOrigin = nil
                parent.onVoiceEnded(
                    CGSize(width: location.x - origin.x, height: location.y - origin.y),
                    location
                )
            case .cancelled, .failed:
                voiceOrigin = nil
                parent.onVoiceCancelled()
            case .possible:
                break
            @unknown default:
                voiceOrigin = nil
                parent.onVoiceCancelled()
            }
        }
    }
}

private struct AIPermissionCard: View {
    let permission: AIPermission
    @ObservedObject var model: AIAssistantModel
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Label("允许执行 \(permission.tool)？", systemImage: "hand.raised").font(.subheadline.bold())
            Text(permission.input).font(.system(.caption, design: .monospaced)).textSelection(.enabled)
            HStack {
                action("拒绝", "denied")
                action("允许一次", "approved")
                action("本会话允许", "approved_for_session")
            }
        }.padding().frame(maxWidth: .infinity, alignment: .leading).background(Color.orange.opacity(0.08), in: RoundedRectangle(cornerRadius: 12))
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
