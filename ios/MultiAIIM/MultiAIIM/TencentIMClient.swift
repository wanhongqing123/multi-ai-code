import Foundation

#if canImport(ImSDK_Plus)
import ImSDK_Plus

final class TencentIMClient: NSObject, RemoteIMClient, V2TIMSimpleMsgListener {
    var onIncomingText: ((IncomingRemoteIMText) -> Void)?
    private var initializedSDKAppID: Int?

    func connect(sdkAppID: Int, userID: String, userSig: String) async throws {
        if initializedSDKAppID != sdkAppID {
            let config = V2TIMSDKConfig()
            let initialized = V2TIMManager.sharedInstance().initSDK(
                Int32(sdkAppID),
                config: config
            )
            guard initialized else { throw RemoteIMClientError.sdkInitializationFailed }
            initializedSDKAppID = sdkAppID
        }
        V2TIMManager.sharedInstance().addSimpleMsgListener(listener: self)
        try await withCheckedThrowingContinuation { continuation in
            V2TIMManager.sharedInstance().login(
                userID: userID,
                userSig: userSig,
                succ: {
                    continuation.resume()
                },
                fail: { code, desc in
                    continuation.resume(
                        throwing: RemoteIMClientError.operationFailed(
                            code: code,
                            description: desc ?? "login failed"
                        )
                    )
                }
            )
        }
    }

    func disconnect() async {
        V2TIMManager.sharedInstance().removeSimpleMsgListener(listener: self)
        await withCheckedContinuation { continuation in
            V2TIMManager.sharedInstance().logout(
                succ: {
                    continuation.resume()
                },
                fail: { _, _ in
                    continuation.resume()
                }
            )
        }
    }

    func sendText(to userID: String, text: String) async throws {
        try await withCheckedThrowingContinuation { continuation in
            _ = V2TIMManager.sharedInstance().sendC2CTextMessage(
                text: text,
                to: userID,
                succ: {
                    continuation.resume()
                },
                fail: { code, desc in
                    continuation.resume(
                        throwing: RemoteIMClientError.operationFailed(
                            code: code,
                            description: desc ?? "send failed"
                        )
                    )
                }
            )
        }
    }

    nonisolated func onRecvC2CTextMessage(msgID: String, sender: V2TIMUserInfo, text: String?) {
        guard let userID = sender.userID, !userID.isEmpty, let text, !text.isEmpty else { return }
        Task { @MainActor [weak self, userID, text] in
            let event = IncomingRemoteIMText(fromUserID: userID, text: text)
            self?.onIncomingText?(event)
        }
    }
}
#else
final class TencentIMClient: RemoteIMClient {
    var onIncomingText: ((IncomingRemoteIMText) -> Void)?

    func connect(sdkAppID: Int, userID: String, userSig: String) async throws {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func disconnect() async {}

    func sendText(to userID: String, text: String) async throws {
        throw RemoteIMClientError.sdkNotIntegrated
    }
}
#endif
