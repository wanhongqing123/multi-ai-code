import Foundation
import MaiChatCore
import UniformTypeIdentifiers

#if canImport(ImSDK_Plus)
import ImSDK_Plus

private final class ApplicationBadgeSnapshot: @unchecked Sendable {
    private let lock = NSLock()
    private var value: UInt32 = 0

    func store(_ nextValue: UInt32) {
        lock.lock()
        value = nextValue
        lock.unlock()
    }

    func load() -> UInt32 {
        lock.lock()
        let currentValue = value
        lock.unlock()
        return currentValue
    }
}

final class TencentIMClient:
    NSObject,
    RemoteIMClient,
    V2TIMSimpleMsgListener,
    V2TIMAdvancedMsgListener,
    V2TIMSDKListener,
    V2TIMAPNSListener
{
    var onIncomingText: ((IncomingRemoteIMText) -> Void)?
    var onIncomingVoice: ((IncomingRemoteIMVoice) -> Void)?
    var onIncomingImage: ((IncomingRemoteIMImage) -> Void)?
    var onIncomingFile: ((IncomingRemoteIMFile) -> Void)?
    var onIncomingVideo: ((IncomingRemoteIMVideo) -> Void)?
    var onPresenceStatusChanged: (([String: RemoteIMPresenceStatus]) -> Void)?
    private var initializedSDKAppID: Int?
    private var hasRegisteredIMSDKListener = false
    private nonisolated let applicationBadgeSnapshot = ApplicationBadgeSnapshot()

    func connect(sdkAppID: Int, userID: String, userSig: String) async throws {
        if initializedSDKAppID != sdkAppID {
            let config = V2TIMSDKConfig()
            let initialized = V2TIMManager.sharedInstance().initSDK(
                Int32(sdkAppID),
                config: config
            )
            guard initialized else {
                Self.logSDK(level: .error, event: "sdk-init-failed")
                throw RemoteIMClientError.sdkInitializationFailed
            }
            initializedSDKAppID = sdkAppID
        }
        if !hasRegisteredIMSDKListener {
            V2TIMManager.sharedInstance().addIMSDKListener(listener: self)
            hasRegisteredIMSDKListener = true
        }
        V2TIMManager.sharedInstance().setAPNSListener(apnsListener: self)
        V2TIMManager.sharedInstance().addSimpleMsgListener(listener: self)
        V2TIMManager.sharedInstance().addAdvancedMsgListener(listener: self)
        Self.logSDK(level: .info, event: "login-start", fields: ["peer": Self.peerTag(userID)])
        return try await withCheckedThrowingContinuation { continuation in
            V2TIMManager.sharedInstance().login(
                userID: userID,
                userSig: userSig,
                succ: {
                    Self.logSDK(
                        level: .info,
                        event: "login-finished",
                        fields: ["result": "ok", "peer": Self.peerTag(userID)]
                    )
                    V2TIMManager.sharedInstance().getTotalUnreadMessageCount(
                        succ: { count in
                            Self.logSDK(
                                level: .info,
                                event: "sdk-unread-count-read",
                                fields: ["unread_count": String(count)]
                            )
                        },
                        fail: { code, _ in
                            Self.logSDK(
                                level: .warning,
                                event: "sdk-unread-count-read-failed",
                                fields: ["code": String(code)]
                            )
                        }
                    )
                    continuation.resume()
                },
                fail: { code, desc in
                    Self.logSDK(
                        level: .error,
                        event: "login-finished",
                        fields: [
                            "result": "failed",
                            "code": String(code),
                            "peer": Self.peerTag(userID)
                        ]
                    )
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
        V2TIMManager.sharedInstance().setAPNSListener(apnsListener: nil)
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

    func updateApplicationBadgeCount(_ count: Int) {
        let safeCount = UInt32(clamping: max(0, count))
        applicationBadgeSnapshot.store(safeCount)
    }

    @objc nonisolated func onSetAPPUnreadCount() -> UInt32 {
        let count = applicationBadgeSnapshot.load()
        Self.logSDK(
            level: .info,
            event: "sdk-app-badge-overridden",
            fields: ["badge_count": String(count)]
        )
        return count
    }

    @objc nonisolated func onConnecting() {
        Self.logSDK(level: .info, event: "sdk-connecting")
    }

    @objc nonisolated func onConnectSuccess() {
        Self.logSDK(level: .info, event: "sdk-connected")
    }

    @objc nonisolated func onConnectFailed(_ code: Int32, err: String?) {
        Self.logSDK(
            level: .error,
            event: "sdk-connect-failed",
            fields: ["code": String(code)]
        )
    }

    @objc nonisolated func onKickedOffline() {
        Self.logSDK(level: .error, event: "sdk-kicked-offline")
    }

    @objc nonisolated func onUserSigExpired() {
        Self.logSDK(level: .error, event: "sdk-user-sig-expired")
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

    func refreshUserProfiles(userIDs: [String]) async throws -> [RemoteIMUserProfile] {
        let cleanedUserIDs = Self.cleanUserIDs(userIDs)
        guard !cleanedUserIDs.isEmpty else { return [] }
        return try await withCheckedThrowingContinuation { continuation in
            V2TIMManager.sharedInstance().getUsersInfo(
                cleanedUserIDs,
                succ: { infoList in
                    continuation.resume(returning: (infoList ?? []).compactMap(Self.profile(from:)))
                },
                fail: { code, desc in
                    continuation.resume(
                        throwing: RemoteIMClientError.operationFailed(
                            code: code,
                            description: desc ?? "getUsersInfo failed"
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

    func sendText(to userID: String, text: String, origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt {
        guard let message = V2TIMManager.sharedInstance().createTextMessage(text: text) else {
            Self.logMessageCreateFailure(kind: "text", peerUserID: userID)
            throw RemoteIMClientError.operationFailed(code: -1, description: "create text message failed")
        }
        return try await send(
            message: message,
            to: userID,
            kind: "text",
            origin: origin,
            metadata: ["content_bytes": String(text.lengthOfBytes(using: .utf8))],
            failureDescription: "send text failed"
        )
    }

    func sendApprovalDecision(
        to userID: String,
        token: String,
        action: RemoteIMApprovalAction
    ) async throws -> RemoteIMSendReceipt {
        guard RemoteIMApprovalRequest.isValidToken(token),
              let message = V2TIMManager.sharedInstance().createTextMessage(
                text: action.decisionDisplayText
              )
        else {
            Self.logMessageCreateFailure(kind: "approval-decision", peerUserID: userID)
            throw RemoteIMClientError.operationFailed(
                code: -1,
                description: "create approval decision failed"
            )
        }
        return try await send(
            message: message,
            to: userID,
            kind: "approval-decision",
            origin: .human,
            cloudCustomData: RemoteIMCloudMetadataCodec.encode(RemoteIMCloudMetadata(
                origin: .human,
                interaction: .approvalDecision(token: token, action: action)
            )),
            metadata: ["action": action.rawValue],
            failureDescription: "send approval decision failed"
        )
    }

    func sendVoice(to userID: String, recording: RemoteIMVoiceRecording, origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt {
        guard let message = V2TIMManager.sharedInstance().createSoundMessage(
            audioFilePath: recording.fileURL.path,
            duration: Int32(recording.durationSeconds)
        ) else {
            Self.logMessageCreateFailure(kind: "voice", peerUserID: userID)
            throw RemoteIMClientError.operationFailed(code: -1, description: "create voice message failed")
        }
        return try await send(
            message: message,
            to: userID,
            kind: "voice",
            origin: origin,
            metadata: ["duration_seconds": String(recording.durationSeconds)],
            failureDescription: "send voice failed"
        )
    }

    func sendImage(to userID: String, image: RemoteIMImageFile, origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt {
        guard let message = V2TIMManager.sharedInstance().createImageMessage(imagePath: image.fileURL.path) else {
            Self.logMessageCreateFailure(kind: "image", peerUserID: userID)
            throw RemoteIMClientError.operationFailed(code: -1, description: "create image message failed")
        }
        return try await send(
            message: message,
            to: userID,
            kind: "image",
            origin: origin,
            metadata: [
                "width": image.width.map(String.init) ?? "unknown",
                "height": image.height.map(String.init) ?? "unknown",
                "bytes": image.sizeBytes.map(String.init) ?? "unknown",
            ],
            failureDescription: "send image failed"
        )
    }

    func sendVideo(to userID: String, video: RemoteIMVideoFile, origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt {
        guard let message = V2TIMManager.sharedInstance().createVideoMessage(
            videoFilePath: video.fileURL.path,
            type: video.fileType,
            duration: Int32(clamping: video.durationSeconds),
            snapshotPath: video.coverFileURL.path
        ) else {
            Self.logMessageCreateFailure(kind: "video", peerUserID: userID)
            throw RemoteIMClientError.operationFailed(code: -1, description: "create video message failed")
        }
        return try await send(
            message: message,
            to: userID,
            kind: "video",
            origin: origin,
            metadata: [
                "duration_seconds": String(video.durationSeconds),
                "width": String(video.width),
                "height": String(video.height),
                "bytes": String(video.sizeBytes),
            ],
            failureDescription: "send video failed"
        )
    }

    func sendFile(to userID: String, file: RemoteIMFile, origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt {
        guard let message = V2TIMManager.sharedInstance().createFileMessage(
            filePath: file.fileURL.path,
            fileName: file.fileName
        ) else {
            Self.logMessageCreateFailure(kind: "file", peerUserID: userID)
            throw RemoteIMClientError.operationFailed(code: -1, description: "create file message failed")
        }
        return try await send(
            message: message,
            to: userID,
            kind: "file",
            origin: origin,
            metadata: [
                "bytes": file.sizeBytes.map(String.init) ?? "unknown",
                "mime_type": file.mimeType,
            ],
            failureDescription: "send file failed"
        )
    }

    func deleteContact(userID: String) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            V2TIMManager.sharedInstance().deleteFromFriendList(
                userIDList: [userID],
                deleteType: V2TIMFriendType(rawValue: 2)!,
                succ: { results in
                    if let failure = results?.first(where: { $0.resultCode != 0 }) {
                        continuation.resume(
                            throwing: RemoteIMClientError.operationFailed(
                                code: Int32(failure.resultCode),
                                description: failure.resultInfo ?? "delete friend failed"
                            )
                        )
                    } else {
                        continuation.resume()
                    }
                },
                fail: { code, desc in
                    continuation.resume(
                        throwing: RemoteIMClientError.operationFailed(
                            code: code,
                            description: desc ?? "delete friend failed"
                        )
                    )
                }
            )
        }
    }

    func clearHistory(userID: String) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            V2TIMManager.sharedInstance().clearC2CHistoryMessage(
                userID: userID,
                succ: {
                    continuation.resume()
                },
                fail: { code, desc in
                    continuation.resume(
                        throwing: RemoteIMClientError.operationFailed(
                            code: code,
                            description: desc ?? "clear history failed"
                        )
                    )
                }
            )
        }
    }

    private func send(
        message: V2TIMMessage,
        to userID: String,
        kind: String,
        origin: RemoteIMMessageOrigin,
        cloudCustomData: Data? = nil,
        metadata: [String: String],
        failureDescription: String
    ) async throws -> RemoteIMSendReceipt {
        message.cloudCustomData = cloudCustomData ?? RemoteIMCloudMetadataCodec.encode(
            RemoteIMCloudMetadata(origin: origin)
        )
        let startedAt = ProcessInfo.processInfo.systemUptime
        var startFields = metadata
        startFields["operation"] = DiagnosticLogPrivacy.stableTag(
            UUID().uuidString,
            prefix: "op"
        )
        startFields["kind"] = kind
        startFields["peer"] = Self.peerTag(userID)
        Self.logSDK(level: .info, event: "message-send-start", fields: startFields)
        return try await withCheckedThrowingContinuation { continuation in
            V2TIMManager.sharedInstance().sendMessage(
                message: message,
                receiver: userID,
                groupID: nil,
                priority: V2TIMMessagePriority(rawValue: 0)!,
                onlineUserOnly: false,
                offlinePushInfo: nil,
                progress: nil,
                succ: {
                    let receipt = Self.receipt(for: message)
                    var fields = startFields
                    fields["result"] = "ok"
                    fields["duration_ms"] = Self.elapsedMilliseconds(since: startedAt)
                    fields["message"] = Self.messageTag(receipt.remoteID)
                    Self.logSDK(level: .info, event: "message-send-finished", fields: fields)
                    continuation.resume(returning: receipt)
                },
                fail: { code, desc in
                    var fields = startFields
                    fields["result"] = "failed"
                    fields["code"] = String(code)
                    fields["duration_ms"] = Self.elapsedMilliseconds(since: startedAt)
                    Self.logSDK(level: .error, event: "message-send-finished", fields: fields)
                    continuation.resume(
                        throwing: RemoteIMClientError.operationFailed(
                            code: code,
                            description: desc ?? failureDescription
                        )
                    )
                }
            )
        }
    }

    private static func receipt(for message: V2TIMMessage) -> RemoteIMSendReceipt {
        RemoteIMSendReceipt(remoteID: message.msgID, createdAt: message.timestamp)
    }

    private nonisolated static func messageMetadata(for message: V2TIMMessage) -> RemoteIMCloudMetadata? {
        RemoteIMCloudMetadataCodec.decode(message.cloudCustomData)
    }

    private static func profile(from info: V2TIMUserFullInfo) -> RemoteIMUserProfile? {
        guard let userID = info.userID?.trimmingCharacters(in: .whitespacesAndNewlines),
              !userID.isEmpty
        else {
            return nil
        }
        let nickname = info.nickName?.trimmingCharacters(in: .whitespacesAndNewlines)
        let avatarURL = info.faceURL?.trimmingCharacters(in: .whitespacesAndNewlines)
        return RemoteIMUserProfile(
            userID: userID,
            displayName: nickname?.isEmpty == false ? nickname! : userID,
            avatarURL: avatarURL?.isEmpty == false ? avatarURL : nil
        )
    }

    nonisolated func onRecvC2CTextMessage(msgID: String, sender: V2TIMUserInfo, text: String?) {
        guard let userID = sender.userID, !userID.isEmpty, let text, !text.isEmpty else {
            let hasSender = !(sender.userID ?? "").isEmpty
            Self.logSDK(
                level: .warning,
                event: "message-dropped",
                fields: [
                    "kind": "text",
                    "reason": hasSender ? "empty-text" : "no-sender",
                    "message": Self.messageTag(msgID)
                ]
            )
            return
        }
        let loginUserID = V2TIMManager.sharedInstance().getLoginUser() ?? ""
        guard RemoteIMPeerPolicy.isValidPeer(
            userID: userID,
            ownerUserID: loginUserID
        ) else {
            Self.logSDK(
                level: .debug,
                event: "message-dropped",
                fields: [
                    "kind": "text",
                    "reason": "sender-is-current-user",
                    "message": Self.messageTag(msgID),
                ]
            )
            return
        }
        Self.logIncomingMessage(
            kind: "text",
            peerUserID: userID,
            messageID: msgID,
            metadata: ["content_bytes": String(text.lengthOfBytes(using: .utf8))]
        )
        let fallbackDate = Date()
        V2TIMManager.sharedInstance().findMessages(
            messageIDList: [msgID],
            succ: { [weak self] messages in
                let message = messages?.first
                guard message?.isSelf != true else {
                    Self.logSDK(
                        level: .debug,
                        event: "message-dropped",
                        fields: [
                            "kind": "text",
                            "reason": "sdk-self-message",
                            "message": Self.messageTag(msgID),
                        ]
                    )
                    return
                }
                let createdAt = message?.timestamp ?? fallbackDate
                let metadata = message.flatMap(Self.messageMetadata(for:))
                if metadata?.approvalRequest != nil {
                    Self.logSDK(
                        level: .info,
                        event: "approval-metadata-resolved",
                        fields: [
                            "kind": "text",
                            "peer": Self.peerTag(userID),
                            "message": Self.messageTag(msgID),
                            "result": "ok",
                            "interaction": "approval-request",
                        ]
                    )
                }
                if metadata?.approvalDecision != nil {
                    Self.logSDK(
                        level: .info,
                        event: "approval-metadata-resolved",
                        fields: [
                            "kind": "text",
                            "peer": Self.peerTag(userID),
                            "message": Self.messageTag(msgID),
                            "result": "ok",
                            "interaction": "approval-resolved",
                        ]
                    )
                }
                self?.emitIncomingText(
                    fromUserID: userID,
                    text: text,
                    remoteID: msgID,
                    origin: metadata?.origin,
                    approvalRequest: metadata?.approvalRequest,
                    approvalDecision: metadata?.approvalDecision,
                    createdAt: createdAt
                )
            },
            fail: { [weak self] code, _ in
                Self.logSDK(
                    level: .warning,
                    event: "text-metadata-resolve-failed",
                    fields: [
                        "kind": "text",
                        "peer": Self.peerTag(userID),
                        "message": Self.messageTag(msgID),
                        "code": String(code),
                    ]
                )
                self?.emitIncomingText(
                    fromUserID: userID,
                    text: text,
                    remoteID: msgID,
                    origin: nil,
                    approvalRequest: nil,
                    approvalDecision: nil,
                    createdAt: fallbackDate
                )
            }
        )
    }

    private struct IncomingElementParts {
        var caption: String?
        var image: V2TIMImageElem?
        var sound: V2TIMSoundElem?
        var video: V2TIMVideoElem?
        var file: V2TIMFileElem?
    }

    private nonisolated static func firstElement(in message: V2TIMMessage) -> V2TIMElem? {
        switch message.elemType.rawValue {
        case 1: return message.textElem
        case 3: return message.imageElem
        case 4: return message.soundElem
        case 5: return message.videoElem
        case 6: return message.fileElem
        default: return nil
        }
    }

    private nonisolated static func incomingElementParts(
        from message: V2TIMMessage
    ) -> IncomingElementParts {
        var parts = IncomingElementParts()
        var element = firstElement(in: message)
        while let current = element {
            if let text = current as? V2TIMTextElem,
               parts.caption == nil,
               let clean = text.text?.trimmingCharacters(in: .whitespacesAndNewlines),
               !clean.isEmpty
            {
                parts.caption = clean
            } else if let image = current as? V2TIMImageElem, parts.image == nil {
                parts.image = image
            } else if let sound = current as? V2TIMSoundElem, parts.sound == nil {
                parts.sound = sound
            } else if let video = current as? V2TIMVideoElem, parts.video == nil {
                parts.video = video
            } else if let file = current as? V2TIMFileElem, parts.file == nil {
                parts.file = file
            }
            element = current.next()
        }
        return parts
    }

    nonisolated func onRecvNewMessage(msg: V2TIMMessage) {
        guard !msg.isSelf else { return }
        let fromUserID = msg.sender ?? msg.userID ?? ""
        guard !fromUserID.isEmpty else {
            Self.logSDK(
                level: .warning,
                event: "message-dropped",
                fields: ["reason": "no-sender", "message": Self.messageTag(msg.msgID)]
            )
            return
        }

        let parts = Self.incomingElementParts(from: msg)
        let metadata = Self.messageMetadata(for: msg)
        let captionAbove = metadata?.captionAbove == true

        // 多元素消息必须先找附件，不能因为存在 textElem 就提前 return；否则
        // [文本, 图片] 会只收到文字、整张图静默消失。
        if let soundElem = parts.sound {
            handleIncomingSound(msg: msg, soundElem: soundElem, fromUserID: fromUserID)
            return
        }

        if let imageElem = parts.image {
            handleIncomingImage(
                msg: msg,
                imageElem: imageElem,
                fromUserID: fromUserID,
                caption: parts.caption,
                captionAbove: captionAbove
            )
            return
        }

        if let videoElem = parts.video {
            handleIncomingVideo(
                msg: msg,
                videoElem: videoElem,
                fromUserID: fromUserID,
                caption: parts.caption,
                captionAbove: captionAbove
            )
            return
        }

        if let fileElem = parts.file {
            handleIncomingFile(
                msg: msg,
                fileElem: fileElem,
                fromUserID: fromUserID,
                caption: parts.caption,
                captionAbove: captionAbove
            )
            return
        }

        // 纯文本消息仍由 SimpleMsgListener 入库，避免高级回调重复投递。
        if parts.caption != nil { return }

        // 一个分支都没命中就说明这类消息本端不认，而且原来是悄悄丢掉的——桌面端那次
        // 语音消息丢失就是死在同样的静默里。elem_type 是这条日志的关键：光知道
        // "有消息没人处理"还得再猜一轮，有了类型号才能直接对上缺哪个分支
        // （例如视频是 5，本端目前没有对应实现）。
        Self.logSDK(
            level: .warning,
            event: "message-unhandled",
            fields: [
                "reason": "no-branch-matched",
                "peer": Self.peerTag(fromUserID),
                "message": Self.messageTag(msg.msgID),
                "elem_type": String(msg.elemType.rawValue)
            ]
        )
    }

    private nonisolated func handleIncomingSound(
        msg: V2TIMMessage,
        soundElem: V2TIMSoundElem,
        fromUserID: String
    ) {
        let createdAt = msg.timestamp ?? Date()
        let durationSeconds = max(1, Int(soundElem.duration))
        let remoteID = soundElem.uuid ?? msg.msgID
        let origin = Self.messageMetadata(for: msg)?.origin
        let targetURL = Self.voiceCacheURL(remoteID: remoteID, messageID: msg.msgID)
        let startedAt = ProcessInfo.processInfo.systemUptime
        let diagnosticFields = Self.messageDiagnosticFields(
            kind: "voice",
            peerUserID: fromUserID,
            messageID: remoteID,
            metadata: ["duration_seconds": String(durationSeconds)]
        )
        Self.logSDK(level: .info, event: "message-receive-callback", fields: diagnosticFields)
        Self.logSDK(level: .info, event: "media-download-start", fields: diagnosticFields)

        soundElem.downloadSound(
            path: targetURL.path,
            progress: nil,
            succ: {
                var fields = diagnosticFields
                fields["result"] = "ok"
                fields["duration_ms"] = Self.elapsedMilliseconds(since: startedAt)
                Self.logSDK(level: .info, event: "media-download-finished", fields: fields)
                Task { @MainActor [weak self, fromUserID, targetURL, durationSeconds, remoteID, origin, createdAt] in
                    let event = IncomingRemoteIMVoice(
                        fromUserID: fromUserID,
                        fileURL: targetURL,
                        durationSeconds: durationSeconds,
                        remoteID: remoteID,
                        origin: origin,
                        createdAt: createdAt
                    )
                    self?.onIncomingVoice?(event)
                }
            },
            fail: { code, _ in
                var fields = diagnosticFields
                fields["result"] = "failed"
                fields["code"] = String(code)
                fields["duration_ms"] = Self.elapsedMilliseconds(since: startedAt)
                Self.logSDK(
                    level: .warning,
                    event: "media-download-finished",
                    fields: fields
                )
            }
        )
    }

    private nonisolated func handleIncomingFile(
        msg: V2TIMMessage,
        fileElem: V2TIMFileElem,
        fromUserID: String,
        caption: String?,
        captionAbove: Bool
    ) {
        let fileName = Self.cleanFileName(fileElem.filename)
        let createdAt = msg.timestamp ?? Date()
        let remoteID = fileElem.uuid ?? msg.msgID
        let origin = Self.messageMetadata(for: msg)?.origin
        let targetURL = Self.fileCacheURL(remoteID: remoteID, messageID: msg.msgID, fileName: fileName)
        let mimeType = Self.mimeType(for: fileName)
        let sizeBytes = fileElem.fileSize > 0 ? Int(fileElem.fileSize) : nil
        let startedAt = ProcessInfo.processInfo.systemUptime
        let diagnosticFields = Self.messageDiagnosticFields(
            kind: "file",
            peerUserID: fromUserID,
            messageID: remoteID,
            metadata: [
                "bytes": sizeBytes.map(String.init) ?? "unknown",
                "mime_type": mimeType,
            ]
        )
        Self.logSDK(level: .info, event: "message-receive-callback", fields: diagnosticFields)
        Self.logSDK(level: .info, event: "media-download-start", fields: diagnosticFields)

        fileElem.downloadFile(
            path: targetURL.path,
            progress: nil,
            succ: {
                var fields = diagnosticFields
                fields["result"] = "ok"
                fields["duration_ms"] = Self.elapsedMilliseconds(since: startedAt)
                Self.logSDK(level: .info, event: "media-download-finished", fields: fields)
                Task { @MainActor [weak self, fromUserID, targetURL, fileName, mimeType, remoteID, sizeBytes, origin, createdAt, caption, captionAbove] in
                    let event = IncomingRemoteIMFile(
                        fromUserID: fromUserID,
                        fileURL: targetURL,
                        fileName: fileName,
                        mimeType: mimeType,
                        remoteID: remoteID,
                        sizeBytes: sizeBytes,
                        caption: caption,
                        captionAbove: captionAbove,
                        origin: origin,
                        createdAt: createdAt
                    )
                    self?.onIncomingFile?(event)
                }
            },
            fail: { code, _ in
                var fields = diagnosticFields
                fields["result"] = "failed"
                fields["code"] = String(code)
                fields["duration_ms"] = Self.elapsedMilliseconds(since: startedAt)
                Self.logSDK(
                    level: .warning,
                    event: "media-download-finished",
                    fields: fields
                )
            }
        )
    }

    private nonisolated func handleIncomingImage(
        msg: V2TIMMessage,
        imageElem: V2TIMImageElem,
        fromUserID: String,
        caption: String?,
        captionAbove: Bool
    ) {
        guard let image = Self.preferredImage(from: imageElem.imageList) else { return }
        let createdAt = msg.timestamp ?? Date()
        let remoteID = image.uuid ?? msg.msgID
        let origin = Self.messageMetadata(for: msg)?.origin
        let targetURL = Self.imageCacheURL(remoteID: remoteID, messageID: msg.msgID, imageURL: image.url)
        let width = image.width > 0 ? Int(image.width) : nil
        let height = image.height > 0 ? Int(image.height) : nil
        let sizeBytes = image.size > 0 ? Int(image.size) : nil
        let startedAt = ProcessInfo.processInfo.systemUptime
        let diagnosticFields = Self.messageDiagnosticFields(
            kind: "image",
            peerUserID: fromUserID,
            messageID: remoteID,
            metadata: [
                "width": width.map(String.init) ?? "unknown",
                "height": height.map(String.init) ?? "unknown",
                "bytes": sizeBytes.map(String.init) ?? "unknown",
            ]
        )
        Self.logSDK(level: .info, event: "message-receive-callback", fields: diagnosticFields)
        Self.logSDK(level: .info, event: "media-download-start", fields: diagnosticFields)
        image.downloadImage(
            path: targetURL.path,
            progress: nil,
            succ: {
                var fields = diagnosticFields
                fields["result"] = "ok"
                fields["duration_ms"] = Self.elapsedMilliseconds(since: startedAt)
                Self.logSDK(level: .info, event: "media-download-finished", fields: fields)
                Task { @MainActor [weak self, fromUserID, targetURL, remoteID, width, height, sizeBytes, origin, createdAt, caption, captionAbove] in
                    let event = IncomingRemoteIMImage(
                        fromUserID: fromUserID,
                        fileURL: targetURL,
                        remoteID: remoteID,
                        width: width,
                        height: height,
                        sizeBytes: sizeBytes,
                        caption: caption,
                        captionAbove: captionAbove,
                        origin: origin,
                        createdAt: createdAt
                    )
                    self?.onIncomingImage?(event)
                }
            },
            fail: { code, _ in
                var fields = diagnosticFields
                fields["result"] = "failed"
                fields["code"] = String(code)
                fields["duration_ms"] = Self.elapsedMilliseconds(since: startedAt)
                Self.logSDK(
                    level: .warning,
                    event: "media-download-finished",
                    fields: fields
                )
            }
        )
    }

    private nonisolated func handleIncomingVideo(
        msg: V2TIMMessage,
        videoElem: V2TIMVideoElem,
        fromUserID: String,
        caption: String?,
        captionAbove: Bool
    ) {
        let createdAt = msg.timestamp ?? Date()
        let remoteID = Self.nonEmpty(videoElem.videoUUID) ?? Self.nonEmpty(msg.msgID) ?? UUID().uuidString
        let origin = Self.messageMetadata(for: msg)?.origin
        let durationSeconds = max(0, Int(videoElem.duration))
        let width = max(0, Int(videoElem.snapshotWidth))
        let height = max(0, Int(videoElem.snapshotHeight))
        let sizeBytes = max(0, Int64(videoElem.videoSize))
        let videoURL = Self.videoCacheURL(
            remoteID: remoteID,
            videoType: videoElem.videoType
        )
        let hasSnapshot = Self.nonEmpty(videoElem.snapshotUUID) != nil ||
            videoElem.snapshotSize > 0 || width > 0 || height > 0
        let coverURL = hasSnapshot
            ? Self.videoCoverCacheURL(remoteID: remoteID)
            : nil
        let diagnosticFields = Self.messageDiagnosticFields(
            kind: "video",
            peerUserID: fromUserID,
            messageID: remoteID,
            metadata: [
                "duration_seconds": String(durationSeconds),
                "width": String(width),
                "height": String(height),
                "bytes": String(sizeBytes),
            ]
        )
        Self.logSDK(level: .info, event: "message-receive-callback", fields: diagnosticFields)

        emitIncomingVideo(
            fromUserID: fromUserID,
            videoFileURL: videoURL,
            coverFileURL: coverURL,
            durationSeconds: durationSeconds,
            width: width,
            height: height,
            sizeBytes: sizeBytes,
            remoteID: remoteID,
            caption: caption,
            captionAbove: captionAbove,
            origin: origin,
            createdAt: createdAt,
            stage: .metadata
        )

        if let coverURL {
            if Self.isUsableCachedFile(coverURL) {
                emitIncomingVideo(
                    fromUserID: fromUserID,
                    videoFileURL: videoURL,
                    coverFileURL: coverURL,
                    durationSeconds: durationSeconds,
                    width: width,
                    height: height,
                    sizeBytes: sizeBytes,
                    remoteID: remoteID,
                    caption: caption,
                    captionAbove: captionAbove,
                    origin: origin,
                    createdAt: createdAt,
                    stage: .coverReady
                )
            } else {
                let coverPartURL = Self.partialDownloadURL(for: coverURL)
                try? FileManager.default.removeItem(at: coverPartURL)
                var coverFields = diagnosticFields
                coverFields["asset"] = "cover"
                let coverStartedAt = ProcessInfo.processInfo.systemUptime
                Self.logSDK(level: .info, event: "media-download-start", fields: coverFields)
                videoElem.downloadSnapshot(
                    path: coverPartURL.path,
                    progress: nil,
                    succ: { [weak self] in
                        var fields = coverFields
                        guard Self.promoteDownloadedFile(from: coverPartURL, to: coverURL) else {
                            fields["result"] = "failed"
                            fields["reason"] = "file-promote-failed"
                            fields["duration_ms"] = Self.elapsedMilliseconds(since: coverStartedAt)
                            Self.logSDK(level: .warning, event: "media-download-finished", fields: fields)
                            return
                        }
                        fields["result"] = "ok"
                        fields["duration_ms"] = Self.elapsedMilliseconds(since: coverStartedAt)
                        Self.logSDK(level: .info, event: "media-download-finished", fields: fields)
                        self?.emitIncomingVideo(
                            fromUserID: fromUserID,
                            videoFileURL: videoURL,
                            coverFileURL: coverURL,
                            durationSeconds: durationSeconds,
                            width: width,
                            height: height,
                            sizeBytes: sizeBytes,
                            remoteID: remoteID,
                            caption: caption,
                            captionAbove: captionAbove,
                            origin: origin,
                            createdAt: createdAt,
                            stage: .coverReady
                        )
                    },
                    fail: { code, _ in
                        try? FileManager.default.removeItem(at: coverPartURL)
                        var fields = coverFields
                        fields["result"] = "failed"
                        fields["code"] = String(code)
                        fields["duration_ms"] = Self.elapsedMilliseconds(since: coverStartedAt)
                        Self.logSDK(level: .warning, event: "media-download-finished", fields: fields)
                    }
                )
            }
        }

        if Self.isUsableCachedFile(videoURL) {
            emitIncomingVideo(
                fromUserID: fromUserID,
                videoFileURL: videoURL,
                coverFileURL: coverURL,
                durationSeconds: durationSeconds,
                width: width,
                height: height,
                sizeBytes: sizeBytes,
                remoteID: remoteID,
                caption: caption,
                captionAbove: captionAbove,
                origin: origin,
                createdAt: createdAt,
                stage: .videoReady
            )
            return
        }

        var videoFields = diagnosticFields
        videoFields["asset"] = "video"
        let videoStartedAt = ProcessInfo.processInfo.systemUptime
        let videoPartURL = Self.partialDownloadURL(for: videoURL)
        try? FileManager.default.removeItem(at: videoPartURL)
        Self.logSDK(level: .info, event: "media-download-start", fields: videoFields)
        videoElem.downloadVideo(
            path: videoPartURL.path,
            progress: nil,
            succ: { [weak self] in
                var fields = videoFields
                guard Self.promoteDownloadedFile(from: videoPartURL, to: videoURL) else {
                    fields["result"] = "failed"
                    fields["reason"] = "file-promote-failed"
                    fields["duration_ms"] = Self.elapsedMilliseconds(since: videoStartedAt)
                    Self.logSDK(level: .warning, event: "media-download-finished", fields: fields)
                    self?.emitIncomingVideo(
                        fromUserID: fromUserID,
                        videoFileURL: videoURL,
                        coverFileURL: coverURL,
                        durationSeconds: durationSeconds,
                        width: width,
                        height: height,
                        sizeBytes: sizeBytes,
                        remoteID: remoteID,
                        caption: caption,
                        captionAbove: captionAbove,
                        origin: origin,
                        createdAt: createdAt,
                        stage: .videoFailed
                    )
                    return
                }
                fields["result"] = "ok"
                fields["duration_ms"] = Self.elapsedMilliseconds(since: videoStartedAt)
                Self.logSDK(level: .info, event: "media-download-finished", fields: fields)
                self?.emitIncomingVideo(
                    fromUserID: fromUserID,
                    videoFileURL: videoURL,
                    coverFileURL: coverURL,
                    durationSeconds: durationSeconds,
                    width: width,
                    height: height,
                    sizeBytes: sizeBytes,
                    remoteID: remoteID,
                    caption: caption,
                    captionAbove: captionAbove,
                    origin: origin,
                    createdAt: createdAt,
                    stage: .videoReady
                )
            },
            fail: { [weak self] code, _ in
                try? FileManager.default.removeItem(at: videoPartURL)
                var fields = videoFields
                fields["result"] = "failed"
                fields["code"] = String(code)
                fields["duration_ms"] = Self.elapsedMilliseconds(since: videoStartedAt)
                Self.logSDK(level: .warning, event: "media-download-finished", fields: fields)
                self?.emitIncomingVideo(
                    fromUserID: fromUserID,
                    videoFileURL: videoURL,
                    coverFileURL: coverURL,
                    durationSeconds: durationSeconds,
                    width: width,
                    height: height,
                    sizeBytes: sizeBytes,
                    remoteID: remoteID,
                    caption: caption,
                    captionAbove: captionAbove,
                    origin: origin,
                    createdAt: createdAt,
                    stage: .videoFailed
                )
            }
        )
    }

    private nonisolated func emitIncomingVideo(
        fromUserID: String,
        videoFileURL: URL,
        coverFileURL: URL?,
        durationSeconds: Int,
        width: Int,
        height: Int,
        sizeBytes: Int64,
        remoteID: String?,
        caption: String?,
        captionAbove: Bool,
        origin: RemoteIMMessageOrigin?,
        createdAt: Date,
        stage: RemoteIMVideoDownloadStage
    ) {
        Task { @MainActor [weak self] in
            self?.onIncomingVideo?(
                IncomingRemoteIMVideo(
                    fromUserID: fromUserID,
                    videoFileURL: videoFileURL,
                    coverFileURL: coverFileURL,
                    durationSeconds: durationSeconds,
                    width: width,
                    height: height,
                    sizeBytes: sizeBytes,
                    caption: caption,
                    captionAbove: captionAbove,
                    remoteID: remoteID,
                    origin: origin,
                    createdAt: createdAt,
                    stage: stage
                )
            )
        }
    }

    private nonisolated func emitIncomingText(
        fromUserID: String,
        text: String,
        remoteID: String?,
        origin: RemoteIMMessageOrigin?,
        approvalRequest: RemoteIMApprovalRequest?,
        approvalDecision: RemoteIMApprovalDecision?,
        createdAt: Date
    ) {
        Task { @MainActor [weak self, fromUserID, text, remoteID, origin, approvalRequest, approvalDecision, createdAt] in
            let event = IncomingRemoteIMText(
                fromUserID: fromUserID,
                text: text,
                remoteID: remoteID,
                origin: origin,
                approvalRequest: approvalRequest,
                approvalDecision: approvalDecision,
                createdAt: createdAt
            )
            self?.onIncomingText?(event)
        }
    }

    private nonisolated static func voiceCacheURL(remoteID: String?, messageID: String?) -> URL {
        let rawName = remoteID ?? messageID ?? UUID().uuidString
        return RemoteIMMediaStorage.fileURL(category: .incomingVoices, stem: rawName, pathExtension: "m4a")
    }

    private nonisolated static func videoCacheURL(
        remoteID: String,
        videoType: String?
    ) -> URL {
        let rawExtension = videoType?
            .trimmingCharacters(in: .whitespacesAndNewlines)
            .trimmingCharacters(in: CharacterSet(charactersIn: "."))
            .lowercased() ?? ""
        let fileExtension = !rawExtension.isEmpty && rawExtension.allSatisfy({ $0.isLetter || $0.isNumber })
            ? rawExtension
            : "mp4"
        return RemoteIMMediaStorage.fileURL(
            category: .incomingVideos,
            stem: remoteID,
            pathExtension: fileExtension
        )
    }

    private nonisolated static func videoCoverCacheURL(remoteID: String) -> URL {
        RemoteIMMediaStorage.fileURL(category: .incomingVideoCovers, stem: remoteID, pathExtension: "jpg")
    }

    private nonisolated static func isUsableCachedFile(_ url: URL) -> Bool {
        guard let values = try? url.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey]) else {
            return false
        }
        return values.isRegularFile == true && (values.fileSize ?? 0) > 0
    }

    private nonisolated static func partialDownloadURL(for finalURL: URL) -> URL {
        RemoteIMMediaStorage.partialDownloadURL(for: finalURL)
    }

    private nonisolated static func promoteDownloadedFile(from partialURL: URL, to finalURL: URL) -> Bool {
        guard isUsableCachedFile(partialURL) else {
            try? FileManager.default.removeItem(at: partialURL)
            return false
        }
        do {
            if FileManager.default.fileExists(atPath: finalURL.path) {
                try FileManager.default.removeItem(at: finalURL)
            }
            try FileManager.default.moveItem(at: partialURL, to: finalURL)
            return isUsableCachedFile(finalURL)
        } catch {
            try? FileManager.default.removeItem(at: partialURL)
            return false
        }
    }

    private nonisolated static func nonEmpty(_ value: String?) -> String? {
        let cleanValue = value?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return cleanValue.isEmpty ? nil : cleanValue
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
        let pathExtension = URL(string: imageURL ?? "")?.pathExtension
        return RemoteIMMediaStorage.fileURL(
            category: .incomingImages,
            stem: rawName,
            pathExtension: pathExtension?.isEmpty == false ? pathExtension! : "jpg"
        )
    }

    private nonisolated static func fileCacheURL(
        remoteID: String?,
        messageID: String?,
        fileName: String
    ) -> URL {
        let rawName = remoteID ?? messageID ?? UUID().uuidString
        let fileExtension = URL(fileURLWithPath: fileName).pathExtension
        return RemoteIMMediaStorage.fileURL(
            category: .incomingFiles,
            stem: rawName,
            pathExtension: fileExtension.isEmpty ? "bin" : fileExtension
        )
    }

    private nonisolated static func cleanFileName(_ value: String?) -> String {
        let cleanValue = value?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return cleanValue.isEmpty ? "remote-im-file.md" : cleanValue
    }

    private nonisolated static func mimeType(for fileName: String) -> String {
        let fileExtension = URL(fileURLWithPath: fileName).pathExtension
        return UTType(filenameExtension: fileExtension)?.preferredMIMEType ?? "application/octet-stream"
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

    private nonisolated static func logMessageCreateFailure(
        kind: String,
        peerUserID: String
    ) {
        logSDK(
            level: .error,
            event: "message-create-failed",
            fields: [
                "kind": kind,
                "peer": peerTag(peerUserID),
            ]
        )
    }

    private nonisolated static func logIncomingMessage(
        kind: String,
        peerUserID: String,
        messageID: String?,
        metadata: [String: String] = [:]
    ) {
        logSDK(
            level: .info,
            event: "message-receive-callback",
            fields: messageDiagnosticFields(
                kind: kind,
                peerUserID: peerUserID,
                messageID: messageID,
                metadata: metadata
            )
        )
    }

    private nonisolated static func messageDiagnosticFields(
        kind: String,
        peerUserID: String,
        messageID: String?,
        metadata: [String: String] = [:]
    ) -> [String: String] {
        var fields = metadata
        fields["kind"] = kind
        fields["peer"] = peerTag(peerUserID)
        fields["message"] = messageTag(messageID)
        return fields
    }

    private nonisolated static func peerTag(_ userID: String) -> String {
        guard !userID.isEmpty else { return "none" }
        return DiagnosticLogPrivacy.stableTag(userID, prefix: "u")
    }

    private nonisolated static func messageTag(_ messageID: String?) -> String {
        guard let messageID, !messageID.isEmpty else { return "none" }
        return DiagnosticLogPrivacy.stableTag(messageID, prefix: "m")
    }

    private nonisolated static func elapsedMilliseconds(since startedAt: TimeInterval) -> String {
        let elapsed = max(ProcessInfo.processInfo.systemUptime - startedAt, 0)
        return String(Int((elapsed * 1_000).rounded()))
    }

    private nonisolated static func logSDK(
        level: DiagnosticLogLevel,
        event: String,
        fields: [String: String] = [:]
    ) {
        Task { @MainActor in
            AppDiagnosticLog.shared.record(
                level: level,
                category: "remote-im",
                event: event,
                fields: fields
            )
        }
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
    var onIncomingFile: ((IncomingRemoteIMFile) -> Void)?
    var onIncomingVideo: ((IncomingRemoteIMVideo) -> Void)?
    var onPresenceStatusChanged: (([String: RemoteIMPresenceStatus]) -> Void)?

    func connect(sdkAppID: Int, userID: String, userSig: String) async throws {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func disconnect() async {}

    func sendText(to userID: String, text: String, origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func sendApprovalDecision(
        to userID: String,
        token: String,
        action: RemoteIMApprovalAction
    ) async throws -> RemoteIMSendReceipt {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func sendVoice(to userID: String, recording: RemoteIMVoiceRecording, origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func sendImage(to userID: String, image: RemoteIMImageFile, origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func sendVideo(to userID: String, video: RemoteIMVideoFile, origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func sendFile(to userID: String, file: RemoteIMFile, origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func deleteContact(userID: String) async throws {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func clearHistory(userID: String) async throws {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func refreshUserProfiles(userIDs: [String]) async throws -> [RemoteIMUserProfile] {
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
