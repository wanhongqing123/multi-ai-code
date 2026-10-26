// 文件作用：将 IM 的联系人、会话和消息操作封装为 Agent 可调用的工具。
// 阅读边界：工具负责输入校验与结果摘要；实际 IM 操作由应用状态层执行。
// 把 Agent 的 IM 操作转换为联系人、发送和查询动作。
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
    func setNickname(
        appState: RemoteIMAppState,
        arguments: [String: Any]
    ) async -> AIMaiChatHostToolExecution {
        guard let nickname = arguments["nickname"] as? String else {
            return .failure(code: "invalid_input", message: "nickname is required")
        }
        do {
            try await appState.setMaiChatNickname(nickname)
            return Self.jsonSuccess(["nickname": appState.maiChatNickname,
                                     "account_id": appState.maiChatAccountID])
        } catch {
            return .failure(code: "internal", message: error.localizedDescription)
        }
    }

    func requestFriend(
        appState: RemoteIMAppState,
        arguments: [String: Any]
    ) async -> AIMaiChatHostToolExecution {
        let account = Self.string(arguments, key: "account")
        guard !account.isEmpty else {
            return .failure(code: "invalid_input", message: "account is required")
        }
        guard await appState.requestFriend(userID: account) else {
            return .failure(code: "internal", message: appState.errorMessage ?? "Friend request failed")
        }
        return Self.jsonSuccess(["requested": true, "account": account])
    }

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

    static func listGroups(
        appState: RemoteIMAppState,
        arguments: [String: Any]
    ) -> AIMaiChatHostToolExecution {
        let query = string(arguments, key: "query")
        let limit = boundedLimit(arguments)
        let groups = appState.groups.lazy.filter { group in
            query.isEmpty || group.groupID.localizedCaseInsensitiveContains(query)
                || group.name.localizedCaseInsensitiveContains(query)
        }.prefix(limit).map { group in
            [
                "group_id": group.groupID,
                "name": group.name,
                "member_count": group.memberCount,
                "unread_count": appState.unreadCountByGroupID[group.groupID] ?? 0,
                "pending_activation": appState.groupCreationRecovery?.groupID == group.groupID,
            ] as [String: Any]
        }
        return jsonSuccess(["groups": Array(groups), "count": groups.count])
    }

    func createGroup(
        appState: RemoteIMAppState,
        arguments: [String: Any]
    ) async -> AIMaiChatHostToolExecution {
        guard appState.connectionState == .connected else {
            return .failure(code: "not_configured", message: "MaiChat IM is not connected")
        }
        if let pending = appState.groupCreationRecovery {
            return Self.jsonSuccess([
                "created": false,
                "group_id": pending.groupID,
                "name": pending.name,
                "member_count": pending.memberCount,
                "pending_activation": true,
                "next_step": "Open this existing group and send one text message; do not create it again.",
            ])
        }
        let name = Self.string(arguments, key: "name")
        guard !name.isEmpty, name.count <= 30, name.lengthOfBytes(using: .utf8) <= 100,
              let rawMembers = arguments["member_ids"] as? [String],
              (1...19).contains(rawMembers.count) else {
            return .failure(code: "invalid_input", message: "name and 1 to 19 member_ids are required")
        }
        let members = rawMembers.map {
            $0.trimmingCharacters(in: .whitespacesAndNewlines)
        }
        guard members.allSatisfy({ !$0.isEmpty && $0 != appState.maiChatAccountID }),
              Set(members).count == members.count else {
            return .failure(code: "invalid_input", message: "member_ids must be unique friends excluding the owner")
        }
        await appState.refreshAcceptedFriends()
        guard members.allSatisfy(appState.acceptedFriendIMIDs.contains) else {
            return .failure(code: "invalid_input", message: "every member_id must be an accepted MaiChat friend")
        }
        guard let group = await appState.createGroup(name: name, memberUserIDs: members) else {
            if let pending = appState.groupCreationRecovery {
                return Self.jsonSuccess([
                    "created": true,
                    "group_id": pending.groupID,
                    "name": pending.name,
                    "member_count": pending.memberCount,
                    "pending_activation": true,
                    "next_step": "Send one text message to this group to notify its members; do not create it again.",
                ])
            }
            return .failure(code: "network",
                            message: appState.errorMessage ?? "MaiChat group creation failed")
        }
        return Self.jsonSuccess([
            "created": true,
            "group_id": group.groupID,
            "name": group.name,
            "member_count": group.memberCount,
            "pending_activation": false,
        ])
    }

    func getGroupMessages(
        appState: RemoteIMAppState,
        arguments: [String: Any]
    ) async -> AIMaiChatHostToolExecution {
        let groupID = Self.string(arguments, key: "group_id")
        guard appState.groups.contains(where: { $0.groupID == groupID }) else {
            return .failure(code: "not_found", message: "group_id is not a joined MaiChat group")
        }
        let limit = min(Self.boundedLimit(arguments, fallback: 20), 20)
        let messages = await appState.groupMessagesForHostTool(groupID: groupID, limit: limit)
        let values: [[String: Any]] = messages.map { message in
            [
                "id": message.id.uuidString,
                "remote_id": message.remoteID ?? "",
                "group_id": groupID,
                "sender_id": message.fromUserID,
                "direction": message.direction == .incoming ? "incoming" : "outgoing",
                "text": message.text,
                "created_at_ms": Int64((message.createdAt.timeIntervalSince1970 * 1_000).rounded()),
                "status": message.status.rawValue,
            ]
        }
        return Self.jsonSuccess(["group_id": groupID,
                                 "messages": values, "count": values.count])
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

    func sendGroupText(
        appState: RemoteIMAppState,
        arguments: [String: Any]
    ) async -> AIMaiChatHostToolExecution {
        let groupID = Self.string(arguments, key: "group_id")
        let text = Self.string(arguments, key: "text")
        guard !text.isEmpty, appState.connectionState == .connected,
              appState.groups.contains(where: { $0.groupID == groupID }) else {
            return .failure(code: "invalid_input", message: "joined group_id and text are required")
        }
        var queuedID: UUID?
        guard await appState.sendGroupText(text, to: groupID,
                                           onQueued: { queuedID = $0 }) else {
            return .failure(code: "network",
                            message: appState.errorMessage ?? "MaiChat did not send the group message")
        }
        let sentMessage = queuedID.flatMap { appState.chatState.message(id: $0) }
        return Self.jsonSuccess([
            "queued": true,
            "sent": true,
            "group_id": groupID,
            "message_id": sentMessage?.id.uuidString ?? "",
            "remote_id": sentMessage?.remoteID ?? "",
        ])
    }

    /// 将 Agent 选择的图片、视频或文件发送到指定 IM 会话。
    /// 先核对账号、联系人与本地路径，发送成功后才返回可引用的消息标识。
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
                sent = await appState.sendImageFile(image, to: peerID, caption: caption)
            case "video":
                let video = try await Self.prepareAgentVideo(source)
                sent = await appState.sendVideoFile(video, to: peerID, caption: caption)
            default:
                let audio = try await Self.prepareAgentAudio(source)
                sent = await appState.sendFile(audio, to: peerID, caption: caption)
            }
            guard sent else {
                return .failure(code: "internal",
                                message: appState.errorMessage ?? "MaiChat did not send the media")
            }
            let mediaMessageID = appState.locallyQueuedMessageID?.uuidString ?? ""
            return Self.jsonSuccess([
                "sent": true, "peer_id": peerID, "type": kind,
                "message_kind": kind == "audio" ? "file" : kind,
                "message_id": mediaMessageID, "caption_attached": !caption.isEmpty,
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
