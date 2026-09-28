import Foundation
import Darwin
import ImageIO
import MaiChatCore
import Photos
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
    var allowForSession: Bool?
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
struct AIReplySuggestions: Codable, Sendable, Equatable {
    let natural: String
    let casual: String
    let professional: String
}
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
        case "mobile_list_photos": return await listPhotos(arguments)
        case "mobile_list_albums": return await listAlbums()
        case "mobile_read_photo": return await readPhoto(arguments)
        case "mobile_export_photo_original": return await exportPhotoOriginal(arguments)
        case "mobile_save_image": return await saveImage(arguments)
        case "mobile_photos_add_to_album": return await addPhotosToAlbum(arguments)
        default: break
        }
        guard let appState else {
            return .failure(code: "not_configured", message: "the MaiChat host is unavailable")
        }

        switch name {
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
        case "maichat_reply_message":
            return await replyMessage(appState: appState, arguments: arguments)
        case "maichat_broadcast_text":
            return await broadcastText(appState: appState, arguments: arguments)
        default:
            return .failure(code: "invalid_input", message: "unknown MaiChat host tool")
        }
    }

    private static func listContacts(
        appState: RemoteIMAppState,
        arguments: [String: Any]
    ) -> AIMaiChatHostToolExecution {
        let query = string(arguments, key: "query")
        let limit = boundedLimit(arguments)
        let contacts = appState.chatState.contacts.lazy
            .filter { contact in
                query.isEmpty || contact.userID.localizedCaseInsensitiveContains(query)
                    || contact.displayName.localizedCaseInsensitiveContains(query)
            }
            .prefix(limit)
            .map { contact in
                [
                    "user_id": contact.userID,
                    "display_name": contact.displayName,
                    "group": contact.groupName,
                ]
            }
        return jsonSuccess(["contacts": Array(contacts), "count": contacts.count])
    }

    private static func listConversations(
        appState: RemoteIMAppState,
        arguments: [String: Any]
    ) -> AIMaiChatHostToolExecution {
        let limit = boundedLimit(arguments)
        let values = appState.chatState.contacts.compactMap { contact -> [String: Any]? in
            guard let latest = appState.chatState.latestMessage(with: contact.userID) else {
                return nil
            }
            return [
                "peer_id": contact.userID,
                "display_name": contact.displayName,
                "unread": appState.unreadCount(for: contact.userID),
                "latest": messageJSON(latest, peerID: contact.userID),
            ]
        }
        .sorted { left, right in
            let leftDate = (left["latest"] as? [String: Any])?["created_at_ms"] as? Int64 ?? 0
            let rightDate = (right["latest"] as? [String: Any])?["created_at_ms"] as? Int64 ?? 0
            return leftDate > rightDate
        }
        let conversations = Array(values.prefix(limit))
        return jsonSuccess(["conversations": conversations, "count": conversations.count])
    }

    private func getMessages(
        appState: RemoteIMAppState,
        arguments: [String: Any]
    ) async -> AIMaiChatHostToolExecution {
        let peerID = Self.string(arguments, key: "peer_id")
        guard Self.hasContact(appState, peerID: peerID) else {
            return .failure(code: "invalid_input", message: "peer_id is not a MaiChat contact")
        }
        let messages = await appState.messagesForHostTool(
            peerUserID: peerID,
            limit: Self.boundedLimit(arguments)
        )
        let values = messages.map { Self.messageJSON($0, peerID: peerID) }
        return Self.jsonSuccess(["peer_id": peerID, "messages": values, "count": values.count])
    }

    private func searchMessages(
        appState: RemoteIMAppState,
        arguments: [String: Any]
    ) async -> AIMaiChatHostToolExecution {
        let query = Self.string(arguments, key: "query")
        let peerID = Self.string(arguments, key: "peer_id")
        guard !query.isEmpty else {
            return .failure(code: "invalid_input", message: "query is required")
        }
        if !peerID.isEmpty, !Self.hasContact(appState, peerID: peerID) {
            return .failure(code: "invalid_input", message: "peer_id is not a MaiChat contact")
        }
        let limit = Self.boundedLimit(arguments)
        let hits = Array(await appState.searchMessages(query, limit: 200)
            .filter { peerID.isEmpty || $0.peerUserID == peerID }
            .prefix(limit))
        appState.chatState.mergeMessages(hits.map(\.message))
        let values = hits.map { Self.messageJSON($0.message, peerID: $0.peerUserID) }
        return Self.jsonSuccess(["query": query, "matches": values, "count": values.count])
    }

    private static func unreadSummary(appState: RemoteIMAppState) -> AIMaiChatHostToolExecution {
        let values = appState.chatState.contacts.compactMap { contact -> [String: Any]? in
            let unread = appState.unreadCount(for: contact.userID)
            guard unread > 0 else { return nil }
            return [
                "peer_id": contact.userID,
                "display_name": contact.displayName,
                "unread": unread,
            ]
        }
        return jsonSuccess(["total_unread": values.reduce(0) { $0 + ($1["unread"] as? Int ?? 0) },
                            "conversations": values])
    }

    private func sendText(
        appState: RemoteIMAppState,
        arguments: [String: Any]
    ) async -> AIMaiChatHostToolExecution {
        let peerID = Self.string(arguments, key: "peer_id")
        let text = Self.string(arguments, key: "text")
        guard Self.hasContact(appState, peerID: peerID), !text.isEmpty else {
            return .failure(code: "invalid_input", message: "peer_id and text are required")
        }
        guard await appState.sendText(text, to: peerID) else {
            return .failure(code: "internal", message: "MaiChat did not send the message")
        }
        return Self.jsonSuccess([
            "sent": true,
            "peer_id": peerID,
            "message_id": appState.locallyQueuedMessageID?.uuidString ?? "",
        ])
    }

    private func replyMessage(
        appState: RemoteIMAppState,
        arguments: [String: Any]
    ) async -> AIMaiChatHostToolExecution {
        let peerID = Self.string(arguments, key: "peer_id")
        let messageID = Self.string(arguments, key: "message_id")
        let text = Self.string(arguments, key: "text")
        guard Self.hasContact(appState, peerID: peerID), !messageID.isEmpty, !text.isEmpty else {
            return .failure(
                code: "invalid_input",
                message: "peer_id, message_id, and text are required"
            )
        }
        guard let message = await appState.messageForHostTool(
            peerUserID: peerID,
            messageID: messageID
        ), let quote = RemoteIMMessageQuotePolicy.quote(for: message) else {
            return .failure(code: "invalid_input", message: "message_id was not found for peer_id")
        }
        guard await appState.sendText(text, quote: quote, to: peerID) else {
            return .failure(code: "internal", message: "MaiChat did not send the reply")
        }
        return Self.jsonSuccess([
            "sent": true,
            "peer_id": peerID,
            "message_id": appState.locallyQueuedMessageID?.uuidString ?? "",
            "reply_to": messageID,
        ])
    }

    private func broadcastText(
        appState: RemoteIMAppState,
        arguments: [String: Any]
    ) async -> AIMaiChatHostToolExecution {
        guard let rawPeerIDs = arguments["peer_ids"] as? [String], rawPeerIDs.count <= 200 else {
            return .failure(code: "invalid_input", message: "peer_ids must contain 1 to 200 contacts")
        }
        let text = Self.string(arguments, key: "text")
        var seen = Set<String>()
        let peerIDs = rawPeerIDs.compactMap { value -> String? in
            let peerID = value.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !peerID.isEmpty, seen.insert(peerID).inserted else { return nil }
            return peerID
        }
        guard !peerIDs.isEmpty, !text.isEmpty else {
            return .failure(code: "invalid_input", message: "peer_ids and text are required")
        }
        guard peerIDs.allSatisfy({ Self.hasContact(appState, peerID: $0) }) else {
            return .failure(code: "invalid_input", message: "peer_ids contains a non-contact")
        }

        let result = await appState.broadcastText(to: peerIDs, text: text)
        guard result.total > 0 else {
            return .failure(code: "internal", message: "MaiChat did not queue the broadcast")
        }
        return Self.jsonSuccess([
            "recipient_count": result.total,
            "sent_count": result.total - result.failedUserIDs.count,
            "failed_peer_ids": result.failedUserIDs,
        ])
    }

    private static func parseArguments(_ value: String) -> [String: Any]? {
        guard let data = value.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data),
              let arguments = object as? [String: Any] else { return nil }
        return arguments
    }

    private static func boundedLimit(_ arguments: [String: Any], fallback: Int = 50) -> Int {
        min(max(arguments["limit"] as? Int ?? fallback, 1), 200)
    }

    private static func string(_ arguments: [String: Any], key: String) -> String {
        (arguments[key] as? String)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
    }

    private static func hasContact(_ appState: RemoteIMAppState, peerID: String) -> Bool {
        !peerID.isEmpty && appState.chatState.contacts.contains(where: { $0.userID == peerID })
    }

    private static func messageJSON(_ message: RemoteIMMessage, peerID: String) -> [String: Any] {
        [
            "id": message.id.uuidString,
            "peer_id": peerID,
            "direction": message.direction == .incoming ? "incoming" : "outgoing",
            "sender_id": message.fromUserID,
            "text": message.text,
            "kind": messageKind(message),
            "created_at_ms": Int64((message.createdAt.timeIntervalSince1970 * 1_000).rounded()),
        ]
    }

    private static func messageKind(_ message: RemoteIMMessage) -> String {
        if message.imageAttachment != nil { return "image" }
        if message.videoAttachment != nil { return "video" }
        if message.voiceAttachment != nil { return "voice" }
        if message.fileAttachment != nil { return "file" }
        return "text"
    }

    private static func jsonSuccess(_ object: [String: Any]) -> AIMaiChatHostToolExecution {
        guard JSONSerialization.isValidJSONObject(object),
              let data = try? JSONSerialization.data(withJSONObject: object),
              let output = String(data: data, encoding: .utf8) else {
            return .failure(code: "internal", message: "failed to encode MaiChat tool output")
        }
        return .success(output)
    }

    private func photoAuthorization() async -> PHAuthorizationStatus {
        let status = PHPhotoLibrary.authorizationStatus(for: .readWrite)
        return status == .notDetermined
            ? await PHPhotoLibrary.requestAuthorization(for: .readWrite)
            : status
    }

    private func listPhotos(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        let status = await photoAuthorization()
        guard status == .authorized || status == .limited else {
            return .failure(code: "canceled", message: "photo library access was not granted")
        }
        let offset = max(0, arguments["offset"] as? Int ?? 0)
        let limit = min(100, max(1, arguments["limit"] as? Int ?? 50))
        let options = PHFetchOptions()
        options.predicate = NSPredicate(format: "mediaType == %d", PHAssetMediaType.image.rawValue)
        options.sortDescriptors = [NSSortDescriptor(key: "creationDate", ascending: false)]
        let albumID = (arguments["album_id"] as? String ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        let photos: PHFetchResult<PHAsset>
        if !albumID.isEmpty && albumID != "all" {
            guard let album = PHAssetCollection.fetchAssetCollections(
                withLocalIdentifiers: [albumID], options: nil
            ).firstObject else { return .failure(code: "not_found", message: "album not found") }
            photos = PHAsset.fetchAssets(in: album, options: options)
        } else {
            photos = PHAsset.fetchAssets(with: .image, options: options)
        }
        let end = min(photos.count, offset + limit)
        var items: [[String: Any]] = []
        if offset < end {
            for index in offset..<end {
                let photo = photos.object(at: index)
                guard photo.mediaType == .image else { continue }
                items.append([
                    "id": photo.localIdentifier,
                    "created_at_ms": Int64(((photo.creationDate ?? .distantPast).timeIntervalSince1970 * 1000).rounded()),
                    "width": photo.pixelWidth,
                    "height": photo.pixelHeight,
                    "favorite": photo.isFavorite,
                ])
            }
        }
        return Self.jsonSuccess([
            "access": status == .authorized ? "full" : "limited",
            "total": photos.count, "offset": offset, "items": items,
        ])
    }

    private func listAlbums() async -> AIMaiChatHostToolExecution {
        let status = await photoAuthorization()
        guard status == .authorized || status == .limited else {
            return .failure(code: "canceled", message: "photo library access was not granted")
        }
        let albums = PHAssetCollection.fetchAssetCollections(with: .album, subtype: .any, options: nil)
        var items: [[String: Any]] = [["id": "all", "name": "所有照片"]]
        albums.enumerateObjects { album, _, _ in
            items.append(["id": album.localIdentifier, "name": album.localizedTitle ?? "未命名相簿"])
        }
        return Self.jsonSuccess([
            "access": status == .authorized ? "full" : "limited", "albums": items,
        ])
    }

    private func readPhoto(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        let status = await photoAuthorization()
        guard status == .authorized || status == .limited else {
            return .failure(code: "canceled", message: "photo library access was not granted")
        }
        guard let id = arguments["id"] as? String, !id.isEmpty,
              let asset = PHAsset.fetchAssets(withLocalIdentifiers: [id], options: nil).firstObject,
              asset.mediaType == .image else {
            return .failure(code: "not_found", message: "photo not found in the authorized library")
        }
        let options = PHImageRequestOptions()
        options.isNetworkAccessAllowed = true
        let data: Data? = await withCheckedContinuation { continuation in
            PHImageManager.default().requestImageDataAndOrientation(for: asset, options: options) {
                data, _, _, _ in continuation.resume(returning: data)
            }
        }
        guard let data, let source = CGImageSourceCreateWithData(data as CFData, nil),
              let preview = CGImageSourceCreateThumbnailAtIndex(source, 0, [
                kCGImageSourceCreateThumbnailFromImageAlways: true,
                kCGImageSourceCreateThumbnailWithTransform: true,
                kCGImageSourceThumbnailMaxPixelSize: 2048,
              ] as CFDictionary),
              let jpeg = UIImage(cgImage: preview).jpegData(compressionQuality: 0.88) else {
            return .failure(code: "internal", message: "photo could not be read")
        }
        let temporary = FileManager.default.temporaryDirectory
            .appendingPathComponent("agent-photo-\(UUID().uuidString).jpg")
        do {
            try jpeg.write(to: temporary, options: .atomic)
            defer { try? FileManager.default.removeItem(at: temporary) }
            guard let imported = await AIAssistantModel.shared.importFile(temporary) else {
                return .failure(code: "internal", message: "photo could not be imported")
            }
            return Self.jsonSuccess([
                "id": id, "path": imported.relativePath, "mime_type": imported.mimeType,
                "next_tool": "view_image",
            ])
        } catch {
            return .failure(code: "internal", message: error.localizedDescription)
        }
    }

    private func exportPhotoOriginal(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        let status = await photoAuthorization()
        guard status == .authorized || status == .limited else {
            return .failure(code: "canceled", message: "photo library access was not granted")
        }
        guard let id = arguments["id"] as? String, !id.isEmpty,
              let asset = PHAsset.fetchAssets(withLocalIdentifiers: [id], options: nil).firstObject,
              asset.mediaType == .image else {
            return .failure(code: "not_found", message: "photo not found in the authorized library")
        }
        let workspacePath = AIAssistantModel.shared.workspacePath
        guard !workspacePath.isEmpty else {
            return .failure(code: "not_configured", message: "Agent working directory is unavailable")
        }
        let options = PHImageRequestOptions()
        options.isNetworkAccessAllowed = true
        options.version = .original
        let result: (Data, String)? = await withCheckedContinuation { continuation in
            PHImageManager.default().requestImageDataAndOrientation(for: asset, options: options) {
                data, type, _, _ in
                guard let data, let type else { continuation.resume(returning: nil); return }
                continuation.resume(returning: (data, type))
            }
        }
        guard let (data, type) = result, !data.isEmpty, data.count <= 100 * 1024 * 1024 else {
            return .failure(code: "invalid_input", message: "photo is unavailable or exceeds 100 MB")
        }
        guard let imageType = UTType(type), imageType.conforms(to: .image),
              let suffix = imageType.preferredFilenameExtension,
              let mimeType = imageType.preferredMIMEType else {
            return .failure(code: "invalid_input", message: "unsupported system photo format")
        }
        let name = "gallery-\(UUID().uuidString).\(suffix)"
        let target = URL(fileURLWithPath: workspacePath, isDirectory: true)
            .appendingPathComponent(name)
        do {
            try await Task.detached(priority: .utility) {
                try data.write(to: target, options: .atomic)
            }.value
            return Self.jsonSuccess([
                "id": id, "path": name, "mime_type": mimeType,
                "bytes": data.count, "width": asset.pixelWidth, "height": asset.pixelHeight,
            ])
        } catch {
            return .failure(code: "internal", message: error.localizedDescription)
        }
    }

    private func saveImage(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        let status = await photoAuthorization()
        guard status == .authorized || status == .limited else {
            return .failure(code: "canceled", message: "photo library access was not granted")
        }
        let path = Self.string(arguments, key: "path")
        let workspacePath = AIAssistantModel.shared.workspacePath
        guard !path.isEmpty, !workspacePath.isEmpty else {
            return .failure(code: "invalid_input", message: "an Agent working-directory image path is required")
        }
        let workspace = URL(fileURLWithPath: workspacePath, isDirectory: true)
            .resolvingSymlinksInPath().standardizedFileURL
        let source = workspace.appendingPathComponent(path)
            .resolvingSymlinksInPath().standardizedFileURL
        guard source.path.hasPrefix(workspace.path + "/") else {
            return .failure(code: "invalid_input", message: "image must be inside the Agent working directory")
        }
        do {
            let values = try source.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
            let suffix = source.pathExtension.lowercased()
            guard values.isRegularFile == true, let bytes = values.fileSize,
                  bytes > 0, bytes <= 50 * 1024 * 1024,
                  ["jpg", "jpeg", "png", "heic", "heif"].contains(suffix),
                  let image = CGImageSourceCreateWithURL(source as CFURL, nil),
                  CGImageSourceGetCount(image) > 0 else {
                return .failure(code: "invalid_input", message: "image must be a JPEG, PNG, or HEIF file up to 50 MB")
            }
            try await performPhotoChanges {
                PHAssetChangeRequest.creationRequestForAssetFromImage(atFileURL: source)
            }
            return Self.jsonSuccess(["saved": true, "source_path": path])
        } catch {
            return .failure(code: "internal", message: error.localizedDescription)
        }
    }

    private func addPhotosToAlbum(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        let status = await photoAuthorization()
        guard status == .authorized || status == .limited else {
            return .failure(code: "canceled", message: "photo library access was not granted")
        }
        guard let rawName = arguments["album_name"] as? String,
              let rawIDs = arguments["photo_ids"] as? [String] else {
            return .failure(code: "invalid_input", message: "album_name and photo_ids are required")
        }
        let name = rawName.trimmingCharacters(in: .whitespacesAndNewlines)
        let ids = Array(Set(rawIDs.map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }))
        guard !name.isEmpty, name.count <= 64, !ids.isEmpty, ids.count <= 50,
              ids.allSatisfy({ !$0.isEmpty }) else {
            return .failure(code: "invalid_input", message: "invalid album name or photo IDs")
        }
        var assets: [PHAsset] = []
        for id in ids {
            guard let asset = PHAsset.fetchAssets(withLocalIdentifiers: [id], options: nil).firstObject,
                  asset.mediaType == .image else {
                return .failure(code: "not_found", message: "a photo is not accessible")
            }
            assets.append(asset)
        }

        func findAlbum() -> PHAssetCollection? {
            let albums = PHAssetCollection.fetchAssetCollections(with: .album, subtype: .any, options: nil)
            for index in 0..<albums.count {
                let album = albums.object(at: index)
                if album.localizedTitle == name { return album }
            }
            return nil
        }

        do {
            var album = findAlbum()
            if album == nil {
                try await performPhotoChanges {
                    PHAssetCollectionChangeRequest.creationRequestForAssetCollection(withTitle: name)
                }
                album = findAlbum()
            }
            guard let album, album.canPerform(.addContent) else {
                return .failure(code: "internal", message: "album cannot be edited")
            }
            try await performPhotoChanges {
                PHAssetCollectionChangeRequest(for: album)?.addAssets(assets as NSArray)
            }
            return Self.jsonSuccess([
                "album_id": album.localIdentifier, "album_name": name,
                "added_count": assets.count, "copied_originals": false,
            ])
        } catch {
            return .failure(code: "internal", message: error.localizedDescription)
        }
    }

    private func performPhotoChanges(_ changes: @escaping () -> Void) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            PHPhotoLibrary.shared().performChanges(changes) { success, error in
                if success { continuation.resume() }
                else {
                    continuation.resume(throwing: error ?? NSError(
                        domain: "MaiChat.Photos", code: -1,
                        userInfo: [NSLocalizedDescriptionKey: "相簿操作失败"]
                    ))
                }
            }
        }
    }
}

private final class AIMaiChatHostToolCallbackContext: @unchecked Sendable {}

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

private let aiMaiChatHostToolHandler: @convention(c) (
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

private let aiMaiChatHostToolResponseFree: @convention(c) (
    UnsafeMutableRawPointer?,
    UnsafePointer<CChar>?
) -> Void = { _, response in
    if let response { free(UnsafeMutableRawPointer(mutating: response)) }
}

private let aiMaiChatHostToolContextRelease: @convention(c) (UnsafeMutableRawPointer?) -> Void = {
    context in
    guard let context else { return }
    Unmanaged<AIMaiChatHostToolCallbackContext>.fromOpaque(context).release()
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

    init() {
        let pointer = maiMobileAgentCreate()
        address = UInt(bitPattern: pointer)
        guard let pointer else { return }
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
    func send(
        _ text: String,
        images: [AIImportedFile] = [],
        expectedSession: String? = nil
    ) async -> Bool {
        guard configured else { showSettings = true; return false }
        guard !isSubmitting, !busy, !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return false }
        isSubmitting = true
        defer { isSubmitting = false }
        let draftSession = selected
        guard expectedSession == nil || expectedSession == draftSession else { return false }
        let attachments = pendingAttachments[draftSession, default: []] + images
        if attachments.contains(where: \.isImage),
           settings.model.trimmingCharacters(in: .whitespacesAndNewlines)
               .caseInsensitiveCompare("glm-5.3") == .orderedSame {
            showTransientError("glm-5.3 仅支持文本，请先切换到 glm-5.3-flash")
            return false
        }
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
