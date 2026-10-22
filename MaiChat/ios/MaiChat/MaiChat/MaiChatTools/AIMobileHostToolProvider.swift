// 文件作用：注册创作中心可调用的手机宿主工具，并把工具请求转发到 iOS 能力实现。
// 阅读边界：工具参数、权限和返回值在这里校验；界面控制由对应播放器及页面执行。
// 向移动 Agent 注册相册、IM、媒体和系统权限工具。
import Foundation
import CoreFoundation
import Darwin
import CoreImage
import ImageIO
import Metal
import MessageUI
import MaiChatCore
import Photos
import SwiftUI
import UIKit
import UniformTypeIdentifiers
import Vision

struct AIMaiChatHostToolExecution: Sendable, Equatable {
    let output: String?
    let errorCode: String?
    let errorMessage: String?

    static func success(_ output: String) -> Self {
        Self(output: output, errorCode: nil, errorMessage: nil)
    }

    static func failure(code: String, message: String) -> Self {
        Self(output: nil, errorCode: code, errorMessage: message)
    }
}

@MainActor
final class AIMobileHostToolProvider {
    static let shared = AIMobileHostToolProvider()

    weak var appState: RemoteIMAppState?
    private var smsDelegate: AITextMessageDelegate?

    /// 校验 Agent 工具名与参数，再分派到对应 iOS 宿主能力。
    /// 所有失败分支返回明确错误码；播放器和媒体工具不得把仅受理误报为完成。
    func execute(name: String, argumentsJSON: String) async -> AIMaiChatHostToolExecution {
        guard let arguments = Self.parseArguments(argumentsJSON) else {
            return .failure(code: "invalid_input", message: "arguments must be a JSON object")
        }

        switch name {
        case "mobile_export_diagnostics": return await exportDiagnostics()
        case "mobile_take_photo":
            do {
                let model = AIAssistantModel.shared
                let path = try await AIMobileLocalMediaRequests.shared.requestCamera(
                    in: model.selected, workspacePath: model.workspacePath)
                return Self.jsonSuccess(["path": path, "mime_type": "image/jpeg"])
            } catch let error as AIMobileLocalMediaRequestError {
                return .failure(code: Self.localMediaErrorCode(error),
                                message: error.localizedDescription)
            } catch {
                return .failure(code: "internal", message: error.localizedDescription)
            }
        case "mobile_play_local_music":
            let path = Self.string(arguments, key: "path")
            if path.isEmpty {
                do {
                    let fileName = try await AIMobileLocalMediaRequests.shared.requestMusicSelection()
                    return Self.jsonSuccess(["opened": true, "file_name": fileName])
                } catch let error as AIMobileLocalMediaRequestError {
                    return .failure(code: Self.localMediaErrorCode(error),
                                    message: error.localizedDescription)
                } catch {
                    return .failure(code: "internal", message: error.localizedDescription)
                }
            }
            let workspace = URL(fileURLWithPath: AIAssistantModel.shared.workspacePath,
                                isDirectory: true).resolvingSymlinksInPath()
            guard AIMobileLocalMediaRequests.shared.presenterActive,
                  !AIAssistantModel.shared.workspacePath.isEmpty,
                  ["mp3", "m4a", "wav"].contains(URL(fileURLWithPath: path).pathExtension.lowercased()),
                  let file = AIAssistantPathPolicy.resolve(
                    path, workspacePath: AIAssistantModel.shared.workspacePath),
                  file.resolvingSymlinksInPath().path.hasPrefix(workspace.path + "/"),
                  FileManager.default.fileExists(atPath: file.path) else {
                return .failure(code: "invalid_input", message: "select an accessible MP3, M4A, or WAV")
            }
            do {
                try MaiLocalMusicPlayer.shared.openLocalFile(file)
                AIMobileLocalMediaRequests.shared.showMusic()
                return Self.jsonSuccess(["opened": true, "file_name": file.lastPathComponent])
            } catch {
                return .failure(code: "unavailable", message: error.localizedDescription)
            }
        case "mobile_request_permission": return await requestSystemPermission(arguments)
        case "mobile_get_location": return await getCurrentLocation()
        case "mobile_list_contacts": return await listSystemContacts(arguments)
        case "mobile_get_contact": return await getSystemContact(arguments)
        case "mobile_dial_phone": return await dialPhone(arguments)
        case "mobile_compose_sms": return composeSMS(arguments)
        case "mobile_decode_text": return decodeLegacyText(arguments)
        case "generate_pdf": return generatePDF(arguments)
        case "mobile_list_photos": return await listPhotos(arguments)
        case "mobile_list_albums": return await listAlbums()
        case "mobile_read_photo": return await readPhoto(arguments)
        case "mobile_export_photo_original": return await exportPhotoOriginal(arguments)
        case "mobile_export_media_original": return await exportPhotoOriginal(arguments)
        case "mobile_save_image": return await saveImage(arguments)
        case "mobile_save_video": return await saveVideo(arguments)
        case "mobile_ssh_password": return await requestSSHPassword(arguments)
        case "mobile_ssh_trust_host": return await trustSSHHost(arguments)
        case "mobile_ark_service_config":
            // Ark 资产登记和上传签名使用当前账号会话，素材字节仍由 C++ 直传 OSS。
            guard let access = try? await MaiChatAuthClient.shared.mediaServiceAccessIfAvailable()
            else { return .failure(code: "not_configured", message: "Account login is required") }
            return Self.jsonSuccess(["base_url": access.baseURL, "token": access.accountToken])
        case "mobile_gpu_info": return gpuInfo()
        case "mobile_transform_image": return await transformImage(arguments)
        case "mobile_beautify_image":
            var edit = arguments
            edit["operation"] = "beautify"
            return await transformImage(edit)
        case "mobile_detect_faces": return await detectFaces(arguments)
        case "mobile_segment_person": return await segmentPerson(arguments)
        case "mobile_preview_image": return previewImage(arguments)
        case "mobile_photos_add_to_album": return await addPhotosToAlbum(arguments)
        case "ark_api_key":
            return Self.jsonSuccess(["key": AICloudCredentialStore.shared.apiKey("ark")])
        case "glm_api_key":
            return Self.jsonSuccess(["key": AICloudCredentialStore.shared.apiKey("glm")])
        case "minimax_api_key":
            return Self.jsonSuccess(["key": AICloudCredentialStore.shared.apiKey("minimax")])
        case "wan_credentials":
            return Self.jsonSuccess([
                "key": AICloudCredentialStore.shared.apiKey("wan"),
                "workspace_id": AICloudCredentialStore.shared.workspaceId()
            ])
        default: break
        }
        guard let appState else {
            return .failure(code: "not_configured", message: "the MaiChat host is unavailable")
        }

        switch name {
        case "maichat_play_video":
            let path = Self.string(arguments, key: "path")
            let startSeconds = arguments["start_seconds"] as? Double ?? 0
            guard startSeconds.isFinite, startSeconds >= 0,
                  startSeconds <= Double(Int64.max) / 1_000_000 else {
                return .failure(code: "invalid_input", message: "start_seconds must be a nonnegative source-media time")
            }
            guard let source = AIAssistantPathPolicy.resolve(
                path, workspacePath: AIAssistantModel.shared.workspacePath
            ), FileManager.default.fileExists(atPath: source.path) else {
                return .failure(code: "not_found", message: "video must be an accessible local file")
            }
            appState.agentVideoPresentation = MaiFfplayPresentation(
                path: source.path, startSeconds: startSeconds)
            AppDiagnosticLog.shared.record(level: .info, category: "ffplay",
                event: "video-open", fields: ["start_seconds": String(startSeconds)])
            return Self.jsonSuccess(["opened": true, "start_seconds": startSeconds])
        case "maichat_video_command":
            let action = Self.string(arguments, key: "action")
            let percent = arguments["percent"] as? Double
            let speed = arguments["speed"] as? Double
            let seconds = arguments["seconds"] as? Double
            guard MaiFfplayMobilePlayer.command(action, percent: percent,
                                                speed: speed, seconds: seconds) else {
                let reason = MaiFfplayMobilePlayer.unavailableReason
                AppDiagnosticLog.shared.record(level: .warning, category: "ffplay",
                    event: "video-command-unavailable", fields: [
                        "action": action, "reason": reason
                    ])
                return .failure(code: "unavailable", message: reason)
            }
            AppDiagnosticLog.shared.record(level: .info, category: "ffplay",
                event: "video-command-accepted", fields: ["action": action])
            return Self.jsonSuccess(["accepted": true])
        case "maichat_list_contacts":
            return Self.listContacts(appState: appState, arguments: arguments)
        case "maichat_request_friend":
            return await requestFriend(appState: appState, arguments: arguments)
        case "maichat_set_nickname":
            return await setNickname(appState: appState, arguments: arguments)
        case "maichat_list_conversations":
            return Self.listConversations(appState: appState, arguments: arguments)
        case "maichat_get_messages":
            return await getMessages(appState: appState, arguments: arguments)
        case "maichat_search_messages":
            return await searchMessages(appState: appState, arguments: arguments)
        case "maichat_get_unread_summary":
            return Self.unreadSummary(appState: appState)
        case "maichat_send_text":
            return await sendText(appState: appState, arguments: arguments)
        case "maichat_send_media":
            return await sendMedia(appState: appState, arguments: arguments)
        case "maichat_reply_message":
            return await replyMessage(appState: appState, arguments: arguments)
        case "maichat_broadcast_text":
            return await broadcastText(appState: appState, arguments: arguments)
        default:
            return .failure(code: "invalid_input", message: "unknown MaiChat host tool")
        }
    }

    private static func parseArguments(_ value: String) -> [String: Any]? {
        guard let data = value.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data),
              let arguments = object as? [String: Any] else { return nil }
        return arguments
    }

    static func boundedLimit(_ arguments: [String: Any], fallback: Int = 50) -> Int {
        min(max(arguments["limit"] as? Int ?? fallback, 1), 200)
    }

    static func string(_ arguments: [String: Any], key: String) -> String {
        (arguments[key] as? String)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
    }

    private static func localMediaErrorCode(_ error: AIMobileLocalMediaRequestError) -> String {
        switch error {
        case .unavailable: return "unavailable"
        case .canceled: return "canceled"
        case .permissionDenied: return "permission_denied"
        case .failed: return "internal"
        }
    }

    /// 保留号码中的数字与可选国际区号；拒绝扩展号、URL 和拨号控制字符。
    static func normalizedPhoneNumber(_ value: String) -> String? {
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty,
              trimmed.unicodeScalars.allSatisfy({
                  CharacterSet(charactersIn: "+0123456789 ()-").contains($0)
              }) else { return nil }
        let number = trimmed.replacingOccurrences(of: " ", with: "")
            .replacingOccurrences(of: "(", with: "")
            .replacingOccurrences(of: ")", with: "")
            .replacingOccurrences(of: "-", with: "")
        let digits = number.hasPrefix("+") ? String(number.dropFirst()) : number
        guard (3...15).contains(digits.utf8.count),
              digits.utf8.allSatisfy({ $0 >= 48 && $0 <= 57 }) else { return nil }
        return number
    }

    /// 只打开系统拨号确认。open 返回成功不代表用户确认或通话接通。
    func dialPhone(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        guard let number = Self.normalizedPhoneNumber(Self.string(arguments, key: "phone_number")),
              let url = URL(string: "tel:\(number)") else {
            return .failure(code: "invalid_input", message: "A valid phone number is required")
        }
        guard UIApplication.shared.applicationState == .active,
              UIApplication.shared.canOpenURL(url) else {
            return .failure(code: "unavailable", message: "Phone dialing is unavailable on this device")
        }
        let opened = await withCheckedContinuation { continuation in
            UIApplication.shared.open(url) { continuation.resume(returning: $0) }
        }
        guard opened else {
            return .failure(code: "unavailable", message: "Could not open the Phone confirmation")
        }
        return Self.jsonSuccess(["status": "phone_confirmation_opened", "phone_number": number])
    }

    /// 只预填系统短信编辑器；发送或取消始终由用户在系统界面决定。
    func composeSMS(_ arguments: [String: Any]) -> AIMaiChatHostToolExecution {
        guard let recipients = arguments["recipients"] as? [String],
              (1...10).contains(recipients.count),
              let body = arguments["body"] as? String,
              !body.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
              body.utf8.count <= 4_000 else {
            return .failure(code: "invalid_input", message: "Recipients and message body are required")
        }
        let numbers = recipients.compactMap(Self.normalizedPhoneNumber)
        guard numbers.count == recipients.count else {
            return .failure(code: "invalid_input", message: "A recipient phone number is invalid")
        }
        guard UIApplication.shared.applicationState == .active,
              MFMessageComposeViewController.canSendText(),
              smsDelegate == nil,
              let presenter = activePresenter() else {
            return .failure(code: "unavailable", message: "SMS composer is unavailable on this device")
        }
        let composer = MFMessageComposeViewController()
        composer.recipients = numbers
        composer.body = body
        let delegate = AITextMessageDelegate { [weak self] in self?.smsDelegate = nil }
        smsDelegate = delegate
        composer.messageComposeDelegate = delegate
        presenter.present(composer, animated: true)
        return Self.jsonSuccess(["status": "sms_composer_opened", "recipients": numbers])
    }

    private func activePresenter() -> UIViewController? {
        guard let scene = UIApplication.shared.connectedScenes
            .compactMap({ $0 as? UIWindowScene })
            .first(where: { $0.activationState == .foregroundActive }),
              let window = scene.windows.first(where: { $0.isKeyWindow }),
              var view = window.rootViewController else { return nil }
        while let presented = view.presentedViewController { view = presented }
        return view
    }

    private func exportDiagnostics() async -> AIMaiChatHostToolExecution {
        let workspacePath = AIAssistantModel.shared.workspacePath
        guard !workspacePath.isEmpty else {
            return .failure(code: "not_configured", message: "Agent workspace is unavailable")
        }
        let fileName = "maichat-diagnostics-\(UUID().uuidString.lowercased()).txt"
        guard let destination = AIAssistantPathPolicy.resolve(
            fileName, workspacePath: workspacePath
        ), !FileManager.default.fileExists(atPath: destination.path) else {
            return .failure(code: "internal", message: "Could not create a new diagnostic file")
        }
        do {
            // 复用原有脱敏快照，再复制到 Agent 工作区；模型只收到路径和大小。
            let source = try await AppDiagnosticLog.shared.makeExportSnapshot()
            let workspace = URL(fileURLWithPath: workspacePath, isDirectory: true)
            try FileManager.default.createDirectory(
                at: workspace, withIntermediateDirectories: true
            )
            try FileManager.default.copyItem(at: source, to: destination)
            let attributes = try FileManager.default.attributesOfItem(atPath: destination.path)
            let bytes = (attributes[.size] as? NSNumber)?.intValue ?? 0
            guard bytes > 0, bytes <= 20 * 1024 * 1024 else {
                try? FileManager.default.removeItem(at: destination)
                return .failure(code: "internal", message: "Diagnostic export is empty or too large")
            }
            return Self.jsonSuccess([
                "path": fileName, "mime_type": "text/plain", "bytes": bytes
            ])
        } catch {
            try? FileManager.default.removeItem(at: destination)
            return .failure(code: "internal", message: error.localizedDescription)
        }
    }

    private func gpuInfo() -> AIMaiChatHostToolExecution {
        guard let device = MTLCreateSystemDefaultDevice() else {
            return Self.jsonSuccess(["available": false])
        }
        var output: [String: Any] = [
            "available": true,
            "name": device.name,
            "unified_memory": device.hasUnifiedMemory,
            "app_allocated_bytes": device.currentAllocatedSize
        ]
        if #available(iOS 16.0, *) {
            output["recommended_working_set_bytes"] = device.recommendedMaxWorkingSetSize
        }
        return Self.jsonSuccess(output)
    }

    static func jsonSuccess(_ object: [String: Any]) -> AIMaiChatHostToolExecution {
        guard JSONSerialization.isValidJSONObject(object),
              let data = try? JSONSerialization.data(withJSONObject: object),
              let output = String(data: data, encoding: .utf8) else {
            return .failure(code: "internal", message: "failed to encode MaiChat tool output")
        }
        return .success(output)
    }


}

@MainActor
private final class AITextMessageDelegate: NSObject, @preconcurrency MFMessageComposeViewControllerDelegate {
    private let onFinish: () -> Void

    init(onFinish: @escaping () -> Void) { self.onFinish = onFinish }

    func messageComposeViewController(_ controller: MFMessageComposeViewController,
                                      didFinishWith result: MessageComposeResult) {
        controller.dismiss(animated: true) { self.onFinish() }
    }
}

final class AIMaiChatHostToolCallbackContext: @unchecked Sendable {}

private final class AIMaiChatHostToolResponseBox: @unchecked Sendable {
    private let lock = NSLock()
    private var value = ""

    func store(_ value: String) {
        lock.lock()
        self.value = value
        lock.unlock()
    }

    func load() -> String {
        lock.lock()
        let result = value
        lock.unlock()
        return result
    }
}

private func aiMaiChatHostToolEnvelope(_ result: AIMaiChatHostToolExecution) -> String {
    let object: [String: Any]
    if let output = result.output {
        object = ["ok": true, "output": output]
    } else {
        object = [
            "ok": false,
            "errorCode": result.errorCode ?? "internal",
            "error": result.errorMessage ?? "the MaiChat host tool failed",
        ]
    }
    guard let data = try? JSONSerialization.data(withJSONObject: object),
          let value = String(data: data, encoding: .utf8) else {
        return #"{"ok":false,"errorCode":"internal","error":"failed to encode host response"}"#
    }
    return value
}

private func aiMaiChatOwnedCString(_ value: String) -> UnsafePointer<CChar>? {
    value.withCString { pointer in
        guard let copied = strdup(pointer) else { return nil }
        return UnsafePointer(copied)
    }
}

let aiMaiChatHostToolHandler: @convention(c) (
    UnsafeMutableRawPointer?,
    UnsafePointer<CChar>?,
    UnsafePointer<CChar>?
) -> UnsafePointer<CChar>? = { context, toolName, argumentsJSON in
    guard context != nil, let toolName, let argumentsJSON else {
        return aiMaiChatOwnedCString(
            #"{"ok":false,"errorCode":"invalid_input","error":"invalid host callback input"}"#
        )
    }
    guard !Thread.isMainThread else {
        return aiMaiChatOwnedCString(
            #"{"ok":false,"errorCode":"internal","error":"host callback ran on the main thread"}"#
        )
    }
    _ = Unmanaged<AIMaiChatHostToolCallbackContext>
        .fromOpaque(context!)
        .takeUnretainedValue()
    let name = String(cString: toolName)
    let arguments = String(cString: argumentsJSON)
    let response = AIMaiChatHostToolResponseBox()
    let finished = DispatchSemaphore(value: 0)
    Task { @MainActor in
        let result = await AIMobileHostToolProvider.shared.execute(
            name: name,
            argumentsJSON: arguments
        )
        response.store(aiMaiChatHostToolEnvelope(result))
        finished.signal()
    }
    finished.wait()
    return aiMaiChatOwnedCString(response.load())
}

let aiMaiChatHostToolResponseFree: @convention(c) (
    UnsafeMutableRawPointer?,
    UnsafePointer<CChar>?
) -> Void = { _, response in
    if let response { free(UnsafeMutableRawPointer(mutating: response)) }
}

let aiMaiChatHostToolContextRelease: @convention(c) (UnsafeMutableRawPointer?) -> Void = {
    context in
    guard let context else { return }
    Unmanaged<AIMaiChatHostToolCallbackContext>.fromOpaque(context).release()
}
