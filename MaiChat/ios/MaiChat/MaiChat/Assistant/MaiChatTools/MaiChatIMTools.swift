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
        let values = messages.map { Self.messageJSON($0, peerID: peerID) }
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
        let values = hits.map { Self.messageJSON($0.message, peerID: $0.peerUserID) }
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
}
