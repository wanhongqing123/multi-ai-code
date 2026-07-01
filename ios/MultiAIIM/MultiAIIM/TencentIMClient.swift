import Foundation

#if canImport(ImSDK_Plus)
import ImSDK_Plus

final class TencentIMClient: NSObject, RemoteIMClient, V2TIMSimpleMsgListener, V2TIMAdvancedMsgListener {
    var onIncomingText: ((IncomingRemoteIMText) -> Void)?
    var onIncomingVoice: ((IncomingRemoteIMVoice) -> Void)?
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
        V2TIMManager.sharedInstance().addAdvancedMsgListener(listener: self)
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
        V2TIMManager.sharedInstance().removeAdvancedMsgListener(listener: self)
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

    func sendVoice(to userID: String, recording: RemoteIMVoiceRecording) async throws {
        let message = V2TIMManager.sharedInstance().createSoundMessage(
            audioFilePath: recording.fileURL.path,
            duration: Int32(recording.durationSeconds)
        )
        try await withCheckedThrowingContinuation { continuation in
            V2TIMManager.sharedInstance().sendMessage(
                message: message,
                receiver: userID,
                groupID: nil,
                priority: V2TIMMessagePriority(rawValue: 0)!,
                onlineUserOnly: false,
                offlinePushInfo: nil,
                progress: nil,
                succ: {
                    continuation.resume()
                },
                fail: { code, desc in
                    continuation.resume(
                        throwing: RemoteIMClientError.operationFailed(
                            code: code,
                            description: desc ?? "send voice failed"
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

    nonisolated func onRecvNewMessage(msg: V2TIMMessage) {
        guard !msg.isSelf, let soundElem = msg.soundElem else { return }
        let fromUserID = msg.sender ?? msg.userID ?? ""
        guard !fromUserID.isEmpty else { return }

        let durationSeconds = max(1, Int(soundElem.duration))
        let remoteID = soundElem.uuid ?? msg.msgID
        let targetURL = Self.voiceCacheURL(remoteID: remoteID, messageID: msg.msgID)

        soundElem.downloadSound(
            path: targetURL.path,
            progress: nil,
            succ: {
                Task { @MainActor [weak self, fromUserID, targetURL, durationSeconds, remoteID] in
                    let event = IncomingRemoteIMVoice(
                        fromUserID: fromUserID,
                        fileURL: targetURL,
                        durationSeconds: durationSeconds,
                        remoteID: remoteID
                    )
                    self?.onIncomingVoice?(event)
                }
            },
            fail: { _, _ in }
        )
    }

    private nonisolated static func voiceCacheURL(remoteID: String?, messageID: String?) -> URL {
        let rawName = remoteID ?? messageID ?? UUID().uuidString
        let safeName = rawName
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: ":", with: "_")
        let directory = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("RemoteIMVoice", isDirectory: true)
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return directory.appendingPathComponent(safeName).appendingPathExtension("m4a")
    }
}
#else
final class TencentIMClient: RemoteIMClient {
    var onIncomingText: ((IncomingRemoteIMText) -> Void)?
    var onIncomingVoice: ((IncomingRemoteIMVoice) -> Void)?

    func connect(sdkAppID: Int, userID: String, userSig: String) async throws {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func disconnect() async {}

    func sendText(to userID: String, text: String) async throws {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func sendVoice(to userID: String, recording: RemoteIMVoiceRecording) async throws {
        throw RemoteIMClientError.sdkNotIntegrated
    }
}
#endif
