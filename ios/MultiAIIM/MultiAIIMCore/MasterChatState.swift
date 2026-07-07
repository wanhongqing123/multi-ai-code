import Foundation

public enum MasterChatStateError: Error, Equatable, LocalizedError {
    case blankUserID
    case blankMessage
    case noSelectedPeer
    case messageNotFound

    public var errorDescription: String? {
        switch self {
        case .blankUserID:
            return "请填写账号 ID"
        case .blankMessage:
            return "请输入消息内容"
        case .noSelectedPeer:
            return "请先选择联系人"
        case .messageNotFound:
            return "消息不存在"
        }
    }
}

public enum RemoteIMContactRelation: String, Codable, Equatable, Hashable {
    case friend
    case slave

    public var displayName: String {
        return "好友"
    }
}

public enum RemoteIMDraftSubmitPolicy {
    public static func textByConsumingTrailingReturn(from text: String) -> String? {
        let normalizedText = text.replacingOccurrences(of: "\r\n", with: "\n")
        guard normalizedText.hasSuffix("\n") else { return nil }
        return String(normalizedText.dropLast())
    }
}

public enum ChatDetailSwipeBackPolicy {
    public static let maxStartX: Double = 32
    public static let minTranslationWidth: Double = 70
    public static let maxVerticalTranslation: Double = 80

    public static func shouldReturnToConversationList(
        startX: Double,
        translationWidth: Double,
        translationHeight: Double
    ) -> Bool {
        startX <= maxStartX &&
            translationWidth >= minTranslationWidth &&
            abs(translationHeight) <= maxVerticalTranslation
    }
}

public enum MessageListAutoScrollPolicy {
    public static func latestMessageID(from messages: [RemoteIMMessage]) -> RemoteIMMessage.ID? {
        messages.last?.id
    }
}

public enum RemoteIMTimestampTextPolicy {
    public static func displayText(
        for date: Date,
        now: Date = Date(),
        calendar: Calendar = .current
    ) -> String {
        if calendar.isDate(date, inSameDayAs: now) {
            return formatted(date, dateFormat: "HH:mm", calendar: calendar)
        }

        if let yesterday = calendar.date(byAdding: .day, value: -1, to: now),
           calendar.isDate(date, inSameDayAs: yesterday)
        {
            return "昨天 " + formatted(date, dateFormat: "HH:mm", calendar: calendar)
        }

        return formatted(date, dateFormat: "M 月 d 日 HH:mm", calendar: calendar)
    }

    private static func formatted(
        _ date: Date,
        dateFormat: String,
        calendar: Calendar
    ) -> String {
        let formatter = DateFormatter()
        formatter.calendar = calendar
        formatter.locale = Locale(identifier: "zh_CN")
        formatter.timeZone = calendar.timeZone
        formatter.dateFormat = dateFormat
        return formatter.string(from: date)
    }
}

public enum RemoteIMPresenceStatus: String, Codable, Equatable, Hashable, Sendable {
    case unknown
    case online
    case offline

    public var isOnline: Bool {
        self == .online
    }
}

public enum RemoteIMPresenceStatusPolicy {
    public static func merged(
        current: [String: RemoteIMPresenceStatus],
        updates: [String: RemoteIMPresenceStatus],
        contactUserIDs: [String]
    ) -> [String: RemoteIMPresenceStatus] {
        let validUserIDs = Set(
            contactUserIDs
                .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
                .filter { !$0.isEmpty }
        )

        var nextStatusByUserID: [String: RemoteIMPresenceStatus] = [:]
        for userID in validUserIDs {
            if let currentStatus = current[userID] {
                nextStatusByUserID[userID] = currentStatus
            }
        }

        for (rawUserID, status) in updates {
            let userID = rawUserID.trimmingCharacters(in: .whitespacesAndNewlines)
            guard validUserIDs.contains(userID) else { continue }
            nextStatusByUserID[userID] = status
        }

        return nextStatusByUserID
    }
}

public struct RemoteIMImagePreviewItem: Identifiable, Equatable {
    public let id: UUID
    public let localFilePath: String

    public init(id: UUID, localFilePath: String) {
        self.id = id
        self.localFilePath = localFilePath
    }
}

public enum RemoteIMImagePreviewPolicy {
    public static func previewItem(for message: RemoteIMMessage) -> RemoteIMImagePreviewItem? {
        guard let attachment = message.imageAttachment else {
            return nil
        }
        return RemoteIMImagePreviewItem(id: message.id, localFilePath: attachment.localFilePath)
    }
}

public struct RemoteIMCredential: Equatable {
    public let sdkAppID: Int
    public let userSigSecretKey: String

    public init(sdkAppID: Int, userSigSecretKey: String) {
        self.sdkAppID = sdkAppID
        self.userSigSecretKey = userSigSecretKey
    }
}

public enum RemoteIMCredentialDefaults {
    public static let sdkAppID = 1_600_148_979
    public static let userSigSecretKey = "aa18d554f5e4a235640745e98145e187977f87770b812b2b4f10ef032bd73861"

    public static func resolvedCredential(sdkAppID _: Int?, secretKey _: String) -> RemoteIMCredential {
        defaultCredential
    }

    private static var defaultCredential: RemoteIMCredential {
        RemoteIMCredential(
            sdkAppID: Self.sdkAppID,
            userSigSecretKey: Self.userSigSecretKey
        )
    }
}

public enum RemoteIMLoginCredentialPolicy {
    public static func validationError(userID: String) -> String? {
        guard !userID.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            return "请填写账号 ID"
        }
        return nil
    }

    public static func validationError(
        sdkAppIDText _: String,
        userID: String,
        secretKey _: String
    ) -> String? {
        validationError(userID: userID)
    }

    public static func isComplete(userID: String) -> Bool {
        validationError(userID: userID) == nil
    }

    public static func isComplete(
        sdkAppIDText _: String,
        userID: String,
        secretKey _: String
    ) -> Bool {
        validationError(userID: userID) == nil
    }
}

public struct RemoteIMContact: Identifiable, Codable, Equatable, Hashable {
    public var id: String { userID }
    public let userID: String
    public var displayName: String
    public var relation: RemoteIMContactRelation

    public init(
        userID: String,
        displayName: String,
        relation: RemoteIMContactRelation = .friend
    ) {
        self.userID = userID
        self.displayName = displayName
        self.relation = .friend
    }
}

public enum RemoteIMMessageDirection: String, Codable, Equatable {
    case incoming
    case outgoing
}

public enum RemoteIMMessageStatus: String, Codable, Equatable {
    case pending
    case sent
    case received
    case failed
}

public struct RemoteIMVoiceAttachment: Codable, Equatable {
    public let localFilePath: String
    public let durationSeconds: Int
    public let remoteID: String?

    public init(
        localFilePath: String,
        durationSeconds: Int,
        remoteID: String? = nil
    ) {
        self.localFilePath = localFilePath
        self.durationSeconds = max(1, durationSeconds)
        self.remoteID = remoteID
    }
}

public struct RemoteIMImageAttachment: Codable, Equatable {
    public let localFilePath: String
    public let remoteID: String?
    public let width: Int?
    public let height: Int?
    public let sizeBytes: Int?

    public init(
        localFilePath: String,
        remoteID: String? = nil,
        width: Int? = nil,
        height: Int? = nil,
        sizeBytes: Int? = nil
    ) {
        self.localFilePath = localFilePath
        self.remoteID = remoteID
        self.width = width
        self.height = height
        self.sizeBytes = sizeBytes
    }
}

public struct RemoteIMMessage: Identifiable, Codable, Equatable {
    public let id: UUID
    public let fromUserID: String
    public let toUserID: String
    public let text: String
    public let voiceAttachment: RemoteIMVoiceAttachment?
    public let imageAttachment: RemoteIMImageAttachment?
    public let direction: RemoteIMMessageDirection
    public var status: RemoteIMMessageStatus
    public let createdAt: Date

    public init(
        id: UUID = UUID(),
        fromUserID: String,
        toUserID: String,
        text: String,
        voiceAttachment: RemoteIMVoiceAttachment? = nil,
        imageAttachment: RemoteIMImageAttachment? = nil,
        direction: RemoteIMMessageDirection,
        status: RemoteIMMessageStatus,
        createdAt: Date
    ) {
        self.id = id
        self.fromUserID = fromUserID
        self.toUserID = toUserID
        self.text = text
        self.voiceAttachment = voiceAttachment
        self.imageAttachment = imageAttachment
        self.direction = direction
        self.status = status
        self.createdAt = createdAt
    }

    public var isVoiceMessage: Bool {
        voiceAttachment != nil
    }

    public var isImageMessage: Bool {
        imageAttachment != nil
    }
}

public struct MasterChatState: Equatable {
    public let ownerUserID: String
    public private(set) var contacts: [RemoteIMContact]
    public private(set) var messages: [RemoteIMMessage]
    public private(set) var selectedPeerID: String?

    public init(ownerUserID: String) {
        self.ownerUserID = ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        self.contacts = []
        self.messages = []
        self.selectedPeerID = nil
    }

    public init(
        ownerUserID: String,
        contacts: [RemoteIMContact],
        messages: [RemoteIMMessage],
        selectedPeerID: String? = nil
    ) {
        self.ownerUserID = ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        self.contacts = Self.normalizedContacts(contacts)
        self.messages = Self.normalizedMessages(messages, ownerUserID: self.ownerUserID)
        Self.addMissingContacts(from: self.messages, ownerUserID: self.ownerUserID, contacts: &self.contacts)

        let cleanSelectedPeerID = selectedPeerID?.trimmingCharacters(in: .whitespacesAndNewlines)
        if let cleanSelectedPeerID,
           !cleanSelectedPeerID.isEmpty,
           self.contacts.contains(where: { $0.userID == cleanSelectedPeerID })
        {
            self.selectedPeerID = cleanSelectedPeerID
        } else {
            self.selectedPeerID = self.contacts.first?.userID
        }
    }

    private static func normalizedContacts(_ contacts: [RemoteIMContact]) -> [RemoteIMContact] {
        var normalizedContacts: [RemoteIMContact] = []
        for contact in contacts {
            let cleanUserID = contact.userID.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !cleanUserID.isEmpty else { continue }
            let cleanDisplayName = contact.displayName.trimmingCharacters(in: .whitespacesAndNewlines)
            let normalizedContact = RemoteIMContact(
                userID: cleanUserID,
                displayName: cleanDisplayName.isEmpty ? cleanUserID : cleanDisplayName,
                relation: .friend
            )
            if let index = normalizedContacts.firstIndex(where: { $0.userID == cleanUserID }) {
                normalizedContacts[index] = normalizedContact
            } else {
                normalizedContacts.append(normalizedContact)
            }
        }
        return normalizedContacts
    }

    private static func normalizedMessages(
        _ messages: [RemoteIMMessage],
        ownerUserID: String
    ) -> [RemoteIMMessage] {
        guard !ownerUserID.isEmpty else { return messages }
        return messages
            .filter { $0.fromUserID == ownerUserID || $0.toUserID == ownerUserID }
            .sorted { $0.createdAt < $1.createdAt }
    }

    private static func addMissingContacts(
        from messages: [RemoteIMMessage],
        ownerUserID: String,
        contacts: inout [RemoteIMContact]
    ) {
        guard !ownerUserID.isEmpty else { return }
        for message in messages {
            let peerID = message.fromUserID == ownerUserID ? message.toUserID : message.fromUserID
            let cleanPeerID = peerID.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !cleanPeerID.isEmpty else { continue }
            if !contacts.contains(where: { $0.userID == cleanPeerID }) {
                contacts.append(
                    RemoteIMContact(
                        userID: cleanPeerID,
                        displayName: cleanPeerID,
                        relation: .friend
                    )
                )
            }
        }
    }

    public mutating func upsertContact(
        userID: String,
        relation: RemoteIMContactRelation,
        displayName: String? = nil
    ) throws {
        let cleanUserID = userID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanUserID.isEmpty else { throw MasterChatStateError.blankUserID }
        let cleanDisplayName = displayName?.trimmingCharacters(in: .whitespacesAndNewlines)
        let contact = RemoteIMContact(
            userID: cleanUserID,
            displayName: cleanDisplayName?.isEmpty == false ? cleanDisplayName! : cleanUserID,
            relation: relation
        )
        if let index = contacts.firstIndex(where: { $0.userID == cleanUserID }) {
            contacts[index] = contact
        } else {
            contacts.append(contact)
        }
        if selectedPeerID == nil {
            selectedPeerID = cleanUserID
        }
    }

    public mutating func upsertFriend(userID: String, displayName: String? = nil) throws {
        try upsertContact(userID: userID, relation: .friend, displayName: displayName)
    }

    public mutating func upsertSlave(userID: String, displayName: String? = nil) throws {
        try upsertContact(userID: userID, relation: .friend, displayName: displayName)
    }

    public mutating func removeContactAndMessages(userID: String) {
        let cleanUserID = userID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanUserID.isEmpty else { return }
        contacts.removeAll { $0.userID == cleanUserID }
        messages.removeAll { message in
            message.fromUserID == cleanUserID || message.toUserID == cleanUserID
        }
        if selectedPeerID == cleanUserID ||
            selectedPeerID.map({ selected in !contacts.contains(where: { $0.userID == selected }) }) == true
        {
            selectedPeerID = contacts.first?.userID
        }
    }

    public mutating func selectPeer(userID: String) {
        selectedPeerID = userID.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    public func messages(with peerID: String) -> [RemoteIMMessage] {
        let cleanPeerID = peerID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanPeerID.isEmpty else { return [] }
        return messages.filter { message in
            (message.fromUserID == ownerUserID && message.toUserID == cleanPeerID) ||
                (message.fromUserID == cleanPeerID && message.toUserID == ownerUserID)
        }
        .sorted {
            if $0.createdAt == $1.createdAt {
                return $0.id.uuidString < $1.id.uuidString
            }
            return $0.createdAt < $1.createdAt
        }
    }

    public func latestMessage(with peerID: String) -> RemoteIMMessage? {
        messages(with: peerID).last
    }

    private static func voiceDisplayText(durationSeconds: Int) -> String {
        "[语音消息 \(max(1, durationSeconds))s]"
    }

    private static func imageDisplayText(filePath: String) -> String {
        let fileName = URL(fileURLWithPath: filePath).lastPathComponent
        return fileName.isEmpty ? "[图片消息]" : "[图片消息] \(fileName)"
    }

    private static func incomingDisplayText(_ text: String) -> String {
        var cleanText = text.trimmingCharacters(in: .whitespacesAndNewlines)
        for prefix in ["【AICLI 输出】", "[AICLI 输出]", "【AICLI输出】", "[AICLI输出]"] {
            if cleanText.hasPrefix(prefix) {
                cleanText.removeFirst(prefix.count)
                return cleanText.trimmingCharacters(in: .whitespacesAndNewlines)
            }
        }
        return cleanText
    }

    @discardableResult
    public mutating func queueOutgoingText(
        _ text: String,
        now: Date = Date()
    ) throws -> RemoteIMMessage {
        let cleanText = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanText.isEmpty else { throw MasterChatStateError.blankMessage }
        guard let peerID = selectedPeerID, !peerID.isEmpty else {
            throw MasterChatStateError.noSelectedPeer
        }
        let message = RemoteIMMessage(
            fromUserID: ownerUserID,
            toUserID: peerID,
            text: cleanText,
            direction: .outgoing,
            status: .pending,
            createdAt: now
        )
        messages.append(message)
        return message
    }

    @discardableResult
    public mutating func queueOutgoingVoice(
        filePath: String,
        durationSeconds: Int,
        now: Date = Date()
    ) throws -> RemoteIMMessage {
        let cleanFilePath = filePath.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanFilePath.isEmpty else { throw MasterChatStateError.blankMessage }
        guard let peerID = selectedPeerID, !peerID.isEmpty else {
            throw MasterChatStateError.noSelectedPeer
        }
        let voiceAttachment = RemoteIMVoiceAttachment(
            localFilePath: cleanFilePath,
            durationSeconds: durationSeconds
        )
        let message = RemoteIMMessage(
            fromUserID: ownerUserID,
            toUserID: peerID,
            text: Self.voiceDisplayText(durationSeconds: voiceAttachment.durationSeconds),
            voiceAttachment: voiceAttachment,
            direction: .outgoing,
            status: .pending,
            createdAt: now
        )
        messages.append(message)
        return message
    }

    @discardableResult
    public mutating func queueOutgoingImage(
        filePath: String,
        width: Int? = nil,
        height: Int? = nil,
        sizeBytes: Int? = nil,
        now: Date = Date()
    ) throws -> RemoteIMMessage {
        let cleanFilePath = filePath.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanFilePath.isEmpty else { throw MasterChatStateError.blankMessage }
        guard let peerID = selectedPeerID, !peerID.isEmpty else {
            throw MasterChatStateError.noSelectedPeer
        }
        let imageAttachment = RemoteIMImageAttachment(
            localFilePath: cleanFilePath,
            width: width,
            height: height,
            sizeBytes: sizeBytes
        )
        let message = RemoteIMMessage(
            fromUserID: ownerUserID,
            toUserID: peerID,
            text: Self.imageDisplayText(filePath: cleanFilePath),
            imageAttachment: imageAttachment,
            direction: .outgoing,
            status: .pending,
            createdAt: now
        )
        messages.append(message)
        return message
    }

    @discardableResult
    public mutating func receiveText(
        _ text: String,
        fromUserID: String,
        now: Date = Date()
    ) -> RemoteIMMessage {
        let cleanFromUserID = fromUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        let message = RemoteIMMessage(
            fromUserID: cleanFromUserID,
            toUserID: ownerUserID,
            text: Self.incomingDisplayText(text),
            direction: .incoming,
            status: .received,
            createdAt: now
        )
        if !cleanFromUserID.isEmpty && !contacts.contains(where: { $0.userID == cleanFromUserID }) {
            contacts.append(
                RemoteIMContact(
                    userID: cleanFromUserID,
                    displayName: cleanFromUserID,
                    relation: .friend
                )
            )
        }
        if selectedPeerID == nil && !cleanFromUserID.isEmpty {
            selectedPeerID = cleanFromUserID
        }
        messages.append(message)
        return message
    }

    @discardableResult
    public mutating func receiveVoice(
        filePath: String,
        durationSeconds: Int,
        fromUserID: String,
        remoteID: String? = nil,
        now: Date = Date()
    ) -> RemoteIMMessage {
        let cleanFromUserID = fromUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        let voiceAttachment = RemoteIMVoiceAttachment(
            localFilePath: filePath.trimmingCharacters(in: .whitespacesAndNewlines),
            durationSeconds: durationSeconds,
            remoteID: remoteID
        )
        let message = RemoteIMMessage(
            fromUserID: cleanFromUserID,
            toUserID: ownerUserID,
            text: Self.voiceDisplayText(durationSeconds: voiceAttachment.durationSeconds),
            voiceAttachment: voiceAttachment,
            direction: .incoming,
            status: .received,
            createdAt: now
        )
        if !cleanFromUserID.isEmpty && !contacts.contains(where: { $0.userID == cleanFromUserID }) {
            contacts.append(
                RemoteIMContact(
                    userID: cleanFromUserID,
                    displayName: cleanFromUserID,
                    relation: .friend
                )
            )
        }
        if selectedPeerID == nil && !cleanFromUserID.isEmpty {
            selectedPeerID = cleanFromUserID
        }
        messages.append(message)
        return message
    }

    @discardableResult
    public mutating func receiveImage(
        filePath: String,
        fromUserID: String,
        remoteID: String? = nil,
        width: Int? = nil,
        height: Int? = nil,
        sizeBytes: Int? = nil,
        now: Date = Date()
    ) -> RemoteIMMessage {
        let cleanFromUserID = fromUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        let cleanFilePath = filePath.trimmingCharacters(in: .whitespacesAndNewlines)
        let imageAttachment = RemoteIMImageAttachment(
            localFilePath: cleanFilePath,
            remoteID: remoteID,
            width: width,
            height: height,
            sizeBytes: sizeBytes
        )
        let message = RemoteIMMessage(
            fromUserID: cleanFromUserID,
            toUserID: ownerUserID,
            text: Self.imageDisplayText(filePath: cleanFilePath),
            imageAttachment: imageAttachment,
            direction: .incoming,
            status: .received,
            createdAt: now
        )
        if !cleanFromUserID.isEmpty && !contacts.contains(where: { $0.userID == cleanFromUserID }) {
            contacts.append(
                RemoteIMContact(
                    userID: cleanFromUserID,
                    displayName: cleanFromUserID,
                    relation: .friend
                )
            )
        }
        if selectedPeerID == nil && !cleanFromUserID.isEmpty {
            selectedPeerID = cleanFromUserID
        }
        messages.append(message)
        return message
    }

    public mutating func updateMessageStatus(
        id: UUID,
        status: RemoteIMMessageStatus
    ) throws {
        guard let index = messages.firstIndex(where: { $0.id == id }) else {
            throw MasterChatStateError.messageNotFound
        }
        messages[index].status = status
    }
}
