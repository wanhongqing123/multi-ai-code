// 文件作用：查看一批屏幕共享截图，明确选择后才复制到开始共享的原会话草稿。
// 关闭界面保留原图；只有“清空本批”会删除档案，已加入草稿的副本不受影响。
import SwiftUI

struct AIMobileScreenFramesView: View {
    @ObservedObject var model: AIAssistantModel
    @ObservedObject private var capture = AIMobileScreenCapture.shared
    @Environment(\.dismiss) private var dismiss
    @State private var binding: AIScreenFrameImportBinding
    @State private var batches: [AIMobileScreenArchiveBatch] = []
    @State private var frames: [AIMobileScreenArchiveFrame] = []
    @State private var selected = Set<String>()
    @State private var unavailable = Set<String>()
    @State private var loading = false
    @State private var importing = false
    @State private var error = ""
    @State private var revision = 0
    @State private var preview: AIMobileScreenArchivePreview?

    init(model: AIAssistantModel, binding: AIScreenFrameImportBinding) {
        self.model = model
        _binding = State(initialValue: binding)
    }

    private var isRecording: Bool {
        batch?.status == "recording" ||
            ((capture.isSharing || capture.isStarting) && capture.archiveContext == binding.context)
    }
    private var batch: AIMobileScreenArchiveBatch? { batches.first { $0.id == binding.id } }
    private var capacity: Int { model.screenFrameAttachmentCapacity(binding) }

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                VStack(alignment: .leading, spacing: 8) {
                    Text(isRecording ? "每 2 秒截取一张，停止后选择" : "本次截取 \(frames.count) 张，选择需要的图片")
                        .font(.headline)
                        .accessibilityIdentifier("ai-screen-frames-summary")
                    Text("只将选中的图片加入原会话草稿，不会自动发送。还可添加 \(capacity) 张。")
                        .font(.footnote).foregroundStyle(.secondary)
                    if batches.count > 1 {
                        Menu {
                            ForEach(batches) { saved in
                                Button("\(Self.batchTime(saved)) · \(saved.frameCount) 张") { selectBatch(saved) }
                            }
                        } label: {
                            Label("\(batch.map(Self.batchTime) ?? "本次截图") · 切换批次", systemImage: "clock")
                                .font(.footnote)
                        }
                        .accessibilityIdentifier("ai-screen-frames-batches")
                    }
                    if let reason = batch?.finishReason, let message = Self.finishMessage(reason) {
                        Text(message).font(.footnote).foregroundStyle(.secondary)
                    }
                    if !error.isEmpty {
                        HStack(alignment: .top) {
                            Text(error).font(.footnote).foregroundStyle(.red)
                            Spacer(minLength: 4)
                            Button("重试") { revision += 1 }
                        }
                        .accessibilityIdentifier("ai-screen-frames-error")
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading).padding(16)
                Divider()
                if loading && frames.isEmpty {
                    ProgressView("正在读取截图…").frame(maxWidth: .infinity, maxHeight: .infinity)
                } else if frames.isEmpty {
                    VStack(spacing: 12) {
                        Image(systemName: "photo.on.rectangle.angled").font(.largeTitle)
                            .foregroundStyle(RemoteIMStyle.blue)
                        Text(isRecording ? "尚未截取画面" : "本次没有截取到图片").font(.headline)
                        Text(isRecording ? "共享期间每 2 秒保存可用的新画面。" : "可以关闭此页后重新开启屏幕共享。")
                            .font(.footnote).foregroundStyle(.secondary)
                        Button("刷新") { revision += 1 }
                    }
                    .padding().frame(maxWidth: .infinity, maxHeight: .infinity)
                    .accessibilityIdentifier("ai-screen-frames-empty")
                } else {
                    ScrollView {
                        LazyVGrid(columns: [GridItem(.adaptive(minimum: 140), spacing: 12)], spacing: 16) {
                            ForEach(Array(frames.enumerated()), id: \.element.id) { entry in
                                frameTile(entry.element, index: entry.offset)
                            }
                        }.padding(16)
                    }
                    .accessibilityIdentifier("ai-screen-frames-grid")
                }
                Divider()
                HStack {
                    Text("已选 \(selected.count) 张").font(.footnote)
                        .accessibilityIdentifier("ai-screen-frames-selection-count")
                    Spacer()
                    Button {
                        Task { await addSelected() }
                    } label: {
                        if importing { ProgressView().tint(.white) }
                        else { Text("加入原会话草稿").font(.body.weight(.semibold)) }
                    }
                    .buttonStyle(.borderedProminent).tint(RemoteIMStyle.blue)
                    .disabled(selected.isEmpty || selected.count > capacity || importing || isRecording)
                    .accessibilityIdentifier("ai-screen-frames-add")
                }.padding(16)
            }
            .navigationTitle("屏幕截图").navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("关闭") { dismiss() }
                        .accessibilityIdentifier("ai-screen-frames-close")
                }
                ToolbarItem(placement: .primaryAction) {
                    Menu {
                        Button("清空本批", role: .destructive) { Task { await discard() } }
                            .disabled(isRecording || importing)
                    } label: { Image(systemName: "ellipsis.circle") }
                    .accessibilityLabel("截图操作")
                    .accessibilityIdentifier("ai-screen-frames-actions")
                }
            }
        }
        .task(id: "\(binding.id)-\(revision)") { await reload() }
        .onChange(of: model.screenFrameCurrentAccountID) { _ in dismiss() }
        .fullScreenCover(item: $preview) { item in
            AIMobileScreenArchiveLargeImage(context: binding.context, frame: item.frame) { preview = nil }
        }
        .interactiveDismissDisabled(importing)
    }

    private func frameTile(_ frame: AIMobileScreenArchiveFrame, index: Int) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            ZStack(alignment: .topTrailing) {
                Button { preview = AIMobileScreenArchivePreview(frame: frame) } label: {
                    AIMobileScreenArchiveThumbnail(context: binding.context, frame: frame,
                        revision: revision, failed: {
                            unavailable.insert(frame.id)
                            selected.remove(frame.id)
                        })
                        .frame(height: 164).frame(maxWidth: .infinity)
                        .clipped().background(Color(uiColor: .secondarySystemBackground))
                        .clipShape(RoundedRectangle(cornerRadius: 10))
                }
                .buttonStyle(.plain)
                .accessibilityLabel("预览第 \(index + 1) 张截图")
                .accessibilityIdentifier("ai-screen-frame-preview-\(index)")
                Button { toggle(frame.id) } label: {
                    Image(systemName: selected.contains(frame.id) ? "checkmark.circle.fill" : "circle")
                        .font(.system(size: 25, weight: .semibold))
                        .foregroundStyle(selected.contains(frame.id) ? RemoteIMStyle.blue : Color.primary)
                        .padding(9).background(.regularMaterial, in: Circle()).padding(5)
                }
                .disabled(unavailable.contains(frame.id) || importing || isRecording)
                .accessibilityLabel("选择第 \(index + 1) 张截图")
                .accessibilityValue(selected.contains(frame.id) ? "已选择" : "未选择")
                .accessibilityIdentifier("ai-screen-frame-select-\(index)")
            }
            HStack {
                Text("第 \(index + 1) 张")
                Spacer()
                Text(Self.frameTime(frame)).monospacedDigit()
            }.font(.caption).foregroundStyle(.secondary)
        }
    }

    private func toggle(_ id: String) {
        if selected.contains(id) { selected.remove(id) }
        else if selected.count < capacity { selected.insert(id); error = "" }
        else { error = capacity == 0 ? "原会话草稿已有 10 项附件，请先移除部分附件。" : "本次最多还可选择 \(capacity) 张。" }
    }

    @MainActor private func reload() async {
        loading = true
        defer { loading = false }
        do {
            guard model.screenFrameBinding(id: binding.id) != nil else {
                throw AIBackendError(message: "账号或原会话已变化，请返回原会话查看截图。")
            }
            let current = binding.context
            let listing = try await AIMobileScreenFrameArchive.shared.list(
                workspace: current.workspace, sessionID: current.ownerSessionID)
            try Task.checkCancellation()
            guard current == binding.context, model.screenFrameBinding(id: binding.id) != nil else { return }
            batches = listing.batches.sorted { $0.startedAtMS > $1.startedAtMS }
            frames = listing.frames.filter { $0.batchID == binding.id }.sorted { $0.elapsedMS < $1.elapsedMS }
            selected.formIntersection(Set(frames.map(\.id)))
            unavailable = []
            error = batches.contains { $0.id == binding.id } ? "" : "这批截图已被清空。"
        } catch is CancellationError { }
        catch { self.error = error.localizedDescription }
    }

    @MainActor private func selectBatch(_ batch: AIMobileScreenArchiveBatch) {
        do {
            let context = AIMobileScreenArchiveContext(workspace: binding.context.workspace,
                ownerSessionID: binding.context.ownerSessionID, batchID: batch.id)
            binding = try model.registerScreenFrameArchive(context: context, snapshot: binding.snapshot)
            selected = []; frames = []; error = ""
        } catch { self.error = error.localizedDescription }
    }

    @MainActor private func addSelected() async {
        importing = true
        defer { importing = false }
        do {
            let ids = frames.filter { selected.contains($0.id) }.map(\.id)
            let count = try await model.addScreenFrames(ids, binding: binding)
            guard count > 0 else { return }
            UIAccessibility.post(notification: .announcement,
                                 argument: "已将 \(count) 张图片加入原会话草稿，尚未发送")
            dismiss()
        } catch { self.error = "未加入草稿：\(error.localizedDescription)" }
    }

    @MainActor private func discard() async {
        importing = true
        defer { importing = false }
        do {
            try await model.discardScreenFrameArchive(binding)
            dismiss()
        } catch { self.error = error.localizedDescription }
    }

    private static func frameTime(_ frame: AIMobileScreenArchiveFrame) -> String {
        Date(timeIntervalSince1970: Double(frame.capturedAtMS) / 1000)
            .formatted(date: .omitted, time: .standard)
    }
    private static func batchTime(_ batch: AIMobileScreenArchiveBatch) -> String {
        Date(timeIntervalSince1970: Double(batch.startedAtMS) / 1000)
            .formatted(date: .abbreviated, time: .shortened)
    }
    private static func finishMessage(_ reason: String) -> String? {
        switch reason {
        case "interrupted": return "上次共享意外中断，已保留截取的图片。"
        case "user", "stopped", "user_stopped": return nil
        case "batch_frames", "batch_bytes", "owner_bytes", "workspace_bytes", "limit":
            return "已达到截图存储上限并停止共享。选图后可清空不再需要的批次。"
        default: return "共享已停止，已截取的图片仍保留。"
        }
    }
}

private struct AIMobileScreenArchivePreview: Identifiable {
    let frame: AIMobileScreenArchiveFrame
    var id: String { frame.id }
}

// Core get 验证和图片解码都离开主线程；LazyVGrid 只加载当前可见缩略图。
private func aiScreenArchiveURL(context: AIMobileScreenArchiveContext,
                                frame: AIMobileScreenArchiveFrame) async throws -> URL {
    let worker = Task.detached(priority: .utility) {
        try Task.checkCancellation()
        return try AIMobileScreenFrameArchive.shared.fileURL(for: frame, context: context)
    }
    return try await withTaskCancellationHandler {
        let url = try await worker.value
        try Task.checkCancellation()
        return url
    } onCancel: { worker.cancel() }
}

private struct AIMobileScreenArchiveThumbnail: View {
    let context: AIMobileScreenArchiveContext
    let frame: AIMobileScreenArchiveFrame
    let revision: Int
    let failed: () -> Void
    @State private var url: URL?
    @State private var unavailable = false
    var body: some View {
        Group {
            if let url {
                RemoteIMAsyncImage(filePath: url.path, maximumPointSize: CGSize(width: 180, height: 180),
                    revision: UInt64(revision)) { image in
                    Image(uiImage: image).resizable().scaledToFit()
                } placeholder: { error in
                    if error { Text("图片不可用").font(.footnote).onAppear(perform: failed) }
                    else { ProgressView() }
                }
            } else if unavailable { Text("图片不可用").font(.footnote) }
            else { ProgressView() }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .onDisappear { url = nil }
        .task(id: "\(frame.id)-\(revision)") {
            unavailable = false
            do { url = try await aiScreenArchiveURL(context: context, frame: frame) }
            catch is CancellationError { }
            catch { unavailable = true; failed() }
        }
    }
}

private struct AIMobileScreenArchiveLargeImage: View {
    let context: AIMobileScreenArchiveContext
    let frame: AIMobileScreenArchiveFrame
    let close: () -> Void
    @State private var url: URL?
    @State private var failed = false
    @State private var revision = 0
    var body: some View {
        GeometryReader { geometry in
            ZStack(alignment: .topTrailing) {
                Color.black.ignoresSafeArea()
                Group {
                    if let url {
                        RemoteIMAsyncImage(filePath: url.path,
                            maximumPointSize: ImagePreviewZoomModifier.maximumDecodePointSize(for: geometry.size)) { image in
                            let scale = min(geometry.size.width / max(1, image.size.width),
                                            geometry.size.height / max(1, image.size.height))
                            let fitted = CGSize(width: image.size.width * scale, height: image.size.height * scale)
                            Image(uiImage: image).resizable().scaledToFit()
                                .frame(width: fitted.width, height: fitted.height)
                                .modifier(ImagePreviewZoomModifier(enabled: true,
                                    imageSize: fitted, viewportSize: geometry.size))
                                .accessibilityLabel("截图大图")
                                .accessibilityIdentifier("ai-screen-frame-large-image")
                        } placeholder: { decodeFailed in
                            if decodeFailed { Text("图片无法显示").foregroundStyle(.white) }
                            else { ProgressView().tint(.white) }
                        }
                    } else if failed {
                        VStack(spacing: 12) {
                            Text("图片不可用或已删除").foregroundStyle(.white)
                            Button("重试") { revision += 1 }
                        }
                    } else { ProgressView().tint(.white) }
                }.frame(maxWidth: .infinity, maxHeight: .infinity)
                Button(action: close) {
                    Image(systemName: "xmark").font(.headline).padding(15)
                        .background(.ultraThinMaterial, in: Circle())
                }
                .padding(16).accessibilityLabel("关闭大图")
                .accessibilityIdentifier("ai-screen-frame-large-close")
            }
        }
        .task(id: revision) {
            failed = false
            do { url = try await aiScreenArchiveURL(context: context, frame: frame) }
            catch is CancellationError { }
            catch { failed = true }
        }
    }
}

#if DEBUG && targetEnvironment(simulator)
// 使用正式 sampler、档案 API 和原会话附件入口；仅素材与账号身份是隔离测试值。
struct AIScreenFramesUITestRoot: View {
    @ObservedObject private var model = AIAssistantModel.shared
    @State private var binding: AIScreenFrameImportBinding?
    @State private var presentation: AIScreenFrameImportBinding?
    @State private var result = "未运行"
    @State private var busy = false
    @State private var hostCopy: URL?

    var body: some View {
        VStack(spacing: 18) {
            Text("屏幕截图选择").font(.title2.bold())
            Button("准备真实采样") { Task { await prepare() } }
                .disabled(busy || binding != nil)
                .accessibilityIdentifier("screen-frames-fixture-prepare")
            Button("打开截图") { presentation = binding }
                .disabled(binding == nil || busy)
                .accessibilityIdentifier("screen-frames-fixture-open")
            Button("切到 B 后打开 A 截图") {
                model.selected = "switch-B"
                presentation = binding
            }
            .disabled(binding == nil || busy)
            .accessibilityIdentifier("screen-frames-fixture-open-from-b")
            Text("A=\(model.attachments(for: "switch-A").count) B=\(model.attachments(for: "switch-B").count)")
                .accessibilityIdentifier("screen-frames-fixture-drafts")
            Button("验证账号隔离") { Task { await verifyAccountIsolation() } }
                .disabled(binding == nil || busy)
                .accessibilityIdentifier("screen-frames-fixture-account")
            Button("验证清空后副本") { Task { await verifyCopies() } }
                .disabled(binding == nil || busy)
                .accessibilityIdentifier("screen-frames-fixture-verify")
            Text(result).font(.caption).accessibilityIdentifier("screen-frames-fixture-result")
        }
        .padding(24)
        .sheet(item: $presentation) { original in
            AIMobileScreenFramesView(model: model, binding: original)
        }
    }

    @MainActor private func prepare() async {
        busy = true
        result = "真实采样中"
        defer { busy = false }
        do {
            let workspace = FileManager.default.temporaryDirectory
                .appendingPathComponent("screen-frame-ui-" + UUID().uuidString, isDirectory: true)
            model.installSessionSwitchUITestFixture()
            model.installScreenFrameUITestWorkspace(workspace.path, accountID: "screen-frame-test-a")
            let original = try model.makeScreenFrameImportSnapshot()
            let context = try await AIMobileScreenFrameArchive.shared.prepareScreenArchiveUITest(
                workspace: workspace, sessionID: original.ownerSessionID)
            let listing = try await AIMobileScreenFrameArchive.shared.list(
                workspace: workspace, sessionID: original.ownerSessionID, batchID: context.batchID)
            guard let first = listing.frames.first else { throw AIBackendError(message: "缺少采样图片") }
            let payload = ["workspace_path": workspace.path, "session_id": original.ownerSessionID,
                           "batch_id": context.batchID, "frame_id": first.id]
            let encoded = String(data: try JSONSerialization.data(withJSONObject: payload), encoding: .utf8)!
            let exported = await AIMobileHostToolProvider.shared.execute(
                name: "mobile_export_screen_frame", argumentsJSON: encoded)
            guard exported.errorCode == nil, let data = exported.output?.data(using: .utf8),
                  let receipt = try JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let file = receipt["file_path"] as? String else {
                throw AIBackendError(message: exported.errorMessage ?? "宿主未导出截图")
            }
            hostCopy = URL(fileURLWithPath: file)
            var foreign = payload
            foreign["session_id"] = original.ownerSessionID + ":another-owner"
            let rejected = await AIMobileHostToolProvider.shared.execute(
                name: "mobile_export_screen_frame", argumentsJSON: String(data:
                    try JSONSerialization.data(withJSONObject: foreign), encoding: .utf8)!)
            guard rejected.errorCode == "not_found" else {
                throw AIBackendError(message: "宿主接受了其他会话的采样图片")
            }
            binding = try model.registerScreenFrameArchive(context: context, snapshot: original)
            result = "采样已准备"
        } catch { result = "FAIL: " + error.localizedDescription }
    }

    @MainActor private func verifyAccountIsolation() async {
        guard let binding else { return }
        busy = true
        defer {
            model.installScreenFrameUITestWorkspace(binding.context.workspace.path,
                                                     accountID: "screen-frame-test-a")
            busy = false
        }
        do {
            let listing = try await AIMobileScreenFrameArchive.shared.list(
                workspace: binding.context.workspace, sessionID: binding.context.ownerSessionID,
                batchID: binding.id)
            guard let first = listing.frames.first else { throw AIBackendError(message: "缺少真实截图") }
            model.installScreenFrameUITestWorkspace(binding.context.workspace.path,
                                                     accountID: "screen-frame-test-b")
            var rejected = false
            do { _ = try await model.addScreenFrames([first.id], binding: binding) }
            catch { rejected = true }
            guard rejected, model.attachments(for: "switch-A").isEmpty,
                  model.attachments(for: "switch-B").isEmpty else {
                throw AIBackendError(message: "另一个账号接受了截图")
            }
            result = "账号隔离通过"
        } catch { result = "FAIL: " + error.localizedDescription }
    }

    @MainActor private func verifyCopies() async {
        guard let binding else { return }
        busy = true
        defer { busy = false }
        do {
            let originals = try await AIMobileScreenFrameArchive.shared.list(
                workspace: binding.context.workspace, sessionID: binding.context.ownerSessionID)
            guard !originals.batches.contains(where: { $0.id == binding.id }),
                  model.attachments(for: "switch-A").count == 2,
                  model.attachments(for: "switch-B").isEmpty else {
                throw AIBackendError(message: "清空档案或原会话附件数量不正确")
            }
            guard let hostCopy else { throw AIBackendError(message: "宿主未保留导出副本") }
            let copies = model.attachments(for: "switch-A").map {
                binding.context.workspace.appendingPathComponent($0.relativePath)
            } + [hostCopy]
            try await Task.detached(priority: .utility) {
                for file in copies {
                    let data = try Data(contentsOf: file)
                    guard data.starts(with: [137, 80, 78, 71, 13, 10, 26, 10]) else {
                        throw AIBackendError(message: "清空档案删除了草稿副本")
                    }
                }
            }.value
            result = "PASS: 原会话2张、其他会话0张、档案已清空且副本可读"
        } catch { result = "FAIL: " + error.localizedDescription }
    }
}
#endif
