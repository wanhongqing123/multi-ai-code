import AVFoundation
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

@MainActor
extension AIMobileHostToolProvider {
    static func listContacts(
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

    static func listConversations(
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

    func getMessages(
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
        let values = messages.map { Self.messageJSON($0, peerID: peerID, includeImagePath: true) }
        return Self.jsonSuccess(["peer_id": peerID, "messages": values, "count": values.count])
    }

    func searchMessages(
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
        let values = hits.map {
            Self.messageJSON($0.message, peerID: $0.peerUserID, includeImagePath: true)
        }
        return Self.jsonSuccess(["query": query, "matches": values, "count": values.count])
    }

    static func unreadSummary(appState: RemoteIMAppState) -> AIMaiChatHostToolExecution {
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

    func sendText(
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

    func sendMedia(
        appState: RemoteIMAppState,
        arguments: [String: Any]
    ) async -> AIMaiChatHostToolExecution {
        let peerID = Self.string(arguments, key: "peer_id")
        let kind = Self.string(arguments, key: "type")
        let requestedPath = Self.string(arguments, key: "file_path")
        let caption = Self.string(arguments, key: "caption")
        guard Self.hasContact(appState, peerID: peerID),
              appState.connectionState == .connected else {
            return .failure(code: "invalid_input", message: "peer_id must be a connected MaiChat contact")
        }
        guard ["image", "video", "audio"].contains(kind),
              let source = AIAssistantPathPolicy.resolve(
                requestedPath, workspacePath: AIAssistantModel.shared.workspacePath
              ), FileManager.default.fileExists(atPath: source.path) else {
            return .failure(code: "invalid_input", message: "type and accessible file_path are required")
        }
        let fileType = UTType(filenameExtension: source.pathExtension)
        guard (kind == "image" && fileType?.conforms(to: .image) == true) ||
              (kind == "video" && fileType?.conforms(to: .movie) == true) ||
              (kind == "audio" && fileType?.conforms(to: .audio) == true) else {
            return .failure(code: "invalid_input", message: "file extension does not match type")
        }

        do {
            let sent: Bool
            switch kind {
            case "image":
                let image = try await Self.prepareAgentImage(source)
                sent = await appState.sendImageFile(image, to: peerID)
            case "video":
                let video = try await Self.prepareAgentVideo(source)
                sent = await appState.sendVideoFile(video, to: peerID)
            default:
                let audio = try await Self.prepareAgentAudio(source)
                sent = await appState.sendFile(audio, to: peerID)
            }
            guard sent else {
                return .failure(code: "internal",
                                message: appState.errorMessage ?? "MaiChat did not send the media")
            }
            let mediaMessageID = appState.locallyQueuedMessageID?.uuidString ?? ""
            var captionSent = true
            if !caption.isEmpty { captionSent = await appState.sendText(caption, to: peerID) }
            return Self.jsonSuccess([
                "sent": true, "peer_id": peerID, "type": kind,
                "message_kind": kind == "audio" ? "file" : kind,
                "message_id": mediaMessageID, "caption_sent": captionSent,
            ])
        } catch {
            return .failure(code: "internal", message: error.localizedDescription)
        }
    }

    private static func prepareAgentImage(_ source: URL) async throws -> RemoteIMImageFile {
        try await RemoteIMBackgroundWork.file {
            let size = try source.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
            guard size > 0, size <= 20 * 1024 * 1024 else {
                throw AIBackendError(message: "图片必须小于 20 MB")
            }
            let data = try Data(contentsOf: source)
            return try makeRemoteIMImageFile(
                data: data,
                contentTypes: [UTType(filenameExtension: source.pathExtension) ?? .jpeg],
                stem: "agent-image-" + UUID().uuidString
            )
        }
    }

    private static func copyAgentMedia(
        _ source: URL, category: RemoteIMMediaStorage.Category
    ) async throws -> URL {
        try await RemoteIMBackgroundWork.file {
            let size = try source.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
            guard size > 0, size <= 1024 * 1024 * 1024 else {
                throw AIBackendError(message: "媒体文件必须小于 1 GB")
            }
            let target = RemoteIMMediaStorage.fileURL(
                category: category,
                stem: "agent-" + UUID().uuidString,
                pathExtension: source.pathExtension
            )
            try FileManager.default.createDirectory(
                at: target.deletingLastPathComponent(),
                withIntermediateDirectories: true
            )
            do {
                try FileManager.default.copyItem(at: source, to: target)
            } catch {
                try? FileManager.default.removeItem(at: target)
                throw error
            }
            return target
        }
    }

    private static func prepareAgentVideo(_ source: URL) async throws -> RemoteIMVideoFile {
        let copied = try await copyAgentMedia(source, category: .outgoingVideos)
        let asset = AVURLAsset(url: copied)
        let duration = try await asset.load(.duration)
        let durationValue = CMTimeGetSeconds(duration)
        let videoTracks = try await asset.loadTracks(withMediaType: .video)
        guard durationValue.isFinite, durationValue > 0,
              let videoTrack = videoTracks.first else {
            throw AIBackendError(message: "视频没有可播放的画面或时长")
        }
        let naturalSize = try await videoTrack.load(.naturalSize)
        let transform = try await videoTrack.load(.preferredTransform)
        let rect = CGRect(origin: .zero, size: naturalSize).applying(transform)
        let generator = AVAssetImageGenerator(asset: asset)
        generator.appliesPreferredTrackTransform = true
        generator.maximumSize = CGSize(width: 1_280, height: 1_280)
        let coverTime = CMTime(seconds: durationValue > 0.2 ? 0.1 : 0,
                               preferredTimescale: 600)
        let coverImage = try await generator.image(at: coverTime)
        guard let coverBytes = UIImage(cgImage: coverImage.image)
            .jpegData(compressionQuality: 0.86) else {
            throw AIBackendError(message: "无法生成视频封面")
        }
        let coverURL = try await RemoteIMBackgroundWork.file {
            let target = RemoteIMMediaStorage.fileURL(
                category: .outgoingVideoCovers,
                stem: copied.deletingPathExtension().lastPathComponent,
                pathExtension: "jpg"
            )
            try coverBytes.write(to: target, options: .atomic)
            return target
        }
        let bytes = try copied.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
        return RemoteIMVideoFile(
            fileURL: copied, coverFileURL: coverURL,
            fileType: copied.pathExtension.lowercased(),
            durationSeconds: max(1, Int(ceil(durationValue))),
            width: max(0, Int(abs(rect.width).rounded())),
            height: max(0, Int(abs(rect.height).rounded())),
            sizeBytes: Int64(bytes)
        )
    }

    private static func prepareAgentAudio(_ source: URL) async throws -> RemoteIMFile {
        let copied = try await copyAgentMedia(source, category: .outgoingFiles)
        let asset = AVURLAsset(url: copied)
        let tracks = try await asset.loadTracks(withMediaType: .audio)
        guard !tracks.isEmpty else {
            throw AIBackendError(message: "文件没有可播放的音轨")
        }
        let bytes = try copied.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
        return RemoteIMFile(
            fileURL: copied, fileName: source.lastPathComponent,
            mimeType: UTType(filenameExtension: source.pathExtension)?
                .preferredMIMEType ?? "audio/mpeg",
            sizeBytes: bytes
        )
    }

    func replyMessage(
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

    func broadcastText(
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

    private static func hasContact(_ appState: RemoteIMAppState, peerID: String) -> Bool {
        !peerID.isEmpty && appState.chatState.contacts.contains(where: { $0.userID == peerID })
    }

    private static func messageJSON(_ message: RemoteIMMessage, peerID: String,
                                    includeImagePath: Bool = false) -> [String: Any] {
        var result: [String: Any] = [
            "id": message.id.uuidString,
            "peer_id": peerID,
            "direction": message.direction == .incoming ? "incoming" : "outgoing",
            "sender_id": message.fromUserID,
            "text": message.text,
            "kind": messageKind(message),
            "created_at_ms": Int64((message.createdAt.timeIntervalSince1970 * 1_000).rounded()),
        ]
        if includeImagePath, let path = imageWorkspaceReference(message) {
            result["workspace_path"] = path
            result["mime_type"] = UTType(filenameExtension: URL(fileURLWithPath: path)
                .pathExtension)?.preferredMIMEType ?? "application/octet-stream"
        }
        return result
    }

    private static func imageWorkspaceReference(_ message: RemoteIMMessage) -> String? {
        guard let attachment = message.imageAttachment else { return nil }
        let workspacePath = AIAssistantModel.shared.workspacePath
        guard !workspacePath.isEmpty else { return nil }
        let sourcePath = attachment.localFilePath
        guard let source = AIAssistantPathPolicy.resolve(sourcePath, workspacePath: workspacePath),
              FileManager.default.fileExists(atPath: source.path) else { return nil }
        let file = try? source.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
        guard file?.isRegularFile == true, let size = file?.fileSize,
              size > 0, size <= 20 * 1024 * 1024 else { return nil }
        let suffix = source.pathExtension.lowercased()
        guard ["jpg", "jpeg", "png", "webp", "gif", "heic", "heif"].contains(suffix) else {
            return source.path
        }
        let name = "im-image-\(message.id.uuidString.lowercased()).\(suffix)"
        let target = URL(fileURLWithPath: workspacePath, isDirectory: true)
            .appendingPathComponent(name)
        if !FileManager.default.fileExists(atPath: target.path) {
            do { try FileManager.default.linkItem(at: source, to: target) }
            catch { return source.path }
        }
        return name
    }

    private static func messageKind(_ message: RemoteIMMessage) -> String {
        if message.imageAttachment != nil { return "image" }
        if message.videoAttachment != nil { return "video" }
        if message.voiceAttachment != nil { return "voice" }
        if message.fileAttachment != nil { return "file" }
        return "text"
    }
}
