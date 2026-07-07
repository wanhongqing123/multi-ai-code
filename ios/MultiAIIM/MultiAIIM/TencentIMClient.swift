import Foundation
import MultiAIIMCore

#if canImport(ImSDK_Plus)
import ImSDK_Plus

final class TencentIMClient: NSObject, RemoteIMClient, V2TIMSimpleMsgListener, V2TIMAdvancedMsgListener, V2TIMSDKListener {
    var onIncomingText: ((IncomingRemoteIMText) -> Void)?
    var onIncomingVoice: ((IncomingRemoteIMVoice) -> Void)?
    var onIncomingImage: ((IncomingRemoteIMImage) -> Void)?
    var onPresenceStatusChanged: (([String: RemoteIMPresenceStatus]) -> Void)?
    private var initializedSDKAppID: Int?
    private var hasRegisteredIMSDKListener = false

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
        if !hasRegisteredIMSDKListener {
            V2TIMManager.sharedInstance().addIMSDKListener(listener: self)
            hasRegisteredIMSDKListener = true
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
        if hasRegisteredIMSDKListener {
            V2TIMManager.sharedInstance().removeIMSDKListener(listener: self)
            hasRegisteredIMSDKListener = false
        }
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

    func refreshPresenceStatuses(userIDs: [String]) async throws -> [String: RemoteIMPresenceStatus] {
        let cleanedUserIDs = Self.cleanUserIDs(userIDs)
        guard !cleanedUserIDs.isEmpty else { return [:] }
        return try await withCheckedThrowingContinuation { continuation in
            V2TIMManager.sharedInstance().getUserStatus(
                userIDList: cleanedUserIDs,
                succ: { userStatusList in
                    continuation.resume(returning: Self.statusMap(from: userStatusList ?? []))
                },
                fail: { code, desc in
                    continuation.resume(
                        throwing: RemoteIMClientError.operationFailed(
                            code: code,
                            description: desc ?? "getUserStatus failed"
                        )
                    )
                }
            )
        }
    }

    func subscribePresenceStatuses(userIDs: [String]) async throws {
        let cleanedUserIDs = Self.cleanUserIDs(userIDs)
        guard !cleanedUserIDs.isEmpty else { return }
        try await withCheckedThrowingContinuation { continuation in
            V2TIMManager.sharedInstance().subscribeUserStatus(
                userIDList: cleanedUserIDs,
                succ: {
                    continuation.resume()
                },
                fail: { code, desc in
                    continuation.resume(
                        throwing: RemoteIMClientError.operationFailed(
                            code: code,
                            description: desc ?? "subscribeUserStatus failed"
                        )
                    )
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

    func sendImage(to userID: String, image: RemoteIMImageFile) async throws {
        let message = V2TIMManager.sharedInstance().createImageMessage(imagePath: image.fileURL.path)
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
                            description: desc ?? "send image failed"
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
        guard !msg.isSelf else { return }
        let fromUserID = msg.sender ?? msg.userID ?? ""
        guard !fromUserID.isEmpty else { return }

        if let soundElem = msg.soundElem {
            handleIncomingSound(msg: msg, soundElem: soundElem, fromUserID: fromUserID)
            return
        }

        if let imageElem = msg.imageElem {
            handleIncomingImage(msg: msg, imageElem: imageElem, fromUserID: fromUserID)
        }
    }

    private nonisolated func handleIncomingSound(
        msg: V2TIMMessage,
        soundElem: V2TIMSoundElem,
        fromUserID: String
    ) {
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

    private nonisolated func handleIncomingImage(
        msg: V2TIMMessage,
        imageElem: V2TIMImageElem,
        fromUserID: String
    ) {
        guard let image = Self.preferredImage(from: imageElem.imageList) else { return }
        let remoteID = image.uuid ?? msg.msgID
        let targetURL = Self.imageCacheURL(remoteID: remoteID, messageID: msg.msgID, imageURL: image.url)
        let width = image.width > 0 ? Int(image.width) : nil
        let height = image.height > 0 ? Int(image.height) : nil
        let sizeBytes = image.size > 0 ? Int(image.size) : nil
        image.downloadImage(
            path: targetURL.path,
            progress: nil,
            succ: {
                Task { @MainActor [weak self, fromUserID, targetURL, remoteID, width, height, sizeBytes] in
                    let event = IncomingRemoteIMImage(
                        fromUserID: fromUserID,
                        fileURL: targetURL,
                        remoteID: remoteID,
                        width: width,
                        height: height,
                        sizeBytes: sizeBytes
                    )
                    self?.onIncomingImage?(event)
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

    private nonisolated static func preferredImage(from imageList: [V2TIMImage]) -> V2TIMImage? {
        imageList.max { left, right in
            imageScore(left) < imageScore(right)
        }
    }

    private nonisolated static func imageScore(_ image: V2TIMImage) -> Int {
        let sizeScore = max(0, Int(image.size))
        if sizeScore > 0 { return sizeScore }
        return max(0, Int(image.width)) * max(0, Int(image.height))
    }

    private nonisolated static func imageCacheURL(
        remoteID: String?,
        messageID: String?,
        imageURL: String?
    ) -> URL {
        let rawName = remoteID ?? messageID ?? UUID().uuidString
        let safeName = rawName
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: ":", with: "_")
        let directory = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("RemoteIMImage", isDirectory: true)
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let pathExtension = URL(string: imageURL ?? "")?.pathExtension
        return directory
            .appendingPathComponent(safeName)
            .appendingPathExtension(pathExtension?.isEmpty == false ? pathExtension! : "jpg")
    }

    @objc nonisolated func onUserStatusChanged(userStatusList: [V2TIMUserStatus]!) {
        guard let userStatusList else { return }
        let updates = Self.statusMap(from: userStatusList)
        guard !updates.isEmpty else { return }
        Task { @MainActor [weak self, updates] in
            self?.onPresenceStatusChanged?(updates)
        }
    }

    private nonisolated static func cleanUserIDs(_ userIDs: [String]) -> [String] {
        let normalized = userIDs
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }
        var deduped: [String] = []
        var visited = Set<String>()
        for userID in normalized where visited.insert(userID).inserted {
            deduped.append(userID)
        }
        return deduped
    }

    private nonisolated static func statusMap(from userStatusList: [V2TIMUserStatus]) -> [String: RemoteIMPresenceStatus] {
        userStatusList.reduce(into: [:]) { partialResult, item in
            guard let userID = item.userID,
                  !userID.isEmpty else { return }
            partialResult[userID] = Self.presenceStatus(from: item.statusType)
        }
    }

    private nonisolated static func presenceStatus(from sdkStatus: V2TIMUserStatusType) -> RemoteIMPresenceStatus {
        let statusValue = Int(sdkStatus.rawValue)
        switch statusValue {
        case 1:
            return .online
        case 2, 3:
            return .offline
        default:
            return .unknown
        }
    }

}
#else
final class TencentIMClient: RemoteIMClient {
    var onIncomingText: ((IncomingRemoteIMText) -> Void)?
    var onIncomingVoice: ((IncomingRemoteIMVoice) -> Void)?
    var onIncomingImage: ((IncomingRemoteIMImage) -> Void)?
    var onPresenceStatusChanged: (([String: RemoteIMPresenceStatus]) -> Void)?

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

    func sendImage(to userID: String, image: RemoteIMImageFile) async throws {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func refreshPresenceStatuses(userIDs: [String]) async throws -> [String: RemoteIMPresenceStatus] {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func subscribePresenceStatuses(userIDs: [String]) async throws {
        throw RemoteIMClientError.sdkNotIntegrated
    }

}
#endif
