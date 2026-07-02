import Foundation

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

struct RemoteIMVoiceRecording: Equatable {
    let fileURL: URL
    let durationSeconds: Int
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

    func connect(sdkAppID: Int, userID: String, userSig: String) async throws
    func disconnect() async
    func sendText(to userID: String, text: String) async throws
    func sendVoice(to userID: String, recording: RemoteIMVoiceRecording) async throws
}
