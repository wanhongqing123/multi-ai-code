import Foundation
import CoreFoundation
import Darwin
import CoreImage
import ImageIO
import Metal
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

    func execute(name: String, argumentsJSON: String) async -> AIMaiChatHostToolExecution {
        guard let arguments = Self.parseArguments(argumentsJSON) else {
            return .failure(code: "invalid_input", message: "arguments must be a JSON object")
        }

        switch name {
        case "mobile_request_permission": return await requestSystemPermission(arguments)
        case "mobile_get_location": return await getCurrentLocation()
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
        case "oss_video_upload_config":
            return await privateServiceStatus()
        case "mobile_oss_upload_video": return await uploadOssVideo(arguments)
        case "mobile_ark_assets": return await arkAssets(arguments)
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
            return Self.jsonSuccess(["key": KeychainSecretStore(account: "seedance-ark-api-key")
                .readSecretKey()])
        case "glm_api_key":
            let video = KeychainSecretStore(account: "glm-video-api-key").readSecretKey()
            let current = KeychainSecretStore(account: "ai-assistant-glm-api-key").readSecretKey()
            let legacy = KeychainSecretStore(account: "ai-assistant-api-key").readSecretKey()
            return Self.jsonSuccess(["key": !video.isEmpty ? video : (current.isEmpty ? legacy : current)])
        case "kling_api_key":
            return Self.jsonSuccess(["key": KeychainSecretStore(account: "kling-creative-api-key")
                .readSecretKey()])
        case "minimax_api_key":
            return Self.jsonSuccess(["key": KeychainSecretStore(account: "minimax-creative-api-key")
                .readSecretKey()])
        case "wan_credentials":
            return Self.jsonSuccess([
                "key": KeychainSecretStore(account: "wan-model-studio-api-key").readSecretKey(),
                "workspace_id": UserDefaults.standard.string(forKey: "wan-model-studio-workspace-id") ?? ""
            ])
        default: break
        }
        guard let appState else {
            return .failure(code: "not_configured", message: "the MaiChat host is unavailable")
        }

        switch name {
        case "maichat_play_video":
            let path = Self.string(arguments, key: "path")
            guard let source = AIAssistantPathPolicy.resolve(
                path, workspacePath: AIAssistantModel.shared.workspacePath
            ), FileManager.default.fileExists(atPath: source.path) else {
                return .failure(code: "not_found", message: "video must be an accessible local file")
            }
            appState.agentVideoPresentation = MaiFfplayPresentation(path: source.path)
            return Self.jsonSuccess(["opened": true])
        case "maichat_video_command":
            let action = Self.string(arguments, key: "action")
            let percent = arguments["percent"] as? Double
            guard MaiFfplayMobilePlayer.command(action, percent: percent) else {
                let reason = MaiFfplayMobilePlayer.unavailableReason
                AppDiagnosticLog.shared.record(level: .warning, category: "ffplay",
                    event: "video-command-unavailable", fields: [
                        "action": action, "reason": reason
                    ])
                return .failure(code: "unavailable", message: reason)
            }
            return Self.jsonSuccess(["accepted": true])
        case "maichat_list_contacts":
            return Self.listContacts(appState: appState, arguments: arguments)
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

    private func privateServiceEndpoint(_ action: String) -> (url: URL, token: String)? {
        let rawURL = UserDefaults.standard.string(forKey: "oss-media-signer-url") ?? ""
        let token = KeychainSecretStore(account: "oss-media-signer-token").readSecretKey()
        guard token.count >= 32, var components = URLComponents(string: rawURL),
              components.scheme == "https", components.host?.isEmpty == false,
              components.user == nil, components.password == nil,
              components.query == nil, components.fragment == nil else {
            return nil
        }
        if components.path.hasSuffix("/sign-upload") {
            components.path = String(components.path.dropLast("sign-upload".count))
        } else if !components.path.hasSuffix("/") {
            components.path += "/"
        }
        components.path += action
        guard let url = components.url else { return nil }
        return (url, token)
    }

    private func ossSignerConfiguration() -> (url: URL, token: String)? {
        privateServiceEndpoint("sign-upload")
    }

    private func privateServiceStatus() async -> AIMaiChatHostToolExecution {
        guard let service = privateServiceEndpoint("credentials") else {
            return Self.jsonSuccess(["configured": false, "assets_configured": false])
        }
        do {
            var request = URLRequest(url: service.url)
            request.httpMethod = "POST"
            request.cachePolicy = .reloadIgnoringLocalCacheData
            request.setValue("application/json", forHTTPHeaderField: "Content-Type")
            request.setValue("Bearer \(service.token)", forHTTPHeaderField: "Authorization")
            request.httpBody = try JSONSerialization.data(withJSONObject: ["action": "status"])
            let session = URLSession(configuration: .ephemeral)
            defer { session.finishTasksAndInvalidate() }
            let (data, response) = try await session.data(for: request)
            guard let http = response as? HTTPURLResponse, http.statusCode == 200,
                  data.count <= 16_384,
                  let status = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
                return Self.jsonSuccess(["configured": false, "assets_configured": false])
            }
            return Self.jsonSuccess([
                "configured": status["storage_configured"] as? Bool ?? false,
                "assets_configured": status["assets_configured"] as? Bool ?? false
            ])
        } catch {
            return Self.jsonSuccess(["configured": false, "assets_configured": false])
        }
    }

    private func uploadOssVideo(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        guard let signer = ossSignerConfiguration() else {
            return .failure(code: "not_configured", message: "Configure the OSS signer in model settings")
        }
        let path = Self.string(arguments, key: "path")
        guard let source = AIAssistantPathPolicy.resolve(
            path, workspacePath: AIAssistantModel.shared.workspacePath
        ), ["mp4", "mov"].contains(source.pathExtension.lowercased()),
           let values = try? source.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey]),
           values.isRegularFile == true, let size = values.fileSize,
           size > 0, size <= 200_000_000 else {
            return .failure(code: "invalid_input", message: "Video must be an accessible MP4/MOV under 200 MB")
        }
        do {
            var signRequest = URLRequest(url: signer.url)
            signRequest.httpMethod = "POST"
            signRequest.setValue("application/json", forHTTPHeaderField: "Content-Type")
            signRequest.setValue("Bearer \(signer.token)", forHTTPHeaderField: "Authorization")
            signRequest.httpBody = try JSONSerialization.data(withJSONObject: [
                "filename": source.lastPathComponent, "size_bytes": size
            ])
            let configuration = URLSessionConfiguration.default
            configuration.timeoutIntervalForRequest = 60
            configuration.timeoutIntervalForResource = 900
            configuration.waitsForConnectivity = true
            let session = URLSession(configuration: configuration)
            defer { session.finishTasksAndInvalidate() }
            let (signedData, signedResponse) = try await session.data(for: signRequest)
            guard let http = signedResponse as? HTTPURLResponse else {
                return .failure(code: "protocol", message: "OSS signer returned no HTTP response")
            }
            if http.statusCode == 401 {
                return .failure(code: "not_configured", message: "OSS signer token was rejected")
            }
            if http.statusCode == 503 {
                return .failure(code: "not_configured", message: "OSS signer is not configured")
            }
            guard http.statusCode == 200, signedData.count <= 16_384,
                  let signed = try JSONSerialization.jsonObject(with: signedData) as? [String: Any],
                  let uploadText = signed["upload_url"] as? String,
                  let readText = signed["read_url"] as? String,
                  let upload = URLComponents(string: uploadText),
                  let read = URLComponents(string: readText),
                  upload.scheme == "https", read.scheme == "https",
                  upload.host?.isEmpty == false, upload.host == read.host,
                  upload.path == read.path,
                  upload.user == nil, read.user == nil,
                  upload.password == nil, read.password == nil,
                  let uploadURL = upload.url, let readURL = read.url,
                  let headers = signed["upload_headers"] as? [String: String],
                  let contentType = headers["Content-Type"],
                  contentType == (source.pathExtension.lowercased() == "mov"
                    ? "video/quicktime" : "video/mp4") else {
                return .failure(code: "upload_failed", message: "OSS did not return valid upload links")
            }
            var uploadRequest = URLRequest(url: uploadURL)
            uploadRequest.httpMethod = "PUT"
            uploadRequest.setValue(contentType, forHTTPHeaderField: "Content-Type")
            let (_, uploadResponse) = try await session.upload(for: uploadRequest, fromFile: source)
            guard let result = uploadResponse as? HTTPURLResponse,
                  (200...299).contains(result.statusCode) else {
                return .failure(code: "upload_failed", message: "OSS rejected the video upload; check the signed URL and Bucket permissions")
            }
            return Self.jsonSuccess(["url": readURL.absoluteString])
        } catch {
            return .failure(code: "upload_failed", message: error.localizedDescription)
        }
    }

    private func arkAssets(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        guard Self.string(arguments, key: "action") != "upload_image" else {
            return .failure(code: "upload_not_configured",
                            message: "Local image upload awaits private OSS storage; use an accessible HTTPS image URL")
        }
        guard let service = privateServiceEndpoint("ark-assets") else {
            return .failure(code: "not_configured", message: "Configure the private Ark Assets service")
        }
        do {
            var request = URLRequest(url: service.url)
            request.httpMethod = "POST"
            request.cachePolicy = .reloadIgnoringLocalCacheData
            request.setValue("application/json", forHTTPHeaderField: "Content-Type")
            request.setValue("Bearer \(service.token)", forHTTPHeaderField: "Authorization")
            request.httpBody = try JSONSerialization.data(withJSONObject: arguments)
            let configuration = URLSessionConfiguration.ephemeral
            configuration.timeoutIntervalForRequest = 60
            let session = URLSession(configuration: configuration)
            defer { session.finishTasksAndInvalidate() }
            let (data, response) = try await session.data(for: request)
            guard let http = response as? HTTPURLResponse, data.count <= 1_000_000,
                  let envelope = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
                return .failure(code: "protocol", message: "Ark Assets service returned an invalid response")
            }
            if http.statusCode != 200 {
                let reason = envelope["provider_message"] as? String ?? envelope["error"] as? String
                    ?? "Ark Assets request failed"
                let code = envelope["provider_code"] as? String ?? "provider_error"
                return .failure(code: code, message: reason)
            }
            guard let result = envelope["result"] as? [String: Any] else {
                return .failure(code: "protocol", message: "Ark Assets service returned no result")
            }
            return Self.jsonSuccess(result)
        } catch {
            return .failure(code: "network", message: error.localizedDescription)
        }
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
