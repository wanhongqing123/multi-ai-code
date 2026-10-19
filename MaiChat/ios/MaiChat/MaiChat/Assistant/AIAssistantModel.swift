// 文件作用：连接 SwiftUI 创作中心与共享 C++ MaiAgent，维护会话、消息、任务及工作区状态。
// 会话切换先读取目标最近一页，成功后一次性替换可见状态；失败时保留原会话和消息。
// 轮询只合并已持久化的新消息，旧页按需从数据库取回，避免刷新时清空当前画面。
// 云端密钥仅驻留内存；消息持久化和工具调用由 MaiMobileAgent 适配器及核心库完成。
import Foundation
import Darwin
import CoreImage
import CryptoKit
import ImageIO
import MaiChatCore
import Photos
import SwiftUI
import UIKit
import UniformTypeIdentifiers
import Vision

func logAIHistoryEvent(_ message: @autoclosure () -> String) {
    #if DEBUG
    NSLog("[AIHistory] %@", message())
    #endif
}

struct AIModelSettings: Codable, Sendable, Equatable {
    static let glmResponsesUrl = "https://open.bigmodel.cn/api/v1"
    static let deepSeekUrl = "https://api.deepseek.com"

    var baseUrl = AIModelSettings.glmResponsesUrl
    var model = "glm-5.3"
    var policy = "on-request"
    var effectiveWire: String { "responses" }

    mutating func selectModel(_ selected: String) {
        model = selected
        if selected == "deepseek-flash" { baseUrl = Self.deepSeekUrl }
        else if selected.hasPrefix("glm-") { baseUrl = Self.glmResponsesUrl }
    }

}

enum AIAssistantWorkspaceLocator {
    private static func baseDirectory() throws -> URL {
        let support = try FileManager.default.url(
            for: .applicationSupportDirectory, in: .userDomainMask,
            appropriateFor: nil, create: true
        )
        let base = support.appendingPathComponent("AIAssistant", isDirectory: true)
        try FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        return base
    }

    static func claimLegacyWorkspace(for userID: String) throws {
        guard !userID.isEmpty else { return }
        let base = try baseDirectory()
        let marker = base.appendingPathComponent("legacy-owner-user-id.txt")
        guard !FileManager.default.fileExists(atPath: marker.path),
              FileManager.default.fileExists(
                atPath: base.appendingPathComponent("sessions.sqlite").path
              ) else { return }
        // 旧版本的 AI 数据库没有账号目录；切换前把它归属给当前账号，不移动付费任务文件。
        try Data(userID.utf8).write(to: marker, options: .atomic)
    }

    static func directory(for userID: String) throws -> URL {
        let base = try baseDirectory()
        if userID.isEmpty {
            let guest = base.appendingPathComponent("Guest", isDirectory: true)
            try FileManager.default.createDirectory(at: guest, withIntermediateDirectories: true)
            return guest
        }
        let marker = base.appendingPathComponent("legacy-owner-user-id.txt")
        if let owner = try? String(contentsOf: marker, encoding: .utf8), owner == userID {
            return base
        }
        let digest = SHA256.hash(data: Data(userID.utf8))
            .map { String(format: "%02x", $0) }.joined()
        let account = base.appendingPathComponent("Accounts", isDirectory: true)
            .appendingPathComponent(digest, isDirectory: true)
        try FileManager.default.createDirectory(at: account, withIntermediateDirectories: true)
        return account
    }
}

final class AICloudCredentialStore: @unchecked Sendable {
    static let shared = AICloudCredentialStore()
    private let lock = NSLock()
    private var values: [String: String] = [:]
    private var arkExpiresAt = Date.distantPast
    private var bailianExpiresAt = Date.distantPast
    private var bailianWorkspaceId = ""

    func replaceAccountModelKeys(_ next: [String: String]) {
        lock.lock()
        for provider in ["glm", "deepseek", "minimax"] {
            if let key = next[provider], !key.isEmpty { values[provider] = key }
            else { values.removeValue(forKey: provider) }
        }
        lock.unlock()
    }

    func clearAll() {
        lock.lock()
        values.removeAll()
        arkExpiresAt = .distantPast
        bailianExpiresAt = .distantPast
        bailianWorkspaceId = ""
        lock.unlock()
    }

    func setTemporaryArk(_ key: String, expiresAt: Date) {
        lock.lock()
        values["ark"] = key
        arkExpiresAt = expiresAt
        lock.unlock()
    }

    func clearTemporaryArk() {
        lock.lock()
        values.removeValue(forKey: "ark")
        arkExpiresAt = .distantPast
        lock.unlock()
    }

    func setTemporaryBailian(_ key: String, workspaceId: String, expiresAt: Date) {
        lock.lock()
        values["wan"] = key
        bailianWorkspaceId = workspaceId
        bailianExpiresAt = expiresAt
        lock.unlock()
    }

    func clearTemporaryBailian() {
        lock.lock()
        values.removeValue(forKey: "wan")
        bailianWorkspaceId = ""
        bailianExpiresAt = .distantPast
        lock.unlock()
    }

    func bailianNeedsRefresh() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return Date().addingTimeInterval(90) >= bailianExpiresAt
    }

    func workspaceId() -> String {
        lock.lock()
        defer { lock.unlock() }
        return Date() < bailianExpiresAt ? bailianWorkspaceId : ""
    }

    func arkNeedsRefresh() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return Date().addingTimeInterval(90) >= arkExpiresAt
    }

    func apiKey(_ provider: String) -> String {
        lock.lock()
        defer { lock.unlock() }
        if provider == "ark", Date() >= arkExpiresAt { return "" }
        if provider == "wan", Date() >= bailianExpiresAt { return "" }
        return values[provider] ?? ""
    }
}

enum AICloudCredentialSync {
    @MainActor private static var arkRefreshTask: Task<Void, Error>?
    @MainActor private static var arkRetryAfter = Date.distantPast
    @MainActor private static var bailianRefreshTask: Task<Void, Error>?
    @MainActor private static var bailianRetryAfter = Date.distantPast

    @MainActor static func invalidateAccountModelCredentials() {
        arkRefreshTask?.cancel()
        arkRefreshTask = nil
        arkRetryAfter = .distantPast
        bailianRefreshTask?.cancel()
        bailianRefreshTask = nil
        bailianRetryAfter = .distantPast
        AICloudCredentialStore.shared.clearAll()
    }

    static func refreshAccountModelKeysIfAvailable() async throws -> Bool {
        guard let issued = try await MaiChatAuthClient.shared.accountModelKeysIfAvailable()
        else { return false }
        var valid: [String: String] = [:]
        for provider in ["glm", "deepseek", "minimax"] {
            guard let key = issued.apiKeys[provider], !key.isEmpty,
                  key.utf8.count <= 4_096,
                  key.unicodeScalars.allSatisfy({
                      !CharacterSet.controlCharacters.contains($0)
                  }) else { continue }
            valid[provider] = key
        }
        guard !valid.isEmpty else {
            throw AIBackendError(message: "当前账号没有可用的主聊天模型")
        }
        AICloudCredentialStore.shared.replaceAccountModelKeys(valid)
        return true
    }

    @MainActor static func refreshBailianIfNeeded(force: Bool = false) async throws {
        // 后台恢复或进程重启后，先凭手机号会话续领；短期 Key 只写进内存。
        guard AICloudCredentialStore.shared.bailianNeedsRefresh() else { return }
        if let bailianRefreshTask { try await bailianRefreshTask.value; return }
        guard force || Date() >= bailianRetryAfter else { return }
        bailianRetryAfter = Date().addingTimeInterval(30)
        let task = Task {
            guard let issued = try await MaiChatAuthClient.shared.bailianCredentialIfAvailable()
            else { return }
            let expiry = Date(timeIntervalSince1970: TimeInterval(issued.expiresAt))
            guard issued.apiKey.hasPrefix("st-"), issued.apiKey.count >= 11,
                  expiry > Date().addingTimeInterval(30),
                  expiry <= Date().addingTimeInterval(360),
                  issued.wanWorkspaceId.range(
                    of: "^(ws|llm)-[A-Za-z0-9_-]{1,120}$", options: .regularExpression
                  ) != nil else {
                throw AIBackendError(message: "百炼临时凭据无效")
            }
            guard !Task.isCancelled else { return }
            AICloudCredentialStore.shared.setTemporaryBailian(
                issued.apiKey, workspaceId: issued.wanWorkspaceId, expiresAt: expiry
            )
        }
        bailianRefreshTask = task
        defer { bailianRefreshTask = nil }
        try await task.value
    }

    @MainActor static func refreshSeedanceIfNeeded(force: Bool = false) async throws {
        guard AICloudCredentialStore.shared.arkNeedsRefresh() else { return }
        if let arkRefreshTask { try await arkRefreshTask.value; return }
        guard force || Date() >= arkRetryAfter else { return }
        arkRetryAfter = Date().addingTimeInterval(30)
        let task = Task {
            guard let issued = try await MaiChatAuthClient.shared.seedanceCredentialIfAvailable()
            else { return }
            let expiry = Date(timeIntervalSince1970: TimeInterval(issued.expiresAt))
            guard issued.apiKey.count >= 16,
                  expiry > Date().addingTimeInterval(30),
                  expiry <= Date().addingTimeInterval(360) else {
                throw AIBackendError(message: "Seedance 临时凭据无效")
            }
            guard !Task.isCancelled else { return }
            AICloudCredentialStore.shared.setTemporaryArk(issued.apiKey, expiresAt: expiry)
        }
        arkRefreshTask = task
        defer { arkRefreshTask = nil }
        try await task.value
    }

}
struct AISession: Codable, Identifiable, Sendable, Equatable {
    let id: String
    let title: String
    let busy: Bool
    var displayTitle: String { title == "New session" ? "新对话" : title }
}

struct AIRemoteSession: Decodable, Identifiable, Sendable, Equatable {
    let sessionID: String
    let title: String
    let busy: Bool
    let deviceSlot: String
    let deviceName: String
    let deviceID: String
    let loginSessionID: String
    let online: Bool

    var id: String { "\(deviceSlot):\(deviceID):\(sessionID)" }
    var displayTitle: String { title == "New session" ? "新对话" : title }

    enum CodingKeys: String, CodingKey {
        case sessionID = "id"
        case title, busy, online
        case deviceSlot = "device_slot"
        case deviceName = "device_name"
        case deviceID = "device_id"
        case loginSessionID = "login_session_id"
    }
}

private actor AIRemoteSessionTransport {
    private struct Catalog: Decodable { let sessions: [AIRemoteSession] }
    private struct Inbox: Decodable { let messages: [Envelope] }
    struct Envelope: Decodable, Sendable {
        let id: Int
        let kind: String
        let sessionID: String
        let clientMessageID: String
        let fromSlot: String
        let fromLoginSessionID: String
        let payload: Payload

        enum CodingKeys: String, CodingKey {
            case id, kind, payload
            case sessionID = "session_id"
            case clientMessageID = "client_message_id"
            case fromSlot = "from_slot"
            case fromLoginSessionID = "from_login_session_id"
        }
    }
    struct Payload: Decodable, Sendable {
        let transferID: String?
        let chunkIndex: Int?
        let chunkCount: Int?
        let sha256: String?
        let dataBase64: String?
        let before: String?
        let limit: Int?
        let action: String?
        let text: String?
        let requestID: String?
        let decision: String?
        let title: String?

        enum CodingKeys: String, CodingKey {
            case transferID = "transfer_id"
            case chunkIndex = "chunk_index"
            case chunkCount = "chunk_count"
            case sha256
            case dataBase64 = "data_base64"
            case before, limit, action, text, decision, title
            case requestID = "id"
        }
    }

    private func request(_ route: MaiChatSessionSyncRoute,
                         body: [String: Any] = [:], wait: Int = 0) async throws -> Data {
        let data = body.isEmpty ? Data() : try JSONSerialization.data(withJSONObject: body)
        return try await MaiChatAuthClient.shared.sessionSyncRequest(
            route, body: data, waitMilliseconds: wait)
    }

    func catalog() async throws -> [AIRemoteSession] {
        try JSONDecoder().decode(Catalog.self, from: await request(.sessions)).sessions
    }

    func presence(deviceID: String, name: String, sessions: [AISession]) async throws {
        let updated = Int64(Date().timeIntervalSince1970 * 1_000)
        let entries: [[String: Any]] = sessions.prefix(200).enumerated().map { index, item in
            let safeTitle = String(String(item.title.prefix(120)).unicodeScalars.filter {
                !CharacterSet.controlCharacters.contains($0)
            })
            return ["id": item.id, "title": safeTitle,
                    "busy": item.busy, "updated_at": updated - Int64(index)]
        }
        _ = try await request(.presence, body: [
            "device_id": deviceID, "name": name, "sessions": entries
        ])
    }

    func poll() async throws -> [Envelope] {
        try JSONDecoder().decode(Inbox.self, from: await request(.poll, wait: 10_000)).messages
    }

    func send(to session: AIRemoteSession, kind: String,
              payload: [String: Any]) async throws -> String {
        try await send(toSlot: session.deviceSlot, toLogin: session.loginSessionID,
                       sessionID: session.sessionID, kind: kind, payload: payload)
    }

    func send(toSlot: String, toLogin: String, sessionID: String,
              kind: String, payload: [String: Any]) async throws -> String {
        let messageID = UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
        _ = try await request(.send, body: [
            "to_slot": toSlot,
            "to_login_session_id": toLogin,
            "client_message_id": messageID,
            "session_id": sessionID,
            "kind": kind,
            "payload": payload
        ])
        return messageID
    }

    func sendPayload(to envelope: Envelope, kind: String,
                     value: [String: Any]) async throws {
        let data = try JSONSerialization.data(withJSONObject: value)
        let chunkSize = 24 * 1024
        let count = max(1, (data.count + chunkSize - 1) / chunkSize)
        guard count <= 128 else { throw AIBackendError(message: "跨设备会话内容过大") }
        let transferID = UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
        let digest = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        for index in 0..<count {
            let start = index * chunkSize
            let end = min(start + chunkSize, data.count)
            _ = try await send(toSlot: envelope.fromSlot,
                               toLogin: envelope.fromLoginSessionID,
                               sessionID: envelope.sessionID, kind: kind,
                               payload: ["transfer_id": transferID,
                                         "chunk_index": index,
                                         "chunk_count": count,
                                         "sha256": digest,
                                         "data_base64": data[start..<end].base64EncodedString()])
        }
    }

    func acknowledge(_ id: Int) async throws {
        _ = try await request(.ack, body: ["ids": [id]])
    }
}

private struct AIRemoteTransfer {
    let sessionID: String
    let fromSlot: String
    let fromLoginSessionID: String
    let kind: String
    let count: Int
    let sha256: String
    var chunks: [Int: Data] = [:]
    var started = Date()
}
struct AIPart: Codable, Identifiable, Sendable, Equatable {
    let id: String
    let kind: String
    var text: String?
    var tool: String?
    var input: String?
    var output: String?
    var error: String?
    var state: String?
    var path: String?
    var mimeType: String?
    var messageId: String? = nil
    var preview: String? = nil
}
struct AIMessage: Codable, Identifiable, Sendable, Equatable {
    let id: String
    let role: String
    let created: Int64
    let completed: Int64
    let active: Bool
    let parts: [AIPart]
    // 思考正文仍由核心持久化；界面快照只保留片段身份。流式思考每个增量因此
    // 不会改变可见消息，也不会触发整行 Markdown 重排或打断键盘输入。
    func withoutReasoningText() -> AIMessage {
        guard parts.contains(where: { $0.kind == "reasoning" }) else { return self }
        return AIMessage(id: id, role: role, created: created, completed: completed,
                         active: active, parts: parts.map { original in
            var part = original
            if part.kind == "reasoning" { part.text = nil }
            return part
        })
    }
    var text: String { parts.filter { $0.kind == "text" }.compactMap(\.text).joined(separator: "\n") }
    var quotePreview: String {
        var media: [String] = []
        for part in parts {
            if part.kind == "image" { media.append("图片") }
            if part.kind == "video" { media.append("视频") }
            if part.kind == "tool", part.tool == "agent_send_media",
               let output = part.output?.data(using: .utf8),
               let payload = (try? JSONSerialization.jsonObject(with: output)) as? [String: Any],
               let type = payload["type"] as? String {
                media.append(type == "video" ? "视频" : type == "image" ? "图片" : "音频")
            }
        }
        let summary = media.isEmpty ? "" : "[\(media.joined(separator: "、"))] "
        let excerpt = text.trimmingCharacters(in: .whitespacesAndNewlines)
        return summary + (excerpt.isEmpty ? "消息" : String(excerpt.prefix(80)))
    }
}
struct AIPermission: Codable, Identifiable, Sendable, Equatable {
    let id: String
    let tool: String
    let input: String
    var allowForSession: Bool?
    var rememberOnApproval: Bool?
    var fileCount: Int?
}
struct AIQuestion: Codable, Identifiable, Sendable, Equatable {
    let id: String
    let question: String
    let options: [String]
}
struct AIImportedFile: Sendable, Equatable {
    let relativePath: String
    let mimeType: String
    let isImage: Bool
}
struct AIAttachmentImportTarget: Sendable, Hashable {
    let sessionID: String
    let emptyDraftToken: UUID?
}
struct AIImagePreview: Identifiable, Equatable {
    let id: UUID
    let filePath: String
    let imageSize: CGSize
    let sourceFrame: CGRect?
    let sequence: MaiMediaSequence?

    init(filePath: String, imageSize: CGSize = .zero, sourceFrame: CGRect? = nil,
         sequence: MaiMediaSequence? = nil, id: UUID = UUID()) {
        self.id = id
        self.filePath = filePath
        self.imageSize = imageSize
        self.sourceFrame = sourceFrame
        self.sequence = sequence
    }
}
struct AIAssistantOpenResult: Sendable {
    let settings: AIModelSettings
    let workspacePath: String
}
struct AIReplySuggestions: Codable, Sendable, Equatable {
    let natural: String
    let casual: String
    let professional: String
}
struct AIResponse: Codable, Sendable {
    let ok: Bool
    var id: String?
    var error: String?
    var changed: Bool?
    var sessions: [AISession]?
    var messages: [AIMessage]?
    var permissions: [AIPermission]?
    var questions: [AIQuestion]?
    var busy: Bool?
    var configured: Bool?
    var natural: String?
    var casual: String?
    var professional: String?
}
struct AIBackendError: LocalizedError {
    let message: String
    var errorDescription: String? { message }
}

// 普通 actor 使用后台执行器。数据库、原生核心、Keychain 和文件都不在 MainActor。
actor AIAssistantBackend {
    private let address: UInt
    private var settings = AIModelSettings()
    private var root: URL?
    private var initialized = false
    private func storedKey(for model: String) -> String {
        AICloudCredentialStore.shared.apiKey(model == "deepseek-flash" ? "deepseek" : "glm")
    }

    init() {
        let pointer = maiMobileAgentCreate()
        address = UInt(bitPattern: pointer)
        guard let pointer else { return }
        if let apiBase = OrtGetApiBase() {
            _ = maiMobileAgentSetOrtApiBase(pointer, UnsafeRawPointer(apiBase))
        }
        let context = Unmanaged.passRetained(AIMaiChatHostToolCallbackContext()).toOpaque()
        let registered = maiMobileAgentSetHostToolHandler(
            pointer,
            context,
            aiMaiChatHostToolHandler,
            aiMaiChatHostToolResponseFree,
            aiMaiChatHostToolContextRelease
        )
        if registered == 0 {
            Unmanaged<AIMaiChatHostToolCallbackContext>.fromOpaque(context).release()
        }
    }
    deinit {
        let address = address
        DispatchQueue.global(qos: .utility).async {
            maiMobileAgentDestroy(UnsafeMutableRawPointer(bitPattern: address))
        }
    }

    private func requestAgentOperation(_ operation: String, values: [String: Any] = [:], force: Bool = false) throws
        -> AIResponse
    {
        var request: [String: Any] = values
        request["op"] = operation
        request["force"] = force
        let data = try JSONSerialization.data(withJSONObject: request)
        guard let text = String(data: data, encoding: .utf8),
            let response = text.withCString({ maiMobileAgentRequest(UnsafeMutableRawPointer(bitPattern: address), $0) })
        else { throw AIBackendError(message: "创作中心初始化失败")
        }
        defer { maiMobileAgentFree(response) }
        let decoded = try JSONDecoder().decode(AIResponse.self, from: Data(String(cString: response).utf8))
        guard decoded.ok else { throw AIBackendError(message: decoded.error ?? "操作失败") }
        return decoded
    }

    /// 准备当前账号的工作区、模型凭证和 C++ Agent，返回界面可使用的设置与路径。
    /// 已初始化时复用同一后端，避免页面反复进入时重建数据库连接和任务状态。
    /// 任一必需目录或配置失败时抛错，由页面保留原画面并显示可恢复的错误。
    func open(accountUserID: String) async throws -> AIAssistantOpenResult {
        if initialized {
            return AIAssistantOpenResult(
                settings: settings,
                workspacePath: root!.appendingPathComponent("Workspace", isDirectory: true).path
            )
        }
        var directory = try AIAssistantWorkspaceLocator.directory(for: accountUserID)
        #if targetEnvironment(simulator)
        let uiTest = ProcessInfo.processInfo.arguments.contains("--ai-ui-test")
            if uiTest {
                directory = FileManager.default.temporaryDirectory.appendingPathComponent(
                    "AIAssistantUITest-" + ProcessInfo.processInfo.environment["MAICHAT_AI_TEST_ID", default: "manual"])
            }
        #endif
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        root = directory
        let file = directory.appendingPathComponent("settings.json")
        if FileManager.default.fileExists(atPath: file.path) {
            settings = try JSONDecoder().decode(AIModelSettings.self, from: Data(contentsOf: file))
        }
        // 先恢复已选主模型，再从服务器刷新仅驻内存的密钥，最后初始化 C++ Agent。
        if !["glm-5.3", "glm-5.3-flash", "deepseek-flash"].contains(settings.model) {
            settings.model = "glm-5.3"
        }
        settings.selectModel(settings.model)
        _ = try? await AICloudCredentialSync.refreshAccountModelKeysIfAvailable()
        try? await AICloudCredentialSync.refreshSeedanceIfNeeded(force: true)
        try? await AICloudCredentialSync.refreshBailianIfNeeded(force: true)
        var key = storedKey(for: settings.model)
        #if targetEnvironment(simulator)
        if uiTest {
            settings.baseUrl = "http://127.0.0.1:18189"
            settings.model = "test-model"
            settings.policy = "on-request"
            key = "test-key"
        }
        #endif
        try configure(settings, key: key)
        #if targetEnvironment(simulator)
        if uiTest { _ = try requestAgentOperation("create") }
        #endif
        initialized = true
        return AIAssistantOpenResult(
            settings: settings,
            workspacePath: directory.appendingPathComponent("Workspace", isDirectory: true).path
        )
    }

    private func configure(_ config: AIModelSettings, key: String) throws {
        // Swift 只负责应用目录、云端密钥和线协议选择；工具调用规则在共享 C++ 层。
        guard let root else { throw AIBackendError(message: "创作中心尚未准备好") }
        guard let caBundle = Bundle.main.url(forResource: "cacert", withExtension: "pem",
                                             subdirectory: "MaiAgentCertificates") else {
            throw AIBackendError(message: "HTTPS 根证书文件缺失")
        }
        let workspace = root.appendingPathComponent("Workspace", isDirectory: true)
        try FileManager.default.createDirectory(at: workspace, withIntermediateDirectories: true)
        let appRootPath = AIAssistantPathPolicy.appRoot(workspacePath: workspace.path)
        _ = try requestAgentOperation(
            "configure",
            values: [
                "database": root.appendingPathComponent("sessions.sqlite").path,
                "workspace": workspace.path, "baseUrl": config.baseUrl, "apiKey": key,
            "wire": config.effectiveWire,
            "model": config.model, "policy": config.policy, "appRoot": appRootPath,
            "caBundle": caBundle.path,
            "temporaryDirectory": FileManager.default.temporaryDirectory.path,
            "cacheDirectory": try FileManager.default.url(for: .cachesDirectory,
                in: .userDomainMask, appropriateFor: nil, create: true).path,
            "rvmModelPath": Bundle.main.url(
                forResource: "rvm_mobilenetv3_fp32", withExtension: "onnx",
                subdirectory: "MaiAgentModels")?.path ?? "",
            "lamaModelPath": Bundle.main.url(
                forResource: "lama_fp32", withExtension: "onnx",
                subdirectory: "MaiAgentModels")?.path ?? ""])
    }

    func save(_ config: AIModelSettings) throws {
        guard ["glm-5.3", "glm-5.3-flash", "deepseek-flash"].contains(config.model) else {
            throw AIBackendError(message: "请选择受支持的主模型")
        }
        var normalized = config
        normalized.selectModel(config.model)
        let effectiveKey = storedKey(for: normalized.model)
        #if targetEnvironment(simulator)
        let isLocalUITest = ProcessInfo.processInfo.arguments.contains("--ai-ui-test")
        #else
        let isLocalUITest = false
        #endif
        guard !effectiveKey.isEmpty || isLocalUITest else {
            throw AIBackendError(message: "请先登录已开通模型服务的账号")
        }
        let previous = settings
        let previousKey = storedKey(for: previous.model)
        try configure(normalized, key: effectiveKey) // 正在工作时核心拒绝，不能先覆盖已保存配置。
        do {
            try JSONEncoder().encode(normalized).write(
                to: root!.appendingPathComponent("settings.json"), options: .atomic)
            settings = normalized
        } catch {
            try? configure(previous, key: previousKey)
            throw error
        }
    }

    func request(_ operation: String, values: [String: String] = [:], force: Bool = false) throws -> AIResponse {
        try requestAgentOperation(operation, values: values.mapValues { $0 as Any }, force: force)
    }

    func snapshot(session: String, limit: Int, force: Bool) throws -> AIResponse {
        try requestAgentOperation("snapshot", values: ["session": session, "messageLimit": limit], force: force)
    }

    func messagesPage(session: String, beforeId: String, limit: Int) throws -> [AIMessage] {
        try requestAgentOperation("messages_page", values: ["session": session, "before": beforeId,
                                            "limit": limit]).messages ?? []
    }

    func send(session: String, text: String, images: [AIImportedFile],
              videos: [AIImportedFile], quoteMessageId: String) throws -> AIResponse {
        try requestAgentOperation("send", values: [
            "session": session,
            "text": text,
            "quoteMessageId": quoteMessageId,
            "images": images.map { ["path": $0.relativePath, "mimeType": $0.mimeType] },
            "videos": videos.map { ["path": $0.relativePath, "mimeType": $0.mimeType] }
        ])
    }

    func suggestReplies(messages: [[String: String]], accountUserID: String) async throws
        -> AIReplySuggestions {
        _ = try await open(accountUserID: accountUserID)
        let response = try requestAgentOperation("suggest_replies", values: ["messages": messages])
        guard let natural = response.natural?.trimmingCharacters(in: .whitespacesAndNewlines),
              let casual = response.casual?.trimmingCharacters(in: .whitespacesAndNewlines),
              let professional = response.professional?.trimmingCharacters(in: .whitespacesAndNewlines),
              !natural.isEmpty, !casual.isEmpty, !professional.isEmpty else {
            throw AIBackendError(message: "模型返回的三种回复不完整，请重试。")
        }
        return AIReplySuggestions(
            natural: natural,
            casual: casual,
            professional: professional
        )
    }

    func importDocument(_ source: URL) throws -> AIImportedFile {
        guard let root else { throw AIBackendError(message: "创作中心尚未准备好") }
        let scoped = source.startAccessingSecurityScopedResource()
        defer { if scoped { source.stopAccessingSecurityScopedResource() } }
        let values = try source.resourceValues(forKeys: [.fileSizeKey, .contentTypeKey])
        let contentType = values.contentType
            ?? UTType(filenameExtension: source.pathExtension)
        let isImage = contentType?.conforms(to: .image) == true
        let size = values.fileSize ?? 0
        let maximumSize = isImage ? 20 * 1024 * 1024 : 5 * 1024 * 1024
        guard size <= maximumSize else {
            throw AIBackendError(
                message: isImage ? "请选择不超过 20 MB 的图片" : "请选择不超过 5 MB 的文本文件"
            )
        }
        var data = try Data(contentsOf: source)
        guard isImage || String(data: data, encoding: .utf8) != nil else {
            throw AIBackendError(message: "目前支持 UTF-8 文本、代码文件和图片")
        }
        var storedName = source.lastPathComponent
        var mimeType = contentType?.preferredMIMEType ?? (isImage ? "image/jpeg" : "text/plain")
        let supportedImageTypes = ["image/jpeg", "image/png", "image/webp", "image/gif"]
        if isImage, !supportedImageTypes.contains(mimeType) {
            guard let image = UIImage(data: data),
                  let jpeg = image.jpegData(compressionQuality: 0.92)
            else { throw AIBackendError(message: "图片格式无法转换") }
            data = jpeg
            storedName = source.deletingPathExtension().lastPathComponent + ".jpg"
            mimeType = "image/jpeg"
        }
        guard data.count <= maximumSize else {
            throw AIBackendError(message: "转换后的图片超过 20 MB")
        }
        let name = UUID().uuidString.prefix(8) + "-" + storedName
        let target = root.appendingPathComponent("Workspace").appendingPathComponent(String(name))
        try data.write(to: target, options: .atomic)
        return AIImportedFile(
            relativePath: String(name),
            mimeType: mimeType,
            isImage: isImage
        )
    }

    func importVideo(_ source: URL) throws -> AIImportedFile {
        guard let root else { throw AIBackendError(message: "创作中心尚未准备好") }
        let scoped = source.startAccessingSecurityScopedResource()
        defer { if scoped { source.stopAccessingSecurityScopedResource() } }
        let values = try source.resourceValues(forKeys: [.fileSizeKey, .contentTypeKey])
        let type = values.contentType ?? UTType(filenameExtension: source.pathExtension)
        guard type?.conforms(to: .movie) == true else {
            throw AIBackendError(message: "所选文件不是可用的视频")
        }
        let size = values.fileSize ?? 0
        guard size > 0, size <= 1024 * 1024 * 1024 else {
            throw AIBackendError(message: "请选择不超过 1 GB 的视频")
        }
        let name = UUID().uuidString.prefix(8) + "-" + source.lastPathComponent
        let target = root.appendingPathComponent("Workspace").appendingPathComponent(String(name))
        do {
            try FileManager.default.copyItem(at: source, to: target)
        } catch {
            try? FileManager.default.removeItem(at: target)
            throw error
        }
        return AIImportedFile(
            relativePath: String(name),
            mimeType: type?.preferredMIMEType ?? "video/quicktime",
            isImage: false
        )
    }
}

@MainActor
final class AIAssistantModel: ObservableObject {
    static var shared = AIAssistantModel()
    private static let recentMessageCount = 8
    private static let olderPageCount = 20

    static func resetForAccountSwitch() {
        shared.disappear()
        shared.stopRemoteSync()
        shared.initialPreparation?.cancel()
        shared.appearingTask?.cancel()
        shared.hostStartup?.cancel()
        shared.transientErrorTask?.cancel()
        shared = AIAssistantModel()
    }
    @Published var sessions: [AISession] = []
    @Published private(set) var remoteSessions: [AIRemoteSession] = []
    @Published private(set) var selectedRemote: AIRemoteSession?
    @Published var messages: [AIMessage] = []
    @Published var permissions: [AIPermission] = []
    @Published var questions: [AIQuestion] = []
    @Published var selected = ""
    @Published var settings = AIModelSettings()
    @Published var configured = false
    @Published var error = ""
    @Published private(set) var remoteStatus = ""
    @Published var ready = false
    @Published var historyLoaded = false
    @Published private(set) var hasOlderMessages = false
    @Published private(set) var visibleMessageLimit = AIAssistantModel.recentMessageCount
    @Published var isSubmitting = false
    @Published private(set) var isCreatingConversation = false
    @Published var scrollRequest = 0
    @Published private(set) var workspacePath = ""
    @Published var previewImage: AIImagePreview?
    @Published var quotedMessage: AIMessage?
    var drafts: [String: String] = [:]
    @Published private(set) var pendingAttachments: [String: [AIImportedFile]] = [:]
    private(set) var firstMessageSessionID = ""
    private var emptyDraftToken = UUID()
    private var createdEmptyDraftSessions: [UUID: String] = [:]
    private var deletedDraftSessions: Set<String> = []
    private let backend = AIAssistantBackend()
    private let remoteTransport = AIRemoteSessionTransport()
    private var poll: Task<Void, Never>?
    private var remotePoll: Task<Void, Never>?
    @Published private(set) var pendingRemoteSelection: AIRemoteSession?
    private var pendingRemoteSelectionStarted = Date.distantPast
    private var remoteTransfers: [String: AIRemoteTransfer] = [:]
    private var pendingRemoteCommands: [String: Date] = [:]
    private var pendingRemoteOlderPage = false
    private var lastRemotePresence = Date.distantPast
    private var lastRemoteHistoryRequest = Date.distantPast
    private var transientErrorTask: Task<Void, Never>?
    private var lastBackendError = ""
    private var initialPreparation: Task<Void, Never>?
    private var hostStartup: Task<Void, Never>?
    private var appearingTask: Task<Void, Never>?
    private var pendingInitialPage: (session: String, newestID: String)?
    private var loadingOlderMessages = false
    private var selectionGeneration = 0
    private var visible = false
    private var pageActive = true
    private var composerTyping = false
    #if targetEnvironment(simulator)
    private var sessionSwitchUITest = false
    #endif
    var busy: Bool {
        if let selectedRemote {
            return remoteSessions.first {
                $0.id == selectedRemote.id &&
                $0.loginSessionID == selectedRemote.loginSessionID
            }?.busy == true
        }
        return sessions.first { $0.id == selected }?.busy == true
    }
    var hasEmptyDraft: Bool {
        !attachments(for: "").isEmpty ||
            !(drafts[""] ?? "").trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }
    var visibleMessages: [AIMessage] { Array(messages.suffix(visibleMessageLimit)) }
    var hasOlderVisibleMessages: Bool {
        messages.count > visibleMessageLimit || hasOlderMessages
    }

    #if DEBUG
    func installVideoToolTestWorkspace(_ path: String) { workspacePath = path }
    #endif

    func setComposerTyping(_ active: Bool) { composerTyping = active }

    #if targetEnvironment(simulator)
    func installHistoryUITestFixture() {
        let session = "history-ui-test"
        sessions = [AISession(id: session, title: "历史会话", busy: false)]
        selected = session
        messages = (0..<80).map { index in
            let part = AIPart(id: "history-part-\(index)", kind: "text",
                              text: "历史消息 \(index)\n第二行内容", tool: nil,
                              input: nil, output: nil, error: nil, state: nil,
                              path: nil, mimeType: nil)
            return AIMessage(id: "history-message-\(index)", role: "assistant",
                             created: Int64(index), completed: Int64(index + 1),
                             active: false, parts: [part])
        }
        ready = true
    }

    func installStreamingTypingUITestFixture() {
        let session = "stream-typing-ui-test"
        sessions = [AISession(id: session, title: "流式输入测试", busy: true)]
        selected = session
        messages = (0..<30).map { index in
            let part = AIPart(id: "stream-old-part-\(index)", kind: "text",
                              text: "先前的消息 \(index)", tool: nil,
                              input: nil, output: nil, error: nil, state: nil,
                              path: nil, mimeType: nil)
            return AIMessage(id: String(format: "stream-message-%02d", index),
                             role: "assistant", created: Int64(index),
                             completed: Int64(index + 1), active: false, parts: [part])
        }
        ready = true
        historyLoaded = true
        Task { @MainActor [weak self] in
            for index in 0..<60 {
                try? await Task.sleep(for: .milliseconds(100))
                guard let self, self.selected == session else { return }
                let part = AIPart(id: "stream-active-part", kind: "text",
                                  text: String(repeating: "正在输出内容。", count: index + 1),
                                  tool: nil, input: nil, output: nil, error: nil, state: nil,
                                  path: nil, mimeType: nil)
                let active = AIMessage(id: "stream-message-30", role: "assistant",
                                       created: 30, completed: 0, active: true, parts: [part])
                if self.messages.count == 30 { self.messages.append(active) }
                else { self.messages[30] = active }
            }
        }
    }

    func installSessionSwitchUITestFixture() {
        sessionSwitchUITest = true
        sessions = ["A", "B", "C"].map {
            AISession(id: "switch-\($0)", title: "对话 \($0)", busy: false)
        }
        remoteSessions = [AIRemoteSession(
            sessionID: "remote-demo", title: "Mac 上的编译任务", busy: false,
            deviceSlot: "mac", deviceName: "工作 Mac", deviceID: "fixturemac",
            loginSessionID: "fixturelogin", online: false)]
        selected = "switch-A"
        messages = sessionSwitchUITestMessages("A")
        ready = true
        historyLoaded = true
    }

    func installMediaDraftUITestFixture() {
        installSessionSwitchUITestFixture()
        let workspace = FileManager.default.temporaryDirectory
            .appendingPathComponent("ai-media-draft-ui-test", isDirectory: true)
        guard let video = Bundle.main.url(forResource: "ffplay-sample", withExtension: "mp4"),
              (try? FileManager.default.createDirectory(
                at: workspace, withIntermediateDirectories: true
              )) != nil else { return }
        let image = UIGraphicsImageRenderer(size: CGSize(width: 220, height: 160))
            .image { context in
                UIColor.systemBlue.setFill()
                context.fill(CGRect(x: 0, y: 0, width: 220, height: 160))
                UIColor.white.setFill()
                context.fill(CGRect(x: 45, y: 45, width: 130, height: 70))
            }
        let imageFile = workspace.appendingPathComponent("draft-image.png")
        let emptyDraftImageFile = workspace.appendingPathComponent("empty-draft-image.png")
        let videoFile = workspace.appendingPathComponent("draft-video.mp4")
        guard let imageData = image.pngData(),
              (try? imageData.write(to: imageFile, options: .atomic)) != nil,
              (try? imageData.write(to: emptyDraftImageFile, options: .atomic)) != nil else {
            return
        }
        if !FileManager.default.fileExists(atPath: videoFile.path) {
            try? FileManager.default.copyItem(at: video, to: videoFile)
        }
        workspacePath = workspace.path
        pendingAttachments[selected] = [
            AIImportedFile(relativePath: imageFile.lastPathComponent,
                           mimeType: "image/png", isImage: true),
            AIImportedFile(relativePath: videoFile.lastPathComponent,
                           mimeType: "video/mp4", isImage: false)
        ]
        pendingAttachments[""] = [
            AIImportedFile(relativePath: emptyDraftImageFile.lastPathComponent,
                           mimeType: "image/png", isImage: true)
        ]
        // 模拟过去的首发目标仍存在，检验手动回到空草稿再选 A 不会误迁移文字。
        firstMessageSessionID = "switch-A"
    }

    private func sessionSwitchUITestMessages(_ title: String) -> [AIMessage] {
        (0..<80).map { index in
            let part = AIPart(id: "switch-part-\(title)-\(index)", kind: "text",
                              text: "\(title) 历史消息 \(index)\n第二行内容", tool: nil,
                              input: nil, output: nil, error: nil, state: nil,
                              path: nil, mimeType: nil)
            return AIMessage(id: "switch-\(title)-message-\(index)", role: "assistant",
                             created: Int64(index), completed: Int64(index + 1),
                             active: false, parts: [part])
        }
    }
    #endif

    func suggestReplies(messages: [[String: String]]) async throws -> AIReplySuggestions {
        let owner = AIMobileHostToolProvider.shared.appState?.masterUserID ?? ""
        return try await backend.suggestReplies(messages: messages, accountUserID: owner)
    }

    func showTransientError(_ message: String, duration: Duration = .seconds(3)) {
        transientErrorTask?.cancel()
        error = message
        transientErrorTask = Task { @MainActor [weak self] in
            try? await Task.sleep(for: duration)
            guard !Task.isCancelled, let self, self.error == message else { return }
            self.error = ""
            self.transientErrorTask = nil
        }
    }

    func prepareFirstPage() async {
        if ready && pendingInitialPage != nil { return }
        if historyLoaded {
            logAIHistoryEvent("prepare skip selected=\(selected) count=\(messages.count)")
            return
        }
        if let initialPreparation {
            await initialPreparation.value
            return
        }
        let task = Task { @MainActor in
            do {
                let owner = AIMobileHostToolProvider.shared.appState?.masterUserID ?? ""
                let opened = try await backend.open(accountUserID: owner)
                settings = opened.settings
                workspacePath = opened.workspacePath
                ready = true
                if selected.isEmpty {
                    let summary = try await backend.snapshot(session: "", limit: 0,
                                                             force: true)
                    sessions = summary.sessions ?? []
                    configured = summary.configured ?? false
                    selected = sessions.first?.id ?? ""
                }
                logAIHistoryEvent("prepare selected=\(selected) sessions=\(sessions.count)")
                guard !selected.isEmpty else { historyLoaded = true; return }
                let session = selected
                if let newestID = try await showNewestMessage(for: session) {
                    pendingInitialPage = (session, newestID)
                }
            } catch {
                logAIHistoryEvent("prepare error=\(error.localizedDescription)")
                self.error = error.localizedDescription
            }
        }
        initialPreparation = task
        await task.value
        initialPreparation = nil
    }

    func appear() {
        visible = true
        guard appearingTask == nil else { return }
        appearingTask = Task { @MainActor in
            await prepareFirstPage()
            if let pending = pendingInitialPage {
                pendingInitialPage = nil
                await completeInitialPage(for: pending.session, newestID: pending.newestID)
            } else if historyLoaded {
                await refresh(force: true)
            }
            startPolling()
            startRemotePolling()
            appearingTask = nil
        }
    }

    func startSessionSyncForAccount() {
        guard hostStartup == nil else { return }
        hostStartup = Task { @MainActor [weak self] in
            guard let self else { return }
            await self.prepareFirstPage()
            if self.ready { self.startRemotePolling() }
            self.hostStartup = nil
        }
    }
    func disappear() {
        visible = false
        composerTyping = false
        poll?.cancel()
        poll = nil
    }

    private func stopRemoteSync() {
        remotePoll?.cancel()
        remotePoll = nil
    }
    func setPageActive(_ active: Bool) {
        pageActive = active
        if active && poll == nil && ready { appear() }
    }
    private func startPolling() {
        poll?.cancel()
        guard visible else { return }
        poll = Task { [weak self] in
            while !Task.isCancelled {
                let busy = self?.sessions.contains(where: \.busy) == true
                // 用户正在输入时把流式界面刷新降到每 250 毫秒一次，优先留出键盘事件时间。
                // 模型和工具继续后台工作；仅合并显示增量，不丢失任何输出。
                let typing = self?.composerTyping == true
                let interval: Duration = self?.pageActive == true
                    ? .milliseconds(busy ? (typing ? 250 : 100) : 500)
                    : .milliseconds(busy ? 500 : 2_000)
                try? await Task.sleep(for: interval)
                guard !Task.isCancelled, let self else { return }
                await self.refresh(force: false)
            }
        }
    }

    private var syncDeviceSlot: String {
        UIDevice.current.userInterfaceIdiom == .pad ? "tablet" : "ios_phone"
    }

    private var syncDeviceID: String {
        let key = "maichat-session-sync-device-id"
        if let saved = UserDefaults.standard.string(forKey: key),
           saved.count == 32, saved.allSatisfy({ $0.isHexDigit }) { return saved }
        let created = UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
        UserDefaults.standard.set(created, forKey: key)
        return created
    }

    private func startRemotePolling() {
        guard remotePoll == nil else { return }
        remotePoll = Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                guard AIMobileHostToolProvider.shared.appState?.hasUnifiedAccount == true,
                      self.ready else {
                    try? await Task.sleep(for: .seconds(2))
                    continue
                }
                do {
                    if Date().timeIntervalSince(self.lastRemotePresence) >= 15 {
                        if (!self.visible || self.selectedRemote != nil),
                           let summary = try? await self.backend.snapshot(
                               session: "", limit: 0, force: true),
                           let localSessions = summary.sessions {
                            self.sessions = localSessions
                        }
                        let name = String(String(UIDevice.current.name.prefix(40))
                            .unicodeScalars.filter {
                                !CharacterSet.controlCharacters.contains($0)
                            })
                        try await self.remoteTransport.presence(
                            deviceID: self.syncDeviceID,
                            name: name.isEmpty ? "iPhone" : name,
                            sessions: self.sessions
                        )
                        let catalog = try await self.remoteTransport.catalog()
                        self.remoteSessions = catalog.filter { $0.deviceSlot != self.syncDeviceSlot }
                        if let selected = self.selectedRemote,
                           let updated = self.remoteSessions.first(where: {
                               $0.id == selected.id &&
                               $0.loginSessionID == selected.loginSessionID
                           }) {
                            self.selectedRemote = updated
                        }
                        self.lastRemotePresence = Date()
                    }
                    if let remote = self.selectedRemote,
                       self.remoteIsOnline(remote),
                       Date().timeIntervalSince(self.lastRemoteHistoryRequest) >= 5 {
                        _ = try await self.remoteTransport.send(
                            to: remote, kind: "history_request", payload: ["limit": 10])
                        self.lastRemoteHistoryRequest = Date()
                    }
                    let envelopes = try await self.remoteTransport.poll()
                    for envelope in envelopes {
                        await self.handleRemoteEnvelope(envelope)
                    }
                    self.expireRemoteRequests()
                } catch {
                    logAIHistoryEvent("session-sync failed: \(error.localizedDescription)")
                    self.expireRemoteRequests()
                    if self.selectedRemote != nil {
                        self.showTransientError(error.localizedDescription, duration: .seconds(8))
                    }
                    try? await Task.sleep(for: .seconds(2))
                }
            }
        }
    }

    private func expireRemoteRequests() {
        let timedOut = pendingRemoteCommands.filter {
            Date().timeIntervalSince($0.value) > 30
        }
        if !timedOut.isEmpty {
            for id in timedOut.keys { pendingRemoteCommands.removeValue(forKey: id) }
            remoteStatus = ""
            showTransientError(
                "所属设备是否执行了操作尚未确定，请先查看会话记录，不要直接重复发送",
                duration: .seconds(12))
        }
        if pendingRemoteSelection != nil,
           Date().timeIntervalSince(pendingRemoteSelectionStarted) > 20 {
            pendingRemoteSelection = nil
            showTransientError("读取所属设备会话超时，请重新选择")
        }
    }

    private func remoteIsOnline(_ remote: AIRemoteSession) -> Bool {
        remoteSessions.contains {
            $0.id == remote.id && $0.loginSessionID == remote.loginSessionID && $0.online
        }
    }

    func selectRemoteConversation(_ remote: AIRemoteSession) async {
        logAIHistoryEvent("remote-select-request slot=\(remote.deviceSlot) online=\(remoteIsOnline(remote))")
        guard remoteIsOnline(remote) else {
            showTransientError("所属设备离线，暂时无法读取这段会话")
            return
        }
        if selectedRemote?.id == remote.id &&
            selectedRemote?.loginSessionID == remote.loginSessionID {
            scrollRequest += 1
            return
        }
        selectionGeneration &+= 1
        pendingRemoteSelection = remote
        pendingRemoteSelectionStarted = Date()
        do {
            _ = try await remoteTransport.send(to: remote, kind: "history_request",
                                               payload: ["limit": Self.recentMessageCount])
            lastRemoteHistoryRequest = Date()
        } catch {
            pendingRemoteSelection = nil
            logAIHistoryEvent("remote-select-error \(error.localizedDescription)")
            showTransientError(error.localizedDescription, duration: .seconds(8))
        }
    }

    private func handleRemoteEnvelope(_ envelope: AIRemoteSessionTransport.Envelope) async {
        logAIHistoryEvent("remote-envelope kind=\(envelope.kind) slot=\(envelope.fromSlot)")
        defer { Task { try? await remoteTransport.acknowledge(envelope.id) } }
        if envelope.kind == "history_request" {
            await answerRemoteHistoryRequest(envelope)
            return
        }
        if envelope.kind == "command" {
            await applyRemoteCommand(envelope)
            return
        }
        guard envelope.kind == "history_page" || envelope.kind == "event",
              let transferID = envelope.payload.transferID,
              let index = envelope.payload.chunkIndex,
              let count = envelope.payload.chunkCount,
              let digest = envelope.payload.sha256,
              let encoded = envelope.payload.dataBase64,
              (1...128).contains(count), (0..<count).contains(index),
              let chunk = Data(base64Encoded: encoded), chunk.count <= 24 * 1024 else { return }
        remoteTransfers = remoteTransfers.filter {
            Date().timeIntervalSince($0.value.started) < 120
        }
        var transfer = remoteTransfers[transferID] ?? AIRemoteTransfer(
            sessionID: envelope.sessionID, fromSlot: envelope.fromSlot,
            fromLoginSessionID: envelope.fromLoginSessionID,
            kind: envelope.kind, count: count, sha256: digest)
        guard transfer.sessionID == envelope.sessionID,
              transfer.fromSlot == envelope.fromSlot,
              transfer.fromLoginSessionID == envelope.fromLoginSessionID,
              transfer.kind == envelope.kind, transfer.count == count,
              transfer.sha256 == digest else { return }
        transfer.chunks[index] = chunk
        if transfer.chunks.count < count {
            remoteTransfers[transferID] = transfer
            return
        }
        remoteTransfers.removeValue(forKey: transferID)
        let data = (0..<count).reduce(into: Data()) { result, part in
            if let bytes = transfer.chunks[part] { result.append(bytes) }
        }
        guard data.count <= 3 * 1024 * 1024,
              SHA256.hash(data: data).map({ String(format: "%02x", $0) }).joined() == digest
        else { return }
        await applyRemotePayload(data, envelope: envelope)
    }

    private func applyRemotePayload(_ data: Data,
                                    envelope: AIRemoteSessionTransport.Envelope) async {
        let source = pendingRemoteSelection ?? selectedRemote
        guard let source, source.sessionID == envelope.sessionID,
              source.deviceSlot == envelope.fromSlot,
              source.loginSessionID == envelope.fromLoginSessionID else { return }
        if let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
           object["type"] as? String == "command_result" {
            if let id = object["command_id"] as? String,
               pendingRemoteCommands.removeValue(forKey: id) != nil {
                remoteStatus = pendingRemoteCommands.isEmpty ? "" : "等待所属设备确认操作…"
                if let result = object["result"] as? [String: Any],
                   result["ok"] as? Bool != true {
                    showTransientError(result["error"] as? String ?? "所属设备未执行操作",
                                       duration: .seconds(8))
                }
            }
            return
        }
        guard let page = try? JSONDecoder().decode(AIResponse.self, from: data),
              page.ok, let received = page.messages else { return }
        let displayMessages = received.map(remoteDisplayMessage)
        if pendingRemoteSelection?.id == source.id {
            var transaction = Transaction(animation: nil)
            transaction.disablesAnimations = true
            withTransaction(transaction) {
                selectedRemote = source
                pendingRemoteSelection = nil
                selected = source.sessionID
                messages = Array(displayMessages.reversed())
                permissions = page.permissions ?? []
                questions = page.questions ?? []
                quotedMessage = nil
                visibleMessageLimit = Self.recentMessageCount
                historyLoaded = true
                hasOlderMessages = received.count == Self.recentMessageCount
                scrollRequest += 1
            }
        } else if selectedRemote?.id == source.id {
            mergeMessages(displayMessages)
            if let permissions = page.permissions { self.permissions = permissions }
            if let questions = page.questions { self.questions = questions }
            if pendingRemoteOlderPage && envelope.kind == "history_page" {
                visibleMessageLimit += received.count
                hasOlderMessages = received.count == Self.olderPageCount
                pendingRemoteOlderPage = false
            }
        }
    }

    private func remoteDisplayMessage(_ message: AIMessage) -> AIMessage {
        var parts: [AIPart] = []
        for original in message.parts {
            var part = original
            if part.kind == "image" || part.kind == "video" {
                // 所属设备的相对路径不能在本机工作区解析，否则可能显示同名的其他文件。
                part.path = nil
                parts.append(part)
                parts.append(AIPart(
                    id: part.id + "-remote-placeholder", kind: "text",
                    text: part.kind == "image" ? "[图片在所属设备，暂不可跨设备预览]" :
                        "[视频在所属设备，暂不可跨设备预览]",
                    tool: nil, input: nil, output: nil, error: nil, state: nil,
                    path: nil, mimeType: nil))
            } else {
                parts.append(part)
            }
        }
        return AIMessage(id: message.id, role: message.role,
                         created: message.created, completed: message.completed,
                         active: message.active, parts: parts).withoutReasoningText()
    }

    private func answerRemoteHistoryRequest(_ envelope: AIRemoteSessionTransport.Envelope) async {
        guard sessions.contains(where: { $0.id == envelope.sessionID }) else { return }
        do {
            let page = try await backend.messagesPage(
                session: envelope.sessionID,
                beforeId: envelope.payload.before ?? "",
                limit: min(max(envelope.payload.limit ?? 10, 1), 20))
            let state = try await backend.snapshot(session: envelope.sessionID,
                                                   limit: 0, force: true)
            let safePage = page.map { message -> AIMessage in
                let parts = message.parts.map { original -> AIPart in
                    var part = original
                    if part.kind == "image" || part.kind == "video" { part.path = nil }
                    return part
                }
                return AIMessage(id: message.id, role: message.role,
                                 created: message.created, completed: message.completed,
                                 active: message.active, parts: parts)
            }
            let encoded = try JSONEncoder().encode(safePage)
            let messages = try JSONSerialization.jsonObject(with: encoded)
            try await remoteTransport.sendPayload(to: envelope, kind: "history_page",
                                                  value: ["ok": true, "messages": messages,
                                                          "permissions": try JSONSerialization.jsonObject(
                                                              with: JSONEncoder().encode(state.permissions ?? [])),
                                                          "questions": try JSONSerialization.jsonObject(
                                                              with: JSONEncoder().encode(state.questions ?? []))])
            if visible && selectedRemote == nil { await refresh(force: true) }
        } catch { /* The requester can retry a read without executing a model operation. */ }
    }

    private func applyRemoteCommand(_ envelope: AIRemoteSessionTransport.Envelope) async {
        guard sessions.contains(where: { $0.id == envelope.sessionID }) else { return }
        var result: [String: Any] = ["ok": true]
        do {
            switch envelope.payload.action {
            case "prompt":
                guard let text = envelope.payload.text, !text.isEmpty,
                      text.utf8.count <= 16 * 1024 else {
                    throw AIBackendError(message: "消息为空或过长")
                }
                _ = try await backend.send(session: envelope.sessionID, text: text,
                                           images: [], videos: [], quoteMessageId: "")
            case "stop":
                _ = try await backend.request("stop", values: ["session": envelope.sessionID])
            case "permission", "answer", "rename":
                var values = ["session": envelope.sessionID]
                if let id = envelope.payload.requestID { values["id"] = id }
                if let decision = envelope.payload.decision { values["decision"] = decision }
                if let text = envelope.payload.text { values["text"] = text }
                if let title = envelope.payload.title { values["title"] = title }
                _ = try await backend.request(envelope.payload.action ?? "", values: values)
            default:
                throw AIBackendError(message: "不支持的远端操作")
            }
        } catch { result = ["ok": false, "error": error.localizedDescription] }
        try? await remoteTransport.sendPayload(
            to: envelope, kind: "event",
            value: ["type": "command_result", "command_id": envelope.clientMessageID,
                    "result": result])
    }
    private func refresh(force: Bool) async {
        if selectedRemote != nil { return }
        try? await AICloudCredentialSync.refreshSeedanceIfNeeded()
        try? await AICloudCredentialSync.refreshBailianIfNeeded()
        let target = selected
        do {
            // 轮询只同步最近几条；旧消息由用户上滑时单独分页读取，避免又把首屏扩成 30 条。
            let result = try await backend.snapshot(session: target,
                                                    limit: Self.recentMessageCount, force: force)
            guard target == selected, result.changed == true else { return }
            if let value = result.sessions, value != sessions { sessions = value }
            if let value = result.messages {
                let knownIDs = Set(messages.map(\.id))
                let newFailure = value.contains { message in
                    !knownIDs.contains(message.id) && message.parts.contains { part in
                        part.kind == "text" &&
                        ((part.text?.hasPrefix("Video task failed. ") == true) ||
                         (part.text?.hasPrefix("Image task failed. ") == true) ||
                         (part.text?.hasPrefix("Video task status unavailable. ") == true) ||
                         (part.text?.hasPrefix("Image task status unavailable. ") == true) ||
                         (part.text?.hasPrefix("Video output unavailable. ") == true) ||
                         (part.text?.hasPrefix("Image output unavailable. ") == true))
                    }
                }
                mergeMessages(value)
                if newFailure {
                    showTransientError("生成任务出现异常，详情见对话。", duration: .seconds(8))
                }
            }
            if let value = result.permissions, value != permissions { permissions = value }
            if let value = result.questions, value != questions { questions = value }
            configured = result.configured ?? false
            if let detail = result.error, !detail.isEmpty {
                if detail != lastBackendError {
                    lastBackendError = detail
                    let display = detail.hasPrefix("model stream inactive")
                        ? "模型暂时没有响应，本次等待已结束。稍后可继续对话。"
                        : detail
                    showTransientError(display, duration: .seconds(8))
                }
            } else {
                lastBackendError = ""
            }
        } catch { showTransientError(error.localizedDescription, duration: .seconds(8)) }
    }

    private func showNewestMessage(for session: String) async throws -> String? {
        let latest = try await backend.messagesPage(session: session, beforeId: "", limit: 1)
        guard selected == session else { return nil }
        messages = Array(latest.reversed()).map { $0.withoutReasoningText() }
        historyLoaded = true
        logAIHistoryEvent(
            """
            first-page session=\(session) count=\(latest.count) newest=\(latest.first?.id ?? "none") \
            role=\(latest.first?.role ?? "none") parts=\(latest.first?.parts.count ?? 0)
            """
        )
        return latest.first?.id
    }

    private func completeInitialPage(for session: String, newestID: String) async {
        do {
            let earlier = try await backend.messagesPage(session: session,
                                                         beforeId: newestID,
                                                         limit: Self.recentMessageCount - 1)
            guard selected == session else { return }
            mergeMessages(earlier)
            hasOlderMessages = earlier.count == Self.recentMessageCount - 1
            logAIHistoryEvent(
                """
                older-page session=\(session) added=\(earlier.count) first=\(messages.first?.id ?? "none") \
                last=\(messages.last?.id ?? "none")
                """
            )
            await refresh(force: true)
        } catch {
            logAIHistoryEvent("older-page error=\(error.localizedDescription)")
            self.error = error.localizedDescription
        }
    }

    private func mergeMessages(_ page: [AIMessage]) {
        // 快照只更新最近的有界消息页；用户已经展开的旧页必须保留。
        // 新旧页有相同稳定 ID 时替换旧行，避免重复显示。
        guard !page.isEmpty else { return }
        var merged = messages
        let previousNewestID = messages.last?.id
        var newlyAppendedCount = 0
        var changed = false
        for rawMessage in page {
            let message = rawMessage.withoutReasoningText()
            var lower = 0
            var upper = merged.count
            while lower < upper {
                let middle = (lower + upper) / 2
                if merged[middle].id < message.id { lower = middle + 1 }
                else { upper = middle }
            }
            if lower < merged.count, merged[lower].id == message.id {
                if merged[lower] != message {
                    merged[lower] = message
                    changed = true
                }
            } else {
                merged.insert(message, at: lower)
                changed = true
                if let previousNewestID, message.id > previousNewestID {
                    newlyAppendedCount += 1
                }
            }
        }
        if changed {
            // 已展开旧页时保持其起点；新消息进入末尾不能把正在阅读的旧行挤出可见窗。
            if visibleMessageLimit > Self.recentMessageCount {
                visibleMessageLimit += newlyAppendedCount
            }
            messages = merged
        }
    }

    func loadOlderMessages() async {
        guard !loadingOlderMessages, hasOlderVisibleMessages,
              let oldest = messages.first?.id, !selected.isEmpty else { return }
        if messages.count > visibleMessageLimit {
            visibleMessageLimit = min(messages.count,
                                      visibleMessageLimit + Self.olderPageCount)
            return
        }
        loadingOlderMessages = true
        defer { loadingOlderMessages = false }
        let session = selected
        if let remote = selectedRemote {
            pendingRemoteOlderPage = true
            do {
                _ = try await remoteTransport.send(to: remote, kind: "history_request",
                                                   payload: ["before": oldest,
                                                             "limit": Self.olderPageCount])
            } catch {
                pendingRemoteOlderPage = false
                showTransientError(error.localizedDescription, duration: .seconds(8))
            }
            return
        }
        do {
            let page = try await backend.messagesPage(session: session,
                                                      beforeId: oldest,
                                                      limit: Self.olderPageCount)
            guard selected == session else { return }
            mergeMessages(page)
            visibleMessageLimit += page.count
            hasOlderMessages = page.count == Self.olderPageCount
        } catch { self.error = error.localizedDescription }
    }

    func messagesForMediaPlaylist(in session: String) async -> [AIMessage] {
        guard !session.isEmpty, selected == session else { return [] }
        var collected: [AIMessage] = []
        var before = ""
        do {
            while !Task.isCancelled {
                // 只读本地会话存储，不把旧消息装进界面；预览仍可遍历全部历史媒体。
                let page = try await backend.messagesPage(session: session,
                                                          beforeId: before, limit: 100)
                guard selected == session else { return [] }
                collected.append(contentsOf: page)
                guard page.count == 100, let oldest = page.last?.id, oldest != before else { break }
                before = oldest
            }
        } catch {
            return messages
        }
        return collected.sorted { $0.id < $1.id }
    }

    /// 打开指定会话：先读取目标最近一页，再在无动画事务中替换可见消息。
    /// 读取期间保留原会话；并发切换通过代次丢弃过时结果，失败时不清空旧画面。
    func selectConversation(_ id: String) async {
        pendingRemoteSelection = nil
        if id.isEmpty {
            selectionGeneration &+= 1
            selectedRemote = nil
            pendingRemoteSelection = nil
            selected = ""
            messages = []
            permissions = []
            questions = []
            quotedMessage = nil
            historyLoaded = true
            hasOlderMessages = false
            scrollRequest += 1
            return
        }
        #if targetEnvironment(simulator)
        if sessionSwitchUITest {
            // 测试入口也遵循正式逻辑：先准备完整页面，再一次性替换当前会话。
            selected = id
            let title = String(id.suffix(1))
            let page = sessionSwitchUITestMessages(title)
            messages = page
            visibleMessageLimit = Self.recentMessageCount
            historyLoaded = true
            scrollRequest += 1
            return
        }
        #endif
        if id == selected && selectedRemote == nil {
            scrollRequest += 1
            return
        }
        selectionGeneration &+= 1
        let generation = selectionGeneration
        do {
            // 先在后台读取新会话最近一页；读取完成前保留当前消息，避免切换时闪白。
            // 一次性替换消息列表，避免“先一条、再整页”造成两次滚动和空白帧。
            let page = try await backend.messagesPage(session: id, beforeId: "",
                                                     limit: Self.recentMessageCount)
            guard generation == selectionGeneration else { return }
            var transaction = Transaction(animation: nil)
            transaction.disablesAnimations = true
            withTransaction(transaction) {
                selectedRemote = nil
                pendingRemoteSelection = nil
                selected = id
                messages = Array(page.reversed()).map { $0.withoutReasoningText() }
                visibleMessageLimit = Self.recentMessageCount
                permissions = []
                questions = []
                error = ""
                quotedMessage = nil
                historyLoaded = true
                hasOlderMessages = page.count == Self.recentMessageCount
                pendingInitialPage = nil
                scrollRequest += 1
            }
            await refresh(force: true)
        } catch {
            guard generation == selectionGeneration else { return }
            // 查询失败时继续显示原会话，用户可重试；不能把可见历史清空。
            showTransientError(error.localizedDescription, duration: .seconds(8))
        }
    }
    func create() async {
        guard !isSubmitting, !isCreatingConversation else { return }
        isCreatingConversation = true
        defer { isCreatingConversation = false }
        do {
            let result = try await backend.request("create")
            await selectConversation(result.id ?? "")
        } catch { self.error = error.localizedDescription }
    }

    private func migrateEmptyDraft(to session: String) {
        firstMessageSessionID = session
        createdEmptyDraftSessions[emptyDraftToken] = session
        emptyDraftToken = UUID()
        let files = pendingAttachments.removeValue(forKey: "") ?? []
        pendingAttachments[session, default: []].append(contentsOf: files)
        if let textDraft = drafts.removeValue(forKey: "") {
            drafts[session] = textDraft
        }
    }
    /// 提交文本及附件，并在核心落库后合并新快照、请求界面定位最新消息。
    /// 输入不合法或任务忙碌时不改动草稿；新建会话也必须保持一个可见的加载状态。
    func sendMessage(_ text: String, expectedSession: String? = nil) async -> Bool {
        if let remote = selectedRemote {
            guard expectedSession == nil || expectedSession == remote.sessionID,
                  remoteIsOnline(remote), !busy, !isSubmitting,
                  !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return false }
            guard attachments(for: remote.sessionID).isEmpty, quotedMessage == nil else {
                showTransientError("跨设备暂只支持文字消息，请移除附件或引用后重试")
                return false
            }
            isSubmitting = true
            defer { isSubmitting = false }
            do {
                let id = try await remoteTransport.send(to: remote, kind: "command",
                                                        payload: ["action": "prompt", "text": text])
                pendingRemoteCommands[id] = Date()
                remoteStatus = "已送达 \(remote.deviceName)，等待设备确认执行"
                return true
            } catch {
                showTransientError(error.localizedDescription, duration: .seconds(8))
                return false
            }
        }
        guard configured else {
            error = "请先登录已开通模型服务的账号"
            return false
        }
        let draftSession = selected
        let quoteMessageId = quotedMessage?.id ?? ""
        guard expectedSession == nil || expectedSession == draftSession else { return false }
        let attachments = pendingAttachments[draftSession, default: []]
        guard !isSubmitting, !isCreatingConversation, !busy,
              !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || !attachments.isEmpty else {
            return false
        }
        isSubmitting = true
        defer { isSubmitting = false }
        try? await AICloudCredentialSync.refreshSeedanceIfNeeded(force: true)
        try? await AICloudCredentialSync.refreshBailianIfNeeded(force: true)
        if attachments.contains(where: \.isImage),
           settings.model.trimmingCharacters(in: .whitespacesAndNewlines)
               .caseInsensitiveCompare("glm-5.3") == .orderedSame {
            showTransientError("glm-5.3 仅支持文本，请先切换到 glm-5.3-flash")
            return false
        }
        var targetSession = draftSession
        if targetSession.isEmpty {
            do {
                let created = try await backend.request("create")
                targetSession = created.id ?? ""
                guard !targetSession.isEmpty else { return false }
                migrateEmptyDraft(to: targetSession)
                if selected == draftSession {
                    selected = targetSession
                    messages = []
                    permissions = []
                    questions = []
                    // 新建空会话时显示欢迎态；不让首次发送期间出现无内容白屏。
                    historyLoaded = true
                    hasOlderMessages = false
                }
            } catch {
                self.error = error.localizedDescription
                return false
            }
        }
        do {
            _ = try await backend.send(
                session: targetSession,
                text: text,
                images: attachments.filter(\.isImage),
                videos: attachments.filter { $0.mimeType.hasPrefix("video/") },
                quoteMessageId: quoteMessageId
            )
            let sentNames = Set(attachments.map(\.relativePath))
            pendingAttachments[targetSession]?.removeAll { sentNames.contains($0.relativePath) }
            if pendingAttachments[targetSession]?.isEmpty == true {
                pendingAttachments.removeValue(forKey: targetSession)
            }
            if selected == targetSession, quotedMessage?.id == quoteMessageId { quotedMessage = nil }
            lastBackendError = ""
            error = ""
            await refresh(force: true)
            // 发送成功后即使快照暂未返回新消息，也保留可显示的已加载状态。
            // 把空快照解释为“历史未加载”会隐藏欢迎态，形成整块白屏。
            historyLoaded = true
            scrollRequest += 1
            startPolling()
            return true
        } catch { self.error = error.localizedDescription; return false }
    }

    func action(_ op: String, values: [String: String] = [:]) async {
        if let remote = selectedRemote {
            guard remoteIsOnline(remote) else {
                showTransientError("所属设备离线，无法执行操作")
                return
            }
            guard ["stop", "permission", "answer", "rename"].contains(op) else {
                showTransientError("此操作暂不支持跨设备执行")
                return
            }
            var payload: [String: Any] = values
            payload["action"] = op
            do {
                let id = try await remoteTransport.send(to: remote, kind: "command",
                                                        payload: payload)
                pendingRemoteCommands[id] = Date()
                remoteStatus = "已送达 \(remote.deviceName)，等待设备确认执行"
            } catch { showTransientError(error.localizedDescription, duration: .seconds(8)) }
            return
        }
        if (op == "delete" || op == "clear") && (isSubmitting || isCreatingConversation) {
            return
        }
        let target = selected
        do {
            var values = values; values["session"] = target
            _ = try await backend.request(op, values: values)
            if op == "delete" {
                deletedDraftSessions.insert(target)
                discardImportedFiles(pendingAttachments.removeValue(forKey: target) ?? [])
                drafts.removeValue(forKey: target)
                createdEmptyDraftSessions = createdEmptyDraftSessions.filter { $0.value != target }
            }
            if op == "delete", selected == target {
                selected = ""; messages = []; historyLoaded = true
                quotedMessage = nil
                firstMessageSessionID = ""
                emptyDraftToken = UUID()
            }
            if op == "clear", selected == target {
                messages = []; historyLoaded = true
                quotedMessage = nil
            }
            await refresh(force: true)
        } catch { self.error = error.localizedDescription }
    }
    func save(_ config: AIModelSettings) async -> Bool {
        do {
            var normalized = config
            normalized.selectModel(config.model)
            try await backend.save(normalized)
            settings = normalized; error = ""
            await refresh(force: true)
            return true
        } catch { self.error = error.localizedDescription; return false }
    }
    func attachments(for session: String) -> [AIImportedFile] {
        pendingAttachments[session] ?? []
    }

    func importTarget() -> AIAttachmentImportTarget {
        AIAttachmentImportTarget(
            sessionID: selected,
            emptyDraftToken: selected.isEmpty ? emptyDraftToken : nil
        )
    }

    func isDeletedDraftSession(_ session: String) -> Bool {
        deletedDraftSessions.contains(session)
    }

    func attachmentSession(for target: AIAttachmentImportTarget) -> String? {
        if !target.sessionID.isEmpty {
            return deletedDraftSessions.contains(target.sessionID) ? nil : target.sessionID
        }
        guard let token = target.emptyDraftToken else { return nil }
        if let created = createdEmptyDraftSessions[token] { return created }
        return token == emptyDraftToken ? "" : nil
    }

    @discardableResult
    func addAttachments(_ files: [AIImportedFile], to target: AIAttachmentImportTarget) -> Int {
        guard !files.isEmpty else { return 0 }
        guard let session = attachmentSession(for: target) else {
            discardImportedFiles(files)
            showTransientError("原会话已关闭，请重新选择媒体")
            return 0
        }
        let remaining = max(0, 10 - pendingAttachments[session, default: []].count)
        pendingAttachments[session, default: []].append(contentsOf: files.prefix(remaining))
        let extra = Array(files.dropFirst(remaining))
        if !extra.isEmpty {
            discardImportedFiles(extra)
            showTransientError("每条消息最多添加 10 项图片或视频，超出的媒体未加入草稿")
        }
        return min(files.count, remaining)
    }

    func discardImportedFile(_ file: AIImportedFile) {
        discardImportedFiles([file])
    }

    private func discardImportedFiles(_ files: [AIImportedFile]) {
        let urls = files.compactMap {
            AIAssistantPathPolicy.resolve($0.relativePath, workspacePath: workspacePath)
        }
        Task.detached(priority: .utility) {
            for url in urls { try? FileManager.default.removeItem(at: url) }
        }
    }

    func removeAttachment(_ file: AIImportedFile, from session: String) {
        let removed = pendingAttachments[session]?.filter {
            $0.relativePath == file.relativePath
        } ?? []
        pendingAttachments[session]?.removeAll { $0.relativePath == file.relativePath }
        if pendingAttachments[session]?.isEmpty == true {
            pendingAttachments.removeValue(forKey: session)
        }
        discardImportedFiles(removed)
    }
    func importFile(_ url: URL) async -> AIImportedFile? {
        do { return try await backend.importDocument(url) }
        catch { self.error = error.localizedDescription; return nil }
    }

    func importVideoFile(_ url: URL) async -> AIImportedFile? {
        do { return try await backend.importVideo(url) }
        catch { self.error = error.localizedDescription; return nil }
    }
}
