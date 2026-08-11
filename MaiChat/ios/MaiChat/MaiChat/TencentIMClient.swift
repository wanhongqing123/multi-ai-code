import Foundation
import MaiChatCore
import UniformTypeIdentifiers

#if canImport(ImSDK_Plus)
import ImSDK_Plus

final class TencentIMClient: NSObject, RemoteIMClient, V2TIMSimpleMsgListener, V2TIMAdvancedMsgListener, V2TIMSDKListener {
    var onIncomingText: ((IncomingRemoteIMText) -> Void)?
    var onIncomingVoice: ((IncomingRemoteIMVoice) -> Void)?
    var onIncomingImage: ((IncomingRemoteIMImage) -> Void)?
    var onIncomingFile: ((IncomingRemoteIMFile) -> Void)?
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
        return try await withCheckedThrowingContinuation { continuation in
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

    func sendText(to userID: String, text: String) async throws -> RemoteIMSendReceipt {
        guard let message = V2TIMManager.sharedInstance().createTextMessage(text: text) else {
            Self.logMessageCreateFailure(kind: "text", peerUserID: userID)
            throw RemoteIMClientError.operationFailed(code: -1, description: "create text message failed")
        }
        return try await send(
            message: message,
            to: userID,
            kind: "text",
            metadata: ["content_bytes": String(text.lengthOfBytes(using: .utf8))],
            failureDescription: "send text failed"
        )
    }

    func sendVoice(to userID: String, recording: RemoteIMVoiceRecording) async throws -> RemoteIMSendReceipt {
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
            metadata: ["duration_seconds": String(recording.durationSeconds)],
            failureDescription: "send voice failed"
        )
    }

    func sendImage(to userID: String, image: RemoteIMImageFile) async throws -> RemoteIMSendReceipt {
        guard let message = V2TIMManager.sharedInstance().createImageMessage(imagePath: image.fileURL.path) else {
            Self.logMessageCreateFailure(kind: "image", peerUserID: userID)
            throw RemoteIMClientError.operationFailed(code: -1, description: "create image message failed")
        }
        return try await send(
            message: message,
            to: userID,
            kind: "image",
            metadata: [
                "width": image.width.map(String.init) ?? "unknown",
                "height": image.height.map(String.init) ?? "unknown",
                "bytes": image.sizeBytes.map(String.init) ?? "unknown",
            ],
            failureDescription: "send image failed"
        )
    }

    func sendFile(to userID: String, file: RemoteIMFile) async throws -> RemoteIMSendReceipt {
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
        metadata: [String: String],
        failureDescription: String
    ) async throws -> RemoteIMSendReceipt {
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
        guard let userID = sender.userID, !userID.isEmpty, let text, !text.isEmpty else { return }
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
                let createdAt = messages?.first?.timestamp ?? fallbackDate
                self?.emitIncomingText(
                    fromUserID: userID,
                    text: text,
                    remoteID: msgID,
                    createdAt: createdAt
                )
            },
            fail: { [weak self] _, _ in
                self?.emitIncomingText(
                    fromUserID: userID,
                    text: text,
                    remoteID: msgID,
                    createdAt: fallbackDate
                )
            }
        )
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
            return
        }

        if let fileElem = msg.fileElem {
            handleIncomingFile(msg: msg, fileElem: fileElem, fromUserID: fromUserID)
        }
    }

    private nonisolated func handleIncomingSound(
        msg: V2TIMMessage,
        soundElem: V2TIMSoundElem,
        fromUserID: String
    ) {
        let createdAt = msg.timestamp ?? Date()
        let durationSeconds = max(1, Int(soundElem.duration))
        let remoteID = soundElem.uuid ?? msg.msgID
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
                Task { @MainActor [weak self, fromUserID, targetURL, durationSeconds, remoteID, createdAt] in
                    let event = IncomingRemoteIMVoice(
                        fromUserID: fromUserID,
                        fileURL: targetURL,
                        durationSeconds: durationSeconds,
                        remoteID: remoteID,
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
        fromUserID: String
    ) {
        let fileName = Self.cleanFileName(fileElem.filename)
        let createdAt = msg.timestamp ?? Date()
        let remoteID = fileElem.uuid ?? msg.msgID
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
                Task { @MainActor [weak self, fromUserID, targetURL, fileName, mimeType, remoteID, sizeBytes, createdAt] in
                    let event = IncomingRemoteIMFile(
                        fromUserID: fromUserID,
                        fileURL: targetURL,
                        fileName: fileName,
                        mimeType: mimeType,
                        remoteID: remoteID,
                        sizeBytes: sizeBytes,
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
        fromUserID: String
    ) {
        guard let image = Self.preferredImage(from: imageElem.imageList) else { return }
        let createdAt = msg.timestamp ?? Date()
        let remoteID = image.uuid ?? msg.msgID
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
                Task { @MainActor [weak self, fromUserID, targetURL, remoteID, width, height, sizeBytes, createdAt] in
                    let event = IncomingRemoteIMImage(
                        fromUserID: fromUserID,
                        fileURL: targetURL,
                        remoteID: remoteID,
                        width: width,
                        height: height,
                        sizeBytes: sizeBytes,
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

    private nonisolated func emitIncomingText(
        fromUserID: String,
        text: String,
        remoteID: String?,
        createdAt: Date
    ) {
        Task { @MainActor [weak self, fromUserID, text, remoteID, createdAt] in
            let event = IncomingRemoteIMText(
                fromUserID: fromUserID,
                text: text,
                remoteID: remoteID,
                createdAt: createdAt
            )
            self?.onIncomingText?(event)
        }
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

    private nonisolated static func fileCacheURL(
        remoteID: String?,
        messageID: String?,
        fileName: String
    ) -> URL {
        let rawName = remoteID ?? messageID ?? UUID().uuidString
        let safeName = rawName
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: ":", with: "_")
        let directory = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("RemoteIMFile", isDirectory: true)
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let fileExtension = URL(fileURLWithPath: fileName).pathExtension
        return directory
            .appendingPathComponent(safeName)
            .appendingPathExtension(fileExtension.isEmpty ? "bin" : fileExtension)
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
    var onPresenceStatusChanged: (([String: RemoteIMPresenceStatus]) -> Void)?

    func connect(sdkAppID: Int, userID: String, userSig: String) async throws {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func disconnect() async {}

    func sendText(to userID: String, text: String) async throws -> RemoteIMSendReceipt {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func sendVoice(to userID: String, recording: RemoteIMVoiceRecording) async throws -> RemoteIMSendReceipt {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func sendImage(to userID: String, image: RemoteIMImageFile) async throws -> RemoteIMSendReceipt {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func sendFile(to userID: String, file: RemoteIMFile) async throws -> RemoteIMSendReceipt {
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
