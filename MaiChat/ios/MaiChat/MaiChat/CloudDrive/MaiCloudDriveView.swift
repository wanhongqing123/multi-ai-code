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
    let url: URL
    let isImage: Bool
    let vod: MaiCloudVodSession?
    var id: String { url.path }
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

struct MaiCloudDriveView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @ObservedObject private var model = MaiCloudDriveModel.shared
    @State private var importing = false
    @State private var fileSelectionAccountID = ""
    @State private var showingPhotoPicker = false
    @State private var photoSelectionAccountID = ""
    @State private var photoItem: PhotosPickerItem?
    @State private var deletingID: String?
    @State private var discardingTaskID: String?
    @State private var preview: MaiCloudDrivePreview?
    @State private var openingID: String?
    @State private var playbackTask: Task<Void, Never>?
    @State private var savingID: String?
    @State private var saveTask: Task<Void, Never>?
    @State private var loadedThumbnailIDs = Set<String>()

    var body: some View {
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
                    sectionTitle("我的文件")
                    if model.availability == .unknown && model.listError.isEmpty {
                        ProgressView("正在连接云盘")
                            .frame(maxWidth: .infinity, minHeight: 180)
                    } else if model.loading && model.videos.isEmpty {
                        ProgressView("正在加载云盘")
                            .frame(maxWidth: .infinity, minHeight: 180)
                    } else if model.videos.isEmpty && model.listError.isEmpty {
                        VStack(spacing: 12) {
                            Image(systemName: "photo.on.rectangle.angled")
                                .font(.system(size: 36, weight: .light))
                                .foregroundStyle(RemoteIMStyle.blue)
                            Text("还没有图片或视频").font(.headline)
                            Text("上传后仅当前 MaiChat 账号可见")
                                .font(.subheadline).foregroundStyle(.secondary)
                        }
                        .frame(maxWidth: .infinity, minHeight: 180)
                        .accessibilityIdentifier("cloud-drive-empty")
                    }
                    ForEach(model.videos) { media in mediaCard(media) }
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
        }
        .background(Color(uiColor: .systemGroupedBackground))
        .navigationTitle("我的云盘")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Menu {
                    Button("从文件选择", systemImage: "folder") {
                        fileSelectionAccountID = appState.maiChatAccountID
                        importing = true
                    }
                    Button("从相册选择", systemImage: "photo.on.rectangle") {
                        photoSelectionAccountID = appState.maiChatAccountID
                        showingPhotoPicker = true
                    }
                } label: {
                    Image(systemName: "plus")
                        .font(.system(size: 18, weight: .semibold))
                        .frame(width: 36, height: 36)
                }
                .accessibilityLabel("上传图片或视频")
                .accessibilityIdentifier("cloud-drive-upload")
                .disabled(model.availability != .available)
            }
            #if DEBUG && targetEnvironment(simulator)
            if ProcessInfo.processInfo.arguments.contains("--cloud-drive-ui-test") {
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
            playbackTask?.cancel()
            saveTask?.cancel()
            closePreview()
            MaiCloudThumbnailCache.shared.clear()
            loadedThumbnailIDs.removeAll()
        }
        .onDisappear {
            playbackTask?.cancel()
            saveTask?.cancel()
        }
        .fullScreenCover(item: $preview) { item in
            if item.isImage {
                ZStack(alignment: .topTrailing) {
                    Color(uiColor: .systemBackground).ignoresSafeArea()
                    RemoteIMQuickLookPreview(filePath: item.url.path)
                        .ignoresSafeArea(edges: .bottom)
                    Button { closePreview() } label: {
                        Image(systemName: "xmark")
                            .font(.system(size: 15, weight: .semibold))
                            .foregroundStyle(.white)
                            .frame(width: 36, height: 36)
                            .background(.black.opacity(0.65), in: Circle())
                    }
                    .padding(20)
                    .accessibilityLabel("关闭图片")
                    .accessibilityIdentifier("cloud-drive-image-close")
                }
            } else {
                MaiFfplayVideoScreen(path: item.url.path, vod: item.vod) {
                    closePreview()
                }
                    .ignoresSafeArea()
                    .statusBarHidden()
            }
        }
        .confirmationDialog("从我的云盘删除这个文件？",
                            isPresented: Binding(get: { deletingID != nil },
                                                 set: { if !$0 { deletingID = nil } })) {
            if let id = deletingID {
                Button("删除文件", role: .destructive) {
                    deletingID = nil
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
                ZStack {
                    MaiCloudThumbnail(accountID: appState.maiChatAccountID,
                                      media: media,
                                      loaded: Binding(
                                        get: { loadedThumbnailIDs.contains(media.id) },
                                        set: { ready in
                                            if ready { loadedThumbnailIDs.insert(media.id) }
                                            else { loadedThumbnailIDs.remove(media.id) }
                                        }))
                    if openingID == media.id || savingID == media.id {
                        RoundedRectangle(cornerRadius: 12)
                            .fill(.black.opacity(0.45))
                        ProgressView().tint(.white)
                    }
                }
                .frame(width: 72, height: 72)
            }
            .disabled(openingID != nil)
            .accessibilityLabel(media.isImage ? "查看图片" : "播放视频")
            .accessibilityValue(loadedThumbnailIDs.contains(media.id)
                ? "封面已加载" : "封面待加载")
            .accessibilityIdentifier("cloud-drive-play-\(media.id)")
            VStack(alignment: .leading, spacing: 5) {
                Text(media.name).font(.subheadline.weight(.semibold))
                    .lineLimit(1).truncationMode(.middle)
                Text(formatBytes(media.sizeBytes))
                    .font(.caption).foregroundStyle(.secondary)
            }
            Spacer(minLength: 0)
            Menu {
                Button(media.isImage ? "查看图片" : "播放视频") { open(media) }
                if !media.isImage {
                    Button("保存到相册", systemImage: "square.and.arrow.down") {
                        saveVideo(media)
                    }
                    .disabled(savingID != nil)
                }
                Button("删除", role: .destructive) { deletingID = media.id }
            } label: {
                Image(systemName: "ellipsis")
                    .frame(width: 44, height: 44)
            }
            .accessibilityIdentifier("cloud-drive-video-menu-\(media.id)")
        }
        .padding(12)
        .background(Color(uiColor: .secondarySystemGroupedBackground),
                    in: RoundedRectangle(cornerRadius: 14))
    }

    private func open(_ media: MaiCloudDriveVideo) {
        guard openingID == nil else { return }
        let expectedAccount = appState.maiChatAccountID
        openingID = media.id
        playbackTask = Task {
            defer { openingID = nil }
            do {
                if media.isImage {
                    let url = try await MaiCloudDriveClient.shared.downloadForPreview(media)
                    guard !Task.isCancelled, expectedAccount == appState.maiChatAccountID else {
                        try? FileManager.default.removeItem(at: url)
                        return
                    }
                    preview = MaiCloudDrivePreview(url: url, isImage: true, vod: nil)
                } else {
                    let vod = try await MaiCloudDriveClient.shared.openVod(
                        videoID: media.id,
                        extensionName: media.contentType == "video/quicktime" ? "mov" : "mp4")
                    guard !Task.isCancelled, expectedAccount == appState.maiChatAccountID else {
                        vod.cancel()
                        return
                    }
                    preview = MaiCloudDrivePreview(
                        url: URL(fileURLWithPath: vod.displayPath), isImage: false, vod: vod)
                }
            } catch {
                if !Task.isCancelled { model.showError(error.localizedDescription) }
            }
        }
    }

    private func closePreview() {
        preview?.vod?.cancel()
        if let preview, preview.isImage {
            try? FileManager.default.removeItem(at: preview.url)
        }
        preview = nil
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
struct MaiCloudDriveUITestRoot: View {
    @StateObject private var appState = RemoteIMAppState(loadedSettings: .empty)
    @State private var loginError = ""
    @State private var accountSwitchResult = ""
    @State private var toolResult = ""
    @State private var cleanupResult = ""
    @State private var canaryLoginReady = false

    var body: some View {
        NavigationStack {
            VStack {
                if !loginError.isEmpty { Text(loginError).foregroundStyle(.red) }
                if canaryLoginReady { Text("云盘灰度账号已登录") }
                if canaryLoginReady { Text(appState.maiChatAccountID) }
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
