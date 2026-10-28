// The current account owns its own durable upload records. A paused transfer
// never loses its C++ multipart checkpoint, source bookmark, or original file.
import CryptoKit
import Foundation
import SwiftUI

struct MaiCloudDriveLocalTask: Codable, Identifiable, Sendable {
    let id: String
    let accountID: String
    let name: String
    let contentType: String
    let sizeBytes: UInt64
    let sourceID: String
    let sourcePath: String?
    let bookmark: Data?
    let ownsSource: Bool
    let createdAt: Date
}

struct MaiCloudDriveUploadProgress: Sendable {
    let uploaded: UInt64
    let total: UInt64
}

@MainActor final class MaiCloudDriveModel: ObservableObject {
    static let shared = MaiCloudDriveModel()

    enum Availability: Equatable { case unknown, available, disabled }
    @Published private(set) var availability: Availability = .unknown

    @Published private(set) var videos: [MaiCloudDriveVideo] = []
    @Published private(set) var folders: [MaiCloudDriveFolder] = []
    @Published private(set) var currentFolderID: String?
    @Published private(set) var folderPath: [MaiCloudDriveFolder] = []
    @Published private(set) var quota = MaiCloudDriveQuota(
        usedBytes: 0, reservedBytes: 0, limitBytes: 10_000_000_000)
    @Published private(set) var localTasks: [MaiCloudDriveLocalTask] = []
    @Published private(set) var progress: [String: MaiCloudDriveUploadProgress] = [:]
    @Published private(set) var errors: [String: String] = [:]
    @Published private(set) var loading = false
    @Published private(set) var loadingMore = false
    @Published private(set) var listError = ""
    @Published private(set) var nextCursor: String?
    @Published private(set) var notice = ""

    private var accountID = ""
    private var transfers: [String: MaiCloudDriveCancelToken] = [:]
    private var aborting = Set<String>()
    private var generation = 0
    private var browseGeneration = 0
    private var visitedCursors = Set<String>()
    private var probingGeneration: Int?
    private let photoImportCleanupCutoff = Date()

    private init() {}

    func activate(accountID newID: String) {
        guard accountID != newID else { return }
        transfers.values.forEach { $0.cancel() }
        transfers.removeAll()
        MaiCloudDriveClient.discardStalePreviews()
        generation &+= 1
        browseGeneration &+= 1
        accountID = newID
        availability = .unknown
        videos = []
        folders = []
        currentFolderID = nil
        folderPath = []
        loading = false
        loadingMore = false
        probingGeneration = nil
        visitedCursors = []
        quota = MaiCloudDriveQuota(usedBytes: 0, reservedBytes: 0,
                                   limitBytes: 10_000_000_000)
        progress = [:]
        errors = [:]
        listError = ""
        nextCursor = nil
        notice = ""
        cleanupOwnedFiles()
        localTasks = loadLocalTasks()
    }

    func probe() async {
        guard !accountID.isEmpty, probingGeneration != generation else { return }
        if availability == .available { return }
        let expected = generation
        let browse = browseGeneration
        let folder = currentFolderID
        probingGeneration = expected
        defer { if probingGeneration == expected { probingGeneration = nil } }
        do {
            let page = try await MaiCloudDriveClient.shared.list(folderID: folder ?? "")
            guard expected == generation, browse == browseGeneration, !Task.isCancelled else { return }
            availability = .available
            videos = page.videos.filter { $0.folderId == folder }
            quota = page.quota
            nextCursor = page.nextCursor
        } catch let error as MaiCloudDriveError
            where error.httpStatus == 404 || error.httpStatus == 503 {
            if expected == generation && browse == browseGeneration && !Task.isCancelled {
                if folder == nil { availability = .disabled }
            }
        } catch {
            if expected == generation && browse == browseGeneration && !Task.isCancelled {
                availability = .unknown
            }
        }
    }

    func reload() async {
        guard !accountID.isEmpty, !loading else { return }
        browseGeneration &+= 1
        let expected = generation
        let browse = browseGeneration
        let folder = currentFolderID
        loading = true
        loadingMore = false
        visitedCursors = []
        listError = ""
        defer { if expected == generation && browse == browseGeneration { loading = false } }
        do {
            async let folderRequest = MaiCloudDriveClient.shared.listFolders()
            async let mediaRequest = MaiCloudDriveClient.shared.list(folderID: folder ?? "")
            let (allFolders, page) = try await (folderRequest, mediaRequest)
            guard expected == generation, browse == browseGeneration, !Task.isCancelled else { return }
            folders = allFolders
            if let folder, !allFolders.contains(where: { $0.id == folder }) {
                videos = []
                nextCursor = nil
                availability = .available
                listError = "文件夹已不存在，请返回上级或根目录"
                return
            }
            folderPath = path(to: folder, in: allFolders)
            videos = page.videos.filter { $0.folderId == folder }
            availability = .available
            quota = page.quota
            nextCursor = page.nextCursor
        } catch {
            if expected == generation && browse == browseGeneration && !Task.isCancelled {
                if folder != nil, let mediaError = error as? MaiCloudDriveError,
                   mediaError.httpStatus == 404 {
                    videos = []
                    nextCursor = nil
                    listError = "文件夹已不存在，请返回上级或根目录"
                } else { listError = error.localizedDescription }
                if let error = error as? MaiCloudDriveError,
                   folder == nil && (error.httpStatus == 404 || error.httpStatus == 503) {
                    availability = .disabled
                }
            }
        }
    }

    func loadMore() async {
        guard let cursor = nextCursor, !loading, !loadingMore else { return }
        let expected = generation
        let browse = browseGeneration
        let folder = currentFolderID
        loadingMore = true
        defer { if expected == generation && browse == browseGeneration { loadingMore = false } }
        do {
            let page = try await MaiCloudDriveClient.shared.list(before: cursor, folderID: folder ?? "")
            guard expected == generation, browse == browseGeneration, !Task.isCancelled else { return }
            videos += page.videos.filter { item in
                item.folderId == folder && !videos.contains { $0.id == item.id }
            }
            if let next = page.nextCursor, next == cursor || visitedCursors.contains(next) {
                listError = "云盘分页未前进，请重试"
                return
            }
            visitedCursors.insert(cursor)
            nextCursor = page.nextCursor
            quota = page.quota
        } catch {
            if expected == generation && browse == browseGeneration && !Task.isCancelled {
                listError = error.localizedDescription
            }
        }
    }

    var childFolders: [MaiCloudDriveFolder] { folders.filter { $0.parentId == currentFolderID } }

    func browse(folderID: String?) async {
        guard folderID == nil || folders.contains(where: { $0.id == folderID }) else {
            listError = "文件夹已不存在，请刷新云盘"
            return
        }
        if currentFolderID != folderID {
            browseGeneration &+= 1
            currentFolderID = folderID
            folderPath = path(to: folderID, in: folders)
            videos = []
            nextCursor = nil
            visitedCursors = []
            loading = false
            loadingMore = false
            listError = ""
            notice = ""
        }
        await reload()
    }

    func browseParent() async {
        let parent = folderPath.last?.parentId
        await browse(folderID: folders.contains(where: { $0.id == parent }) ? parent : nil)
    }

    private func path(to folderID: String?, in folders: [MaiCloudDriveFolder]) -> [MaiCloudDriveFolder] {
        var result: [MaiCloudDriveFolder] = []
        var cursor = folderID
        var seen = Set<String>()
        while let id = cursor, seen.insert(id).inserted,
              let folder = folders.first(where: { $0.id == id }) {
            result.insert(folder, at: 0)
            cursor = folder.parentId
        }
        return result
    }

    func addFile(_ file: URL, expectedAccountID: String) async {
        guard !accountID.isEmpty, expectedAccountID == accountID else {
            notice = "账号已切换，请重新选择文件"
            return
        }
        do {
            let task = try prepareTask(file: file, allowBookmark: true)
            try save(task)
            localTasks.insert(task, at: 0)
            upload(task)
        } catch { notice = error.localizedDescription }
    }

    @discardableResult
    func addAgentVideo(_ file: URL, expectedAccountID: String) async -> String? {
        guard !accountID.isEmpty, expectedAccountID == accountID else {
            notice = "账号已切换，请重新选择文件"
            return nil
        }
        do {
            let task = try prepareTask(file: file, allowBookmark: false)
            try save(task)
            localTasks.insert(task, at: 0)
            if upload(task) { return task.id }
            removeRecord(task)
            localTasks.removeAll { $0.id == task.id }
            notice = "无法开始上传，请稍后再试"
            return nil
        } catch { notice = error.localizedDescription; return nil }
    }

    func addStagedPhoto(_ file: URL, displayName: String,
                        expectedAccountID: String) async {
        guard !accountID.isEmpty, expectedAccountID == accountID else {
            let stagedDirectory = FileManager.default.urls(
                for: .applicationSupportDirectory, in: .userDomainMask)[0]
                .appendingPathComponent("MaiCloudDrivePhotoImports", isDirectory: true)
            if file.deletingLastPathComponent() == stagedDirectory {
                try? FileManager.default.removeItem(at: file)
            }
            notice = "账号已切换，请重新选择文件"
            return
        }
        do {
            try FileManager.default.createDirectory(at: directory(),
                                                    withIntermediateDirectories: true)
            let target = directory().appendingPathComponent(UUID().uuidString)
                .appendingPathExtension(file.pathExtension)
            try FileManager.default.moveItem(at: file, to: target)
            do {
                let task = try prepareTask(file: target, allowBookmark: false,
                                           displayName: displayName,
                                           ownsSource: true)
                try save(task)
                localTasks.insert(task, at: 0)
                upload(task)
            } catch {
                try? FileManager.default.removeItem(at: target)
                throw error
            }
        } catch { notice = error.localizedDescription }
    }

    private func prepareTask(file: URL, allowBookmark: Bool,
                             displayName: String? = nil,
                             ownsSource: Bool = false) throws -> MaiCloudDriveLocalTask {
        let name = displayName ?? file.lastPathComponent
        var info = MaiCloudDriveMediaInfo()
        guard name.withCString({ mai_cloud_drive_media_info($0, &info) }) != 0,
              let mime = info.content_type_utf8.map({ String(cString: $0) }),
              let kind = info.media_type_utf8.map({ String(cString: $0) }) else {
            throw MaiCloudDriveError(status: 0, httpStatus: 0,
                                     detail: "请选择 MP4、MOV、JPEG、PNG、WebP 或 HEIC 文件")
        }
        let ext = file.pathExtension.lowercased()
        guard name.lowercased().hasSuffix("." + ext) else {
            throw MaiCloudDriveError(status: 0, httpStatus: 0,
                                     detail: "文件名与内容格式不匹配")
        }
        let hasScope = file.startAccessingSecurityScopedResource()
        defer { if hasScope { file.stopAccessingSecurityScopedResource() } }
        let size = try file.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
        guard size > 0, UInt64(size) <= info.max_size_bytes else {
            throw MaiCloudDriveError(status: 0, httpStatus: 0,
                                     detail: kind == "image"
                                        ? "图片必须在 20 MB 以内且不能是空文件"
                                        : "视频必须在 2 GB 以内且不能是空文件")
        }
        let id = UUID().uuidString.lowercased()
        guard !allowBookmark || hasScope ||
                file.path.hasPrefix(NSHomeDirectory() + "/")
        else {
            throw MaiCloudDriveError(status: 0, httpStatus: 0,
                                     detail: "文件访问权无法保存，请先将文件存入本机文件")
        }
        let bookmark: Data?
        if allowBookmark && hasScope {
            bookmark = try file.bookmarkData(options: .minimalBookmark,
                                             includingResourceValuesForKeys: nil,
                                             relativeTo: nil)
            var stale = false
            let resolved = try URL(resolvingBookmarkData: bookmark!, options: [],
                                   relativeTo: nil, bookmarkDataIsStale: &stale)
            let reopened = resolved.startAccessingSecurityScopedResource()
            defer { if reopened { resolved.stopAccessingSecurityScopedResource() } }
            guard reopened, let handle = try? FileHandle(forReadingFrom: resolved) else {
                throw MaiCloudDriveError(status: 0, httpStatus: 0,
                                         detail: "文件授权不能持久保存，请将文件存入本机文件后重试")
            }
            try? handle.close()
        } else { bookmark = nil }
        return MaiCloudDriveLocalTask(
            id: id, accountID: accountID, name: name,
            contentType: mime,
            sizeBytes: UInt64(size), sourceID: bookmark == nil ? file.path : "bookmark:" + id,
            sourcePath: bookmark == nil ? file.path : nil, bookmark: bookmark,
            ownsSource: ownsSource,
            createdAt: Date())
    }

    @discardableResult
    func upload(_ local: MaiCloudDriveLocalTask) -> Bool {
        guard local.accountID == accountID, transfers[local.id] == nil,
              !aborting.contains(local.id),
              let operation = MaiCloudDriveCancelToken() else { return false }
        let expected = generation
        transfers[local.id] = operation
        progress[local.id] = MaiCloudDriveUploadProgress(uploaded: 0,
                                                         total: local.sizeBytes)
        errors[local.id] = nil
        let checkpoint = checkpointURL(for: local)
        Task {
            do {
                _ = try await MaiCloudDriveClient.shared.upload(
                    source: local, checkpointURL: checkpoint,
                    cancel: operation) { [weak self] uploaded, total in
                        Task { @MainActor [weak self] in
                            guard let self, self.generation == expected,
                                  self.transfers[local.id] != nil else { return }
                            self.progress[local.id] = MaiCloudDriveUploadProgress(
                                uploaded: uploaded, total: total)
                        }
                    }
                guard expected == generation else { return }
                if !finishCompletedTask(local) {
                    errors[local.id] = "文件已上传，本地缓存尚未清理；重新打开云盘会继续清理"
                } else {
                    localTasks.removeAll { $0.id == local.id }
                    notice = currentFolderID == nil ? "已保存到我的云盘" :
                        "已上传到根目录，可以让 AI 移入当前文件夹"
                }
                await reload()
            } catch {
                guard expected == generation else { return }
                if !(error is CancellationError) &&
                    (error as? MaiCloudDriveError)?.status !=
                    Int32(MAI_CLOUD_DRIVE_CANCELED.rawValue) {
                    errors[local.id] = error.localizedDescription
                }
            }
            if expected == generation {
                transfers[local.id] = nil
                progress[local.id] = nil
            }
        }
        return true
    }

    func pause(taskID: String) { transfers[taskID]?.cancel() }
    func isUploading(_ id: String) -> Bool { transfers[id] != nil }

    enum AbortResult { case notFound, active, aborted }

    func abortPausedTask(taskID: String) async throws -> AbortResult {
        guard let task = localTasks.first(where: { $0.id == taskID && $0.accountID == accountID })
        else { return .notFound }
        guard transfers[taskID] == nil, !aborting.contains(taskID) else { return .active }
        // A completed checkpoint represents a saved file, even if removing
        // its local record failed. Never abort the server's completed file.
        guard completedCheckpoint(for: task) == nil else { return .notFound }
        let expected = generation
        let folder = directory()
        let checkpoint = checkpointURL(for: task)
        let record = recordURL(for: task)
        aborting.insert(taskID)
        defer { aborting.remove(taskID) }
        try await MaiCloudDriveClient.shared.abort(checkpointURL: checkpoint)

        // Only sources staged by this account's app may be removed. Files
        // selected from Files or an Agent workspace remain the user's files.
        if task.ownsSource {
            guard let path = task.sourcePath else {
                throw MaiCloudDriveError(status: Int32(MAI_CLOUD_DRIVE_FILE_ERROR.rawValue),
                                         httpStatus: 0, detail: "上传源文件记录无效")
            }
            let source = URL(fileURLWithPath: path).standardizedFileURL
            guard source.deletingLastPathComponent() == folder.standardizedFileURL,
                  UUID(uuidString: source.deletingPathExtension().lastPathComponent) != nil,
                  isSupportedMediaName(source.lastPathComponent) else {
                throw MaiCloudDriveError(status: Int32(MAI_CLOUD_DRIVE_FILE_ERROR.rawValue),
                                         httpStatus: 0, detail: "上传源文件不属于当前账号")
            }
            if FileManager.default.fileExists(atPath: source.path) {
                try FileManager.default.removeItem(at: source)
            }
        }
        if FileManager.default.fileExists(atPath: record.path) {
            try FileManager.default.removeItem(at: record)
        }
        if expected == generation {
            localTasks.removeAll { $0.id == taskID }
            progress[taskID] = nil
            errors[taskID] = nil
        }
        return .aborted
    }

    func delete(videoID: String) async {
        do {
            try await deleteForAgent(videoID: videoID)
            notice = "已从云盘删除"
        } catch { listError = error.localizedDescription }
    }

    func deleteForAgent(videoID: String) async throws {
        try await MaiCloudDriveClient.shared.delete(videoID: videoID)
        videos.removeAll { $0.id == videoID }
        await reload()
    }

    func showError(_ message: String) { listError = message }
    func showNotice(_ message: String) { notice = message }

    func discardLocalTask(_ task: MaiCloudDriveLocalTask) {
        guard transfers[task.id] == nil, !aborting.contains(task.id) else { return }
        if completedCheckpoint(for: task) != nil {
            if finishCompletedTask(task) {
                localTasks.removeAll { $0.id == task.id }
            } else {
                errors[task.id] = "文件已上传，本地缓存尚未清理；重新打开云盘会继续清理"
            }
            return
        }
        // The server upload session is abandoned by the shared client before
        // its checkpoint may be removed; a transient error keeps this record.
        let expected = generation
        Task {
            do {
                _ = try await abortPausedTask(taskID: task.id)
            } catch {
                if expected == generation { errors[task.id] = error.localizedDescription }
            }
        }
    }

    private func directory() -> URL {
        let hash = SHA256.hash(data: Data(accountID.utf8))
            .map { String(format: "%02x", $0) }.joined()
        return FileManager.default.urls(for: .applicationSupportDirectory,
                                        in: .userDomainMask)[0]
            .appendingPathComponent("MaiCloudDrive", isDirectory: true)
            .appendingPathComponent(hash, isDirectory: true)
    }

    private func checkpointURL(for task: MaiCloudDriveLocalTask) -> URL {
        directory().appendingPathComponent(task.id + ".maicloud.json")
    }

    private func recordURL(for task: MaiCloudDriveLocalTask) -> URL {
        directory().appendingPathComponent(task.id + ".task.json")
    }

    private func save(_ task: MaiCloudDriveLocalTask) throws {
        try FileManager.default.createDirectory(at: directory(),
                                                withIntermediateDirectories: true)
        let data = try JSONEncoder().encode(task)
        try data.write(to: recordURL(for: task), options: .atomic)
    }

    private func removeRecord(_ task: MaiCloudDriveLocalTask) {
        try? FileManager.default.removeItem(at: recordURL(for: task))
    }

    private func completedCheckpoint(for task: MaiCloudDriveLocalTask) -> [String: Any]? {
        guard let data = try? Data(contentsOf: checkpointURL(for: task)),
              let body = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              body["completed"] as? Bool == true,
              body["source_id"] as? String == task.sourceID,
              body["name"] as? String == task.name,
              body["file_size"] as? UInt64 == task.sizeBytes,
              let videoID = body["video_id"] as? String,
              UUID(uuidString: videoID) != nil else { return nil }
        return body
    }

    @discardableResult private func finishCompletedTask(_ task: MaiCloudDriveLocalTask) -> Bool {
        guard completedCheckpoint(for: task) != nil else { return false }
        let files = FileManager.default
        if task.ownsSource {
            guard let path = task.sourcePath,
                  URL(fileURLWithPath: path).standardizedFileURL
                    .deletingLastPathComponent() == directory().standardizedFileURL else {
                return false
            }
            do {
                if files.fileExists(atPath: path) {
                    try files.removeItem(atPath: path)
                }
            } catch { return false }
        }
        do {
            if files.fileExists(atPath: recordURL(for: task).path) {
                try files.removeItem(at: recordURL(for: task))
            }
            try files.removeItem(at: checkpointURL(for: task))
            return true
        } catch { return false }
    }

    private func cleanupOwnedFiles() {
        let files = FileManager.default
        let accountDirectory = directory()
        guard var entries = try? files.contentsOfDirectory(at: accountDirectory,
            includingPropertiesForKeys: [.isRegularFileKey]) else {
            cleanupPhotoImports()
            return
        }
        // A bad record/checkpoint may be the only surviving pointer to a
        // staged video. Preserve everything in that case.
        var tasks: [MaiCloudDriveLocalTask] = []
        var referenced = Set<String>()
        for entry in entries where entry.lastPathComponent.hasSuffix(".task.json") {
            guard let data = try? Data(contentsOf: entry),
                  let task = try? JSONDecoder().decode(MaiCloudDriveLocalTask.self, from: data),
                  task.accountID == accountID else {
                cleanupPhotoImports()
                return
            }
            tasks.append(task)
            if let path = task.sourcePath {
                referenced.insert(URL(fileURLWithPath: path).standardizedFileURL.path)
            }
        }
        for entry in entries where entry.lastPathComponent.hasSuffix(".maicloud.json") {
            guard let data = try? Data(contentsOf: entry),
                  let body = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let source = body["source_id"] as? String else {
                cleanupPhotoImports()
                return
            }
            if source.hasPrefix("/") {
                referenced.insert(URL(fileURLWithPath: source).standardizedFileURL.path)
            }
        }
        for task in tasks where completedCheckpoint(for: task) != nil {
            _ = finishCompletedTask(task)
        }
        entries = (try? files.contentsOfDirectory(at: accountDirectory,
            includingPropertiesForKeys: [.isRegularFileKey])) ?? []
        for entry in entries where entry.lastPathComponent.hasSuffix(".maicloud.json") {
            let taskID = String(entry.lastPathComponent.dropLast(".maicloud.json".count))
            let record = accountDirectory.appendingPathComponent(taskID + ".task.json")
            guard !files.fileExists(atPath: record.path),
                  let data = try? Data(contentsOf: entry),
                  let body = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  body["completed"] as? Bool == true else { continue }
            if let source = body["source_id"] as? String {
                let sourceURL = URL(fileURLWithPath: source).standardizedFileURL
                if sourceURL.deletingLastPathComponent() == accountDirectory.standardizedFileURL,
                   UUID(uuidString: sourceURL.deletingPathExtension().lastPathComponent) != nil,
                   isSupportedMediaName(sourceURL.lastPathComponent),
                   files.fileExists(atPath: sourceURL.path) {
                    do { try files.removeItem(at: sourceURL) }
                    catch { continue }
                }
                referenced.remove(sourceURL.path)
            }
            try? files.removeItem(at: entry)
        }
        // Source names in this directory are app-created UUIDs; never touch
        // a selected Files URL or any path outside this account directory.
        for entry in entries {
            guard isSupportedMediaName(entry.lastPathComponent),
                  UUID(uuidString: entry.deletingPathExtension().lastPathComponent) != nil,
                  entry.deletingLastPathComponent().standardizedFileURL ==
                    accountDirectory.standardizedFileURL,
                  (try? entry.resourceValues(forKeys: [.isRegularFileKey]))?.isRegularFile == true,
                  !referenced.contains(entry.standardizedFileURL.path) else { continue }
            try? files.removeItem(at: entry)
        }
        cleanupPhotoImports()
    }

    private func cleanupPhotoImports() {
        let folder = FileManager.default.urls(for: .applicationSupportDirectory,
            in: .userDomainMask)[0].appendingPathComponent(
                "MaiCloudDrivePhotoImports", isDirectory: true)
        guard let entries = try? FileManager.default.contentsOfDirectory(at: folder,
            includingPropertiesForKeys: [.contentModificationDateKey, .isRegularFileKey]) else {
            return
        }
        for entry in entries {
            let name = entry.lastPathComponent
            let partial = name.hasSuffix(".maicloud-part")
            let candidate = partial ? String(name.dropLast(".maicloud-part".count)) : name
            let complete = isSupportedMediaName(candidate)
            let stem = URL(fileURLWithPath: candidate).deletingPathExtension()
                .lastPathComponent
            guard complete, UUID(uuidString: stem) != nil,
                  let attributes = try? entry.resourceValues(forKeys: [
                    .contentModificationDateKey, .isRegularFileKey]),
                  attributes.isRegularFile == true,
                  let modified = attributes.contentModificationDate,
                  modified < photoImportCleanupCutoff else { continue }
            try? FileManager.default.removeItem(at: entry)
        }
    }

    private func loadLocalTasks() -> [MaiCloudDriveLocalTask] {
        let files = (try? FileManager.default.contentsOfDirectory(
            at: directory(), includingPropertiesForKeys: nil)) ?? []
        return files.filter { $0.lastPathComponent.hasSuffix(".task.json") }
            .compactMap { try? JSONDecoder().decode(MaiCloudDriveLocalTask.self,
                                                    from: Data(contentsOf: $0)) }
            .filter { $0.accountID == accountID }
            .sorted { $0.createdAt > $1.createdAt }
    }

    private func isSupportedMediaName(_ name: String) -> Bool {
        var info = MaiCloudDriveMediaInfo()
        return name.withCString { mai_cloud_drive_media_info($0, &info) != 0 }
    }

    #if targetEnvironment(simulator)
    func localDirectoryForUITests() -> URL { directory() }
    #endif
}
