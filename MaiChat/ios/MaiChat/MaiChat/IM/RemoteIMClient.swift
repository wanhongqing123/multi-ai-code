// 文件作用：定义远程 IM 客户端协议及页面与具体 SDK 之间共享的数据契约。
// 定义 IM 登录、消息、媒体和状态事件的客户端接口。
// 具体 SDK 或测试实现经同一协议接入状态层，界面不直接操作供应商对象。
import Foundation
import MaiChatCore

struct IncomingRemoteIMText: Equatable {
    let fromUserID: String
    let text: String
    let remoteID: String?
    let origin: RemoteIMMessageOrigin?
    let approvalRequest: RemoteIMApprovalRequest?
    let approvalDecision: RemoteIMApprovalDecision?
    var quote: RemoteIMQuote? = nil
    let createdAt: Date
}

struct IncomingRemoteIMActivity: Equatable {
    let fromUserID: String
    let signal: RemoteIMActivitySignal
}

struct IncomingRemoteIMVoice: Equatable {
    let fromUserID: String
    let fileURL: URL
    let durationSeconds: Int
    let remoteID: String?
    let origin: RemoteIMMessageOrigin?
    var quote: RemoteIMQuote? = nil
    let createdAt: Date
}

struct IncomingRemoteIMImage: Equatable {
    let fromUserID: String
    let fileURL: URL
    let remoteID: String?
    let width: Int?
    let height: Int?
    let sizeBytes: Int?
    let caption: String?
    let captionAbove: Bool
    let origin: RemoteIMMessageOrigin?
    var quote: RemoteIMQuote? = nil
    let createdAt: Date
}

struct IncomingRemoteIMFile: Equatable {
    /// SDK 账号在异步下载开始前固定，不到交付时才读取当前账号。
    let accountTag: String
    let fromUserID: String
    let fileURL: URL
    let fileName: String
    let mimeType: String
    let remoteID: String?
    let sizeBytes: Int?
    let caption: String?
    let captionAbove: Bool
    let origin: RemoteIMMessageOrigin?
    var quote: RemoteIMQuote? = nil
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
    let caption: String?
    let captionAbove: Bool
    let remoteID: String?
    let origin: RemoteIMMessageOrigin?
    var quote: RemoteIMQuote? = nil
    let createdAt: Date
    let stage: RemoteIMVideoDownloadStage
}

struct RemoteIMVoiceRecording: Equatable, Sendable {
    let fileURL: URL
    let durationSeconds: Int
}

struct RemoteIMImageFile: Equatable, Sendable {
    let fileURL: URL
    let width: Int?
    let height: Int?
    let sizeBytes: Int?
}

struct RemoteIMFile: Equatable, Sendable {
    let fileURL: URL
    let fileName: String
    let mimeType: String
    let sizeBytes: Int?
}

struct RemoteIMVideoFile: Equatable, Sendable {
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
    var onIncomingActivity: ((IncomingRemoteIMActivity) -> Void)? { get set }
    var onIncomingVoice: ((IncomingRemoteIMVoice) -> Void)? { get set }
    var onIncomingImage: ((IncomingRemoteIMImage) -> Void)? { get set }
    var onIncomingFile: ((IncomingRemoteIMFile) -> Void)? { get set }
    var onIncomingVideo: ((IncomingRemoteIMVideo) -> Void)? { get set }
    var onPresenceStatusChanged: (([String: RemoteIMPresenceStatus]) -> Void)? { get set }

    func connect(sdkAppID: Int, userID: String, userSig: String) async throws
    func disconnect() async
    func sendText(
        to userID: String,
        text: String,
        origin: RemoteIMMessageOrigin,
        quote: RemoteIMQuote?
    ) async throws -> RemoteIMSendReceipt
    func sendActivity(
        to userID: String,
        signal: RemoteIMActivitySignal
    ) async throws
    func sendApprovalDecision(
        to userID: String,
        token: String,
        action: RemoteIMApprovalAction
    ) async throws -> RemoteIMSendReceipt
    func sendVoice(to userID: String, recording: RemoteIMVoiceRecording, origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt
    func sendImage(to userID: String, image: RemoteIMImageFile, origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt
    func sendVideo(to userID: String, video: RemoteIMVideoFile, origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt
    func sendFile(to userID: String, file: RemoteIMFile, origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt
    func deleteContact(userID: String) async throws
    func clearHistory(userID: String) async throws
    func refreshUserProfiles(userIDs: [String]) async throws -> [RemoteIMUserProfile]
    func setSelfAvatar(url: String) async throws
    func uploadAvatar(fileURL: URL) async throws -> String
    func refreshPresenceStatuses(userIDs: [String]) async throws -> [String: RemoteIMPresenceStatus]
    func subscribePresenceStatuses(userIDs: [String]) async throws
    func updateApplicationBadgeCount(_ count: Int)
}

extension RemoteIMClient {
    func uploadAvatar(fileURL: URL) async throws -> String {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func setSelfAvatar(url: String) async throws {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    // MaiChat 界面主动发起的消息默认标记为用户来源。
    func sendText(to userID: String, text: String) async throws -> RemoteIMSendReceipt {
        try await sendText(to: userID, text: text, origin: .human, quote: nil)
    }

    func sendText(
        to userID: String,
        text: String,
        origin: RemoteIMMessageOrigin
    ) async throws -> RemoteIMSendReceipt {
        try await sendText(to: userID, text: text, origin: origin, quote: nil)
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
