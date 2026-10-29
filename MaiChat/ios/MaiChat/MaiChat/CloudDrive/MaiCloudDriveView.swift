// Account-private media cloud drive. The page never exposes an upload publicly.
import ImageIO
import PhotosUI
import SwiftUI
import UniformTypeIdentifiers

private final class MaiCloudPhotoStream {
    let file: FileHandle
    init(url: URL) throws { file = try FileHandle(forReadingFrom: url) }
    deinit { try? file.close() }
}

private let maiCloudPhotoReadNext: MaiCloudDriveReadNext = {
    context, output, capacity, bytesRead in
    guard let context, let output, let bytesRead else { return 0 }
    do {
        let stream = Unmanaged<MaiCloudPhotoStream>.fromOpaque(context).takeUnretainedValue()
        let bytes = try stream.file.read(upToCount: capacity) ?? Data()
        bytes.withUnsafeBytes { raw in
            if let base = raw.baseAddress {
                output.update(from: base.assumingMemoryBound(to: UInt8.self),
                              count: bytes.count)
            }
        }
        bytesRead.pointee = bytes.count
        return 1
    } catch { return 0 }
}

private struct MaiCloudPhotoMedia: Transferable, Sendable {
    let url: URL
    let displayName: String

    static var transferRepresentation: some TransferRepresentation {
        FileRepresentation(contentType: .movie) { media in
            SentTransferredFile(media.url)
        } importing: { received in
            try stage(source: received.file, isImage: false)
        }
        FileRepresentation(contentType: .image) { media in
            SentTransferredFile(media.url)
        } importing: { received in
            try stage(source: received.file, isImage: true)
        }
    }

    private static func stage(source: URL, isImage: Bool) throws -> MaiCloudPhotoMedia {
        let extensionName: String
        if isImage {
            guard let imageSource = CGImageSourceCreateWithURL(source as CFURL, nil),
                  let imageType = CGImageSourceGetType(imageSource) as String?,
                  let type = UTType(imageType) else {
                throw MaiCloudDriveError(status: 0, httpStatus: 0,
                                         detail: "无法识别选中的相册图片")
            }
            if type.conforms(to: .jpeg) { extensionName = "jpg" }
            else if type.conforms(to: .png) { extensionName = "png" }
            else if type.conforms(to: .heic) { extensionName = "heic" }
            else if let webp = UTType(filenameExtension: "webp"),
                    type.conforms(to: webp) { extensionName = "webp" }
            else {
                throw MaiCloudDriveError(status: 0, httpStatus: 0,
                                         detail: "云盘支持 JPEG、PNG、WebP 和 HEIC 图片")
            }
        } else {
            extensionName = source.pathExtension.lowercased() == "mp4" ? "mp4" : "mov"
        }
        let size = try source.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
        let limit = isImage ? 20_000_000 : 2_000_000_000
        guard size > 0, size <= limit else {
            throw MaiCloudDriveError(status: 0, httpStatus: 0,
                                     detail: isImage ? "相册图片必须在 20 MB 以内"
                                                     : "相册视频必须在 2 GB 以内")
        }
        let directory = FileManager.default.urls(for: .applicationSupportDirectory,
            in: .userDomainMask)[0]
            .appendingPathComponent("MaiCloudDrivePhotoImports", isDirectory: true)
        try FileManager.default.createDirectory(at: directory,
                                                withIntermediateDirectories: true)
        let target = directory.appendingPathComponent(UUID().uuidString)
            .appendingPathExtension(extensionName)
        let stream = try MaiCloudPhotoStream(url: source)
        var result = MaiCloudDriveResult()
        target.path.withCString { path in
            mai_cloud_drive_stage_stream(path, UInt64(size),
                maiCloudPhotoReadNext, Unmanaged.passUnretained(stream).toOpaque(),
                nil, nil, nil, &result)
        }
        let status = result.status
        mai_cloud_drive_result_free(&result)
        guard status == MAI_CLOUD_DRIVE_OK else {
            throw MaiCloudDriveError(status: Int32(status.rawValue), httpStatus: 0,
                                     detail: "无法准备相册文件")
        }
        let sourceName = source.lastPathComponent
        let sourceStem = source.deletingPathExtension().lastPathComponent
        let usefulName = sourceName.utf8.count <= 240 &&
            UUID(uuidString: sourceStem) == nil &&
            sourceName.rangeOfCharacter(from: CharacterSet(charactersIn: "/\\")) == nil &&
            source.pathExtension.lowercased() == extensionName
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyyMMdd_HHmmss"
        let name = usefulName ? sourceName :
            "相册\(isImage ? "图片" : "视频")_\(formatter.string(from: Date())).\(extensionName)"
        return MaiCloudPhotoMedia(url: target, displayName: name)
    }
}

private struct MaiCloudDrivePreview: Identifiable {
    let id = UUID().uuidString
    var items: [MaiCloudDriveVideo]
    let initialIndex: Int
    let accountID: String
    let folderID: String?
    var nextCursor: String?
    var visitedCursors = Set<String>()
}

@MainActor private final class MaiCloudThumbnailCache {
    static let shared = MaiCloudThumbnailCache()
    private let images = NSCache<NSString, UIImage>()
    private var failures: [String: Date] = [:]

    private init() { images.totalCostLimit = 24 * 1024 * 1024 }

    func image(for key: String) -> UIImage? { images.object(forKey: key as NSString) }
    func shouldRetry(_ key: String) -> Bool {
        guard let failedAt = failures[key] else { return true }
        return Date().timeIntervalSince(failedAt) > 300
    }
    func store(_ image: UIImage, for key: String) {
        let cost = Int(image.size.width * image.scale * image.size.height * image.scale * 4)
        images.setObject(image, forKey: key as NSString, cost: cost)
        failures.removeValue(forKey: key)
    }
    func failed(_ key: String) {
        if failures.count >= 512 {
            failures = failures.filter { Date().timeIntervalSince($0.value) < 300 }
            if failures.count >= 512, let first = failures.keys.first {
                failures.removeValue(forKey: first)
            }
        }
        failures[key] = Date()
    }
    func clear() { images.removeAllObjects(); failures.removeAll() }
}

private struct MaiCloudThumbnail: View {
    let accountID: String
    let media: MaiCloudDriveVideo
    @Binding var loaded: Bool
    @State private var image: UIImage?

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 12)
                .fill(RemoteIMStyle.blueSoft)
            if let image {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
            } else {
                Image(systemName: media.isImage ? "photo" : "play.fill")
                    .font(.system(size: 18, weight: .semibold))
                    .foregroundStyle(RemoteIMStyle.blue)
            }
            if image != nil && !media.isImage {
                Image(systemName: "play.fill")
                    .font(.system(size: 11, weight: .bold))
                    .foregroundStyle(.white)
                    .frame(width: 23, height: 23)
                    .background(.black.opacity(0.6), in: Circle())
            }
        }
        .frame(width: 72, height: 72)
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .task(id: accountID + ":" + media.id) {
            let key = accountID + ":" + media.id
            if let cached = MaiCloudThumbnailCache.shared.image(for: key) {
                image = cached
                loaded = true
                return
            }
            guard MaiCloudThumbnailCache.shared.shouldRetry(key) else { return }
            do {
                let bytes = try await MaiCloudDriveClient.shared.thumbnailData(videoID: media.id)
                guard !Task.isCancelled, let decoded = UIImage(data: bytes) else {
                    if !Task.isCancelled { MaiCloudThumbnailCache.shared.failed(key) }
                    return
                }
                MaiCloudThumbnailCache.shared.store(decoded, for: key)
                image = decoded
                loaded = true
            } catch {
                if !Task.isCancelled { MaiCloudThumbnailCache.shared.failed(key) }
            }
        }
    }
}

// ScrollView does not host native swipeActions. Keep the existing card alive
// while revealing its trailing action, and let vertical drags scroll normally.
private struct MaiCloudDriveHorizontalPan: UIViewRepresentable {
    let enabled: Bool
    let changed: (CGFloat) -> Void
    let ended: () -> Void

    func makeUIView(context: Context) -> PanView { PanView() }
    func updateUIView(_ view: PanView, context: Context) {
        view.pan.isEnabled = enabled
        view.changed = changed
        view.ended = ended
    }

    static func dismantleUIView(_ view: PanView, coordinator: ()) {
        view.pan.view?.removeGestureRecognizer(view.pan)
    }

    final class PanView: UIView, UIGestureRecognizerDelegate {
        var changed: (CGFloat) -> Void = { _ in }
        var ended: () -> Void = {}
        lazy var pan: UIPanGestureRecognizer = {
            let value = UIPanGestureRecognizer(target: self, action: #selector(dragged))
            value.delegate = self
            value.maximumNumberOfTouches = 1
            return value
        }()

        override func hitTest(_ point: CGPoint, with event: UIEvent?) -> UIView? { nil }
        override func didMoveToWindow() {
            super.didMoveToWindow()
            pan.view?.removeGestureRecognizer(pan)
            window?.addGestureRecognizer(pan)
        }

        func gestureRecognizer(_ recognizer: UIGestureRecognizer,
                               shouldReceive touch: UITouch) -> Bool {
            guard bounds.contains(touch.location(in: self)), window != nil else { return false }
            var ancestor: UIView? = self
            while let current = ancestor {
                if current.isHidden || current.alpha < 0.01 { return false }
                if current.clipsToBounds && !current.bounds.contains(touch.location(in: current)) {
                    return false
                }
                ancestor = current.superview
            }
            return true
        }

        override func gestureRecognizerShouldBegin(_ recognizer: UIGestureRecognizer) -> Bool {
            let velocity = pan.velocity(in: self)
            return abs(velocity.x) > abs(velocity.y) * 1.2
        }

        @objc private func dragged() {
            switch pan.state {
            case .began, .changed: changed(pan.translation(in: self).x)
            case .ended, .cancelled, .failed: ended()
            default: break
            }
        }
    }
}

private struct MaiCloudDriveSwipeRow<Content: View>: View {
    let id: String
    let enabled: Bool
    @Binding var revealedID: String?
    let delete: () -> Void
    @ViewBuilder let content: () -> Content
    @State private var dragOffset: CGFloat?
    @State private var visible = true
    private let actionWidth: CGFloat = 84

    private var offset: CGFloat { dragOffset ?? (revealedID == id ? -actionWidth : 0) }

    var body: some View {
        ZStack(alignment: .trailing) {
            if offset < 0 {
                Button(role: .destructive, action: delete) {
                    Label("删除", systemImage: "trash")
                        .labelStyle(.titleAndIcon)
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(.white)
                        .frame(width: actionWidth - 8)
                        .frame(maxHeight: .infinity)
                        .background(.red, in: RoundedRectangle(cornerRadius: 14))
                }
                .buttonStyle(.plain)
                .accessibilityIdentifier("cloud-drive-delete-\(id)")
            }
            content()
                .offset(x: offset)
        }
        .clipShape(RoundedRectangle(cornerRadius: 14))
        .contentShape(Rectangle())
        .background(MaiCloudDriveHorizontalPan(enabled: enabled && visible,
            changed: { translation in
                let start = revealedID == id ? -actionWidth : 0
                dragOffset = min(max(start + translation, -actionWidth), 0)
            }, ended: {
                let shouldReveal = offset < -actionWidth / 2
                withAnimation(.easeOut(duration: 0.2)) {
                    revealedID = shouldReveal ? id : nil
                    dragOffset = nil
                }
            }))
        .onAppear { visible = true }
        .onDisappear { visible = false }
        .accessibilityElement(children: .contain)
        .accessibilityAction(named: "删除") { delete() }
        .accessibilityIdentifier("cloud-drive-row-\(id)")
    }
}

struct MaiCloudDriveView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @ObservedObject private var model = MaiCloudDriveModel.shared
    @State private var importing = false
    @State private var fileSelectionAccountID = ""
    @State private var showingPhotoPicker = false
    @State private var photoSelectionAccountID = ""
    @State private var photoItem: PhotosPickerItem?
    @State private var deletingID: String?
    @State private var revealedMediaID: String?
    @State private var discardingTaskID: String?
    @State private var preview: MaiCloudDrivePreview?
    @State private var previewPageTask: Task<Void, Never>?
    @State private var previewPageRequestID: UUID?
    @State private var savingID: String?
    @State private var saveTask: Task<Void, Never>?
    @State private var loadedThumbnailIDs = Set<String>()

    var body: some View {
        ScrollViewReader { scroll in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 18) {
                    if model.availability == .disabled {
                        VStack(spacing: 12) {
                            Image(systemName: "icloud.slash")
                                .font(.system(size: 36, weight: .light))
                                .foregroundStyle(RemoteIMStyle.textSecondary)
                            Text("云盘暂不可用").font(.headline)
                            Text("当前账号尚未开通云盘，或服务暂时不可用")
                                .font(.subheadline)
                                .foregroundStyle(RemoteIMStyle.textSecondary)
                            Button("重试") { Task { await model.reload() } }
                                .font(.subheadline.weight(.semibold))
                        }
                        .frame(maxWidth: .infinity, minHeight: 240)
                        .accessibilityIdentifier("cloud-drive-unavailable")
                    } else {
                        if model.availability == .available { quotaCard }
                        if !model.notice.isEmpty {
                            Text(model.notice)
                                .font(.subheadline)
                                .foregroundStyle(RemoteIMStyle.textPrimary)
                                .padding(12)
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .background(RemoteIMStyle.blueSoft,
                                            in: RoundedRectangle(cornerRadius: 12))
                        }
                        if !model.localTasks.isEmpty {
                            sectionTitle("待上传")
                            ForEach(model.localTasks) { task in uploadCard(task) }
                        }
                        if model.currentFolderID != nil {
                            folderNavigation
                                .id("cloud-drive-directory-navigation-top")
                        }
                        if model.availability == .unknown && model.listError.isEmpty {
                            ProgressView("正在连接云盘")
                                .frame(maxWidth: .infinity, minHeight: 180)
                        } else if model.loading && model.videos.isEmpty {
                            ProgressView("正在加载云盘")
                                .frame(maxWidth: .infinity, minHeight: 180)
                        } else if model.videos.isEmpty && model.childFolders.isEmpty && model.listError.isEmpty {
                            VStack(spacing: 12) {
                                Image(systemName: "photo.on.rectangle.angled")
                                    .font(.system(size: 36, weight: .light))
                                    .foregroundStyle(RemoteIMStyle.blue)
                                Text(model.currentFolderID == nil ? "还没有图片或视频" : "文件夹是空的")
                                    .font(.headline)
                                Text(model.currentFolderID == nil
                                     ? "上传后仅当前 MaiChat 账号可见"
                                     : "可以让 AI 将云盘文件整理到这里")
                                    .font(.subheadline).foregroundStyle(.secondary)
                            }
                            .frame(maxWidth: .infinity, minHeight: 180)
                            .accessibilityIdentifier("cloud-drive-empty")
                        }
                        ForEach(model.childFolders, id: \.id) { folder in folderRow(folder) }
                        ForEach(model.videos) { media in
                            MaiCloudDriveSwipeRow(id: media.id,
                                enabled: preview == nil && deletingID == nil &&
                                    discardingTaskID == nil && !importing && !showingPhotoPicker,
                                revealedID: $revealedMediaID,
                                delete: { deletingID = media.id }) {
                                mediaCard(media)
                            }
                        }
                        if model.nextCursor != nil {
                            Button(model.loadingMore ? "加载中…" : "加载更多") {
                                Task { await model.loadMore() }
                            }
                            .disabled(model.loadingMore)
                            .frame(maxWidth: .infinity)
                            .padding(12)
                            .accessibilityIdentifier("cloud-drive-load-more")
                        }
                        if !model.listError.isEmpty {
                            VStack(alignment: .leading, spacing: 9) {
                                Text(model.listError).foregroundStyle(.red)
                                Button("重试") { Task { await model.reload() } }
                            }
                            .font(.subheadline)
                            .padding(14)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .background(Color.red.opacity(0.07),
                                        in: RoundedRectangle(cornerRadius: 12))
                        }
                    }
                }
                .padding(18)
                .id("cloud-drive-root-top")
            }
            .onChange(of: model.currentFolderID) { _ in
                scroll.scrollTo(model.currentFolderID == nil ? "cloud-drive-root-top" :
                    "cloud-drive-directory-navigation-top", anchor: .top)
            }
            .onChange(of: appState.maiChatAccountID) { _ in
                scroll.scrollTo(model.currentFolderID == nil ? "cloud-drive-root-top" :
                    "cloud-drive-directory-navigation-top", anchor: .top)
            }
        }
        .background(Color(uiColor: .systemGroupedBackground))
        .navigationTitle("我的云盘")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Menu {
                    Button(model.currentFolderID == nil ? "从文件选择" : "从文件选择并上传到根目录",
                           systemImage: "folder") {
                        fileSelectionAccountID = appState.maiChatAccountID
                        importing = true
                    }
                    Button(model.currentFolderID == nil ? "从相册选择" : "从相册选择并上传到根目录",
                           systemImage: "photo.on.rectangle") {
                        photoSelectionAccountID = appState.maiChatAccountID
                        showingPhotoPicker = true
                    }
                } label: {
                    Image(systemName: "plus")
                        .font(.system(size: 18, weight: .semibold))
                        .frame(width: 36, height: 36)
                }
                .accessibilityLabel(model.currentFolderID == nil ? "上传图片或视频" : "上传到根目录")
                .accessibilityIdentifier("cloud-drive-upload")
                .disabled(model.availability != .available)
            }
            #if DEBUG && targetEnvironment(simulator)
            if ProcessInfo.processInfo.arguments.contains("--cloud-drive-ui-test") &&
                !ProcessInfo.processInfo.arguments.contains("--cloud-drive-folder-browse-test") {
                ToolbarItem(placement: .topBarLeading) {
                    Button("上传样本") {
                        guard let sample = Bundle.main.url(forResource: "ffplay-sample",
                                                           withExtension: "mp4") else { return }
                        let account = appState.maiChatAccountID
                        Task {
                            await model.addAgentVideo(sample,
                                                      expectedAccountID: account)
                        }
                    }
                    .accessibilityIdentifier("cloud-drive-upload-sample")
                }
                ToolbarItem(placement: .topBarLeading) {
                    Button("上传图片样本") {
                        let account = appState.maiChatAccountID
                        let source = FileManager.default.urls(
                            for: .applicationSupportDirectory, in: .userDomainMask)[0]
                            .appendingPathComponent("MaiCloudDriveUITests", isDirectory: true)
                            .appendingPathComponent("sample-\(UUID().uuidString).jpg")
                        do {
                            try FileManager.default.createDirectory(
                                at: source.deletingLastPathComponent(),
                                withIntermediateDirectories: true)
                            let sample = UIGraphicsImageRenderer(
                                size: CGSize(width: 120, height: 90)).image { context in
                                UIColor.systemTeal.setFill()
                                context.fill(CGRect(x: 0, y: 0, width: 120, height: 90))
                                UIColor.white.setFill()
                                context.fill(CGRect(x: 20, y: 20, width: 80, height: 50))
                            }
                            guard let data = sample.jpegData(compressionQuality: 0.88)
                            else { return }
                            try data.write(to: source, options: .atomic)
                            Task { await model.addFile(source, expectedAccountID: account) }
                        } catch { model.showError(error.localizedDescription) }
                    }
                    .accessibilityIdentifier("cloud-drive-upload-image-sample")
                }
                ToolbarItem(placement: .topBarLeading) {
                    Button("续传样本") {
                        Task {
                            guard let sample = Bundle.main.url(forResource: "ffplay-sample",
                                                               withExtension: "mp4") else { return }
                            let directory = FileManager.default.urls(
                                for: .applicationSupportDirectory, in: .userDomainMask)[0]
                                .appendingPathComponent("MaiCloudDriveUITests", isDirectory: true)
                            try? FileManager.default.createDirectory(
                                at: directory, withIntermediateDirectories: true)
                            let source = directory.appendingPathComponent("resumable-sample.mp4")
                            try? FileManager.default.removeItem(at: source)
                            do {
                                try FileManager.default.copyItem(at: sample, to: source)
                                let handle = try FileHandle(forWritingTo: source)
                                try handle.truncate(atOffset: 20 * 1024 * 1024)
                                try handle.close()
                                await model.addAgentVideo(
                                    source, expectedAccountID: appState.maiChatAccountID)
                            } catch { model.showError(error.localizedDescription) }
                        }
                    }
                    .accessibilityIdentifier("cloud-drive-upload-large-sample")
                }
            }
            #endif
        }
        .fileImporter(isPresented: $importing,
                      allowedContentTypes: filePickerTypes) { result in
            switch result {
            case let .success(file):
                let expected = fileSelectionAccountID
                Task { await model.addFile(file, expectedAccountID: expected) }
            case let .failure(error):
                let nsError = error as NSError
                if nsError.domain != NSCocoaErrorDomain ||
                    nsError.code != NSUserCancelledError {
                    model.showError(error.localizedDescription)
                }
            }
        }
        .photosPicker(isPresented: $showingPhotoPicker,
                      selection: $photoItem, matching: .any(of: [.images, .videos]),
                      preferredItemEncoding: .current)
        .onChange(of: photoItem) { item in
            guard let item else { return }
            let expected = photoSelectionAccountID
            Task {
                defer { photoItem = nil }
                do {
                    guard let media = try await item.loadTransferable(
                        type: MaiCloudPhotoMedia.self) else {
                        model.showError("无法读取选中的相册文件")
                        return
                    }
                    await model.addStagedPhoto(media.url,
                                               displayName: media.displayName,
                                               expectedAccountID: expected)
                } catch { model.showError(error.localizedDescription) }
            }
        }
        .refreshable { await model.reload() }
        .task(id: appState.maiChatAccountID) {
            model.activate(accountID: appState.maiChatAccountID)
            await model.reload()
        }
        .onChange(of: appState.maiChatAccountID) { _ in
            saveTask?.cancel()
            closePreview()
            MaiCloudThumbnailCache.shared.clear()
            loadedThumbnailIDs.removeAll()
            revealedMediaID = nil
        }
        .onChange(of: model.currentFolderID) { _ in
            closePreview()
            revealedMediaID = nil
            deletingID = nil
            loadedThumbnailIDs.removeAll()
        }
        .onDisappear {
            if preview == nil { previewPageTask?.cancel() }
            saveTask?.cancel()
        }
        .fullScreenCover(item: $preview) { item in
            MaiCloudDriveMediaPager(items: item.items,
                initialIndex: item.initialIndex, loadImage: { media in
                    let file = try await MaiCloudDriveClient.shared.downloadForPreview(media)
                    guard !Task.isCancelled,
                          item.accountID == appState.maiChatAccountID,
                          item.folderID == model.currentFolderID else {
                        try? FileManager.default.removeItem(at: file)
                        throw CancellationError()
                    }
                    return file
                }, loadVideo: { media in
                    let vod = try await MaiCloudDriveClient.shared.openVod(
                        videoID: media.id,
                        extensionName: media.contentType == "video/quicktime" ? "mov" : "mp4")
                    guard !Task.isCancelled,
                          item.accountID == appState.maiChatAccountID,
                          item.folderID == model.currentFolderID else {
                        vod.cancel()
                        throw CancellationError()
                    }
                    return vod
                }, close: { closePreview() }, hasMore: item.nextCursor != nil,
                loadMore: { loadMorePreviewMedia(item.id) })
                .ignoresSafeArea()
        }
        .alert("从我的云盘删除这个文件？",
                            isPresented: Binding(get: { deletingID != nil },
                                                 set: { if !$0 { deletingID = nil } })) {
            if let id = deletingID {
                Button("删除文件", role: .destructive) {
                    deletingID = nil
                    revealedMediaID = nil
                    Task { await model.delete(videoID: id) }
                }
            }
            Button("取消", role: .cancel) { deletingID = nil }
        } message: {
            Text("删除后无法恢复，其他设备上的云盘列表也会同步移除。")
        }
        .confirmationDialog("取消这次上传？",
                            isPresented: Binding(get: { discardingTaskID != nil },
                                                 set: { if !$0 { discardingTaskID = nil } })) {
            if let id = discardingTaskID,
               let task = model.localTasks.first(where: { $0.id == id }) {
                Button("取消并删除上传任务", role: .destructive) {
                    discardingTaskID = nil
                    model.discardLocalTask(task)
                }
            }
            Button("继续保留", role: .cancel) { discardingTaskID = nil }
        } message: {
            Text("已上传的分片会从云端清理；原文件不会删除。")
        }
        .accessibilityIdentifier("cloud-drive-screen")
    }

    private var folderNavigation: some View {
        VStack(alignment: .leading, spacing: 10) {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 8) {
                    Button("我的云盘") {
                        Task { await model.browse(folderID: nil) }
                    }
                    .accessibilityIdentifier("cloud-drive-folder-root")
                    ForEach(model.folderPath, id: \.id) { folder in
                        Image(systemName: "chevron.right")
                            .font(.caption2.weight(.semibold))
                            .foregroundStyle(.secondary)
                        Button(folder.name) {
                            Task { await model.browse(folderID: folder.id) }
                        }
                        .accessibilityIdentifier("cloud-drive-breadcrumb-\(folder.id)")
                    }
                }
                .font(.subheadline)
                .frame(minHeight: 36)
            }
        }
    }

    private func folderRow(_ folder: MaiCloudDriveFolder) -> some View {
        Button {
            revealedMediaID = nil
            Task { await model.browse(folderID: folder.id) }
        } label: {
            HStack(spacing: 14) {
                Image(systemName: "folder.fill")
                    .font(.system(size: 28))
                    .foregroundStyle(RemoteIMStyle.blue)
                    .frame(width: 56, height: 56)
                    .background(RemoteIMStyle.blueSoft, in: RoundedRectangle(cornerRadius: 12))
                Text(folder.name)
                    .font(.body.weight(.medium))
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                    .lineLimit(2)
                    .frame(maxWidth: .infinity, alignment: .leading)
                Image(systemName: "chevron.right")
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(.secondary)
            }
            .padding(12)
            .background(Color(uiColor: .secondarySystemGroupedBackground),
                        in: RoundedRectangle(cornerRadius: 14))
        }
        .buttonStyle(.plain)
        .accessibilityLabel(folder.name)
        .accessibilityIdentifier("cloud-drive-folder-\(folder.id)")
    }

    private var quotaCard: some View {
        VStack(alignment: .leading, spacing: 13) {
            HStack {
                Label("云盘空间", systemImage: "icloud.fill")
                    .font(.headline)
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                Spacer()
                Text(formatBytes(model.quota.usedBytes + model.quota.reservedBytes)
                     + " / " + formatBytes(model.quota.limitBytes))
                    .font(.subheadline.monospacedDigit())
                    .foregroundStyle(RemoteIMStyle.textSecondary)
            }
            ProgressView(value: Double(model.quota.usedBytes + model.quota.reservedBytes),
                         total: Double(max(model.quota.limitBytes, 1)))
                .tint(RemoteIMStyle.blue)
            Text(model.quota.reservedBytes > 0
                 ? "其中 \(formatBytes(model.quota.reservedBytes)) 已预留 · 视频 2 GB / 图片 20 MB"
                 : "仅自己可见 · 视频 2 GB / 图片 20 MB")
                .font(.footnote).foregroundStyle(RemoteIMStyle.textSecondary)
        }
        .padding(17)
        .background(Color(uiColor: .secondarySystemGroupedBackground),
                    in: RoundedRectangle(cornerRadius: 16))
        .accessibilityIdentifier("cloud-drive-quota")
    }

    private func sectionTitle(_ title: String) -> some View {
        Text(title).font(.headline).foregroundStyle(RemoteIMStyle.textPrimary)
            .padding(.horizontal, 2)
    }

    private var filePickerTypes: [UTType] {
        var types: [UTType] = [.mpeg4Movie, .quickTimeMovie, .jpeg, .png, .heic]
        if let webp = UTType(filenameExtension: "webp") { types.append(webp) }
        return types
    }

    private func uploadCard(_ task: MaiCloudDriveLocalTask) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 10) {
                Image(systemName: task.contentType.hasPrefix("image/")
                      ? "photo" : "arrow.up.doc")
                    .font(.system(size: 18, weight: .medium))
                    .foregroundStyle(RemoteIMStyle.blue)
                    .frame(width: 38, height: 38)
                    .background(RemoteIMStyle.blueSoft,
                                in: RoundedRectangle(cornerRadius: 10))
                VStack(alignment: .leading, spacing: 4) {
                    Text(task.name).font(.subheadline.weight(.semibold))
                        .lineLimit(1).truncationMode(.middle)
                    Text(formatBytes(task.sizeBytes))
                        .font(.caption).foregroundStyle(.secondary)
                }
                Spacer(minLength: 0)
                Button(model.isUploading(task.id) ? "暂停" : "继续") {
                    if model.isUploading(task.id) { model.pause(taskID: task.id) }
                    else { model.upload(task) }
                }
                .font(.subheadline.weight(.semibold))
                .accessibilityIdentifier("cloud-drive-upload-action-\(task.id)")
                if !model.isUploading(task.id) {
                    Menu {
                        Button("取消上传", role: .destructive) {
                            discardingTaskID = task.id
                        }
                    } label: {
                        Image(systemName: "ellipsis")
                            .frame(width: 36, height: 36)
                    }
                }
            }
            if let progress = model.progress[task.id] {
                ProgressView(value: Double(progress.uploaded),
                             total: Double(max(progress.total, 1)))
                Text("已上传 \(formatBytes(progress.uploaded)) / \(formatBytes(progress.total))")
                    .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
            } else if let error = model.errors[task.id] {
                Text(error).font(.footnote).foregroundStyle(.red)
            } else {
                Text("已暂停，稍后可继续")
                    .font(.footnote).foregroundStyle(.secondary)
            }
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color(uiColor: .secondarySystemGroupedBackground),
                    in: RoundedRectangle(cornerRadius: 14))
        .accessibilityIdentifier("cloud-drive-upload-task-\(task.id)")
    }

    private func mediaCard(_ media: MaiCloudDriveVideo) -> some View {
        HStack(spacing: 12) {
            Button { open(media) } label: {
                HStack(spacing: 12) {
                    ZStack {
                        MaiCloudThumbnail(accountID: appState.maiChatAccountID,
                                          media: media,
                                          loaded: Binding(
                                            get: { loadedThumbnailIDs.contains(media.id) },
                                            set: { ready in
                                                if ready { loadedThumbnailIDs.insert(media.id) }
                                                else { loadedThumbnailIDs.remove(media.id) }
                                            }))
                        if savingID == media.id {
                            RoundedRectangle(cornerRadius: 12)
                                .fill(.black.opacity(0.45))
                            ProgressView().tint(.white)
                        }
                    }
                    .frame(width: 72, height: 72)
                    VStack(alignment: .leading, spacing: 5) {
                        Text(media.name).font(.subheadline.weight(.semibold))
                            .lineLimit(1).truncationMode(.middle)
                        Text(formatBytes(media.sizeBytes))
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    Spacer(minLength: 0)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(media.isImage ? "查看图片" : "播放视频")
            .accessibilityValue(loadedThumbnailIDs.contains(media.id)
                ? "封面已加载" : "封面待加载")
            .accessibilityIdentifier("cloud-drive-play-\(media.id)")
            if !media.isImage {
                Button {
                    saveVideo(media)
                } label: {
                    Image(systemName: "arrow.down.to.line")
                        .frame(width: 44, height: 44)
                }
                .buttonStyle(.plain)
                .disabled(savingID != nil)
                .accessibilityLabel("保存到相册")
                .accessibilityIdentifier("cloud-drive-video-menu-\(media.id)")
            }
        }
        .padding(12)
        .background(Color(uiColor: .secondarySystemGroupedBackground),
                    in: RoundedRectangle(cornerRadius: 14))
    }

    private func open(_ media: MaiCloudDriveVideo) {
        if revealedMediaID == media.id {
            withAnimation(.easeOut(duration: 0.2)) { revealedMediaID = nil }
            return
        }
        guard preview == nil,
              let index = model.videos.firstIndex(where: { $0.id == media.id }) else { return }
        let next = MaiCloudDrivePreview(items: model.videos, initialIndex: index,
            accountID: appState.maiChatAccountID, folderID: model.currentFolderID,
            nextCursor: model.nextCursor)
        preview = next
        loadMorePreviewMedia(next.id)
    }

    private func closePreview() {
        previewPageTask?.cancel()
        previewPageTask = nil
        previewPageRequestID = nil
        preview = nil
    }

    private func loadMorePreviewMedia(_ id: String) {
        guard previewPageTask == nil, let initial = preview,
              initial.id == id, initial.nextCursor != nil else { return }
        let requestID = UUID()
        previewPageRequestID = requestID
        previewPageTask = Task { @MainActor in
            defer {
                if previewPageRequestID == requestID {
                    previewPageTask = nil
                    previewPageRequestID = nil
                }
            }
            // Render the selected media immediately. Append metadata in bounded
            // batches; approaching the loaded end can request another batch.
            for _ in 0..<50 {
                guard !Task.isCancelled, let current = preview,
                      current.id == id, current.accountID == appState.maiChatAccountID,
                      let cursor = current.nextCursor else { return }
                do {
                    let page = try await MaiCloudDriveClient.shared.list(
                        before: cursor, folderID: current.folderID ?? "")
                    guard !Task.isCancelled, var updated = preview, updated.id == id,
                          updated.accountID == appState.maiChatAccountID,
                          updated.folderID == model.currentFolderID else { return }
                    var known = Set(updated.items.map(\.id))
                    updated.items.append(contentsOf: page.videos.filter {
                        $0.folderId == updated.folderID && known.insert($0.id).inserted
                    })
                    if let next = page.nextCursor,
                       next == cursor || updated.visitedCursors.contains(next) {
                        // Keep the last request cursor retryable. A repeating
                        // server cursor is neither an end marker nor progress.
                        preview = updated
                        return
                    }
                    updated.visitedCursors.insert(cursor)
                    updated.nextCursor = page.nextCursor
                    preview = updated
                    if page.nextCursor == nil { return }
                } catch { return }
            }
        }
    }

    private func saveVideo(_ media: MaiCloudDriveVideo) {
        guard savingID == nil, !media.isImage else { return }
        let expectedAccount = appState.maiChatAccountID
        savingID = media.id
        saveTask = Task {
            defer { savingID = nil }
            do {
                let file = try await MaiCloudDriveClient.shared.downloadForExplicitSave(media)
                defer { try? FileManager.default.removeItem(at: file) }
                guard !Task.isCancelled,
                      expectedAccount == appState.maiChatAccountID else { return }
                guard await MaiPhotoLibraryMediaWriter.requestAddAccess() else {
                    model.showError("请允许 MaiChat 添加视频到相册")
                    return
                }
                guard !Task.isCancelled,
                      expectedAccount == appState.maiChatAccountID else { return }
                try await MaiPhotoLibraryMediaWriter.saveVideo(at: file, createdAt: Date())
                model.showNotice("已保存到相册")
            } catch {
                if !Task.isCancelled { model.showError(error.localizedDescription) }
            }
        }
    }

    private func formatBytes(_ bytes: UInt64) -> String {
        if bytes == 0 { return "0 B" }
        return ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .decimal)
    }
}

#if targetEnvironment(simulator)
@MainActor
private final class MaiCloudMixedMediaFixture: ObservableObject {
    @Published var items: [MaiCloudDriveVideo] = []
    @Published var error = ""
    @Published var opens = 0
    @Published var cleanup = ""
    private var imageFiles: [URL] = []

    func prepare() async {
        do {
            let environment = ProcessInfo.processInfo.environment
            guard environment["MAICHAT_LOCAL_CLOUD_DRIVE_URL"] == "https://localhost:18448",
                  environment["MAICHAT_LOCAL_UI_PHONE"] == "sync-ui-test",
                  let password = environment["MAICHAT_LOCAL_UI_PASSWORD"],
                  let sample = Bundle.main.url(forResource: "ffplay-seek-sample",
                                               withExtension: "mp4"),
                  let cancel = MaiCloudDriveCancelToken() else {
                throw CocoaError(.fileReadNoSuchFile)
            }
            let session = try await MaiChatAuthClient.shared.loginWithPassword(
                account: "sync-ui-test", password: password, ageConfirmed: true)
            let bytes = try sample.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
            let id = UUID().uuidString.lowercased()
            let task = MaiCloudDriveLocalTask(id: id, accountID: session.user.id,
                name: sample.lastPathComponent, contentType: "video/mp4", sizeBytes: UInt64(bytes),
                sourceID: sample.path, sourcePath: sample.path, bookmark: nil,
                ownsSource: false, createdAt: Date())
            let checkpoint = FileManager.default.temporaryDirectory
                .appendingPathComponent(id + ".maicloud.json")
            defer { try? FileManager.default.removeItem(at: checkpoint) }
            let uploaded = try await MaiCloudDriveClient.shared.upload(source: task,
                checkpointURL: checkpoint, cancel: cancel, progress: { _, _ in })
            guard let receipt = try JSONSerialization.jsonObject(with: uploaded) as? [String: Any],
                  let mediaID = receipt["video_id"] as? String,
                  let video = try await MaiCloudDriveClient.shared.list().videos.first(where: {
                    $0.id == mediaID
                  }) else { throw CocoaError(.fileReadCorruptFile) }
            func image(_ id: String, _ name: String) -> MaiCloudDriveVideo {
                MaiCloudDriveVideo(id: id, name: name, sizeBytes: 1024,
                    contentType: "image/png", mediaType: "image",
                    createdAt: 1_793_181_600_000, folderId: nil)
            }
            items = [image("223e4567-e89b-42d3-a456-426614174000", "图片一.png"), video,
                     image("323e4567-e89b-42d3-a456-426614174000", "图片三.png")]
        } catch { self.error = error.localizedDescription }
    }

    func loadImage(_ item: MaiCloudDriveVideo) throws -> URL {
        let first = item.id == items.first?.id
        let pixels = UIGraphicsImageRenderer(size: CGSize(width: 900, height: 1200)).image { context in
            (first ? UIColor.systemRed : UIColor.systemBlue).setFill()
            context.fill(CGRect(x: 0, y: 0, width: 900, height: 1200))
            ((first ? "IMAGE BEFORE VIDEO" : "IMAGE AFTER VIDEO") as NSString).draw(
                at: CGPoint(x: 40, y: 550), withAttributes: [
                    .font: UIFont.boldSystemFont(ofSize: 58), .foregroundColor: UIColor.white])
        }
        let file = FileManager.default.temporaryDirectory
            .appendingPathComponent("cloud-mixed-image-" + UUID().uuidString + ".png")
        try pixels.pngData()!.write(to: file)
        imageFiles.append(file)
        return file
    }

    func loadVideo(_ item: MaiCloudDriveVideo) async throws -> MaiCloudVodSession {
        let source = try await MaiCloudDriveClient.shared.openVod(videoID: item.id)
        opens += 1
        return source
    }

    func checkCleanup() async {
        for _ in 0..<20 {
            if MaiFfplayMobilePlayer.active == nil && imageFiles.allSatisfy({
                !FileManager.default.fileExists(atPath: $0.path)
            }) {
                cleanup = "混合预览已停止且缓存已清理；视频打开\(opens)次"
                return
            }
            try? await Task.sleep(for: .milliseconds(100))
        }
        cleanup = "混合预览清理失败"
    }
}

struct MaiCloudMixedMediaUITestRoot: View {
    private struct Presentation: Identifiable {
        let index: Int
        var id: Int { index }
    }
    @StateObject private var fixture = MaiCloudMixedMediaFixture()
    @State private var presentation: Presentation?

    var body: some View {
        VStack(spacing: 20) {
            if !fixture.error.isEmpty { Text(fixture.error).foregroundStyle(.red) }
            if fixture.items.isEmpty { ProgressView("正在准备混合媒体") }
            else {
                Button("从图片开始") { presentation = Presentation(index: 0) }
                    .accessibilityIdentifier("cloud-mixed-open-image")
                Button("从视频开始") { presentation = Presentation(index: 1) }
                    .accessibilityIdentifier("cloud-mixed-open-video")
            }
            if !fixture.cleanup.isEmpty { Text(fixture.cleanup) }
        }
        .task { await fixture.prepare() }
        .fullScreenCover(item: $presentation, onDismiss: {
            Task { await fixture.checkCleanup() }
        }) { selected in
            MaiCloudDriveMediaPager(items: fixture.items, initialIndex: selected.index,
                loadImage: { try fixture.loadImage($0) },
                loadVideo: { try await fixture.loadVideo($0) }, close: { presentation = nil })
                .ignoresSafeArea()
        }
    }
}

private struct MaiCloudFolderBrowseUITestRoot: View {
    @StateObject private var appState = RemoteIMAppState(loadedSettings: .empty)
    @ObservedObject private var model = MaiCloudDriveModel.shared
    @State private var ready = false
    @State private var result = ""
    @State private var emptyFolderID: String?

    var body: some View {
        NavigationStack {
            VStack(spacing: 6) {
                HStack {
                    Button("准备样本") { Task { await prepare() } }
                        .accessibilityIdentifier("cloud-folder-browse-prepare")
                    Button("目录竞态") { Task { await navigationRace() } }
                        .accessibilityIdentifier("cloud-folder-browse-navigation-race")
                    Button("切号竞态") { Task { await accountRace() } }
                        .accessibilityIdentifier("cloud-folder-browse-account-race")
                    Button("远端移除") { Task { await removeEmpty() } }
                        .accessibilityIdentifier("cloud-folder-browse-remove-empty")
                }
                .font(.caption)
                .disabled(!ready)
                if !result.isEmpty { Text(result).font(.caption) }
                MaiCloudDriveView().environmentObject(appState)
            }
        }
        .task {
            guard ProcessInfo.processInfo.environment["MAICHAT_LOCAL_CLOUD_DRIVE_URL"] ==
                "https://localhost:18448" else { return }
            do {
                let session = try await MaiChatAuthClient.shared.loginWithPassword(
                    account: "sync-ui-test", password: "local sync UI test passphrase",
                    ageConfirmed: true)
                appState.maiChatAccountID = session.user.id
                ready = true
            } catch { result = "浏览样本登录失败：" + error.localizedDescription }
        }
    }

    private func prepare() async {
        guard ready, let sample = Bundle.main.url(forResource: "ffplay-sample", withExtension: "mp4"),
              let cancel = MaiCloudDriveCancelToken() else { return }
        do {
            let client = MaiCloudDriveClient.shared
            for index in 0..<15 {
                _ = try await client.createFolder(name: String(format: "样本目录%02d", index))
            }
            let parent = try await client.createFolder(name: "旅行资料")
            for index in 0..<15 {
                _ = try await client.createFolder(name: String(format: "子目录%02d", index),
                                                   parentID: parent.id)
            }
            let child = try await client.createFolder(name: "桂山岛之旅", parentID: parent.id)
            let empty = try await client.createFolder(name: "空文件夹")
            emptyFolderID = empty.id
            let id = UUID().uuidString.lowercased()
            let size = try sample.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
            let source = MaiCloudDriveLocalTask(id: id, accountID: appState.maiChatAccountID,
                name: sample.lastPathComponent, contentType: "video/mp4", sizeBytes: UInt64(size),
                sourceID: sample.path, sourcePath: sample.path, bookmark: nil,
                ownsSource: false, createdAt: Date())
            let checkpoint = FileManager.default.temporaryDirectory.appendingPathComponent(id + ".maicloud.json")
            defer { try? FileManager.default.removeItem(at: checkpoint) }
            let bytes = try await client.upload(source: source, checkpointURL: checkpoint,
                cancel: cancel, progress: { _, _ in })
            let receipt = try JSONSerialization.jsonObject(with: bytes) as? [String: Any]
            guard let mediaID = receipt?["video_id"] as? String else { throw CocoaError(.fileReadCorruptFile) }
            try await client.moveMedia(id: mediaID, folderID: child.id)
            await model.reload()
            result = "文件夹浏览样本已准备"
        } catch { result = "浏览样本失败：" + error.localizedDescription }
    }

    private func navigationRace() async {
        guard model.currentFolderID != nil else { return }
        let pending = Task { await model.reload() }
        await Task.yield()
        let inFlight = model.loading
        await model.browse(folderID: nil)
        await pending.value
        result = inFlight && model.currentFolderID == nil && !model.loading &&
            model.videos.isEmpty && !model.childFolders.isEmpty
            ? "目录切换未混入旧文件" : "目录竞态验收失败"
    }

    private func accountRace() async {
        let pending = Task { await model.reload() }
        await Task.yield()
        let inFlight = model.loading
        model.activate(accountID: "sync-ui-viewer")
        let resetLoading = !model.loading && !model.loadingMore
        do {
            let session = try await MaiChatAuthClient.shared.loginWithPassword(
                account: "sync-ui-viewer", password: "local sync UI test passphrase", ageConfirmed: true)
            appState.maiChatAccountID = session.user.id
            await model.reload()
            await pending.value
            for _ in 0..<100 where model.loading { try? await Task.sleep(for: .milliseconds(20)) }
            result = inFlight && resetLoading && model.currentFolderID == nil &&
                !model.loading && model.folders.isEmpty && model.videos.isEmpty
                ? "切号加载状态已恢复且无旧目录" : "切号竞态验收失败"
        } catch { result = "切号样本失败：" + error.localizedDescription }
    }

    private func removeEmpty() async {
        guard let id = emptyFolderID, model.currentFolderID == id else { return }
        do {
            try await MaiCloudDriveClient.shared.deleteFolder(id: id)
            await model.reload()
            result = "已模拟远端移除空文件夹"
        } catch { result = error.localizedDescription }
    }
}

struct MaiCloudDriveUITestRoot: View {
    @StateObject private var appState = RemoteIMAppState(loadedSettings: .empty)
    @State private var loginError = ""
    @State private var accountSwitchResult = ""
    @State private var toolResult = ""
    @State private var cleanupResult = ""
    @State private var canaryLoginReady = false

    var body: some View {
        if ProcessInfo.processInfo.arguments.contains("--cloud-drive-folder-browse-test") {
            MaiCloudFolderBrowseUITestRoot()
        } else {
            ordinaryTestBody
        }
    }

    private var ordinaryTestBody: some View {
        NavigationStack {
            VStack {
                if !loginError.isEmpty { Text(loginError).foregroundStyle(.red) }
                if canaryLoginReady { Text("云盘灰度账号已登录") }
                if canaryLoginReady { Text(appState.maiChatAccountID) }
                if ProcessInfo.processInfo.arguments.contains("--cloud-drive-folder-tool-test") {
                    Button("测试云盘分组工具") {
                        Task { await runFolderToolsTest() }
                    }
                    .accessibilityIdentifier("cloud-drive-folder-tools-test")
                }
                Button("模拟切号时相册导入") {
                    Task {
                        let originalAccount = appState.maiChatAccountID
                        guard let sample = Bundle.main.url(forResource: "ffplay-sample",
                                                           withExtension: "mp4") else { return }
                        let directory = FileManager.default.urls(
                            for: .applicationSupportDirectory, in: .userDomainMask)[0]
                            .appendingPathComponent("MaiCloudDrivePhotoImports",
                                                isDirectory: true)
                        do {
                            try FileManager.default.createDirectory(
                                at: directory, withIntermediateDirectories: true)
                            let staged = directory.appendingPathComponent(
                                UUID().uuidString + ".mp4")
                            try FileManager.default.copyItem(at: sample, to: staged)
                            try await Task.sleep(for: .milliseconds(350))
                            appState.maiChatAccountID = "sync-ui-viewer"
                            MaiCloudDriveModel.shared.activate(accountID: "sync-ui-viewer")
                            await MaiCloudDriveModel.shared.addStagedPhoto(
                                staged, displayName: "IMG_0042.mp4",
                                expectedAccountID: originalAccount)
                            accountSwitchResult =
                                !FileManager.default.fileExists(atPath: staged.path) &&
                                MaiCloudDriveModel.shared.localTasks.isEmpty
                                ? "已阻止跨账号上传" : "跨账号导入失败"
                        } catch { accountSwitchResult = error.localizedDescription }
                    }
                }
                .accessibilityIdentifier("cloud-drive-simulate-account-switch")
                Button("模拟相册原名") {
                    Task {
                        guard let sample = Bundle.main.url(forResource: "ffplay-sample",
                                                           withExtension: "mp4") else { return }
                        let directory = FileManager.default.urls(
                            for: .applicationSupportDirectory, in: .userDomainMask)[0]
                            .appendingPathComponent("MaiCloudDrivePhotoImports",
                                                isDirectory: true)
                        do {
                            try FileManager.default.createDirectory(
                                at: directory, withIntermediateDirectories: true)
                            let staged = directory.appendingPathComponent(
                                UUID().uuidString + ".mp4")
                            try FileManager.default.copyItem(at: sample, to: staged)
                            await MaiCloudDriveModel.shared.addStagedPhoto(
                                staged, displayName: "IMG_0042.mp4",
                                expectedAccountID: appState.maiChatAccountID)
                        } catch { accountSwitchResult = error.localizedDescription }
                    }
                }
                .accessibilityIdentifier("cloud-drive-photo-original-name")
                Button("测试云盘缓存恢复") {
                    let files = FileManager.default
                    let previousAccount = appState.maiChatAccountID
                    let testAccount = "cloud-cleanup-" + UUID().uuidString
                    appState.maiChatAccountID = testAccount
                    let model = MaiCloudDriveModel.shared
                    model.activate(accountID: testAccount)
                    let folder = model.localDirectoryForUITests()
                    let imports = files.urls(for: .applicationSupportDirectory,
                        in: .userDomainMask)[0].appendingPathComponent(
                            "MaiCloudDrivePhotoImports", isDirectory: true)
                    do {
                        try files.createDirectory(at: folder,
                                                  withIntermediateDirectories: true)
                        try files.createDirectory(at: imports,
                                                  withIntermediateDirectories: true)
                        func source() throws -> URL {
                            let file = folder.appendingPathComponent(
                                UUID().uuidString + ".mp4")
                            try Data("video".utf8).write(to: file)
                            return file
                        }
                        func record(_ file: URL, completed: Bool) throws ->
                            MaiCloudDriveLocalTask {
                            let task = MaiCloudDriveLocalTask(
                                id: UUID().uuidString.lowercased(),
                                accountID: testAccount, name: "Video.mp4",
                                contentType: "video/mp4", sizeBytes: 5,
                                sourceID: file.path, sourcePath: file.path,
                                bookmark: nil, ownsSource: true, createdAt: Date())
                            try JSONEncoder().encode(task).write(to: folder
                                .appendingPathComponent(task.id + ".task.json"))
                            let checkpoint: [String: Any] = [
                                "source_id": file.path, "name": task.name,
                                "file_size": 5, "completed": completed,
                                "video_id": UUID().uuidString]
                            try JSONSerialization.data(withJSONObject: checkpoint)
                                .write(to: folder.appendingPathComponent(
                                    task.id + ".maicloud.json"))
                            return task
                        }
                        let orphan = try source()
                        let resumable = try source()
                        let pending = try record(resumable, completed: false)
                        let finished = try source()
                        let done = try record(finished, completed: true)
                        let staleImport = imports.appendingPathComponent(
                            UUID().uuidString + ".mp4")
                        try Data("video".utf8).write(to: staleImport)
                        try files.setAttributes([.modificationDate: Date.distantPast],
                                                ofItemAtPath: staleImport.path)
                        let currentImport = imports.appendingPathComponent(
                            UUID().uuidString + ".mp4")
                        try Data("video".utf8).write(to: currentImport)
                        model.activate(accountID: "cloud-cleanup-other")
                        model.activate(accountID: testAccount)
                        let passed = !files.fileExists(atPath: orphan.path) &&
                            files.fileExists(atPath: resumable.path) &&
                            model.localTasks.contains(where: { $0.id == pending.id }) &&
                            !files.fileExists(atPath: finished.path) &&
                            !files.fileExists(atPath: folder.appendingPathComponent(
                                done.id + ".task.json").path) &&
                            !files.fileExists(atPath: folder.appendingPathComponent(
                                done.id + ".maicloud.json").path) &&
                            !files.fileExists(atPath: staleImport.path) &&
                            files.fileExists(atPath: currentImport.path)
                        cleanupResult = passed ? "云盘缓存恢复通过" : "云盘缓存恢复失败"
                        try? files.removeItem(at: folder)
                        try? files.removeItem(at: currentImport)
                    } catch { cleanupResult = error.localizedDescription }
                    appState.maiChatAccountID = previousAccount
                    model.activate(accountID: previousAccount)
                }
                .accessibilityIdentifier("cloud-drive-cleanup-recovery")
                Button("测试云盘工具") {
                    Task {
                        let host = AIMobileHostToolProvider.shared
                        let listed = await host.execute(
                            name: "maichat_cloud_drive_list_media",
                            argumentsJSON: "{}")
                        let pending = await host.execute(
                            name: "maichat_cloud_drive_list_pending_uploads",
                            argumentsJSON: "{}")
                        guard listed.errorCode == nil, pending.errorCode == nil,
                              let sample = Bundle.main.url(
                                forResource: "ffplay-sample", withExtension: "mp4") else {
                            toolResult = "工具列表错误: " +
                                (listed.errorCode ?? pending.errorCode ?? "unknown")
                            return
                        }
                        let workspace = FileManager.default.urls(
                            for: .applicationSupportDirectory, in: .userDomainMask)[0]
                            .appendingPathComponent("CloudToolTest/Workspace",
                                                isDirectory: true)
                        do {
                            try FileManager.default.createDirectory(
                                at: workspace, withIntermediateDirectories: true)
                            let source = workspace.appendingPathComponent("tool-sample.mp4")
                            try? FileManager.default.removeItem(at: source)
                            try FileManager.default.copyItem(at: sample, to: source)
                            AIAssistantModel.shared.installVideoToolTestWorkspace(workspace.path)
                            let data = try JSONSerialization.data(withJSONObject: [
                                "file_path": source.path])
                            let result = await host.execute(
                                name: "maichat_cloud_drive_upload_media",
                                argumentsJSON: String(decoding: data, as: UTF8.self))
                            guard let output = result.output,
                                  let object = try JSONSerialization.jsonObject(
                                    with: Data(output.utf8)) as? [String: Any],
                                  object["accepted"] as? Bool == true,
                                  object["task_id"] as? String != nil else {
                                toolResult = "工具上传未受理: " +
                                    (result.errorCode ?? "unknown")
                                return
                            }
                            toolResult = "云盘工具已受理上传"
                        } catch { toolResult = error.localizedDescription }
                    }
                }
                .accessibilityIdentifier("cloud-drive-tool-smoke")
                if ProcessInfo.processInfo.arguments.contains(
                    "--cloud-drive-production-canary-tools") {
                    Button("验收云盘工具") {
                        Task { await runProductionCanaryTools() }
                    }
                    .accessibilityIdentifier("cloud-drive-production-tool-run")
                    Button("检查云盘账号隔离") {
                        Task {
                            do {
                                _ = try await MaiCloudDriveClient.shared.list()
                                toolResult = "云盘账号隔离失败：B 可访问"
                            } catch let error as MaiCloudDriveError {
                                toolResult = error.httpStatus == 404
                                    ? "云盘账号 B 接口返回 404"
                                    : "云盘账号隔离失败：状态码错误"
                            } catch { toolResult = "云盘账号隔离失败：网络错误" }
                        }
                    }
                    .accessibilityIdentifier("cloud-drive-production-peer-probe")
                }
                Button("工具继续上传") {
                    Task {
                        guard let task = MaiCloudDriveModel.shared.localTasks.first(where: {
                            !MaiCloudDriveModel.shared.isUploading($0.id)
                        }) else { toolResult = "没有暂停的任务"; return }
                        let data = try? JSONSerialization.data(withJSONObject: [
                            "task_id": task.id])
                        let result = await AIMobileHostToolProvider.shared.execute(
                            name: "maichat_cloud_drive_resume_upload",
                            argumentsJSON: data.map { String(decoding: $0, as: UTF8.self) }
                                ?? "{}")
                        toolResult = result.errorCode == nil &&
                            result.output?.contains("\"accepted\":true") == true
                            ? "云盘工具已继续" : "云盘工具继续失败"
                    }
                }
                .accessibilityIdentifier("cloud-drive-tool-resume")
                Button("工具暂停并取消上传") {
                    Task {
                        guard let task = MaiCloudDriveModel.shared.localTasks.first else {
                            toolResult = "没有可暂停的任务"
                            return
                        }
                        let payload = "{\"task_id\":\"\(task.id)\"}"
                        let host = AIMobileHostToolProvider.shared
                        let activeAbort = await host.execute(
                            name: "maichat_cloud_drive_abort_upload",
                            argumentsJSON: payload)
                        guard activeAbort.errorCode == "busy" else {
                            toolResult = "活动上传取消未被阻止: " +
                                (activeAbort.errorCode ?? "unknown")
                            return
                        }
                        let pause = await host.execute(
                            name: "maichat_cloud_drive_pause_upload",
                            argumentsJSON: payload)
                        guard pause.output?.contains("\"state\":\"pausing\"") == true else {
                            toolResult = "暂停未受理: " + (pause.errorCode ?? "unknown")
                            return
                        }
                        for _ in 0..<120 where MaiCloudDriveModel.shared.isUploading(task.id) {
                            try? await Task.sleep(for: .milliseconds(100))
                        }
                        guard !MaiCloudDriveModel.shared.isUploading(task.id) else {
                            toolResult = "暂停超时"
                            return
                        }
                        let paused = await host.execute(
                            name: "maichat_cloud_drive_pause_upload",
                            argumentsJSON: payload)
                        guard paused.output?.contains("\"state\":\"paused\"") == true else {
                            toolResult = "已暂停任务返回错误: " +
                                (paused.errorCode ?? "unknown")
                            return
                        }
                        let aborted = await host.execute(
                            name: "maichat_cloud_drive_abort_upload",
                            argumentsJSON: payload)
                        guard aborted.output?.contains("\"aborted\":true") == true,
                              !MaiCloudDriveModel.shared.localTasks.contains(where: {
                                  $0.id == task.id
                              }) else {
                            toolResult = "取消上传失败: " +
                                (aborted.errorCode ?? "unknown")
                            return
                        }
                        let missing = await host.execute(
                            name: "maichat_cloud_drive_pause_upload",
                            argumentsJSON: payload)
                        toolResult = missing.errorCode == "not_found"
                            ? "云盘工具暂停并取消通过" : "取消后任务仍存在"
                    }
                }
                .accessibilityIdentifier("cloud-drive-tool-pause-abort")
                Button("工具播放并删除") {
                    Task {
                        let id = "123e4567-e89b-42d3-a456-426614174000"
                        let payload = "{\"media_id\":\"\(id)\"}"
                        let host = AIMobileHostToolProvider.shared
                        let played = await host.execute(
                            name: "maichat_cloud_drive_open_media",
                            argumentsJSON: payload)
                        guard played.errorCode == nil,
                              let vod = appState.agentVideoPresentation?.vod,
                              let playbackPath = appState.agentVideoPresentation?.path else {
                            toolResult = "云盘工具播放失败：" +
                                (played.errorCode ?? "没有播放器路径")
                            return
                        }
                        appState.agentVideoPresentation = nil
                        guard vod.displayPath == playbackPath,
                              !FileManager.default.fileExists(atPath: playbackPath) else {
                            toolResult = "云盘工具点播源异常"
                            return
                        }
                        let deleted = await host.execute(
                            name: "maichat_cloud_drive_delete_media",
                            argumentsJSON: payload)
                        toolResult = deleted.errorCode == nil &&
                            deleted.output?.contains("\"deleted\":true") == true
                            ? "云盘工具已播放并删除" : "云盘工具删除失败"
                    }
                }
                .accessibilityIdentifier("cloud-drive-tool-play-delete")
                Button("工具图片上传预览删除") {
                    Task {
                        let host = AIMobileHostToolProvider.shared
                        let workspace = FileManager.default.urls(
                            for: .applicationSupportDirectory, in: .userDomainMask)[0]
                            .appendingPathComponent("CloudToolImageWorkspace",
                                                isDirectory: true)
                        let name = "agent-image-\(UUID().uuidString).jpg"
                        let source = workspace.appendingPathComponent(name)
                        defer { try? FileManager.default.removeItem(at: source) }
                        do {
                            try FileManager.default.createDirectory(
                                at: workspace, withIntermediateDirectories: true)
                            let sample = UIGraphicsImageRenderer(
                                size: CGSize(width: 120, height: 90)).image { context in
                                UIColor.systemTeal.setFill()
                                context.fill(CGRect(x: 0, y: 0, width: 120, height: 90))
                            }
                            guard let data = sample.jpegData(compressionQuality: 0.85) else {
                                toolResult = "云盘图片工具样本生成失败"
                                return
                            }
                            try data.write(to: source, options: .atomic)
                            AIAssistantModel.shared.installVideoToolTestWorkspace(workspace.path)
                            func json(_ object: [String: String]) -> String {
                                guard let data = try? JSONSerialization.data(
                                    withJSONObject: object) else { return "{}" }
                                return String(decoding: data, as: UTF8.self)
                            }
                            func body(_ value: String?) -> [String: Any]? {
                                guard let value else { return nil }
                                return try? JSONSerialization.jsonObject(
                                    with: Data(value.utf8)) as? [String: Any]
                            }
                            let uploaded = await host.execute(
                                name: "maichat_cloud_drive_upload_media",
                                argumentsJSON: json(["file_path": source.path]))
                            guard uploaded.errorCode == nil,
                                  body(uploaded.output)?["accepted"] as? Bool == true else {
                                toolResult = "云盘图片工具上传失败: " +
                                    (uploaded.errorCode ?? "unknown")
                                return
                            }
                            var mediaID: String?
                            for _ in 0..<90 {
                                let listed = await host.execute(
                                    name: "maichat_cloud_drive_list_media",
                                    argumentsJSON: "{}")
                                let media = body(listed.output)?["media"] as? [[String: Any]]
                                mediaID = media?.first(where: {
                                    $0["name"] as? String == name &&
                                    $0["media_type"] as? String == "image"
                                })?["media_id"] as? String
                                if mediaID != nil { break }
                                try? await Task.sleep(for: .seconds(1))
                            }
                            guard let mediaID else {
                                toolResult = "云盘图片工具列表缺失"
                                return
                            }
                            let argument = json(["media_id": mediaID])
                            let opened = await host.execute(
                                name: "maichat_cloud_drive_open_media",
                                argumentsJSON: argument)
                            guard opened.errorCode == nil,
                                  body(opened.output)?["media_type"] as? String == "image",
                                  let path = AIAssistantModel.shared.previewImage?.filePath,
                                  MaiCloudDriveAgentPlaybackCache.shared.ownsAgentImage(
                                    path: path) else {
                                toolResult = "云盘图片工具预览失败: " +
                                    (opened.errorCode ?? "unknown")
                                return
                            }
                            AIAssistantModel.shared.previewImage = nil
                            MaiCloudDriveAgentPlaybackCache.shared.previewChanged(to: nil)
                            guard !FileManager.default.fileExists(atPath: path) else {
                                toolResult = "云盘图片工具预览缓存未清理"
                                return
                            }
                            let deleted = await host.execute(
                                name: "maichat_cloud_drive_delete_media",
                                argumentsJSON: argument)
                            guard deleted.errorCode == nil,
                                  body(deleted.output)?["deleted"] as? Bool == true else {
                                toolResult = "云盘图片工具删除失败"
                                return
                            }
                            toolResult = "云盘图片工具已打开并删除"
                        } catch { toolResult = error.localizedDescription }
                    }
                }
                .accessibilityIdentifier("cloud-drive-tool-image-open-delete")
                Button("检查云盘播放缓存") {
                    let files = FileManager.default
                    let directory = files.temporaryDirectory.appendingPathComponent(
                        "mai-cloud-drive-agent-playback", isDirectory: true)
                    let original = files.urls(for: .applicationSupportDirectory,
                                              in: .userDomainMask)[0]
                        .appendingPathComponent("agent-playback-original.mp4")
                    do {
                        try files.createDirectory(at: directory,
                                                  withIntermediateDirectories: true)
                        try Data("original".utf8).write(to: original)
                        func managed() throws -> URL {
                            let file = directory.appendingPathComponent(
                                UUID().uuidString + ".mp4")
                            try Data("cached".utf8).write(to: file)
                            return file
                        }
                        func managedImage() throws -> URL {
                            let file = directory.appendingPathComponent(
                                UUID().uuidString + ".png")
                            try Data("cached image".utf8).write(to: file)
                            return file
                        }
                        let closed = try managed()
                        guard MaiCloudDriveAgentPlaybackCache.shared.adopt(
                            closed, accountID: "cache-owner") else {
                            cleanupResult = "云盘播放缓存收养失败"
                            return
                        }
                        appState.agentVideoPresentation = MaiFfplayPresentation(
                            path: closed.path)
                        appState.agentVideoPresentation = nil
                        let closePassed = !files.fileExists(atPath: closed.path)
                        let switched = try managed()
                        guard MaiCloudDriveAgentPlaybackCache.shared.adopt(
                            switched, accountID: "cache-owner") else {
                            cleanupResult = "云盘播放缓存切号准备失败"
                            return
                        }
                        appState.agentVideoPresentation = MaiFfplayPresentation(
                            path: switched.path)
                        appState.maiChatAccountID = "cache-other"
                        let switchPassed = !files.fileExists(atPath: switched.path) &&
                            appState.agentVideoPresentation?.path == nil
                        appState.maiChatAccountID = "cache-image-owner"
                        let closedImage = try managedImage()
                        guard MaiCloudDriveAgentPlaybackCache.shared.adopt(
                            closedImage, accountID: "cache-image-owner") else {
                            cleanupResult = "云盘图片缓存收养失败"
                            return
                        }
                        AIAssistantModel.shared.previewImage = AIImagePreview(
                            filePath: closedImage.path)
                        AIAssistantModel.shared.previewImage = nil
                        MaiCloudDriveAgentPlaybackCache.shared.previewChanged(to: nil)
                        let imageClosePassed = !files.fileExists(atPath: closedImage.path)
                        let switchedImage = try managedImage()
                        guard MaiCloudDriveAgentPlaybackCache.shared.adopt(
                            switchedImage, accountID: "cache-image-owner") else {
                            cleanupResult = "云盘图片缓存切号准备失败"
                            return
                        }
                        AIAssistantModel.shared.previewImage = AIImagePreview(
                            filePath: switchedImage.path)
                        appState.maiChatAccountID = "cache-image-other"
                        let imageSwitchPassed =
                            !files.fileExists(atPath: switchedImage.path) &&
                            AIAssistantModel.shared.previewImage == nil
                        let orphan = try managed()
                        let partial = directory.appendingPathComponent(
                            UUID().uuidString + ".mp4.maitmp-probe")
                        try Data("partial".utf8).write(to: partial)
                        let orphanImage = try managedImage()
                        let partialImage = directory.appendingPathComponent(
                            UUID().uuidString + ".png.maitmp-probe")
                        try Data("partial image".utf8).write(to: partialImage)
                        UserDefaults.standard.set([orphan.path, partial.path,
                                                   orphanImage.path, partialImage.path,
                                                   original.path],
                                                  forKey: "cloud-agent-cache-relaunch-check")
                        cleanupResult = closePassed && switchPassed && imageClosePassed &&
                            imageSwitchPassed &&
                            files.fileExists(atPath: original.path)
                            ? "云盘播放关闭与切号通过" : "云盘播放缓存生命周期失败"
                    } catch { cleanupResult = "云盘播放缓存文件准备失败" }
                }
                .accessibilityIdentifier("cloud-drive-agent-cache-lifecycle")
                Button("检查云盘重启清理") {
                    let files = FileManager.default
                    guard let paths = UserDefaults.standard.stringArray(
                        forKey: "cloud-agent-cache-relaunch-check"), paths.count == 5 else {
                        cleanupResult = "没有云盘重启检查点"
                        return
                    }
                    cleanupResult = !files.fileExists(atPath: paths[0]) &&
                        !files.fileExists(atPath: paths[1]) &&
                        !files.fileExists(atPath: paths[2]) &&
                        !files.fileExists(atPath: paths[3]) &&
                        files.fileExists(atPath: paths[4])
                        ? "云盘孤儿清理通过" : "云盘孤儿清理失败"
                    try? files.removeItem(atPath: paths[4])
                    UserDefaults.standard.removeObject(
                        forKey: "cloud-agent-cache-relaunch-check")
                }
                .accessibilityIdentifier("cloud-drive-agent-cache-relaunch")
                if !accountSwitchResult.isEmpty { Text(accountSwitchResult) }
                if !toolResult.isEmpty { Text(toolResult) }
                if !cleanupResult.isEmpty { Text(cleanupResult) }
                MaiCloudDriveView()
            }
        }
        .environmentObject(appState)
        .task {
            if ProcessInfo.processInfo.arguments.contains(
                "--cloud-drive-production-canary-tools") {
                do {
                    let account = try MaiCloudDriveProductionCanaryCredential.consume()
                    let session = try await MaiChatAuthClient.shared.loginWithPassword(
                        account: account.account, password: account.password,
                        ageConfirmed: true)
                    appState.maiChatAccountID = session.user.id
                    appState.maiChatNickname = session.user.nickname ?? ""
                    AIMobileHostToolProvider.shared.appState = appState
                    canaryLoginReady = true
                } catch {
                    loginError = "云盘测试账号登录失败"
                }
                return
            }
            guard let account = ProcessInfo.processInfo.environment["MAICHAT_LOCAL_UI_PHONE"],
                  let password = ProcessInfo.processInfo.environment[
                    "MAICHAT_LOCAL_UI_PASSWORD"] else {
                loginError = "本机测试账号未提供"
                return
            }
            do {
                let session = try await MaiChatAuthClient.shared.loginWithPassword(
                    account: account, password: password, ageConfirmed: true)
                appState.maiChatAccountID = session.user.id
                appState.maiChatNickname = session.user.nickname ?? ""
                AIMobileHostToolProvider.shared.appState = appState
            } catch { loginError = error.localizedDescription }
        }
    }

    private func runFolderToolsTest() async {
        let host = AIMobileHostToolProvider.shared
        func call(_ name: String, _ arguments: [String: String] = [:]) async throws
            -> [String: Any] {
            let json = try JSONSerialization.data(withJSONObject: arguments)
            let result = await host.execute(name: "maichat_cloud_drive_" + name,
                argumentsJSON: String(decoding: json, as: UTF8.self))
            guard let output = result.output, result.errorCode == nil,
                  let body = try JSONSerialization.jsonObject(
                    with: Data(output.utf8)) as? [String: Any],
                  !output.contains("token"), !output.contains("read_url") else {
                throw NSError(domain: "CloudFolderTest", code: 1,
                              userInfo: [NSLocalizedDescriptionKey: name + ":" +
                                (result.errorCode ?? "invalid_output")])
            }
            return body
        }
        func check(_ condition: Bool, _ step: String) throws {
            if !condition {
                throw NSError(domain: "CloudFolderTest", code: 2,
                    userInfo: [NSLocalizedDescriptionKey: step])
            }
        }
        do {
            let list = try await call("list_media")
            guard let id = (list["media"] as? [[String: Any]])?.first?["media_id"] as? String
            else { throw NSError(domain: "CloudFolderTest", code: 3) }
            let parent = try await call("create_folder", ["name": "工作资料"])
            let parentID = parent["id"] as? String ?? ""
            let child = try await call("create_folder", ["name": "项目一", "parent_id": parentID])
            let childID = child["id"] as? String ?? ""
            let renamed = try await call("update_folder", ["folder_id": childID, "name": "项目二"])
            try check(renamed["name"] as? String == "项目二", "rename")
            let normal = try await MaiCloudDriveClient.shared.listFolders()
            try check(normal.count == 2 && normal.contains(where: {
                $0.id == childID && $0.parentId == parentID
            }), "normal_client_tree")
            _ = try await call("move_media", ["media_id": id, "folder_id": childID])
            let renamedMedia = try await call("rename_media", [
                "media_id": id, "name": "项目影片.MP4"])
            try check(renamedMedia["name"] as? String == "项目影片.MP4" &&
                renamedMedia["folder_id"] as? String == childID, "rename_media_receipt")
            let normalMedia = try await MaiCloudDriveClient.shared.list(folderID: childID)
            try check(normalMedia.videos.first?.name == "项目影片.MP4", "normal_client_rename")
            let directRename = try await MaiCloudDriveClient.shared.renameMedia(
                id: id, name: "归档影片.mp4")
            try check(directRename.id == id && directRename.name == "归档影片.mp4" &&
                directRename.folderId == childID, "normal_client_rename_receipt")
            let invalidRename = await host.execute(name: "maichat_cloud_drive_rename_media",
                argumentsJSON: "{\"media_id\":\"\(id)\",\"name\":\"不能转换.mov\"}")
            try check(invalidRename.errorCode == "invalid_input", "reject_extension_change")
            let grouped = try await call("list_media", ["folder_id": childID])
            let items = grouped["media"] as? [[String: Any]] ?? []
            try check(items.count == 1 && items.first?["folder_id"] as? String == childID,
                      "folder_filter")
            try check(items.first?["name"] as? String == "归档影片.mp4", "tool_sees_normal_rename")
            let root = try await call("list_media", ["folder_id": ""])
            try check((root["media"] as? [[String: Any]])?.isEmpty == true, "root_filter")
            let nonempty = await host.execute(name: "maichat_cloud_drive_delete_folder",
                argumentsJSON: "{\"folder_id\":\"\(childID)\"}")
            try check(nonempty.errorCode == "conflict", "nonempty_folder")
            let listed = try await call("list_folders")
            try check((listed["folders"] as? [[String: Any]])?.count == 2, "list_tree")
            _ = try await call("move_media", ["media_id": id, "folder_id": ""])
            _ = try await call("delete_folder", ["folder_id": childID])
            _ = try await call("delete_folder", ["folder_id": parentID])
            let after = try await call("list_folders")
            try check((after["folders"] as? [[String: Any]])?.isEmpty == true, "cleanup")
            toolResult = "云盘分组工具全链路通过"
        } catch { toolResult = "云盘分组工具失败：" + error.localizedDescription }
    }

    private func runProductionCanaryTools() async {
        guard !appState.maiChatAccountID.isEmpty,
              let sample = Bundle.main.url(forResource: "ffplay-sample",
                                           withExtension: "mp4") else {
            toolResult = "云盘真实工具失败：账号或样本未就绪"
            return
        }
        let host = AIMobileHostToolProvider.shared
        func object(_ value: String?) -> [String: Any]? {
            guard let value, let data = value.data(using: .utf8) else { return nil }
            return try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        }
        func payload(_ key: String, _ value: String) -> String {
            guard let data = try? JSONSerialization.data(withJSONObject: [key: value])
            else { return "{}" }
            return String(decoding: data, as: UTF8.self)
        }
        let listed = await host.execute(name: "maichat_cloud_drive_list_media",
                                        argumentsJSON: "{}")
        let pending = await host.execute(name: "maichat_cloud_drive_list_pending_uploads",
                                         argumentsJSON: "{}")
        guard listed.errorCode == nil, pending.errorCode == nil,
              object(listed.output)?["media"] is [[String: Any]],
              object(pending.output)?["uploads"] is [[String: Any]] else {
            toolResult = "云盘真实工具失败：列表"
            return
        }

        // The real OSS canary uses one small, valid bundled MP4. A unique name
        // keeps its cleanup scoped to the object created by this invocation.
        let workspace = FileManager.default.urls(
            for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("CloudCanaryWorkspace", isDirectory: true)
        let name = "ios-canary-" + UUID().uuidString.lowercased() + ".mp4"
        let source = workspace.appendingPathComponent(name)
        defer { try? FileManager.default.removeItem(at: source) }
        do {
            try FileManager.default.createDirectory(at: workspace,
                                                    withIntermediateDirectories: true)
            try FileManager.default.copyItem(at: sample, to: source)
            AIAssistantModel.shared.installVideoToolTestWorkspace(workspace.path)
            let uploaded = await host.execute(name: "maichat_cloud_drive_upload_media",
                                              argumentsJSON: payload("file_path", source.path))
            guard uploaded.errorCode == nil,
                  object(uploaded.output)?["accepted"] as? Bool == true else {
                toolResult = "云盘真实工具失败：上传未受理"
                return
            }
            var mediaID: String?
            for _ in 0..<90 {
                let result = await host.execute(name: "maichat_cloud_drive_list_media",
                                                argumentsJSON: "{}")
                if let media = object(result.output)?["media"] as? [[String: Any]] {
                    mediaID = media.first(where: { $0["name"] as? String == name })?["media_id"]
                        as? String
                }
                if mediaID != nil { break }
                try? await Task.sleep(for: .seconds(1))
            }
            guard let mediaID else {
                toolResult = "云盘真实工具失败：上传未完成"
                return
            }
            let played = await host.execute(name: "maichat_cloud_drive_open_media",
                                            argumentsJSON: payload("media_id", mediaID))
            guard played.errorCode == nil,
                  object(played.output)?["opened"] as? Bool == true,
                  let path = appState.agentVideoPresentation?.path else {
                toolResult = "云盘真实工具失败：播放"
                return
            }
            appState.agentVideoPresentation = nil
            try? FileManager.default.removeItem(atPath: path)
            let deleted = await host.execute(name: "maichat_cloud_drive_delete_media",
                                             argumentsJSON: payload("media_id", mediaID))
            guard deleted.errorCode == nil,
                  object(deleted.output)?["deleted"] as? Bool == true else {
                toolResult = "云盘真实工具失败：删除"
                return
            }
            let after = await host.execute(name: "maichat_cloud_drive_list_media",
                                           argumentsJSON: "{}")
            let media = object(after.output)?["media"] as? [[String: Any]] ?? []
            guard after.errorCode == nil,
                  !media.contains(where: { $0["media_id"] as? String == mediaID }) else {
                toolResult = "云盘真实工具失败：删除后仍在列表"
                return
            }
            // Full positive pause/resume/abort flows are covered by the local
            // delayed multipart fixture. Check production host registration here.
            let absent = payload("task_id", UUID().uuidString.lowercased())
            let pause = await host.execute(name: "maichat_cloud_drive_pause_upload",
                                           argumentsJSON: absent)
            let resume = await host.execute(name: "maichat_cloud_drive_resume_upload",
                                            argumentsJSON: absent)
            let abort = await host.execute(name: "maichat_cloud_drive_abort_upload",
                                           argumentsJSON: absent)
            toolResult = pause.errorCode == "not_found" &&
                resume.errorCode == "not_found" && abort.errorCode == "not_found"
                ? "云盘真实工具八项通过" : "云盘真实工具失败：暂停续传取消"
        } catch {
            toolResult = "云盘真实工具失败：本地样本"
        }
    }
}
#endif
