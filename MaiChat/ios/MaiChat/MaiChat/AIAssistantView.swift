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
    @FocusState private var composerFocused: Bool

    var body: some View {
        ZStack(alignment: .topTrailing) {
            VStack(spacing: 0) {
                AIHeader(
                    openSessions: {
                        composerFocused = false
                        showActions = false
                        openSessionDrawer()
                    },
                    openActions: { composerFocused = false; showActions.toggle() }
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
                                    ForEach(model.messages) { message in AIMessageRow(message: message) }
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
                            .onTapGesture { composerFocused = false }
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
                AIComposer(model: model, focused: $composerFocused)
                    .padding(.horizontal, 12).padding(.vertical, 8)
            }
            .background(Color(uiColor: .systemBackground))
            if showActions {
                Color.black.opacity(0.001).ignoresSafeArea().contentShape(Rectangle())
                    .onTapGesture { showActions = false }
                AIActionPanel(
                    canClear: !model.busy && !model.selected.isEmpty,
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
        }
        .simultaneousGesture(sessionDrawerOpenGesture)
        .sheet(isPresented: $model.showSettings) { AISettingsView(model: model) }
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
                composerFocused = false
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
    let clear: () -> Void

    var body: some View {
        VStack(spacing: 0) {
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
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            if message.role == "user" {
                HStack {
                    Spacer(minLength: 30)
                    Text(message.text).font(.system(size: 14)).textSelection(.enabled).padding(14)
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
                        Button { UIPasteboard.general.string = message.text } label: { Image(systemName: "doc.on.doc") }
                            .accessibilityLabel("复制回复")
                    }.foregroundStyle(.secondary).font(.caption)
                }
            }
        }.frame(maxWidth: .infinity, alignment: .leading)
    }
    private func toolStatus(_ state: String?) -> String {
        switch state { case "completed": return "已完成"; case "error": return "失败"; case "pending": return "等待授权"; default: return "正在运行" }
    }
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

private struct AIComposer: View {
    @ObservedObject var model: AIAssistantModel
    @FocusState.Binding var focused: Bool
    @StateObject private var speechRecognizer = TencentRealtimeSpeechRecognizer(
        appId: TencentASRCredentials.appId,
        secretId: TencentASRCredentials.secretId,
        secretKey: TencentASRCredentials.secretKey
    )
    @State private var draft = ""
    @State private var importing = false
    @State private var draftSession = ""
    @State private var isPressingVoice = false
    @State private var isCancellingVoice = false
    @State private var voiceBaseDraft = ""
    @State private var voiceStartTask: Task<Bool, Never>?
    var body: some View {
        VStack(spacing: 10) {
            ZStack(alignment: .topLeading) {
                AIComposerTextView(
                    text: $draft,
                    focused: $focused,
                    voiceTranscriptionEnabled: draft.isEmpty || isPressingVoice,
                    onSubmit: { submitDraft($0) },
                    onVoiceChanged: { translation in
                        beginVoiceTranscription()
                        isCancellingVoice = translation.height < -60
                    },
                    onVoiceEnded: { _ in finishVoiceGesture() },
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
                        .foregroundStyle(isCancellingVoice ? Color.red : Color.secondary)
                        .font(.system(size: 17, weight: isPressingVoice ? .semibold : .regular))
                        .allowsHitTesting(false)
                        .accessibilityHidden(true)
                }
            }
            HStack {
                Button { importing = true } label: {
                    Image(systemName: "plus").font(.system(size: 17, weight: .medium)).frame(width: 30, height: 30)
                }.buttonStyle(.plain).foregroundStyle(Color.blue).accessibilityLabel("导入文本文件")
                Button { model.showSettings = true } label: {
                    HStack(spacing: 5) {
                        Image(systemName: "shield")
                        Text(model.settings.policy == "never" ? "完全访问" : model.settings.policy == "unless-trusted" ? "帮我批准" : "请求批准")
                    }.font(.system(size: 12, weight: .medium))
                        .foregroundStyle(model.settings.policy == "never" ? Color.orange : Color.secondary)
                        .padding(.horizontal, 8).frame(height: 28)
                        .background(Color(uiColor: .secondarySystemBackground), in: Capsule())
                }.buttonStyle(.plain).accessibilityLabel("模型与权限设置")
                Spacer(minLength: 4)
                Button(model.configured ? model.settings.model : "未配置模型") { model.showSettings = true }
                    .buttonStyle(.plain).font(.system(size: 12, weight: .medium)).lineLimit(1)
                    .foregroundStyle(Color.blue).padding(.horizontal, 8).frame(height: 28)
                    .background(Color.blue.opacity(0.08), in: Capsule())
                if model.busy {
                    Button { Task { await model.action("stop") } } label: {
                        Image(systemName: "stop.fill").font(.system(size: 15, weight: .bold))
                            .foregroundStyle(.white).frame(width: 34, height: 34).background(.primary, in: Circle())
                    }
                    .disabled(!model.ready || model.isSubmitting)
                    .accessibilityLabel("停止")
                }
            }
        }.padding(14).background(Color(uiColor: .systemBackground), in: RoundedRectangle(cornerRadius: 20))
            .overlay(RoundedRectangle(cornerRadius: 20).stroke(Color.gray.opacity(0.22)))
            .fileImporter(isPresented: $importing, allowedContentTypes: [.plainText, .sourceCode, .json]) { result in
                if case let .success(url) = result {
                    let target = model.selected
                    Task {
                        if let name = await model.importFile(url) {
                            if target == model.selected {
                                draft += (draft.isEmpty ? "" : "\n") + "请查看文件：\(name)"
                                focused = true
                            } else {
                                let previous = model.drafts[target, default: ""]
                                model.drafts[target] = previous
                                    + (previous.isEmpty ? "" : "\n")
                                    + "请查看文件：\(name)"
                            }
                        }
                    }
                }
            }
            .onAppear {
                draftSession = model.selected
                draft = model.drafts[draftSession] ?? ""
                speechRecognizer.onLiveTextUpdate = { text, _ in
                    guard isPressingVoice else { return }
                    draft = voiceText(base: voiceBaseDraft, transcript: text)
                }
            }
            .onDisappear {
                model.drafts[draftSession] = draft
                voiceStartTask?.cancel()
                voiceStartTask = nil
                speechRecognizer.onLiveTextUpdate = nil
                speechRecognizer.cancel()
            }
            .onChange(of: model.selected) { selected in
                cancelVoiceTranscription(restoresDraft: true)
                model.drafts[draftSession] = draft
                let sendingFirstMessage = model.isSubmitting && draftSession.isEmpty
                draftSession = selected
                if !sendingFirstMessage { draft = model.drafts[selected] ?? "" }
            }
    }

    private var composerPrompt: String {
        if isCancellingVoice { return "松开取消" }
        if isPressingVoice { return "松开完成" }
        return "可按住转文字"
    }

    private func submitDraft(_ submittedText: String? = nil) {
        guard model.ready, !model.busy, !model.isSubmitting else { return }
        let sent = submittedText ?? draft
        guard !sent.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return }
        Task {
            if await model.send(sent), draft == sent { draft = "" }
        }
    }

    private func finishVoiceGesture() {
        guard isPressingVoice else { return }
        let cancel = isCancellingVoice
        isPressingVoice = false
        isCancellingVoice = false
        if cancel {
            cancelVoiceTranscription(restoresDraft: true)
        } else {
            finishVoiceTranscription()
        }
    }

    private func beginVoiceTranscription() {
        guard !isPressingVoice else { return }
        guard draft.isEmpty else { return }
        guard speechRecognizer.isAvailable else {
            model.error = "语音转文字凭证未配置"
            return
        }
        focused = false
        isPressingVoice = true
        isCancellingVoice = false
        voiceBaseDraft = draft
        voiceStartTask = Task { @MainActor in
            do {
                try await speechRecognizer.start(diagnosticFields: [
                    "surface": "ai-assistant",
                    "session": model.selected,
                ])
                return true
            } catch is CancellationError {
                return false
            } catch {
                isPressingVoice = false
                model.error = "语音转文字失败：\(error.localizedDescription)"
                return false
            }
        }
    }

    private func finishVoiceTranscription() {
        let startTask = voiceStartTask
        voiceStartTask = nil
        Task { @MainActor in
            let didStart = await startTask?.value ?? speechRecognizer.isRecognizing
            guard didStart, speechRecognizer.isRecognizing else { return }
            do {
                let text = try await speechRecognizer.stop()
                    .trimmingCharacters(in: .whitespacesAndNewlines)
                guard !text.isEmpty else {
                    model.error = "没有识别到文字"
                    return
                }
                draft = voiceText(base: voiceBaseDraft, transcript: text)
            } catch is CancellationError {
            } catch {
                model.error = "语音转文字失败：\(error.localizedDescription)"
            }
        }
    }

    private func cancelVoiceTranscription(restoresDraft: Bool) {
        let hadActiveTranscription = isPressingVoice || speechRecognizer.isRecognizing || voiceStartTask != nil
        voiceStartTask?.cancel()
        voiceStartTask = nil
        speechRecognizer.cancel()
        isPressingVoice = false
        isCancellingVoice = false
        if restoresDraft, hadActiveTranscription { draft = voiceBaseDraft }
    }

    private func voiceText(base: String, transcript: String) -> String {
        let cleaned = transcript.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !base.isEmpty, !cleaned.isEmpty else { return base.isEmpty ? cleaned : base }
        let separator = base.last?.isWhitespace == true ? "" : " "
        return base + separator + cleaned
    }
}

private struct AIComposerTextView: UIViewRepresentable {
    @Binding var text: String
    @FocusState.Binding var focused: Bool
    let voiceTranscriptionEnabled: Bool
    let onSubmit: (String) -> Void
    let onVoiceChanged: (CGSize) -> Void
    let onVoiceEnded: (CGSize) -> Void
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
        if textView.text != text {
            textView.text = text
            textView.selectedRange = NSRange(location: (text as NSString).length, length: 0)
            textView.invalidateIntrinsicContentSize()
        }
        if focused, !textView.isFirstResponder {
            textView.becomeFirstResponder()
        } else if !focused, textView.isFirstResponder {
            textView.resignFirstResponder()
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

        func textViewDidBeginEditing(_: UITextView) {
            parent.focused = true
        }

        func textViewDidEndEditing(_: UITextView) {
            parent.focused = false
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
                parent.onVoiceChanged(.zero)
            case .changed:
                guard let origin = voiceOrigin else { return }
                parent.onVoiceChanged(
                    CGSize(width: location.x - origin.x, height: location.y - origin.y)
                )
            case .ended:
                guard let origin = voiceOrigin else { return }
                voiceOrigin = nil
                parent.onVoiceEnded(
                    CGSize(width: location.x - origin.x, height: location.y - origin.y)
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
    @Environment(\.dismiss) private var dismiss
    @State private var settings = AIModelSettings()
    @State private var apiKey = ""
    @State private var saving = false
    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Button("取消") { dismiss() }.disabled(saving)
                    .buttonStyle(.plain).font(.system(size: 14, weight: .medium))
                Spacer()
                Text("模型与权限").font(.system(size: 17, weight: .bold))
                Spacer()
                Button(saving ? "保存中" : "保存") {
                    saving = true
                    Task { if await model.save(settings, key: apiKey) { dismiss() }; saving = false }
                }.disabled(saving || model.sessions.contains(where: \.busy))
                    .buttonStyle(.plain).font(.system(size: 14, weight: .semibold)).foregroundStyle(Color.blue)
            }.padding(.horizontal, 18).frame(height: 56)
            Divider().overlay(Color.black.opacity(0.06))
            ScrollView {
                VStack(alignment: .leading, spacing: 22) {
                    settingsSection("模型") {
                    TextField("API 地址（以 /v4 或 /v1 结尾）", text: $settings.baseUrl)
                        .keyboardType(.URL).textInputAutocapitalization(.never).autocorrectionDisabled()
                        .aiSettingsField()
                    TextField("模型名称", text: $settings.model).textInputAutocapitalization(.never).autocorrectionDisabled()
                        .aiSettingsField()
                    SecureField(model.configured ? "API Key（留空保留原密钥）" : "API Key", text: $apiKey)
                        .aiSettingsField()
                    Text("支持兼容 Chat Completions 的接口。密钥仅保存在本机 Keychain。")
                        .font(.system(size: 12)).foregroundStyle(.secondary)
                    }
                    settingsSection("操作权限") {
                        ForEach([
                            ("on-request", "请求批准", "修改文件和访问网络前询问。"),
                            ("unless-trusted", "帮我批准", "允许工作区内文件修改，网络访问仍询问。"),
                            ("never", "完全访问", "自动执行手机工作区内的文件修改及网络请求。")
                        ], id: \.0) { item in
                            let (value, title, detail) = item
                            Button {
                                settings.policy = value
                            } label: {
                                HStack(alignment: .top, spacing: 12) {
                                    Image(systemName: settings.policy == value ? "checkmark.circle.fill" : "circle")
                                        .foregroundStyle(settings.policy == value ? Color.blue : Color.secondary)
                                    VStack(alignment: .leading, spacing: 4) {
                                        Text(title).font(.system(size: 14, weight: .semibold))
                                        Text(detail).font(.system(size: 12)).foregroundStyle(.secondary)
                                    }
                                    Spacer(minLength: 0)
                                }.padding(12)
                                    .background(settings.policy == value ? Color.blue.opacity(0.07) : Color.clear,
                                                in: RoundedRectangle(cornerRadius: 10))
                            }.buttonStyle(.plain).foregroundStyle(Color.primary)
                        }
                    }
                    if !model.error.isEmpty {
                        Text(model.error).foregroundStyle(.red).font(.system(size: 12))
                            .padding(12).frame(maxWidth: .infinity, alignment: .leading)
                            .background(Color.red.opacity(0.06), in: RoundedRectangle(cornerRadius: 10))
                    }
                }
                .padding(18)
            }
        }.background(Color(uiColor: .systemBackground)).onAppear { settings = model.settings }
    }

    private func settingsSection<Content: View>(_ title: String,
                                                @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(title).font(.system(size: 13, weight: .bold)).foregroundStyle(.secondary)
            VStack(alignment: .leading, spacing: 10) { content() }
                .padding(12).background(Color(uiColor: .secondarySystemBackground),
                                        in: RoundedRectangle(cornerRadius: 14))
        }
    }
}

private extension View {
    func aiSettingsField() -> some View {
        padding(.horizontal, 12).frame(minHeight: 42)
            .background(Color(uiColor: .systemBackground), in: RoundedRectangle(cornerRadius: 9))
            .overlay(RoundedRectangle(cornerRadius: 9).stroke(Color.black.opacity(0.07)))
    }
}
