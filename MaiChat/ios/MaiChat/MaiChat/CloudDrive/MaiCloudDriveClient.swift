// Private cloud-drive transport. The portable C++ core owns HTTP, multipart
// upload, checkpoints and file download; this file supplies iOS account access.
import Foundation
import CryptoKit

struct MaiCloudDriveVideo: Decodable, Identifiable, Sendable {
    let id: String
    let name: String
    let sizeBytes: UInt64
    let contentType: String
    let mediaType: String?
    let createdAt: Int64
    let folderId: String?

    var isImage: Bool { mediaType == "image" || contentType.hasPrefix("image/") }
}

struct MaiCloudDriveFolder: Decodable, Sendable {
    let id: String
    let name: String
    let parentId: String?
    let createdAt: Int64
    let updatedAt: Int64
}

private struct MaiCloudDriveFolders: Decodable {
    let folders: [MaiCloudDriveFolder]
}

private struct MaiCloudDriveReadyMedia: Decodable {
    let videoId: String
    let status: String
    let name: String
    let sizeBytes: UInt64
    let contentType: String
    let mediaType: String
    let folderId: String?
    let createdAt: Int64

    var media: MaiCloudDriveVideo {
        MaiCloudDriveVideo(id: videoId, name: name, sizeBytes: sizeBytes,
            contentType: contentType, mediaType: mediaType, createdAt: createdAt,
            folderId: folderId)
    }
}

struct MaiCloudDriveQuota: Decodable, Sendable {
    let usedBytes: UInt64
    let reservedBytes: UInt64
    let limitBytes: UInt64
}

struct MaiCloudDrivePage: Decodable, Sendable {
    let videos: [MaiCloudDriveVideo]
    let nextCursor: String?
    let quota: MaiCloudDriveQuota
}

private struct MaiCloudDrivePlayLink: Decodable, Sendable {
    let readUrl: String
    let sizeBytes: UInt64?
    let contentType: String?
    let mediaType: String?
}

enum MaiCloudDriveAgentPreview {
    case image(URL)
    case video(MaiCloudVodSession)
}

private struct MaiCloudDriveThumbnailLink: Decodable, Sendable {
    let thumbnailUrl: String
}

private final class MaiCloudVodCancelToken: @unchecked Sendable {
    let pointer: OpaquePointer
    private let lock = NSLock()
    private var canceled = false
    init?() {
        guard let pointer = mai_cloud_vod_cancel_create() else { return nil }
        self.pointer = pointer
    }
    deinit { mai_cloud_vod_cancel_destroy(pointer) }
    var isCanceled: Bool {
        lock.lock()
        defer { lock.unlock() }
        return canceled
    }
    func cancel() {
        lock.lock()
        canceled = true
        lock.unlock()
        mai_cloud_vod_cancel_request(pointer)
    }
}

/// A read-only cloud video source. Only bounded chunks live in the shared C++ cache.
/// Keep this object alive until the FFplay worker has stopped reading it.
final class MaiCloudVodSession: @unchecked Sendable {
    let pointer: OpaquePointer
    let displayPath: String
    private let vodCancel: MaiCloudVodCancelToken
    private let accountCancel: MaiCloudDriveCancelToken
    private let proxyContext: MaiChatAttachmentProxyContext

    fileprivate init(pointer: OpaquePointer, extensionName: String,
                     vodCancel: MaiCloudVodCancelToken,
                     accountCancel: MaiCloudDriveCancelToken,
                     proxyContext: MaiChatAttachmentProxyContext) {
        self.pointer = pointer
        displayPath = "maicloud-vod-\(UUID().uuidString).\(extensionName)"
        self.vodCancel = vodCancel
        self.accountCancel = accountCancel
        self.proxyContext = proxyContext
    }

    func cancel() {
        mai_cloud_vod_cancel(pointer)
        vodCancel.cancel()
        accountCancel.cancel()
    }

    deinit {
        mai_cloud_vod_close(pointer)
    }
}

struct MaiCloudDriveError: LocalizedError, Sendable {
    let status: Int32
    let httpStatus: Int
    let detail: String

    var errorDescription: String? {
        if status == Int32(MAI_CLOUD_DRIVE_CANCELED.rawValue) { return "已暂停，可稍后继续上传" }
        if status == Int32(MAI_CLOUD_DRIVE_CHECKPOINT_MISMATCH.rawValue) ||
            status == Int32(MAI_CLOUD_DRIVE_PART_MISMATCH.rawValue) {
            return "源文件或已上传分片发生变化，请保留文件并重新选择"
        }
        if status == Int32(MAI_CLOUD_DRIVE_INSUFFICIENT_SPACE.rawValue) {
            return "设备空间不足，无法准备此文件"
        }
        if httpStatus == 401 { return "登录已过期，请重新登录 MaiChat" }
        if httpStatus == 413 { return "云盘空间不足，或文件超过大小限制" }
        if httpStatus == 404 { return "文件已删除，或云盘暂未开放" }
        if !detail.isEmpty { return detail }
        return "云盘暂时不可用，请稍后重试"
    }
}

final class MaiCloudDriveCancelToken: @unchecked Sendable {
    let pointer: OpaquePointer
    init?() {
        guard let pointer = mai_cloud_drive_cancel_create() else { return nil }
        self.pointer = pointer
    }
    deinit { mai_cloud_drive_cancel_destroy(pointer) }
    func cancel() { mai_cloud_drive_cancel_request(pointer) }
}

private final class MaiCloudDriveDownloadCancelToken: @unchecked Sendable {
    let pointer: OpaquePointer
    init?() {
        guard let pointer = mai_download_cancel_create() else { return nil }
        self.pointer = pointer
    }
    deinit { mai_download_cancel_destroy(pointer) }
    func cancel() { mai_download_cancel_request(pointer) }
}

private final class MaiCloudDriveReadContext {
    let file: FileHandle
    let url: URL
    let scoped: Bool

    init(url: URL) throws {
        self.url = url
        scoped = url.startAccessingSecurityScopedResource()
        do { file = try FileHandle(forReadingFrom: url) }
        catch {
            if scoped { url.stopAccessingSecurityScopedResource() }
            throw error
        }
    }

    deinit {
        try? file.close()
        if scoped { url.stopAccessingSecurityScopedResource() }
    }

    func read(at offset: UInt64, output: UnsafeMutablePointer<UInt8>, count: Int) -> Int32 {
        do {
            try file.seek(toOffset: offset)
            var filled = 0
            while filled < count {
                guard let data = try file.read(upToCount: count - filled), !data.isEmpty
                else { return 0 }
                data.withUnsafeBytes { bytes in
                    guard let base = bytes.baseAddress else { return }
                    output.advanced(by: filled).withMemoryRebound(to: UInt8.self,
                                                                   capacity: data.count) {
                        $0.update(from: base.assumingMemoryBound(to: UInt8.self),
                                  count: data.count)
                    }
                }
                filled += data.count
            }
            return 1
        } catch { return 0 }
    }
}

private let maiCloudDriveReadAt: MaiCloudDriveReadAt = { context, offset, output, count in
    guard let context, let output else { return 0 }
    return Unmanaged<MaiCloudDriveReadContext>.fromOpaque(context)
        .takeUnretainedValue().read(at: offset, output: output, count: count)
}

private final class MaiCloudDriveProgressContext {
    let callback: @Sendable (UInt64, UInt64) -> Void
    init(_ callback: @escaping @Sendable (UInt64, UInt64) -> Void) { self.callback = callback }
}

private let maiCloudDriveProgress: MaiCloudDriveProgress = { uploaded, total, context in
    guard let context else { return 0 }
    Unmanaged<MaiCloudDriveProgressContext>.fromOpaque(context)
        .takeUnretainedValue().callback(uploaded, total)
    return 0
}

struct MaiCloudDriveClient: Sendable {
    static let shared = MaiCloudDriveClient()

    struct SessionRoom: Sendable {
        let ownerSlot: String
        let ownerLoginSessionID: String
        let sessionID: String

        var key: String { "\(ownerSlot):\(sessionID)" }
    }

    private struct SessionAttachmentLink: Decodable {
        let readUrl: String
        let sizeBytes: UInt64
        let contentType: String
        let mediaType: String
        let name: String
        let roomKey: String
    }

    struct SessionAttachmentReceipt: Decodable, Sendable {
        let attachmentId: String
        let roomKey: String
        let name: String
        let sizeBytes: UInt64
        let contentType: String
        let mediaType: String
    }

    struct DownloadedSessionAttachment: Sendable {
        let fileURL: URL
        let name: String
        let contentType: String
        let mediaType: String
    }

    func uploadSessionAttachment(fileURL: URL, displayName: String,
                                 room: SessionRoom)
        async throws -> SessionAttachmentReceipt {
        let name = displayName
        var info = MaiCloudDriveMediaInfo()
        guard name.withCString({ mai_session_attachment_media_info($0, &info) != 0 }),
              let size = try fileURL.resourceValues(forKeys: [.fileSizeKey]).fileSize,
              size > 0, UInt64(size) <= info.max_size_bytes,
              let cancel = MaiCloudDriveCancelToken() else {
            throw MaiCloudDriveError(status: 0, httpStatus: 0,
                                     detail: "会话附件格式或大小无效")
        }
        let key = SHA256.hash(data: Data((room.key + ":" + fileURL.path).utf8))
            .map { String(format: "%02x", $0) }.joined()
        let directory = FileManager.default.urls(for: .applicationSupportDirectory,
                                                 in: .userDomainMask)[0]
            .appendingPathComponent("MaiSessionAttachmentUploads", isDirectory: true)
        try FileManager.default.createDirectory(at: directory,
                                                withIntermediateDirectories: true)
        let checkpoint = directory.appendingPathComponent(key + ".maicloud.json")
        let task = MaiCloudDriveLocalTask(id: key, accountID: "", name: name,
            contentType: String(cString: info.content_type_utf8),
            sizeBytes: UInt64(size), sourceID: fileURL.path,
            sourcePath: fileURL.path, bookmark: nil, ownsSource: false,
            createdAt: Date())
        let data = try await upload(source: task, checkpointURL: checkpoint,
                                    cancel: cancel, room: room, progress: { _, _ in })
        let receipt = try decode(SessionAttachmentReceipt.self, from: data)
        guard receipt.roomKey == room.key,
              receipt.name == name,
              receipt.contentType == task.contentType,
              receipt.mediaType == String(cString: info.media_type_utf8),
              receipt.sizeBytes == task.sizeBytes else {
            throw MaiCloudDriveError(status: 0, httpStatus: 0,
                                     detail: "会话附件上传回执不匹配")
        }
        return receipt
    }

    private static func contentInfo(_ contentType: String)
        -> (extensionName: String, isImage: Bool, maxBytes: UInt64)? {
        switch contentType {
        case "video/mp4": return ("mp4", false, 2_000_000_000)
        case "video/quicktime": return ("mov", false, 2_000_000_000)
        case "image/jpeg": return ("jpg", true, 20_000_000)
        case "image/png": return ("png", true, 20_000_000)
        case "image/webp": return ("webp", true, 20_000_000)
        case "image/heic": return ("heic", true, 20_000_000)
        default: return nil
        }
    }

    static func discardStalePreviews() {
        let files = FileManager.default
        for folderName in ["mai-cloud-drive-playback", "mai-cloud-drive-thumbnails"] {
            let folder = files.temporaryDirectory.appendingPathComponent(
                folderName, isDirectory: true)
            guard let entries = try? files.contentsOfDirectory(at: folder,
                includingPropertiesForKeys: [.isRegularFileKey]) else { continue }
            for entry in entries {
                guard UUID(uuidString: entry.deletingPathExtension().lastPathComponent) != nil,
                      (try? entry.resourceValues(forKeys: [.isRegularFileKey]))?.isRegularFile
                        == true else { continue }
                try? files.removeItem(at: entry)
            }
        }
    }

    private func decode<T: Decodable>(_ type: T.Type, from data: Data) throws -> T {
        let decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
        return try decoder.decode(type, from: data)
    }

    // A nil folder preserves the account-wide media view. An empty folder ID
    // explicitly selects the root; a UUID selects one folder's direct media.
    func list(before: String? = nil, folderID: String? = nil) async throws -> MaiCloudDrivePage {
        var payload = before.map { ["before": $0] } ?? [:]
        if let folderID { payload["folder_id"] = folderID }
        return try decode(MaiCloudDrivePage.self,
                          from: await request(action: MAI_CLOUD_DRIVE_LIST_VIDEOS,
                                              payload: payload))
    }

    func listFolders() async throws -> [MaiCloudDriveFolder] {
        try decode(MaiCloudDriveFolders.self,
                   from: await request(action: MAI_CLOUD_DRIVE_LIST_FOLDERS,
                                       payload: [:])).folders
    }

    func createFolder(name: String, parentID: String? = nil)
        async throws -> MaiCloudDriveFolder {
        var payload = ["name": name]
        if let parentID { payload["parent_id"] = parentID }
        return try decode(MaiCloudDriveFolder.self,
                          from: await request(action: MAI_CLOUD_DRIVE_CREATE_FOLDER,
                                              payload: payload))
    }

    // nil leaves the parent unchanged; an empty string moves to the root.
    func updateFolder(id: String, name: String? = nil, parentID: String? = nil)
        async throws -> MaiCloudDriveFolder {
        var payload = ["folder_id": id]
        if let name { payload["name"] = name }
        if let parentID { payload["parent_id"] = parentID }
        return try decode(MaiCloudDriveFolder.self,
                          from: await request(action: MAI_CLOUD_DRIVE_UPDATE_FOLDER,
                                              payload: payload))
    }

    func deleteFolder(id: String) async throws {
        _ = try await request(action: MAI_CLOUD_DRIVE_DELETE_FOLDER,
                              payload: ["folder_id": id])
    }

    func moveMedia(id: String, folderID: String) async throws {
        _ = try await request(action: MAI_CLOUD_DRIVE_MOVE_MEDIA,
                              payload: ["media_id": id, "folder_id": folderID])
    }

    func renameMedia(id: String, name: String) async throws -> MaiCloudDriveVideo {
        try decode(MaiCloudDriveReadyMedia.self,
            from: await request(action: MAI_CLOUD_DRIVE_RENAME_MEDIA,
                                payload: ["media_id": id, "name": name])).media
    }

    func delete(videoID: String) async throws {
        _ = try await request(action: MAI_CLOUD_DRIVE_DELETE_VIDEO,
                              payload: ["video_id": videoID])
    }

    func abort(checkpointURL: URL) async throws {
        guard FileManager.default.fileExists(atPath: checkpointURL.path) else { return }
        guard let access = try await MaiChatAuthClient.shared.cloudDriveAccessIfAvailable(),
              let cancel = MaiCloudDriveCancelToken() else {
            throw MaiChatAuthError.sessionExpired
        }
        let certificate = try Self.certificate()
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation {
                (continuation: CheckedContinuation<Void, Error>) in
                DispatchQueue.global(qos: .utility).async {
                    let proxy = MaiChatAttachmentProxyContext()
                    var native = MaiCloudDriveResult()
                    access.authBaseURL.withCString { auth in
                        access.accountToken.withCString { token in
                            certificate.path.withCString { ca in
                                checkpointURL.path.withCString { checkpoint in
                                    var options = MaiCloudDriveOptions()
                                    options.auth_base_url_utf8 = auth
                                    options.account_token_utf8 = token
                                    options.ca_path_utf8 = ca
                                    options.proxy_resolver = maiChatAttachmentProxyResolver
                                    options.proxy_context = Unmanaged.passUnretained(proxy).toOpaque()
                                    // 账号控制请求使用健康探测选出的线路；OSS 仍独立选路。
                                    options.first_party_route_selector = maiChatSelectFirstPartyRoute
                                    options.first_party_route_invalidator = maiChatInvalidateFirstPartyRoute
                                    let invoke = { mai_cloud_drive_abort_resumable(
                                        &options, checkpoint, cancel.pointer, &native) }
                                    if let pem = certificate.pem {
                                        pem.withUnsafeBytes { bytes in
                                            options.ca_pem = bytes.baseAddress?
                                                .assumingMemoryBound(to: CChar.self)
                                            options.ca_pem_length = bytes.count
                                            options.ca_path_utf8 = nil
                                            invoke()
                                        }
                                    } else { invoke() }
                                }
                            }
                        }
                    }
                    let data = native.response_json_utf8.map {
                        Data(bytes: $0, count: native.response_bytes)
                    } ?? Data("{}".utf8)
                    let error = Self.nativeError(native, response: data)
                    mai_cloud_drive_result_free(&native)
                    if let error { continuation.resume(throwing: error) }
                    else { continuation.resume() }
                }
            }
        } onCancel: { cancel.cancel() }
    }

    func playLink(videoID: String) async throws -> String {
        try decode(MaiCloudDrivePlayLink.self,
                   from: await request(action: MAI_CLOUD_DRIVE_PLAY_LINK,
                                       payload: ["video_id": videoID])).readUrl
    }

    func downloadSessionAttachment(id: String, room: SessionRoom)
        async throws -> DownloadedSessionAttachment {
        let link = try decode(SessionAttachmentLink.self,
            from: await request(action: MAI_CLOUD_DRIVE_PLAY_LINK,
                                payload: ["video_id": id], room: room))
        var info = MaiCloudDriveMediaInfo()
        guard link.roomKey == room.key,
              link.sizeBytes > 0,
              link.name.withCString({ mai_session_attachment_media_info($0, &info) != 0 }),
              link.contentType == String(cString: info.content_type_utf8),
              link.mediaType == String(cString: info.media_type_utf8),
              link.sizeBytes <= info.max_size_bytes else {
            throw MaiCloudDriveError(status: 0, httpStatus: 0,
                                     detail: "会话附件归属或文件信息无效")
        }
        let url = try await download(signedURL: link.readUrl,
                                  fileExtension: URL(fileURLWithPath: link.name).pathExtension,
                                  maxBytes: link.sizeBytes,
                                  folderName: "mai-session-attachments",
                                  timeoutMilliseconds: 2 * 60 * 60 * 1000)
        guard let actual = try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize,
              actual > 0, UInt64(actual) == link.sizeBytes else {
            try? FileManager.default.removeItem(at: url)
            throw MaiCloudDriveError(status: 0, httpStatus: 0,
                                     detail: "会话附件下载长度不匹配")
        }
        return DownloadedSessionAttachment(fileURL: url, name: link.name,
            contentType: link.contentType, mediaType: link.mediaType)
    }

    func cleanupSessionRoom(_ room: SessionRoom) async throws {
        _ = try await request(action: MAI_SESSION_ATTACHMENT_CLEANUP_ROOM,
                              payload: [:], room: room)
    }

    func openVod(videoID: String,
                 extensionName: String = "mp4") async throws -> MaiCloudVodSession {
        guard let access = try await MaiChatAuthClient.shared.cloudDriveAccessIfAvailable(),
              let vodCancel = MaiCloudVodCancelToken(),
              let accountCancel = MaiCloudDriveCancelToken() else {
            throw MaiChatAuthError.sessionExpired
        }
        let certificate = try Self.certificate()
        let proxy = MaiChatAttachmentProxyContext()
        let signedHost = access.authBaseURL.contains("localhost") ||
            access.authBaseURL.contains("127.0.0.1") ? "localhost" : "aliyuncs.com"
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation {
                (continuation: CheckedContinuation<MaiCloudVodSession, Error>) in
                DispatchQueue.global(qos: .utility).async {
                    var result = MaiCloudVodResult()
                    let source = access.authBaseURL.withCString { auth in
                        access.accountToken.withCString { token in
                            certificate.path.withCString { ca in
                                signedHost.withCString { host in
                                    videoID.withCString { id in
                                        var options = MaiCloudDriveOptions()
                                        options.auth_base_url_utf8 = auth
                                        options.account_token_utf8 = token
                                        options.ca_path_utf8 = ca
                                        options.signed_upload_host_suffix_utf8 = host
                                        options.proxy_resolver = maiChatAttachmentProxyResolver
                                        options.proxy_context = Unmanaged.passUnretained(proxy).toOpaque()
                                        // 播放签名刷新仍是账号请求，沿用登录的备用线路。
                                        options.first_party_route_selector = maiChatSelectFirstPartyRoute
                                        options.first_party_route_invalidator = maiChatInvalidateFirstPartyRoute
                                        if let pem = certificate.pem {
                                            return pem.withUnsafeBytes { bytes in
                                                options.ca_pem = bytes.baseAddress?
                                                    .assumingMemoryBound(to: CChar.self)
                                                options.ca_pem_length = bytes.count
                                                options.ca_path_utf8 = nil
                                                return mai_cloud_vod_open_cloud_drive(
                                                    &options, id, accountCancel.pointer,
                                                    vodCancel.pointer, &result)
                                            }
                                        }
                                        return mai_cloud_vod_open_cloud_drive(
                                            &options, id, accountCancel.pointer,
                                            vodCancel.pointer, &result)
                                    }
                                }
                            }
                        }
                    }
                    guard let source else {
                        continuation.resume(throwing: MaiCloudDriveError(
                            status: Int32(result.status.rawValue),
                            httpStatus: Int(result.http_status),
                            detail: "云盘视频点播失败，请重试"))
                        return
                    }
                    if vodCancel.isCanceled {
                        mai_cloud_vod_close(source)
                        continuation.resume(throwing: CancellationError())
                        return
                    }
                    continuation.resume(returning: MaiCloudVodSession(
                        pointer: source, extensionName: extensionName,
                        vodCancel: vodCancel, accountCancel: accountCancel,
                        proxyContext: proxy))
                }
            }
        } onCancel: {
            accountCancel.cancel()
            vodCancel.cancel()
        }
    }

    func thumbnailData(videoID: String) async throws -> Data {
        let link = try decode(MaiCloudDriveThumbnailLink.self,
                              from: await request(action: MAI_CLOUD_DRIVE_THUMBNAIL_LINK,
                                                  payload: ["video_id": videoID]))
        let file = try await download(signedURL: link.thumbnailUrl,
                                      fileExtension: "jpg", maxBytes: 2_000_000,
                                      folderName: "mai-cloud-drive-thumbnails",
                                      timeoutMilliseconds: 60_000)
        defer { try? FileManager.default.removeItem(at: file) }
        return try Data(contentsOf: file)
    }

    func downloadForPreview(_ media: MaiCloudDriveVideo) async throws -> URL {
        let signedURL = try await playLink(videoID: media.id)
        guard let info = Self.contentInfo(media.contentType),
              media.isImage, info.isImage else {
            throw MaiCloudDriveError(status: 0, httpStatus: 0,
                                     detail: "云盘文件格式无效")
        }
        return try await download(signedURL: signedURL,
                                  fileExtension: info.extensionName,
                                  maxBytes: info.maxBytes,
                                  folderName: "mai-cloud-drive-playback",
                                  timeoutMilliseconds: 600_000)
    }

    // A full download is allowed only for an explicit save request. Point-of-demand
    // playback always uses openVod and never calls this method.
    func downloadForExplicitSave(_ media: MaiCloudDriveVideo) async throws -> URL {
        guard !media.isImage, let info = Self.contentInfo(media.contentType),
              !info.isImage, media.sizeBytes > 0,
              media.sizeBytes <= info.maxBytes else {
            throw MaiCloudDriveError(status: 0, httpStatus: 0,
                                     detail: "云盘视频信息无效")
        }
        let signedURL = try await playLink(videoID: media.id)
        return try await download(signedURL: signedURL,
                                  fileExtension: info.extensionName,
                                  maxBytes: info.maxBytes,
                                  folderName: "mai-cloud-drive-explicit-saves",
                                  timeoutMilliseconds: 2 * 60 * 60 * 1000)
    }

    func openForAgentPreview(mediaID: String) async throws -> MaiCloudDriveAgentPreview {
        let link = try decode(MaiCloudDrivePlayLink.self,
                              from: await request(action: MAI_CLOUD_DRIVE_PLAY_LINK,
                                                  payload: ["video_id": mediaID]))
        guard let contentType = link.contentType,
              let info = Self.contentInfo(contentType),
              let size = link.sizeBytes, size > 0, size <= info.maxBytes,
              link.mediaType == (info.isImage ? "image" : "video") else {
            throw MaiCloudDriveError(status: 0, httpStatus: 0,
                                     detail: "云盘文件格式或大小无效")
        }
        if !info.isImage {
            return .video(try await openVod(videoID: mediaID,
                                            extensionName: info.extensionName))
        }
        let url = try await download(signedURL: link.readUrl,
                                     fileExtension: info.extensionName,
                                     maxBytes: info.maxBytes,
                                     folderName: "mai-cloud-drive-agent-playback",
                                     timeoutMilliseconds: 600_000)
        return .image(url)
    }

    private func download(signedURL: String, fileExtension: String,
                          maxBytes: UInt64, folderName: String,
                          timeoutMilliseconds: Int32) async throws -> URL {
        let folder = FileManager.default.temporaryDirectory
            .appendingPathComponent(folderName, isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let target = folder.appendingPathComponent(UUID().uuidString)
            .appendingPathExtension(fileExtension)
        let certificate = try Self.certificate()
        guard let cancel = MaiCloudDriveDownloadCancelToken() else {
            throw Self.error(status: MAI_CLOUD_DRIVE_NETWORK_ERROR)
        }
        do {
            try await withTaskCancellationHandler {
                try await withCheckedThrowingContinuation {
                    (continuation: CheckedContinuation<Void, Error>) in
                    DispatchQueue.global(qos: .utility).async {
                        let proxy = MaiChatAttachmentProxyContext()
                        var result = MaiDownloadResult()
                        signedURL.withCString { url in
                            target.path.withCString { path in
                                certificate.path.withCString { ca in
                                    var options = MaiDownloadOptions()
                                    options.url_utf8 = url
                                    options.target_path_utf8 = path
                                    options.max_bytes = maxBytes
                                    options.ca_path_utf8 = ca
                                    options.proxy_resolver = maiChatAttachmentProxyResolver
                                    options.proxy_context = Unmanaged.passUnretained(proxy).toOpaque()
                                    options.transfer_timeout_ms = timeoutMilliseconds
                                    if let pem = certificate.pem {
                                        pem.withUnsafeBytes { bytes in
                                            options.ca_pem = bytes.baseAddress?
                                                .assumingMemoryBound(to: CChar.self)
                                            options.ca_pem_length = bytes.count
                                            options.ca_path_utf8 = nil
                                            mai_download_file(&options, cancel.pointer, &result)
                                        }
                                    } else { mai_download_file(&options, cancel.pointer, &result) }
                                }
                            }
                        }
                        if result.status == MAI_DOWNLOAD_OK { continuation.resume() }
                        else { continuation.resume(throwing: MaiCloudDriveError(
                            status: Int32(result.status.rawValue),
                            httpStatus: Int(result.http_status), detail: "云盘文件下载失败")) }
                    }
                }
            } onCancel: { cancel.cancel() }
            return target
        } catch {
            try? FileManager.default.removeItem(at: target)
            throw error
        }
    }

    // The stable ID is recorded by the C++ checkpoint. The host record owns
    // its bookmark or private file path and can reopen it after process death.
    func upload(source: MaiCloudDriveLocalTask,
                checkpointURL: URL,
                cancel: MaiCloudDriveCancelToken,
                room: SessionRoom? = nil,
                progress: @escaping @Sendable (UInt64, UInt64) -> Void) async throws -> Data {
        guard let access = try await MaiChatAuthClient.shared.cloudDriveAccessIfAvailable()
        else { throw MaiChatAuthError.sessionExpired }
        let certificate = try Self.certificate()
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation {
                (continuation: CheckedContinuation<Data, Error>) in
                DispatchQueue.global(qos: .utility).async {
                    let response = Self.performUpload(source: source,
                                                      checkpoint: checkpointURL,
                                                      access: access,
                                                      certificate: certificate,
                                                      cancel: cancel,
                                                      room: room,
                                                      progress: progress)
                    continuation.resume(with: response)
                }
            }
        } onCancel: { cancel.cancel() }
    }

    private static func performUpload(source: MaiCloudDriveLocalTask,
                                      checkpoint: URL,
                                      access: MaiChatCloudDriveAccess,
                                      certificate: (path: String, pem: Data?),
                                      cancel: MaiCloudDriveCancelToken,
                                      room: SessionRoom?,
                                      progress: @escaping @Sendable (UInt64, UInt64) -> Void)
        -> Result<Data, Error> {
        let proxy = MaiChatAttachmentProxyContext()
        let progressContext = MaiCloudDriveProgressContext(progress)
        let signedHost = access.authBaseURL.contains("localhost") ||
            access.authBaseURL.contains("127.0.0.1") ? "localhost" : "aliyuncs.com"
        let opened: MaiCloudDriveReadContext?
        if let bookmark = source.bookmark {
            do {
                var stale = false
                let url = try URL(resolvingBookmarkData: bookmark, options: [],
                                  relativeTo: nil, bookmarkDataIsStale: &stale)
                opened = try MaiCloudDriveReadContext(url: url)
            } catch {
                return .failure(MaiCloudDriveError(
                    status: Int32(MAI_CLOUD_DRIVE_FILE_ERROR.rawValue), httpStatus: 0,
                    detail: "源视频已无法访问，请重新选择该文件"))
            }
        } else { opened = nil }
        var native = MaiCloudDriveResult()
        access.authBaseURL.withCString { auth in
            access.accountToken.withCString { token in
                certificate.path.withCString { ca in
                    signedHost.withCString { host in
                        checkpoint.path.withCString { checkpointPath in
                            source.name.withCString { name in
                                source.contentType.withCString { contentType in
                                    var options = MaiCloudDriveOptions()
                                    options.auth_base_url_utf8 = auth
                                    options.account_token_utf8 = token
                                    options.ca_path_utf8 = ca
                                    options.proxy_resolver = maiChatAttachmentProxyResolver
                                    options.proxy_context = Unmanaged.passUnretained(proxy).toOpaque()
                                    // 上传会交替访问账号接口和 OSS；核心仅对账号接口使用此线路。
                                    options.first_party_route_selector = maiChatSelectFirstPartyRoute
                                    options.first_party_route_invalidator = maiChatInvalidateFirstPartyRoute
                                    options.signed_upload_host_suffix_utf8 = host
                                    options.retain_completed_checkpoint = 1
                                    let invokeUpload = {
                                        if let opened {
                                            source.sourceID.withCString { stableID in
                                                var sourceInfo = MaiCloudDriveSource()
                                                sourceInfo.stable_id_utf8 = stableID
                                                sourceInfo.size_bytes = source.sizeBytes
                                                sourceInfo.read_at = maiCloudDriveReadAt
                                                sourceInfo.context = Unmanaged
                                                    .passUnretained(opened).toOpaque()
                                                mai_cloud_drive_upload_resumable_source(
                                                    &options, &sourceInfo, checkpointPath,
                                                    name, contentType, cancel.pointer,
                                                    maiCloudDriveProgress,
                                                    Unmanaged.passUnretained(progressContext)
                                                        .toOpaque(), &native)
                                            }
                                        } else if let path = source.sourcePath {
                                            path.withCString { filePath in
                                                mai_cloud_drive_upload_resumable(
                                                    &options, filePath, checkpointPath,
                                                    name, contentType, cancel.pointer,
                                                    maiCloudDriveProgress,
                                                    Unmanaged.passUnretained(progressContext)
                                                        .toOpaque(), &native)
                                            }
                                        }
                                    }
                                    let invoke = {
                                        if let room {
                                            room.ownerSlot.withCString { slot in
                                                room.ownerLoginSessionID.withCString { login in
                                                    room.sessionID.withCString { session in
                                                        options.owner_device_slot_utf8 = slot
                                                        options.owner_login_session_id_utf8 = login
                                                        options.session_id_utf8 = session
                                                        invokeUpload()
                                                    }
                                                }
                                            }
                                        } else { invokeUpload() }
                                    }
                                    if let pem = certificate.pem {
                                        pem.withUnsafeBytes { bytes in
                                            options.ca_pem = bytes.baseAddress?
                                                .assumingMemoryBound(to: CChar.self)
                                            options.ca_pem_length = bytes.count
                                            options.ca_path_utf8 = nil
                                            invoke()
                                        }
                                    } else { invoke() }
                                }
                            }
                        }
                    }
                }
            }
        }
        let response = native.response_json_utf8.map { Data(bytes: $0, count: native.response_bytes) }
            ?? Data("{}".utf8)
        let error = Self.nativeError(native, response: response)
        mai_cloud_drive_result_free(&native)
        if let error { return .failure(error) }
        return .success(response)
    }

    private func request(action: MaiCloudDriveAction,
                         payload: [String: String],
                         room: SessionRoom? = nil) async throws -> Data {
        guard let access = try await MaiChatAuthClient.shared.cloudDriveAccessIfAvailable()
        else { throw MaiChatAuthError.sessionExpired }
        let certificate = try Self.certificate()
        let payload = try JSONSerialization.data(withJSONObject: payload)
        guard let json = String(data: payload, encoding: .utf8),
              let cancel = MaiCloudDriveCancelToken() else {
            throw Self.error(status: MAI_CLOUD_DRIVE_INVALID_ARGUMENT)
        }
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation {
                (continuation: CheckedContinuation<Data, Error>) in
                DispatchQueue.global(qos: .utility).async {
                    continuation.resume(with: Self.performRequest(
                        action: action, payload: json, access: access,
                        certificate: certificate, cancel: cancel, room: room))
                }
            }
        } onCancel: { cancel.cancel() }
    }

    private static func performRequest(action: MaiCloudDriveAction, payload: String,
                                       access: MaiChatCloudDriveAccess,
                                       certificate: (path: String, pem: Data?),
                                       cancel: MaiCloudDriveCancelToken,
                                       room: SessionRoom?)
        -> Result<Data, Error> {
        let proxy = MaiChatAttachmentProxyContext()
        let signedHost = access.authBaseURL.contains("localhost") ||
            access.authBaseURL.contains("127.0.0.1") ? "localhost" : "aliyuncs.com"
        var native = MaiCloudDriveResult()
        access.authBaseURL.withCString { auth in
            access.accountToken.withCString { token in
                certificate.path.withCString { ca in
                    signedHost.withCString { host in
                        payload.withCString { json in
                            var options = MaiCloudDriveOptions()
                            options.auth_base_url_utf8 = auth
                            options.account_token_utf8 = token
                            options.ca_path_utf8 = ca
                            options.signed_upload_host_suffix_utf8 = host
                            options.proxy_resolver = maiChatAttachmentProxyResolver
                            options.proxy_context = Unmanaged.passUnretained(proxy).toOpaque()
                            // 文件操作只在探测成功后发送一次，失败留给下一次重新选路。
                            options.first_party_route_selector = maiChatSelectFirstPartyRoute
                            options.first_party_route_invalidator = maiChatInvalidateFirstPartyRoute
                            let invokeRequest = { mai_cloud_drive_request(
                                &options, action, json, cancel.pointer, &native) }
                            let invoke = {
                                if let room {
                                    room.ownerSlot.withCString { slot in
                                        room.ownerLoginSessionID.withCString { login in
                                            room.sessionID.withCString { session in
                                                options.owner_device_slot_utf8 = slot
                                                options.owner_login_session_id_utf8 = login
                                                options.session_id_utf8 = session
                                                invokeRequest()
                                            }
                                        }
                                    }
                                } else { invokeRequest() }
                            }
                            if let pem = certificate.pem {
                                pem.withUnsafeBytes { bytes in
                                    options.ca_pem = bytes.baseAddress?
                                        .assumingMemoryBound(to: CChar.self)
                                    options.ca_pem_length = bytes.count
                                    options.ca_path_utf8 = nil
                                    invoke()
                                }
                            } else { invoke() }
                        }
                    }
                }
            }
        }
        let response = native.response_json_utf8.map { Data(bytes: $0, count: native.response_bytes) }
            ?? Data("{}".utf8)
        let error = nativeError(native, response: response)
        mai_cloud_drive_result_free(&native)
        if let error { return .failure(error) }
        return .success(response)
    }

    private static func nativeError(_ result: MaiCloudDriveResult, response: Data) -> Error? {
        guard result.status != MAI_CLOUD_DRIVE_OK else { return nil }
        let body = (try? JSONSerialization.jsonObject(with: response)) as? [String: Any]
        return MaiCloudDriveError(status: Int32(result.status.rawValue),
                                  httpStatus: Int(result.http_status),
                                  detail: body?["error"] as? String ?? "")
    }

    private static func error(status: MaiCloudDriveStatus) -> MaiCloudDriveError {
        MaiCloudDriveError(status: Int32(status.rawValue), httpStatus: 0, detail: "")
    }

    private static func certificate() throws -> (path: String, pem: Data?) {
        #if DEBUG && targetEnvironment(simulator)
        if let encoded = ProcessInfo.processInfo.environment["MAICHAT_LOCAL_CLOUD_DRIVE_CA_BASE64"],
           let pem = Data(base64Encoded: encoded), !pem.isEmpty { return ("", pem) }
        #endif
        guard let certificate = Bundle.main.url(forResource: "cacert", withExtension: "pem",
                                                subdirectory: "MaiAgentCertificates") else {
            throw MaiCloudDriveError(status: Int32(MAI_CLOUD_DRIVE_NETWORK_ERROR.rawValue),
                                     httpStatus: 0, detail: "服务证书不可用")
        }
        return (certificate.path, nil)
    }
}
