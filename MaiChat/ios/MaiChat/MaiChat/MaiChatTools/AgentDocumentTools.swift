// 文件作用：为 Agent 提供移动端文档读取与文件交付工具，统一处理访问路径和错误。
// 处理 Agent 导入文档、工作区文件和用户可见路径。
// 返回真实可读取的文件结果，失败时保留原始素材。
import Foundation
import CoreFoundation
import Darwin
import CoreImage
import ImageIO
import MaiChatCore
import Photos
import SwiftUI
import UIKit
import UniformTypeIdentifiers
import Vision

/// 普通 Swift 文档 API：与 Agent 的 PDF 保护选项复用同一份 C++ qpdf 核心，
/// 密码以独立 UTF-8 字节参数传入，绝不拼进工具/业务 JSON 或日志。
enum MaiPdfSecurity {
    enum Failure: Error { case invalidInput, encryptionFailed, invalidResponse }

    static func encrypt(source: URL, output: URL, password: String,
                        replaceExisting: Bool = false) async throws -> URL {
        guard source.isFileURL, output.isFileURL,
              !source.path.contains("\0"), !output.path.contains("\0"),
              AIPdfPasswordRequests.isValid(password, confirmation: password) else {
            throw Failure.invalidInput
        }
        try Task.checkCancellation()
        // 原子写入/替换由核心负责。提交完成即交付成功，不在其后取消时删除已提交文件。
        return try await Task.detached(priority: .userInitiated) {
            var bytes = Array(password.utf8)
            defer { for index in bytes.indices { bytes[index] = 0 } }
            let response = source.path.withCString { input in
                output.path.withCString { destination in
                    bytes.withUnsafeBufferPointer { secret in
                        maiMobilePdfEncrypt(input, destination, secret.baseAddress,
                                            secret.count, replaceExisting ? 1 : 0)
                    }
                }
            }
            guard let response else { throw Failure.encryptionFailed }
            defer { maiMobileAgentFree(response) }
            let data = Data(String(cString: response).utf8)
            guard let value = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  value["ok"] as? Bool == true else { throw Failure.encryptionFailed }
            guard let result = value["output"] as? [String: Any],
                  let path = result["path"] as? String, path.hasPrefix("/"),
                  result["password_protected"] as? Bool == true else { throw Failure.invalidResponse }
            // iOS 的 /var 与 /private/var 可指向同一沙盒；核心返回 canonical 路径，不能用原拼写误报。
            let actual = URL(fileURLWithPath: path)
            guard actual.standardizedFileURL.resolvingSymlinksInPath().path ==
                    output.standardizedFileURL.resolvingSymlinksInPath().path else {
                throw Failure.invalidResponse
            }
            return actual
        }.value
    }

    @MainActor
    static func encryptWithPasswordForm(source: URL, output: URL,
        appState: RemoteIMAppState, replaceExisting: Bool = false) async throws -> URL {
        let password = try await AIPdfPasswordRequests.shared.requestPassword(appState: appState)
        return try await encrypt(source: source, output: output, password: password,
                                 replaceExisting: replaceExisting)
    }
}

private final class AIPDFPageRenderer: UIPrintPageRenderer {
    private let page = CGRect(x: 0, y: 0, width: 595, height: 842)

    override var paperRect: CGRect { page }
    override var printableRect: CGRect { page.insetBy(dx: 36, dy: 36) }
}

/// 普通 UIKit 排版入口与 generate_pdf 宿主共用；仅生成未保护的输入 PDF，
/// 打开密码保护由独立 C++ 核心负责，不能用重新绘制图片代替加密。
@MainActor
enum MaiPdfRenderer {
    enum Failure: Error { case invalidInput, invalidOutput }
    static func render(html: String, output: URL) throws -> [String: Int] {
        guard !html.isEmpty, html.utf8.count <= 10 * 1024 * 1024,
              output.isFileURL, output.pathExtension.lowercased() == "pdf",
              !FileManager.default.fileExists(atPath: output.path) else { throw Failure.invalidInput }
        let formatter = UIMarkupTextPrintFormatter(markupText: html)
        let renderer = AIPDFPageRenderer()
        renderer.addPrintFormatter(formatter, startingAtPageAt: 0)
        let pages = renderer.numberOfPages
        guard pages > 0, pages <= 200 else { throw Failure.invalidInput }
        let data = UIGraphicsPDFRenderer(bounds: renderer.paperRect).pdfData { context in
            for page in 0..<pages {
                context.beginPage()
                renderer.drawPage(at: page, in: renderer.paperRect)
            }
        }
        guard data.starts(with: Data("%PDF-".utf8)), data.count <= 100 * 1024 * 1024 else {
            throw Failure.invalidOutput
        }
        try data.write(to: output, options: .atomic)
        return ["pages": pages, "bytes": data.count]
    }
}

/// 私有 PDF 密码交接：与账号密码无关，只把安全表单输入一次性交给本进程加密核心。
/// request_id/status 可查询；密码不落盘、不进公开工具参数、Agent 历史或诊断日志。
@MainActor
final class AIPdfPasswordRequests: ObservableObject {
    static let shared = AIPdfPasswordRequests()
    struct Request: Identifiable {
        let id: UUID
        let accountIdentity: String
    }
    private struct Record {
        let request: Request
        var state = "pending"
        var password: String?
    }
    @Published private(set) var presentation: Request?
    private var records: [UUID: Record] = [:]
    private var order: [UUID] = []

    nonisolated static func isValid(_ password: String, confirmation: String) -> Bool {
        // PDF R6 上限按 UTF-8 字节计算，不能 trim/截断后悄悄改变用户真正输入的密码。
        !password.isEmpty && password == confirmation && password.utf8.count <= 127 &&
            !password.unicodeScalars.contains(where: { $0.value == 0 })
    }

    func begin(id: UUID, appState: RemoteIMAppState) throws {
        guard appState.hasUnifiedAccount, AIAccountPasswordRequests.shared.presenterActive,
              UIApplication.shared.applicationState == .active,
              AIAccountPasswordRequests.shared.presentation == nil else {
            throw PromptError.unavailable
        }
        if let record = records[id] {
            guard record.request.accountIdentity == appState.remoteDiagnosticsIdentity else {
                throw PromptError.unavailable
            }
            return
        }
        guard presentation == nil else { throw PromptError.unavailable }
        let request = Request(id: id, accountIdentity: appState.remoteDiagnosticsIdentity)
        records[id] = Record(request: request)
        order.append(id)
        while order.count > 20 { records.removeValue(forKey: order.removeFirst()) }
        presentation = request
        Task { @MainActor in
            try? await Task.sleep(for: .seconds(120))
            // 系统未展示、调用方消失或用户不操作都不能永久持有密码/悬挂 Agent。
            cancel(id)
        }
    }

    func complete(_ id: UUID, password: String, confirmation: String, appState: RemoteIMAppState) -> Bool {
        guard Self.isValid(password, confirmation: confirmation), var record = records[id],
              record.state == "pending", appState.hasUnifiedAccount,
              record.request.accountIdentity == appState.remoteDiagnosticsIdentity else {
            cancel(id)
            return false
        }
        record.password = password
        record.state = "ready"
        records[id] = record
        presentation = nil
        return true
    }

    func poll(_ id: UUID, appState: RemoteIMAppState) -> [String: String] {
        guard var record = records[id], appState.hasUnifiedAccount,
              record.request.accountIdentity == appState.remoteDiagnosticsIdentity else {
            cancel(id)
            return ["status": "canceled"]
        }
        if record.state == "ready", let password = record.password {
            // 只有私有回调读取一次；取走后清除宿主持有的引用，后续 poll 不再返回密码。
            record.password = nil
            record.state = "canceled"
            records[id] = record
            return ["status": "ready", "password": password]
        }
        return ["status": record.state]
    }

    func dismissed(_ id: UUID) {
        // 用户确认后收起 sheet 时保留 ready，直到 C++ 一次性取走；取消页才清空 pending。
        if records[id]?.state == "pending" { cancel(id) }
    }

    func cancel(_ id: UUID) {
        if var record = records[id] {
            record.password = nil
            record.state = "canceled"
            records[id] = record
        }
        if presentation?.id == id { presentation = nil }
    }

    func cancelAll() {
        for id in order { cancel(id) }
    }

    /// 普通 Swift 调用与 Agent 私有桥共用同一表单及校验；Task 取消关闭精确 request。
    func requestPassword(appState: RemoteIMAppState) async throws -> String {
        let id = UUID()
        try begin(id: id, appState: appState)
        return try await withTaskCancellationHandler {
            do {
                while true {
                    try Task.checkCancellation()
                    let value = poll(id, appState: appState)
                    if value["status"] == "ready", let password = value["password"] {
                        try Task.checkCancellation()
                        return password
                    }
                    if value["status"] == "canceled" { throw CancellationError() }
                    try await Task.sleep(for: .milliseconds(100))
                }
            } catch {
                cancel(id)
                throw error
            }
        } onCancel: {
            Task { @MainActor in self.cancel(id) }
        }
    }

    enum PromptError: Error { case unavailable }
}

/// 本地 PDF 打开密码双输入页，不修改账号密码，不经问答工具，也不把输入保存为草稿。
struct AIPdfPasswordView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @Environment(\.dismiss) private var dismiss
    let request: AIPdfPasswordRequests.Request
    @State private var password = ""
    @State private var confirmation = ""

    var body: some View {
        Form {
            Section {
                SecureField("PDF 打开密码", text: $password)
                    .textContentType(.newPassword)
                    .accessibilityIdentifier("pdf-password-new")
                SecureField("再次输入 PDF 密码", text: $confirmation)
                    .textContentType(.newPassword)
                    .accessibilityIdentifier("pdf-password-confirm")
            } footer: {
                Text("仅设置本次 PDF 的打开密码，不修改 MaiChat 登录密码。密码最多 127 个 UTF-8 字节，不会发给 AI 或保存在聊天记录中。")
            }
            Button("保护 PDF") {
                if AIPdfPasswordRequests.shared.complete(request.id, password: password,
                        confirmation: confirmation, appState: appState) {
                    password = ""
                    confirmation = ""
                    dismiss()
                }
            }
            .disabled(!AIPdfPasswordRequests.isValid(password, confirmation: confirmation))
            .accessibilityIdentifier("pdf-password-submit")
        }
        .navigationTitle("PDF 密码保护")
        // 与账号安全表单统一用行内标题，保留半透明导航栏下清晰的标题对比度。
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) {
                Button("取消") {
                    AIPdfPasswordRequests.shared.cancel(request.id)
                    dismiss()
                }.accessibilityIdentifier("pdf-password-cancel")
            }
        }
        .onDisappear {
            password = ""
            confirmation = ""
            AIPdfPasswordRequests.shared.dismissed(request.id)
        }
    }
}

@MainActor
extension AIMobileHostToolProvider {
    func pdfPasswordRequest(_ arguments: [String: Any]) -> AIMaiChatHostToolExecution {
        // 此名字只供 C++ 密码 provider 调用，绝不注册为 Agent 工具；ready 响应仅被加密核心消费。
        guard Set(arguments.keys).isSubset(of: ["action", "request_id"]),
              let action = arguments["action"] as? String,
              let raw = arguments["request_id"] as? String, let id = UUID(uuidString: raw),
              let appState else {
            return .failure(code: "invalid_input", message: "invalid private PDF password request")
        }
        if action == "cancel" {
            AIPdfPasswordRequests.shared.cancel(id)
            return Self.jsonSuccess(["status": "canceled"])
        }
        if action == "poll" { return Self.jsonSuccess(AIPdfPasswordRequests.shared.poll(id, appState: appState)) }
        guard action == "begin" else {
            return .failure(code: "invalid_input", message: "invalid private PDF password action")
        }
        do {
            try AIPdfPasswordRequests.shared.begin(id: id, appState: appState)
            return Self.jsonSuccess(["status": "pending"])
        } catch {
            return .failure(code: "not_configured", message: "open MaiChat and close other secure forms first")
        }
    }

    func decodeLegacyText(_ arguments: [String: Any]) -> AIMaiChatHostToolExecution {
        guard let base64 = arguments["base64"] as? String,
              let bytes = Data(base64Encoded: base64), bytes.count <= 8 * 1024 * 1024,
              let requested = arguments["encoding"] as? String else {
            return .failure(code: "invalid_input", message: "valid base64 text and encoding are required")
        }
        guard AIAssistantTextDecoder.supports(requested) else {
            return .failure(code: "invalid_input", message: "unsupported file encoding: \(requested)")
        }
        guard let text = AIAssistantTextDecoder.decode(bytes, encodingName: requested) else {
            return .failure(code: "invalid_input", message: "file could not be decoded as \(requested)")
        }
        return .success(text)
    }

    func generatePDF(_ arguments: [String: Any]) -> AIMaiChatHostToolExecution {
        guard let html = arguments["html"] as? String, !html.isEmpty,
              let outputPath = arguments["output_path"] as? String, !outputPath.isEmpty else {
            return .failure(code: "invalid_input", message: "HTML and PDF output path are required")
        }
        guard let destination = AIAssistantPathPolicy.resolve(
                  outputPath, workspacePath: AIAssistantModel.shared.workspacePath),
              destination.path.lowercased().hasSuffix(".pdf"),
              !FileManager.default.fileExists(atPath: destination.path) else {
            return .failure(code: "invalid_input", message: "PDF output must be a new file in the App container")
        }
        do {
            return Self.jsonSuccess(try MaiPdfRenderer.render(html: html, output: destination))
        } catch {
            return .failure(code: "internal", message: "iOS could not render a valid PDF")
        }
    }
}
