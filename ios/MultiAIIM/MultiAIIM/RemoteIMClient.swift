import Foundation
import MultiAIIMCore

struct IncomingRemoteIMText: Equatable {
    let fromUserID: String
    let text: String
}

struct IncomingRemoteIMVoice: Equatable {
    let fromUserID: String
    let fileURL: URL
    let durationSeconds: Int
    let remoteID: String?
}

struct IncomingRemoteIMImage: Equatable {
    let fromUserID: String
    let fileURL: URL
    let remoteID: String?
    let width: Int?
    let height: Int?
    let sizeBytes: Int?
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

enum RemoteIMClientError: Error, LocalizedError {
    case sdkNotIntegrated
    case sdkInitializationFailed
    case operationFailed(code: Int32, description: String)

    var errorDescription: String? {
        switch self {
        case .sdkNotIntegrated:
            return "IM SDK 未集成。请在 ios/MultiAIIM 下执行 pod install，并打开 MultiAIIM.xcworkspace。"
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
    var onPresenceStatusChanged: (([String: RemoteIMPresenceStatus]) -> Void)? { get set }

    func connect(sdkAppID: Int, userID: String, userSig: String) async throws
    func disconnect() async
    func sendText(to userID: String, text: String) async throws
    func sendVoice(to userID: String, recording: RemoteIMVoiceRecording) async throws
    func sendImage(to userID: String, image: RemoteIMImageFile) async throws
    func refreshPresenceStatuses(userIDs: [String]) async throws -> [String: RemoteIMPresenceStatus]
    func subscribePresenceStatuses(userIDs: [String]) async throws
}
