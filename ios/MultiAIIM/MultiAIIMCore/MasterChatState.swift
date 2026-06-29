import Foundation

public enum MasterChatStateError: Error, Equatable, LocalizedError {
    case blankUserID
    case blankMessage
    case noSelectedPeer
    case messageNotFound

    public var errorDescription: String? {
        switch self {
        case .blankUserID:
            return "UserID is required"
        case .blankMessage:
            return "Message text is required"
        case .noSelectedPeer:
            return "Select a slave before sending"
        case .messageNotFound:
            return "Message was not found"
        }
    }
}

public enum RemoteIMContactRelation: String, Codable, Equatable, Hashable {
    case friend
    case slave

    public var displayName: String {
        switch self {
        case .friend:
            return "好友"
        case .slave:
            return "奴隶"
        }
    }
}

public enum RemoteIMDraftSubmitPolicy {
    public static func textByConsumingTrailingReturn(from text: String) -> String? {
        let normalizedText = text.replacingOccurrences(of: "\r\n", with: "\n")
        guard normalizedText.hasSuffix("\n") else { return nil }
        return String(normalizedText.dropLast())
    }
}

public struct RemoteIMContact: Identifiable, Equatable, Hashable {
    public var id: String { userID }
    public let userID: String
    public var displayName: String
    public var relation: RemoteIMContactRelation

    public init(
        userID: String,
        displayName: String,
        relation: RemoteIMContactRelation = .slave
    ) {
        self.userID = userID
        self.displayName = displayName
        self.relation = relation
    }
}

public enum RemoteIMMessageDirection: String, Equatable {
    case incoming
    case outgoing
}

public enum RemoteIMMessageStatus: String, Equatable {
    case pending
    case sent
    case received
    case failed
}

public struct RemoteIMMessage: Identifiable, Equatable {
    public let id: UUID
    public let fromUserID: String
    public let toUserID: String
    public let text: String
    public let direction: RemoteIMMessageDirection
    public var status: RemoteIMMessageStatus
    public let createdAt: Date

    public init(
        id: UUID = UUID(),
        fromUserID: String,
        toUserID: String,
        text: String,
        direction: RemoteIMMessageDirection,
        status: RemoteIMMessageStatus,
        createdAt: Date
    ) {
        self.id = id
        self.fromUserID = fromUserID
        self.toUserID = toUserID
        self.text = text
        self.direction = direction
        self.status = status
        self.createdAt = createdAt
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
        try upsertContact(userID: userID, relation: .slave, displayName: displayName)
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
    }

    public func latestMessage(with peerID: String) -> RemoteIMMessage? {
        messages(with: peerID).last
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
    public mutating func receiveText(
        _ text: String,
        fromUserID: String,
        now: Date = Date()
    ) -> RemoteIMMessage {
        let cleanFromUserID = fromUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        let message = RemoteIMMessage(
            fromUserID: cleanFromUserID,
            toUserID: ownerUserID,
            text: text.trimmingCharacters(in: .whitespacesAndNewlines),
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
