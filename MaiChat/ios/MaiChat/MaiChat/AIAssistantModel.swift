import Foundation
import SwiftUI
import UIKit
import UniformTypeIdentifiers

struct AIModelSettings: Codable, Sendable, Equatable {
    var baseUrl = "https://open.bigmodel.cn/api/coding/paas/v4"
    var model = "glm-5.3"
    var policy = "on-request"
}
struct AISession: Codable, Identifiable, Sendable, Equatable {
    let id: String
    let title: String
    let busy: Bool
    var displayTitle: String { title == "New session" ? "新对话" : title }
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
}
struct AIMessage: Codable, Identifiable, Sendable, Equatable {
    let id: String
    let role: String
    let created: Int64
    let completed: Int64
    let active: Bool
    let parts: [AIPart]
    var text: String { parts.filter { $0.kind == "text" }.compactMap(\.text).joined(separator: "\n") }
}
struct AIPermission: Codable, Identifiable, Sendable, Equatable {
    let id: String
    let tool: String
    let input: String
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
struct AIAssistantOpenResult: Sendable {
    let settings: AIModelSettings
    let workspacePath: String
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
}
private struct AIBackendError: LocalizedError {
    let message: String
    var errorDescription: String? { message }
}

// 普通 actor 使用后台执行器。数据库、原生核心、Keychain 和文件都不在 MainActor。
actor AIAssistantBackend {
    private let address: UInt
    private var settings = AIModelSettings()
    private var root: URL?
    private var initialized = false
    private let keys = KeychainSecretStore(account: "ai-assistant-api-key")

    init() { address = UInt(bitPattern: maiMobileAgentCreate()) }
    deinit {
        let address = address
        DispatchQueue.global(qos: .utility).async {
            maiMobileAgentDestroy(UnsafeMutableRawPointer(bitPattern: address))
        }
    }

    private func call(_ op: String, values: [String: Any] = [:], force: Bool = false) throws -> AIResponse {
        var request: [String: Any] = values
        request["op"] = op
        request["force"] = force
        let data = try JSONSerialization.data(withJSONObject: request)
        guard let text = String(data: data, encoding: .utf8),
              let response = text.withCString({ maiMobileAgentRequest(UnsafeMutableRawPointer(bitPattern: address), $0) }) else {
            throw AIBackendError(message: "AI 助手初始化失败")
        }
        defer { maiMobileAgentFree(response) }
        let decoded = try JSONDecoder().decode(AIResponse.self, from: Data(String(cString: response).utf8))
        guard decoded.ok else { throw AIBackendError(message: decoded.error ?? "操作失败") }
        return decoded
    }

    func open() throws -> AIAssistantOpenResult {
        if initialized {
            return AIAssistantOpenResult(
                settings: settings,
                workspacePath: root!.appendingPathComponent("Workspace", isDirectory: true).path
            )
        }
        var directory = try FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask,
                                                     appropriateFor: nil, create: true).appendingPathComponent("AIAssistant")
        #if targetEnvironment(simulator)
        let uiTest = ProcessInfo.processInfo.arguments.contains("--ai-ui-test")
        if uiTest { directory = FileManager.default.temporaryDirectory.appendingPathComponent("AIAssistantUITest-" + ProcessInfo.processInfo.environment["MAICHAT_AI_TEST_ID", default: "manual"]) }
        #endif
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        root = directory
        let file = directory.appendingPathComponent("settings.json")
        if FileManager.default.fileExists(atPath: file.path) {
            settings = try JSONDecoder().decode(AIModelSettings.self, from: Data(contentsOf: file))
        }
        var key = keys.readSecretKey()
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
        if uiTest { _ = try call("create") }
        #endif
        initialized = true
        return AIAssistantOpenResult(
            settings: settings,
            workspacePath: directory.appendingPathComponent("Workspace", isDirectory: true).path
        )
    }

    private func configure(_ config: AIModelSettings, key: String) throws {
        guard let root else { throw AIBackendError(message: "AI 助手尚未准备好") }
        let workspace = root.appendingPathComponent("Workspace", isDirectory: true)
        try FileManager.default.createDirectory(at: workspace, withIntermediateDirectories: true)
        _ = try call("configure", values: ["database": root.appendingPathComponent("sessions.sqlite").path,
            "workspace": workspace.path, "baseUrl": config.baseUrl, "apiKey": key,
            "model": config.model, "policy": config.policy])
    }

    func save(_ config: AIModelSettings, newKey: String) throws {
        guard let url = URL(string: config.baseUrl), url.scheme == "https", url.host != nil,
              url.user == nil, url.password == nil, url.query == nil, url.fragment == nil,
              !config.model.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            throw AIBackendError(message: "请填写有效的 HTTPS API 地址和模型名称")
        }
        let key = newKey.trimmingCharacters(in: .whitespacesAndNewlines)
        if key.isEmpty, url.host != URL(string: settings.baseUrl)?.host {
            throw AIBackendError(message: "更换模型服务商时，请重新填写 API Key")
        }
        let effectiveKey = key.isEmpty ? keys.readSecretKey() : key
        guard !effectiveKey.isEmpty else { throw AIBackendError(message: "请填写 API Key") }
        let previous = settings
        let oldKey = keys.readSecretKey()
        try configure(config, key: effectiveKey) // 正在工作时核心拒绝，不能先覆盖已保存配置。
        do {
            if !key.isEmpty { try keys.saveSecretKey(key) }
            try JSONEncoder().encode(config).write(to: root!.appendingPathComponent("settings.json"), options: .atomic)
            settings = config
        } catch {
            try? keys.saveSecretKey(oldKey)
            try? configure(previous, key: oldKey)
            throw error
        }
    }

    func request(_ operation: String, values: [String: String] = [:], force: Bool = false) throws -> AIResponse {
        try call(operation, values: values.mapValues { $0 as Any }, force: force)
    }

    func send(session: String, text: String, images: [AIImportedFile]) throws -> AIResponse {
        try call("send", values: [
            "session": session,
            "text": text,
            "images": images.map { ["path": $0.relativePath, "mimeType": $0.mimeType] }
        ])
    }

    func importDocument(_ source: URL) throws -> AIImportedFile {
        guard let root else { throw AIBackendError(message: "AI 助手尚未准备好") }
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
}

@MainActor
final class AIAssistantModel: ObservableObject {
    static let shared = AIAssistantModel()
    @Published var sessions: [AISession] = []
    @Published var messages: [AIMessage] = []
    @Published var permissions: [AIPermission] = []
    @Published var questions: [AIQuestion] = []
    @Published var selected = ""
    @Published var settings = AIModelSettings()
    @Published var configured = false
    @Published var error = ""
    @Published var ready = false
    @Published var isSubmitting = false
    @Published var showSettings = false
    @Published var scrollRequest = 0
    @Published private(set) var workspacePath = ""
    var drafts: [String: String] = [:]
    private var pendingAttachments: [String: [AIImportedFile]] = [:]
    private let backend = AIAssistantBackend()
    private var poll: Task<Void, Never>?
    private var transientErrorTask: Task<Void, Never>?
    private var opening = false
    private var visible = false
    var busy: Bool { sessions.first { $0.id == selected }?.busy == true }

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

    func appear() {
        visible = true
        guard !opening else { return }
        opening = true
        Task {
            defer { opening = false }
            do {
                let opened = try await backend.open()
                settings = opened.settings
                workspacePath = opened.workspacePath
                ready = true
                await refresh(force: true)
                if selected.isEmpty, let first = sessions.first { await select(first.id) }
                startPolling()
            } catch { self.error = error.localizedDescription }
        }
    }
    func disappear() { visible = false; poll?.cancel(); poll = nil }
    private func startPolling() {
        poll?.cancel()
        guard visible else { return }
        poll = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(for: .milliseconds(self?.sessions.contains(where: \.busy) == true ? 100 : 500))
                guard !Task.isCancelled, let self else { return }
                await self.refresh(force: false)
            }
        }
    }
    private func refresh(force: Bool) async {
        let target = selected
        do {
            let result = try await backend.request("snapshot", values: ["session": target], force: force)
            guard target == selected, result.changed == true else { return }
            if let value = result.sessions, value != sessions { sessions = value }
            if let value = result.messages, value != messages { messages = value }
            if let value = result.permissions, value != permissions { permissions = value }
            if let value = result.questions, value != questions { questions = value }
            configured = result.configured ?? false
            if let detail = result.error, !detail.isEmpty { error = detail }
        } catch { self.error = error.localizedDescription }
    }
    func select(_ id: String) async {
        selected = id; messages = []; permissions = []; questions = []; error = ""
        await refresh(force: true)
        scrollRequest += 1
    }
    func create() async {
        do {
            let result = try await backend.request("create")
            await select(result.id ?? "")
        } catch { self.error = error.localizedDescription }
    }
    func send(_ text: String) async -> Bool {
        guard configured else { showSettings = true; return false }
        guard !isSubmitting, !busy, !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return false }
        isSubmitting = true
        defer { isSubmitting = false }
        let draftSession = selected
        let attachments = pendingAttachments[draftSession, default: []]
        if selected.isEmpty { await create() }
        guard !selected.isEmpty else { return false }
        do {
            _ = try await backend.send(
                session: selected,
                text: text,
                images: attachments.filter(\.isImage)
            )
            pendingAttachments.removeValue(forKey: draftSession)
            error = ""
            await refresh(force: true)
            scrollRequest += 1
            startPolling()
            return true
        } catch { self.error = error.localizedDescription; return false }
    }
    func action(_ op: String, values: [String: String] = [:]) async {
        let target = selected
        do {
            var values = values; values["session"] = target
            _ = try await backend.request(op, values: values)
            if op == "delete", selected == target { selected = ""; messages = [] }
            await refresh(force: true)
        } catch { self.error = error.localizedDescription }
    }
    func save(_ config: AIModelSettings, key: String) async -> Bool {
        do {
            try await backend.save(config, newKey: key)
            settings = config; error = ""
            await refresh(force: true)
            return true
        } catch { self.error = error.localizedDescription; return false }
    }
    func addAttachment(_ file: AIImportedFile, to session: String) {
        pendingAttachments[session, default: []].append(file)
    }
    func importFile(_ url: URL) async -> AIImportedFile? {
        do { return try await backend.importDocument(url) }
        catch { self.error = error.localizedDescription; return nil }
    }
}
