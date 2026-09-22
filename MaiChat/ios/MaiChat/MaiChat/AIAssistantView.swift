import SwiftUI
import UniformTypeIdentifiers

struct AIAssistantView: View {
    @ObservedObject private var model = AIAssistantModel.shared
    @Environment(\.scenePhase) private var scenePhase
    @State private var showSessions = false
    @State private var confirmClear = false
    @State private var followsBottom = true
    @State private var userDragging = false
    @State private var latestY: CGFloat = 0

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
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
                                                Button("配置模型", action: { model.showSettings = true }).buttonStyle(.borderedProminent)
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
                AIComposer(model: model).padding(.horizontal, 12).padding(.vertical, 8)
            }
            .background(Color(uiColor: .systemBackground))
            .navigationTitle("AI 助手")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .navigationBarLeading) {
                    Button { showSessions = true } label: { Image(systemName: "sidebar.left") }
                        .accessibilityLabel("对话列表")
                }
                ToolbarItem(placement: .navigationBarTrailing) {
                    Menu {
                        Button("新对话", systemImage: "plus") { Task { await model.create() } }
                        Button("模型与权限", systemImage: "slider.horizontal.3") { model.showSettings = true }
                        Button("清空当前对话", role: .destructive) { confirmClear = true }.disabled(model.busy || model.selected.isEmpty)
                    } label: { Image(systemName: "ellipsis.circle") }
                }
            }
            .sheet(isPresented: $showSessions) { sessionList }
            .sheet(isPresented: $model.showSettings) { AISettingsView(model: model) }
            .confirmationDialog("清空当前对话的所有消息？", isPresented: $confirmClear, titleVisibility: .visible) {
                Button("清空消息", role: .destructive) { Task { await model.action("clear") } }
            }
        }
        .onAppear { model.appear() }
        .onDisappear { model.disappear() }
        .onChange(of: scenePhase) { phase in
            if phase == .active { model.appear() } else { model.disappear() }
        }
    }

    private var sessionList: some View {
        NavigationStack {
            List {
                Button { showSessions = false; Task { await model.create() } } label: { Label("新对话", systemImage: "plus") }
                ForEach(model.sessions) { session in
                    Button {
                        showSessions = false
                        Task { await model.select(session.id) }
                    } label: {
                        HStack {
                            Text(session.displayTitle).lineLimit(2)
                            Spacer()
                            if session.busy { ProgressView() }
                            else if session.id == model.selected { Image(systemName: "checkmark") }
                        }
                    }
                    .swipeActions {
                        Button("删除", role: .destructive) {
                            Task { await model.select(session.id); await model.action("delete") }
                        }.disabled(session.busy)
                    }
                }
            }.navigationTitle("对话")
                .toolbar { ToolbarItem(placement: .confirmationAction) { Button("完成") { showSessions = false } } }
        }
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
                        DisclosureGroup("思考过程") { Text(text).font(.caption).textSelection(.enabled) }
                            .font(.caption).foregroundStyle(.secondary)
                    } else if part.kind == "tool" {
                        DisclosureGroup {
                            VStack(alignment: .leading, spacing: 8) {
                                Text(part.input ?? "").font(.system(.caption, design: .monospaced))
                                if let output = part.output, !output.isEmpty { Text(output).font(.system(.caption, design: .monospaced)) }
                                if let error = part.error, !error.isEmpty { Text(error).foregroundStyle(.red) }
                            }.textSelection(.enabled).padding(10).frame(maxWidth: .infinity, alignment: .leading)
                                .background(Color(uiColor: .secondarySystemBackground), in: RoundedRectangle(cornerRadius: 8))
                        } label: {
                            Label("\(toolStatus(part.state)) \(part.tool ?? "")", systemImage: "terminal")
                        }.font(.caption).foregroundStyle(.secondary)
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
private struct AIComposer: View {
    @ObservedObject var model: AIAssistantModel
    @State private var draft = ""
    @State private var importing = false
    @State private var draftSession = ""
    @FocusState private var focused: Bool
    var body: some View {
        VStack(spacing: 10) {
            TextField("随心输入", text: $draft, axis: .vertical).lineLimit(1...6).focused($focused)
                .accessibilityIdentifier("ai-composer")
            HStack {
                Button { importing = true } label: { Image(systemName: "plus") }.accessibilityLabel("导入文本文件")
                Menu {
                    Button("模型与权限设置") { model.showSettings = true }
                } label: {
                    Text(model.settings.policy == "never" ? "完全访问" : model.settings.policy == "unless-trusted" ? "帮我批准" : "请求批准")
                        .font(.caption).foregroundStyle(model.settings.policy == "never" ? .orange : .secondary)
                }
                Spacer(minLength: 4)
                Button(model.configured ? model.settings.model : "未配置模型") { model.showSettings = true }
                    .font(.caption).lineLimit(1)
                Button {
                    if model.busy { Task { await model.action("stop") } }
                    else {
                        let sent = draft
                        Task { if await model.send(sent), draft == sent { draft = "" } }
                    }
                } label: {
                    Image(systemName: model.busy ? "stop.fill" : "arrow.up").font(.system(size: 15, weight: .bold))
                        .foregroundStyle(.white).frame(width: 34, height: 34).background(.primary, in: Circle())
                }.disabled(!model.ready || model.isSubmitting || (!model.busy && draft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty))
                    .accessibilityLabel(model.busy ? "停止" : "发送")
            }
        }.padding(14).background(Color(uiColor: .systemBackground), in: RoundedRectangle(cornerRadius: 20))
            .overlay(RoundedRectangle(cornerRadius: 20).stroke(Color.gray.opacity(0.22)))
            .fileImporter(isPresented: $importing, allowedContentTypes: [.plainText, .sourceCode, .json]) { result in
                if case let .success(url) = result {
                    let target = model.selected
                    Task {
                        if let name = await model.importFile(url) {
                            let mention = "\n请查看文件：\(name)\n"
                            if target == model.selected { draft += mention; focused = true }
                            else { model.drafts[target, default: ""] += mention }
                        }
                    }
                }
            }
            .onAppear { draftSession = model.selected; draft = model.drafts[draftSession] ?? "" }
            .onDisappear { model.drafts[draftSession] = draft }
            .onChange(of: model.selected) { selected in
                model.drafts[draftSession] = draft
                let sendingFirstMessage = model.isSubmitting && draftSession.isEmpty
                draftSession = selected
                if !sendingFirstMessage { draft = model.drafts[selected] ?? "" }
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
            }.buttonStyle(.bordered)
        }.padding().frame(maxWidth: .infinity, alignment: .leading).background(Color.orange.opacity(0.08), in: RoundedRectangle(cornerRadius: 12))
    }
    private func action(_ title: String, _ decision: String) -> some View {
        Button(title) { Task { await model.action("permission", values: ["id": permission.id, "decision": decision]) } }.font(.caption)
    }
}
private struct AIQuestionCard: View {
    let question: AIQuestion
    @ObservedObject var model: AIAssistantModel
    @State private var answer = ""
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(question.question).font(.subheadline.bold())
            ForEach(question.options, id: \.self) { option in Button(option) { reply(option) }.buttonStyle(.bordered) }
            HStack {
                TextField("你的回答", text: $answer)
                Button("回复") { reply(answer) }.disabled(answer.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
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
        NavigationStack {
            Form {
                Section("模型") {
                    TextField("API 地址（以 /v4 或 /v1 结尾）", text: $settings.baseUrl)
                        .keyboardType(.URL).textInputAutocapitalization(.never).autocorrectionDisabled()
                    TextField("模型名称", text: $settings.model).textInputAutocapitalization(.never).autocorrectionDisabled()
                    SecureField(model.configured ? "API Key（留空保留原密钥）" : "API Key", text: $apiKey)
                    Text("支持兼容 Chat Completions 的接口。密钥仅保存在本机 Keychain。").font(.caption).foregroundStyle(.secondary)
                }
                Section("操作权限") {
                    Picker("批准方式", selection: $settings.policy) {
                        Text("请求批准").tag("on-request")
                        Text("帮我批准").tag("unless-trusted")
                        Text("完全访问").tag("never")
                    }
                    Text(settings.policy == "never" ? "自动执行手机工作区内的文件修改及网络请求。" : settings.policy == "unless-trusted" ? "允许工作区内文件修改，网络访问仍询问。" : "修改文件和访问网络前询问。")
                        .font(.caption).foregroundStyle(.secondary)
                }
                if !model.error.isEmpty { Text(model.error).foregroundStyle(.red).font(.caption) }
            }.navigationTitle("模型与权限").navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItem(placement: .cancellationAction) { Button("取消") { dismiss() }.disabled(saving) }
                    ToolbarItem(placement: .confirmationAction) {
                        Button(saving ? "保存中" : "保存") {
                            saving = true
                            Task { if await model.save(settings, key: apiKey) { dismiss() }; saving = false }
                        }.disabled(saving || model.sessions.contains(where: \.busy))
                    }
                }
        }.onAppear { settings = model.settings }
    }
}
