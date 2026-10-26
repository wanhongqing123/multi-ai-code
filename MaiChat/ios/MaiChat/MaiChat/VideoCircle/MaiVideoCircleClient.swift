// The portable MaiChat client owns Video Circle HTTP, OSS upload and download.
// This file only supplies the account session, platform TLS/proxy settings and Swift models.
import Foundation

struct MaiVideoCircleFeedPost: Decodable, Identifiable, Sendable {
    let id: String
    let title: String
    let durationMs: Int
    let publishedAt: Int64
    let authorId: String
    let authorName: String
}

struct MaiVideoCircleMinePost: Decodable, Identifiable, Sendable {
    let id: String
    let title: String
    let durationMs: Int
    let status: String
    let createdAt: Int64
    let reviewReason: String?
}

struct MaiVideoCircleFeedPage: Decodable, Sendable {
    let posts: [MaiVideoCircleFeedPost]
    let nextCursor: String?
}

struct MaiVideoCircleMinePage: Decodable, Sendable {
    let posts: [MaiVideoCircleMinePost]
    let nextCursor: String?
}

struct MaiVideoCircleSubmission: Decodable, Sendable {
    let postId: String
    let status: String
}

struct MaiVideoCircleWithdrawal: Decodable, Sendable {
    let postId: String
    let status: String
    let cleanupPending: Bool
}

struct MaiVideoCircleSignedUpload: Decodable, Sendable {
    let uploadUrl: String
    let objectKey: String
}

struct MaiVideoCirclePlaybackLink: Decodable, Sendable {
    let readUrl: String
    let readExpiresIn: Int
}

struct MaiVideoCircleBlockedAuthor: Decodable, Identifiable, Sendable {
    let authorId: String
    let authorName: String
    var id: String { authorId }
}

struct MaiVideoCircleBlocksPage: Decodable, Sendable {
    let authors: [MaiVideoCircleBlockedAuthor]
}

struct MaiVideoCircleCardProvenance: Decodable, Sendable {
    let path: String
    let sourceTool: String
    let sourceTaskId: String
}

struct MaiVideoCircleTransportError: LocalizedError, Sendable {
    let status: Int32
    let httpStatus: Int
    let message: String

    var isFeatureUnavailable: Bool { httpStatus == 404 }
    var errorDescription: String? {
        if status == Int32(MAI_VIDEO_CIRCLE_CANCELED.rawValue) { return "操作已取消" }
        if httpStatus == 401 { return "请重新登录 MaiChat" }
        if httpStatus == 404 { return "视频圈暂未开放，或作品已不可用" }
        if httpStatus == 429 { return "发布过于频繁，请稍后再试" }
        return message.isEmpty ? "视频圈暂时不可用，请稍后重试" : message
    }
}

private enum MaiVideoCircleRequestKind: Sendable {
    case signUpload, publishDraft, feed, mine, playLink, withdraw, report, block
    case blocksList, unblock
    case uploadFile(signedJSON: String, filePath: String, bytes: UInt64)

    var nativeAction: MaiVideoCircleAction {
        switch self {
        case .signUpload: MAI_VIDEO_CIRCLE_SIGN_UPLOAD
        case .publishDraft: MAI_VIDEO_CIRCLE_PUBLISH_DRAFT
        case .feed: MAI_VIDEO_CIRCLE_FEED
        case .mine: MAI_VIDEO_CIRCLE_MINE
        case .playLink: MAI_VIDEO_CIRCLE_PLAY_LINK
        case .withdraw: MAI_VIDEO_CIRCLE_WITHDRAW
        case .report: MAI_VIDEO_CIRCLE_REPORT
        case .block: MAI_VIDEO_CIRCLE_BLOCK
        case .blocksList: MAI_VIDEO_CIRCLE_BLOCKS_LIST
        case .unblock: MAI_VIDEO_CIRCLE_UNBLOCK
        case .uploadFile: MAI_VIDEO_CIRCLE_SIGN_UPLOAD
        }
    }
}

private final class MaiVideoCircleCancelToken: @unchecked Sendable {
    let pointer: OpaquePointer
    init?() {
        guard let pointer = mai_video_circle_cancel_create() else { return nil }
        self.pointer = pointer
    }
    deinit { mai_video_circle_cancel_destroy(pointer) }
    func cancel() { mai_video_circle_cancel_request(pointer) }
}

private final class MaiVideoCircleDownloadCancelToken: @unchecked Sendable {
    let pointer: OpaquePointer
    init?() {
        guard let pointer = mai_download_cancel_create() else { return nil }
        self.pointer = pointer
    }
    deinit { mai_download_cancel_destroy(pointer) }
    func cancel() { mai_download_cancel_request(pointer) }
}

private final class MaiVideoCircleMarkCancelToken: @unchecked Sendable {
    let pointer: OpaquePointer
    init?() {
        guard let pointer = mai_video_circle_mark_cancel_create() else { return nil }
        self.pointer = pointer
    }
    deinit { mai_video_circle_mark_cancel_destroy(pointer) }
    func cancel() { mai_video_circle_mark_cancel_request(pointer) }
}

private struct MaiVideoCircleMarkedCopy: Sendable {
    let url: URL
    let sizeBytes: UInt64
    let durationMs: Int
}

struct MaiVideoCircleClient: Sendable {
    static let shared = MaiVideoCircleClient()

    private func decode<T: Decodable>(_ type: T.Type, from data: Data) throws -> T {
        let decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
        return try decoder.decode(type, from: data)
    }

    static func inspectCompletedCard(outputJSON: String) -> MaiVideoCircleCardProvenance? {
        guard let output = try? JSONSerialization.jsonObject(with: Data(outputJSON.utf8)),
              let cardData = try? JSONSerialization.data(withJSONObject: [
                "tool": "agent_send_media", "state": "completed", "output": output
              ]), let cardJSON = String(data: cardData, encoding: .utf8) else { return nil }
        var result = MaiVideoCircleResult()
        cardJSON.withCString { mai_video_circle_inspect_card($0, &result) }
        defer { mai_video_circle_result_free(&result) }
        guard result.status == MAI_VIDEO_CIRCLE_OK,
              let pointer = result.response_json_utf8, result.response_bytes > 0 else { return nil }
        let data = Data(bytes: pointer, count: result.response_bytes)
        let decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
        return try? decoder.decode(MaiVideoCircleCardProvenance.self, from: data)
    }

    func feed(before: String? = nil) async throws -> MaiVideoCircleFeedPage {
        try decode(MaiVideoCircleFeedPage.self, from: await request(
            .feed, payload: before.map { ["before": $0] } ?? [:]))
    }

    func mine(before: String? = nil) async throws -> MaiVideoCircleMinePage {
        try decode(MaiVideoCircleMinePage.self,
                   from: await request(.mine,
                                       payload: before.map { ["before": $0] } ?? [:]))
    }

    func playLink(postID: String) async throws -> MaiVideoCirclePlaybackLink {
        try decode(MaiVideoCirclePlaybackLink.self,
                           from: await request(.playLink, payload: ["post_id": postID]))
    }

    func withdraw(postID: String) async throws -> MaiVideoCircleWithdrawal {
        try decode(MaiVideoCircleWithdrawal.self,
                   from: await request(.withdraw, payload: ["post_id": postID]))
    }

    func report(postID: String, reason: String) async throws {
        _ = try await request(.report, payload: ["post_id": postID, "reason": reason])
    }

    func block(authorID: String) async throws {
        _ = try await request(.block, payload: ["author_id": authorID])
    }

    func blocks() async throws -> MaiVideoCircleBlocksPage {
        try decode(MaiVideoCircleBlocksPage.self,
                           from: await request(.blocksList, payload: [:]))
    }

    func unblock(authorID: String) async throws {
        _ = try await request(.unblock, payload: ["author_id": authorID])
    }

    func publish(title: String, source: MaiVideoCirclePublishSource) async throws
        -> MaiVideoCircleSubmission {
        let marked = try await prepareMarkedCopy(source: source.fileURL)
        defer { try? FileManager.default.removeItem(at: marked.url) }
        let signedData = try await request(.signUpload, payload: [
            "filename": "video.mp4", "size_bytes": marked.sizeBytes,
            "purpose": "video_circle"
        ])
        let signed = try decode(MaiVideoCircleSignedUpload.self, from: signedData)
        guard let signedJSON = String(data: signedData, encoding: .utf8) else {
            throw MaiVideoCircleTransportError(status: Int32(MAI_VIDEO_CIRCLE_INVALID_RESPONSE.rawValue),
                                               httpStatus: 0, message: "上传凭据无效")
        }
        _ = try await request(.uploadFile(signedJSON: signedJSON,
                                          filePath: marked.url.path,
                                          bytes: marked.sizeBytes),
                              payload: [:])
        let cardOutput = try JSONSerialization.jsonObject(with: Data(source.outputJSON.utf8))
        return try decode(MaiVideoCircleSubmission.self, from: await request(
            .publishDraft, payload: [
                "title": title, "object_key": signed.objectKey,
                "size_bytes": marked.sizeBytes, "duration_ms": marked.durationMs,
                "completed_card": [
                    "tool": "agent_send_media", "state": "completed", "output": cardOutput
                ]
            ]))
    }

    private func prepareMarkedCopy(source: URL) async throws -> MaiVideoCircleMarkedCopy {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("mai-video-circle-marked", isDirectory: true)
        try FileManager.default.createDirectory(at: directory,
                                                withIntermediateDirectories: true)
        let target = directory.appendingPathComponent(UUID().uuidString + ".mp4")
        guard let cancel = MaiVideoCircleMarkCancelToken() else {
            throw MaiVideoCircleTransportError(
                status: Int32(MAI_VIDEO_CIRCLE_MARK_IO_ERROR.rawValue),
                httpStatus: 0, message: "无法准备视频")
        }
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation {
                (continuation: CheckedContinuation<MaiVideoCircleMarkedCopy, Error>) in
                DispatchQueue.global(qos: .utility).async {
                    var result = MaiVideoCircleMarkResult()
                    source.path.withCString { sourcePath in
                        target.path.withCString { targetPath in
                            mai_video_circle_prepare_marked_mp4(
                                sourcePath, targetPath, cancel.pointer, &result)
                        }
                    }
                    guard result.status == MAI_VIDEO_CIRCLE_MARK_OK,
                          result.visible_mark_verified == 1,
                          result.metadata_mark_verified == 1,
                          result.size_bytes > 0, result.duration_ms > 0 else {
                        try? FileManager.default.removeItem(at: target)
                        let message: String
                        switch result.status {
                        case MAI_VIDEO_CIRCLE_MARK_INVALID_SOURCE:
                            message = "视频文件已不可用"
                        case MAI_VIDEO_CIRCLE_MARK_UNSUPPORTED_MP4:
                            message = "需要可播放的 MP4 视频，画面至少 160×64"
                        case MAI_VIDEO_CIRCLE_MARK_SIZE_LIMIT:
                            message = "标记后的视频不能超过 200 MB"
                        case MAI_VIDEO_CIRCLE_MARK_DURATION_LIMIT:
                            message = "视频时长不能超过 60 秒"
                        case MAI_VIDEO_CIRCLE_MARK_CANCELED:
                            message = "操作已取消"
                        default:
                            message = "无法给视频加入 AI 标识，请检查视频格式"
                        }
                        continuation.resume(throwing: MaiVideoCircleTransportError(
                            status: Int32(result.status.rawValue), httpStatus: 0,
                            message: message))
                        return
                    }
                    continuation.resume(returning: MaiVideoCircleMarkedCopy(
                        url: target, sizeBytes: result.size_bytes,
                        durationMs: Int(result.duration_ms)))
                }
            }
        } onCancel: {
            cancel.cancel()
        }
    }

    func downloadForPlayback(postID: String) async throws -> URL {
        let link = try await playLink(postID: postID)
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("mai-video-circle", isDirectory: true)
        try FileManager.default.createDirectory(at: directory,
                                                withIntermediateDirectories: true)
        // A new path per playback prevents a withdrawn video's old cached bytes from
        // becoming a second playback route. The short signed link is fetched each time.
        let destination = directory.appendingPathComponent(UUID().uuidString + ".mp4")
        do {
            try await download(signedURL: link.readUrl, target: destination)
            return destination
        } catch {
            try? FileManager.default.removeItem(at: destination)
            throw error
        }
    }

    private func download(signedURL: String, target: URL) async throws {
        let certificate = try Self.certificate()
        guard let cancel = MaiVideoCircleDownloadCancelToken() else {
            throw MaiVideoCircleTransportError(status: Int32(MAI_DOWNLOAD_NETWORK_ERROR.rawValue),
                                               httpStatus: 0, message: "无法开始下载")
        }
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
                                options.max_bytes = 200_000_000
                                options.ca_path_utf8 = ca
                                options.proxy_resolver = maiChatAttachmentProxyResolver
                                options.proxy_context = Unmanaged.passUnretained(proxy).toOpaque()
                                options.transfer_timeout_ms = 600_000
                                if let pem = certificate.pem {
                                    pem.withUnsafeBytes { bytes in
                                        options.ca_pem = bytes.baseAddress?
                                            .assumingMemoryBound(to: CChar.self)
                                        options.ca_pem_length = bytes.count
                                        options.ca_path_utf8 = nil
                                        mai_download_file(&options, cancel.pointer, &result)
                                    }
                                } else {
                                    mai_download_file(&options, cancel.pointer, &result)
                                }
                            }
                        }
                    }
                    if result.status == MAI_DOWNLOAD_OK { continuation.resume() }
                    else {
                        continuation.resume(throwing: MaiVideoCircleTransportError(
                            status: Int32(result.status.rawValue),
                            httpStatus: Int(result.http_status),
                            message: "视频下载失败，请稍后重试"))
                    }
                }
            }
        } onCancel: {
            cancel.cancel()
        }
    }

    private func request(_ kind: MaiVideoCircleRequestKind,
                         payload: [String: Any]) async throws -> Data {
        guard let access = try await MaiChatAuthClient.shared.videoCircleAccessIfAvailable() else {
            throw MaiVideoCircleTransportError(status: Int32(MAI_VIDEO_CIRCLE_INVALID_ARGUMENT.rawValue),
                                               httpStatus: 0, message: "请先登录 MaiChat")
        }
        let payloadData = try JSONSerialization.data(withJSONObject: payload)
        guard let payloadJSON = String(data: payloadData, encoding: .utf8),
              let cancel = MaiVideoCircleCancelToken() else {
            throw MaiVideoCircleTransportError(status: Int32(MAI_VIDEO_CIRCLE_INVALID_ARGUMENT.rawValue),
                                               httpStatus: 0, message: "请求无法开始")
        }
        let certificate = try Self.certificate()
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Data, Error>) in
                DispatchQueue.global(qos: .utility).async {
                    let response = Self.perform(kind, access: access, certificate: certificate,
                                                payloadJSON: payloadJSON, cancel: cancel)
                    continuation.resume(with: response)
                }
            }
        } onCancel: {
            cancel.cancel()
        }
    }

    private static func perform(_ kind: MaiVideoCircleRequestKind,
                                access: MaiChatVideoCircleAccess,
                                certificate: (path: String, pem: Data?),
                                payloadJSON: String,
                                cancel: MaiVideoCircleCancelToken) -> Result<Data, Error> {
        let proxy = MaiChatAttachmentProxyContext()
        let signedHost = access.authBaseURL.contains("localhost") ||
            access.authBaseURL.contains("127.0.0.1") ? "localhost" : "aliyuncs.com"
        var native = MaiVideoCircleResult()
        access.authBaseURL.withCString { auth in
            access.mediaBaseURL.withCString { media in
                access.accountToken.withCString { token in
                    certificate.path.withCString { ca in
                        signedHost.withCString { host in
                            payloadJSON.withCString { json in
                                var options = MaiVideoCircleOptions()
                                options.auth_base_url_utf8 = auth
                                options.media_base_url_utf8 = media
                                options.account_token_utf8 = token
                                options.ca_path_utf8 = ca
                                options.proxy_resolver = maiChatAttachmentProxyResolver
                                options.proxy_context = Unmanaged.passUnretained(proxy).toOpaque()
                                options.signed_upload_host_suffix_utf8 = host
                                let invoke = {
                                    if case let .uploadFile(signedJSON, filePath, bytes) = kind {
                                        signedJSON.withCString { signed in
                                            filePath.withCString { file in
                                                mai_video_circle_upload_file(
                                                    &options, signed, file, bytes,
                                                    cancel.pointer, &native)
                                            }
                                        }
                                    } else {
                                        mai_video_circle_request(&options, kind.nativeAction,
                                                                 json, cancel.pointer, &native)
                                    }
                                }
                                if let pem = certificate.pem {
                                    pem.withUnsafeBytes { buffer in
                                        options.ca_pem = buffer.baseAddress?.assumingMemoryBound(to: CChar.self)
                                        options.ca_pem_length = buffer.count
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
        let data: Data
        if let bytes = native.response_json_utf8, native.response_bytes > 0 {
            data = Data(bytes: bytes, count: native.response_bytes)
        } else { data = Data("{}".utf8) }
        let status = Int32(native.status.rawValue)
        let httpStatus = Int(native.http_status)
        mai_video_circle_result_free(&native)
        guard status == Int32(MAI_VIDEO_CIRCLE_OK.rawValue) else {
            let errorObject = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
            let message = errorObject?["error"] as? String ?? ""
            return .failure(MaiVideoCircleTransportError(status: status,
                                                         httpStatus: httpStatus, message: message))
        }
        return .success(data)
    }

    private static func certificate() throws -> (path: String, pem: Data?) {
        #if DEBUG && targetEnvironment(simulator)
        if let encoded = ProcessInfo.processInfo.environment["MAICHAT_LOCAL_VIDEO_CIRCLE_CA_BASE64"],
           let pem = Data(base64Encoded: encoded), !pem.isEmpty { return ("", pem) }
        #endif
        guard let certificate = Bundle.main.url(forResource: "cacert", withExtension: "pem",
                                                subdirectory: "MaiAgentCertificates") else {
            throw MaiVideoCircleTransportError(status: Int32(MAI_VIDEO_CIRCLE_NETWORK_ERROR.rawValue),
                                               httpStatus: 0, message: "服务证书不可用")
        }
        return (certificate.path, nil)
    }
}
