import Foundation
import MaiChatCore

enum RemoteIMMessageOrigin: String, Equatable, Sendable {
    case human
    case machine
}

struct IncomingRemoteIMText: Equatable {
    let fromUserID: String
    let text: String
    let remoteID: String?
    let origin: RemoteIMMessageOrigin?
    let createdAt: Date
}

struct IncomingRemoteIMVoice: Equatable {
    let fromUserID: String
    let fileURL: URL
    let durationSeconds: Int
    let remoteID: String?
    let origin: RemoteIMMessageOrigin?
    let createdAt: Date
}

struct IncomingRemoteIMImage: Equatable {
    let fromUserID: String
    let fileURL: URL
    let remoteID: String?
    let width: Int?
    let height: Int?
    let sizeBytes: Int?
    let origin: RemoteIMMessageOrigin?
    let createdAt: Date
}

struct IncomingRemoteIMFile: Equatable {
    let fromUserID: String
    let fileURL: URL
    let fileName: String
    let mimeType: String
    let remoteID: String?
    let sizeBytes: Int?
    let origin: RemoteIMMessageOrigin?
    let createdAt: Date
}

struct IncomingRemoteIMVideo: Equatable {
    let fromUserID: String
    let videoFileURL: URL
    let coverFileURL: URL?
    let durationSeconds: Int
    let width: Int
    let height: Int
    let sizeBytes: Int64
    let remoteID: String?
    let origin: RemoteIMMessageOrigin?
    let createdAt: Date
    let stage: RemoteIMVideoDownloadStage
}

struct RemoteIMVoiceRecording: Equatable {
    let fileURL: URL
    let durationSeconds: Int
}

struct RemoteIMImageFile: Equatable {
    let fileURL: URL
    let width: Int?
    let height: Int?
    let sizeBytes: Int?
}

struct RemoteIMFile: Equatable {
    let fileURL: URL
    let fileName: String
    let mimeType: String
    let sizeBytes: Int?
}

struct RemoteIMVideoFile: Equatable {
    let fileURL: URL
    let coverFileURL: URL
    let fileType: String
    let durationSeconds: Int
    let width: Int
    let height: Int
    let sizeBytes: Int64
}

struct RemoteIMSendReceipt: Equatable {
    let remoteID: String?
    let createdAt: Date?
}

struct RemoteIMUserProfile: Equatable {
    let userID: String
    let displayName: String
    let avatarURL: String?
}

enum RemoteIMClientError: Error, LocalizedError {
    case sdkNotIntegrated
    case sdkInitializationFailed
    case operationFailed(code: Int32, description: String)

    var errorDescription: String? {
        switch self {
        case .sdkNotIntegrated:
            return "IM SDK 未集成。请在 ios/MaiChat 下执行 pod install，并打开 MaiChat.xcworkspace。"
        case .sdkInitializationFailed:
            return "IM SDK 初始化失败"
        case let .operationFailed(code, description):
            return "IM 操作失败(\(code)): \(description)"
        }
    }
}

@MainActor
protocol RemoteIMClient: AnyObject {
    var onIncomingText: ((IncomingRemoteIMText) -> Void)? { get set }
    var onIncomingVoice: ((IncomingRemoteIMVoice) -> Void)? { get set }
    var onIncomingImage: ((IncomingRemoteIMImage) -> Void)? { get set }
    var onIncomingFile: ((IncomingRemoteIMFile) -> Void)? { get set }
    var onIncomingVideo: ((IncomingRemoteIMVideo) -> Void)? { get set }
    var onPresenceStatusChanged: (([String: RemoteIMPresenceStatus]) -> Void)? { get set }

    func connect(sdkAppID: Int, userID: String, userSig: String) async throws
    func disconnect() async
    func sendText(to userID: String, text: String, origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt
    func sendVoice(to userID: String, recording: RemoteIMVoiceRecording, origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt
    func sendImage(to userID: String, image: RemoteIMImageFile, origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt
    func sendVideo(to userID: String, video: RemoteIMVideoFile, origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt
    func sendFile(to userID: String, file: RemoteIMFile, origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt
    func deleteContact(userID: String) async throws
    func clearHistory(userID: String) async throws
    func refreshUserProfiles(userIDs: [String]) async throws -> [RemoteIMUserProfile]
    func refreshPresenceStatuses(userIDs: [String]) async throws -> [String: RemoteIMPresenceStatus]
    func subscribePresenceStatuses(userIDs: [String]) async throws
}

extension RemoteIMClient {
    // Messages initiated from the MaiChat UI are human-originated by default.
    func sendText(to userID: String, text: String) async throws -> RemoteIMSendReceipt {
        try await sendText(to: userID, text: text, origin: .human)
    }

    func sendVoice(to userID: String, recording: RemoteIMVoiceRecording) async throws -> RemoteIMSendReceipt {
        try await sendVoice(to: userID, recording: recording, origin: .human)
    }

    func sendImage(to userID: String, image: RemoteIMImageFile) async throws -> RemoteIMSendReceipt {
        try await sendImage(to: userID, image: image, origin: .human)
    }

    func sendVideo(to userID: String, video: RemoteIMVideoFile) async throws -> RemoteIMSendReceipt {
        try await sendVideo(to: userID, video: video, origin: .human)
    }

    func sendFile(to userID: String, file: RemoteIMFile) async throws -> RemoteIMSendReceipt {
        try await sendFile(to: userID, file: file, origin: .human)
    }
}
