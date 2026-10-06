import Foundation
import Darwin
import CoreImage
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
struct AIImagePreview: Identifiable, Equatable {
    let id: UUID
    let filePath: String
    let imageSize: CGSize
    let sourceFrame: CGRect?

    init(filePath: String, imageSize: CGSize = .zero, sourceFrame: CGRect? = nil) {
        id = UUID()
        self.filePath = filePath
        self.imageSize = imageSize
        self.sourceFrame = sourceFrame
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
    private let keys = KeychainSecretStore(account: "ai-assistant-api-key")

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
        let appRootPath = AIAssistantPathPolicy.appRoot(workspacePath: workspace.path)
        _ = try call("configure", values: ["database": root.appendingPathComponent("sessions.sqlite").path,
            "workspace": workspace.path, "baseUrl": config.baseUrl, "apiKey": key,
            "model": config.model, "policy": config.policy, "appRoot": appRootPath,
            "temporaryDirectory": FileManager.default.temporaryDirectory.path,
            "cacheDirectory": try FileManager.default.url(for: .cachesDirectory,
                in: .userDomainMask, appropriateFor: nil, create: true).path,
            "rvmModelPath": Bundle.main.url(
                forResource: "rvm_mobilenetv3_fp32", withExtension: "onnx",
                subdirectory: "MaiAgentModels")?.path ?? ""])
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

    func snapshot(session: String, limit: Int, force: Bool) throws -> AIResponse {
        try call("snapshot", values: ["session": session, "messageLimit": limit], force: force)
    }

    func messagesPage(session: String, beforeId: String, limit: Int) throws -> [AIMessage] {
        try call("messages_page", values: ["session": session, "before": beforeId,
                                            "limit": limit]).messages ?? []
    }

    func send(session: String, text: String, images: [AIImportedFile],
              videos: [AIImportedFile]) throws -> AIResponse {
        try call("send", values: [
            "session": session,
            "text": text,
            "images": images.map { ["path": $0.relativePath, "mimeType": $0.mimeType] },
            "videos": videos.map { ["path": $0.relativePath, "mimeType": $0.mimeType] }
        ])
    }

    func suggestReplies(messages: [[String: String]]) throws -> AIReplySuggestions {
        _ = try open()
        let response = try call("suggest_replies", values: ["messages": messages])
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

    func importVideo(_ source: URL) throws -> AIImportedFile {
        guard let root else { throw AIBackendError(message: "AI 助手尚未准备好") }
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
    @Published var historyLoaded = false
    @Published private(set) var hasOlderMessages = false
    @Published var isSubmitting = false
    @Published var showSettings = false
    @Published var scrollRequest = 0
    @Published private(set) var workspacePath = ""
    @Published var previewImage: AIImagePreview?
    var drafts: [String: String] = [:]
    private var pendingAttachments: [String: [AIImportedFile]] = [:]
    private let backend = AIAssistantBackend()
    private var poll: Task<Void, Never>?
    private var transientErrorTask: Task<Void, Never>?
    private var initialPreparation: Task<Void, Never>?
    private var appearingTask: Task<Void, Never>?
    private var pendingInitialPage: (session: String, newestID: String)?
    private var loadingOlderMessages = false
    private var visible = false
    private var pageActive = true
    var busy: Bool { sessions.first { $0.id == selected }?.busy == true }

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
    #endif

    func suggestReplies(messages: [[String: String]]) async throws -> AIReplySuggestions {
        try await backend.suggestReplies(messages: messages)
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
                let opened = try await backend.open()
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
            appearingTask = nil
        }
    }
    func disappear() { visible = false; poll?.cancel(); poll = nil }
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
                let interval: Duration = self?.pageActive == true
                    ? .milliseconds(busy ? 100 : 500)
                    : .milliseconds(busy ? 500 : 2_000)
                try? await Task.sleep(for: interval)
                guard !Task.isCancelled, let self else { return }
                await self.refresh(force: false)
            }
        }
    }
    private func refresh(force: Bool) async {
        let target = selected
        do {
            let result = try await backend.snapshot(session: target, limit: 30, force: force)
            guard target == selected, result.changed == true else { return }
            if let value = result.sessions, value != sessions { sessions = value }
            if let value = result.messages {
                let knownIDs = Set(messages.map(\.id))
                let newFailure = value.contains { message in
                    !knownIDs.contains(message.id) && message.parts.contains { part in
                        part.kind == "text" &&
                        ((part.text?.hasPrefix("Video task failed. ") == true) ||
                         (part.text?.hasPrefix("Image task failed. ") == true))
                    }
                }
                mergeMessages(value)
                if newFailure {
                    showTransientError("生成任务失败，详情见对话。", duration: .seconds(8))
                }
            }
            if let value = result.permissions, value != permissions { permissions = value }
            if let value = result.questions, value != questions { questions = value }
            configured = result.configured ?? false
            if let detail = result.error, !detail.isEmpty { error = detail }
        } catch { self.error = error.localizedDescription }
    }

    private func showNewestMessage(for session: String) async throws -> String? {
        let latest = try await backend.messagesPage(session: session, beforeId: "", limit: 1)
        guard selected == session else { return nil }
        messages = Array(latest.reversed())
        historyLoaded = true
        logAIHistoryEvent("first-page session=\(session) count=\(latest.count) newest=\(latest.first?.id ?? "none") role=\(latest.first?.role ?? "none") parts=\(latest.first?.parts.count ?? 0)")
        return latest.first?.id
    }

    private func completeInitialPage(for session: String, newestID: String) async {
        do {
            let earlier = try await backend.messagesPage(session: session,
                                                         beforeId: newestID, limit: 29)
            guard selected == session else { return }
            mergeMessages(earlier)
            hasOlderMessages = earlier.count == 29
            logAIHistoryEvent("older-page session=\(session) added=\(earlier.count) first=\(messages.first?.id ?? "none") last=\(messages.last?.id ?? "none")")
            await refresh(force: true)
        } catch {
            logAIHistoryEvent("older-page error=\(error.localizedDescription)")
            self.error = error.localizedDescription
        }
    }

    private func mergeMessages(_ page: [AIMessage]) {
        // A snapshot updates only its bounded newest page; keep older pages the
        // user has already opened and replace matching rows by their stable ID.
        guard !page.isEmpty else { return }
        var merged = messages
        var changed = false
        for message in page {
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
            }
        }
        if changed { messages = merged }
    }

    func loadOlderMessages() async {
        guard !loadingOlderMessages, hasOlderMessages,
              let oldest = messages.first?.id, !selected.isEmpty else { return }
        loadingOlderMessages = true
        defer { loadingOlderMessages = false }
        let session = selected
        do {
            let page = try await backend.messagesPage(session: session,
                                                      beforeId: oldest, limit: 30)
            guard selected == session else { return }
            mergeMessages(page)
            hasOlderMessages = page.count == 30
        } catch { self.error = error.localizedDescription }
    }

    func select(_ id: String) async {
        selected = id; messages = []; permissions = []; questions = []; error = ""
        historyLoaded = false
        hasOlderMessages = false
        pendingInitialPage = nil
        do {
            if let newestID = try await showNewestMessage(for: id) {
                await completeInitialPage(for: id, newestID: newestID)
            } else {
                await refresh(force: true)
            }
        } catch { self.error = error.localizedDescription }
    }
    func create() async {
        do {
            let result = try await backend.request("create")
            await select(result.id ?? "")
        } catch { self.error = error.localizedDescription }
    }
    func send(
        _ text: String,
        images: [AIImportedFile] = [],
        videos: [AIImportedFile] = [],
        expectedSession: String? = nil
    ) async -> Bool {
        guard configured else { showSettings = true; return false }
        let draftSession = selected
        guard expectedSession == nil || expectedSession == draftSession else { return false }
        let attachments = pendingAttachments[draftSession, default: []] + images + videos
        guard !isSubmitting, !busy,
              !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || !attachments.isEmpty else {
            return false
        }
        isSubmitting = true
        defer { isSubmitting = false }
        if attachments.contains(where: \.isImage),
           settings.model.trimmingCharacters(in: .whitespacesAndNewlines)
               .caseInsensitiveCompare("glm-5.3") == .orderedSame {
            showTransientError("glm-5.3 仅支持文本，请先切换到 glm-5.3-flash")
            return false
        }
        if selected.isEmpty {
            do {
                let created = try await backend.request("create")
                selected = created.id ?? ""
                messages = []
                permissions = []
                questions = []
                historyLoaded = false
                hasOlderMessages = false
            } catch {
                self.error = error.localizedDescription
                return false
            }
        }
        guard !selected.isEmpty else { return false }
        do {
            _ = try await backend.send(
                session: selected,
                text: text,
                images: attachments.filter(\.isImage),
                videos: attachments.filter { $0.mimeType.hasPrefix("video/") }
            )
            pendingAttachments.removeValue(forKey: draftSession)
            error = ""
            await refresh(force: true)
            historyLoaded = !messages.isEmpty
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
            if op == "delete", selected == target {
                selected = ""; messages = []; historyLoaded = true
            }
            if op == "clear", selected == target {
                messages = []; historyLoaded = true
            }
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

    func importVideoFile(_ url: URL) async -> AIImportedFile? {
        do { return try await backend.importVideo(url) }
        catch { self.error = error.localizedDescription; return nil }
    }
}
