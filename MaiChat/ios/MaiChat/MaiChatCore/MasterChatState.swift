import Foundation

public enum MasterChatStateError: Error, Equatable, LocalizedError {
    case blankUserID
    case selfContactNotAllowed
    case blankMessage
    case noSelectedPeer
    case messageNotFound

    public var errorDescription: String? {
        switch self {
        case .blankUserID:
            return "请填写账号 ID"
        case .selfContactNotAllowed:
            return "不能添加当前登录账号为好友"
        case .blankMessage:
            return "请输入消息内容"
        case .noSelectedPeer:
            return "请先选择联系人"
        case .messageNotFound:
            return "消息不存在"
        }
    }
}

public enum RemoteIMPeerPolicy {
    public static func isValidPeer(userID: String, ownerUserID: String) -> Bool {
        let cleanUserID = userID.trimmingCharacters(in: .whitespacesAndNewlines)
        let cleanOwnerUserID = ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        return !cleanUserID.isEmpty && (cleanOwnerUserID.isEmpty || cleanUserID != cleanOwnerUserID)
    }
}

public enum RemoteIMContactRelation: String, Codable, Equatable, Hashable, Sendable {
    case friend
    case slave

    public var displayName: String {
        return "好友"
    }
}

public enum RemoteIMDraftSubmitPolicy {
    public static func shouldSubmit(replacementText: String) -> Bool {
        replacementText == "\n" || replacementText == "\r\n"
    }

    @available(*, deprecated, message: "Inspect text input events with shouldSubmit(replacementText:) instead")
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

public enum RemoteIMApprovalAction: String, Codable, CaseIterable, Equatable, Hashable, Sendable {
    case approveOnce = "approve-once"
    case approvePrefix = "approve-prefix"
    case reject
    case resolved
    case autoDeclined = "auto-declined"

    public var title: String {
        switch self {
        case .approveOnce: return "同意本次"
        case .approvePrefix: return "同意并记住"
        case .reject: return "拒绝"
        case .resolved: return "审批已处理"
        case .autoDeclined: return "审批已自动拒绝"
        }
    }

    public var decisionDisplayText: String {
        "审批操作：\(title)"
    }
}

public struct RemoteIMApprovalRequest: Codable, Equatable, Sendable {
    public let token: String
    public let actions: [RemoteIMApprovalAction]

    public init?(token: String, actions: [RemoteIMApprovalAction]) {
        guard Self.isValidToken(token),
              actions.count >= 2,
              actions.count <= 3,
              actions.allSatisfy({
                  $0 == .approveOnce || $0 == .approvePrefix || $0 == .reject
              }),
              Set(actions).count == actions.count,
              actions.contains(.approveOnce),
              actions.contains(.reject)
        else { return nil }
        self.token = token
        self.actions = actions
    }

    public func allows(_ action: RemoteIMApprovalAction) -> Bool {
        actions.contains(action)
    }

    public static func isValidToken(_ token: String) -> Bool {
        token.hasPrefix("approval-") &&
            token.count > "approval-".count &&
            token.count <= 200 &&
            token.dropFirst("approval-".count).allSatisfy { character in
                character.isASCII &&
                    (character.isLetter || character.isNumber || character == "-" || character == "_")
            }
    }
}

public struct RemoteIMApprovalDecision: Codable, Equatable, Sendable {
    public let token: String
    public let action: RemoteIMApprovalAction

    public init?(token: String, action: RemoteIMApprovalAction) {
        guard RemoteIMApprovalRequest.isValidToken(token) else { return nil }
        self.token = token
        self.action = action
    }
}

public enum RemoteIMMessageOrigin: String, Codable, Equatable, Sendable {
    case human
    case machine
}

public enum RemoteIMTextInteraction: Equatable, Sendable {
    case approvalRequest(RemoteIMApprovalRequest)
    case approvalDecision(token: String, action: RemoteIMApprovalAction)
}

public struct RemoteIMCloudMetadata: Equatable, Sendable {
    public let origin: RemoteIMMessageOrigin
    public let interaction: RemoteIMTextInteraction?

    public init(origin: RemoteIMMessageOrigin, interaction: RemoteIMTextInteraction? = nil) {
        self.origin = origin
        self.interaction = interaction
    }

    public var approvalRequest: RemoteIMApprovalRequest? {
        guard case let .approvalRequest(request) = interaction else { return nil }
        return request
    }

    public var approvalDecision: RemoteIMApprovalDecision? {
        guard case let .approvalDecision(token, action) = interaction else { return nil }
        return RemoteIMApprovalDecision(token: token, action: action)
    }
}

public enum RemoteIMCloudMetadataCodec {
    public static let namespace = "multi-ai-code"
    public static let version = 2

    private struct WireInteraction: Codable {
        let kind: String
        let token: String
        let actions: [String]?
        let action: String?
        let outcome: String?
    }

    private struct WireMetadata: Codable {
        let namespace: String
        let version: Int
        let origin: String
        let interaction: WireInteraction?
    }

    public static func encode(_ metadata: RemoteIMCloudMetadata) -> Data {
        let wireInteraction: WireInteraction?
        switch metadata.interaction {
        case let .approvalRequest(request):
            wireInteraction = WireInteraction(
                kind: "approval-request",
                token: request.token,
                actions: request.actions.map(\.rawValue),
                action: nil,
                outcome: nil
            )
        case let .approvalDecision(token, action):
            wireInteraction = WireInteraction(
                kind: "approval-decision",
                token: token,
                actions: nil,
                action: action.rawValue,
                outcome: nil
            )
        case nil:
            wireInteraction = nil
        }
        return try! JSONEncoder().encode(WireMetadata(
            namespace: namespace,
            version: version,
            origin: metadata.origin.rawValue,
            interaction: wireInteraction
        ))
    }

    public static func decode(_ data: Data?) -> RemoteIMCloudMetadata? {
        guard let data,
              let wire = try? JSONDecoder().decode(WireMetadata.self, from: data),
              wire.namespace == namespace,
              wire.version == version,
              let origin = RemoteIMMessageOrigin(rawValue: wire.origin)
        else { return nil }
        guard let interaction = wire.interaction else {
            return RemoteIMCloudMetadata(origin: origin)
        }

        if interaction.kind == "approval-request",
           origin == .machine,
           interaction.action == nil,
           let rawActions = interaction.actions,
           rawActions.count == Set(rawActions).count
        {
            let actions = rawActions.compactMap(RemoteIMApprovalAction.init(rawValue:))
            guard actions.count == rawActions.count,
                  let request = RemoteIMApprovalRequest(token: interaction.token, actions: actions)
            else { return nil }
            return RemoteIMCloudMetadata(
                origin: origin,
                interaction: .approvalRequest(request)
            )
        }

        if interaction.kind == "approval-decision",
           origin == .human,
           interaction.actions == nil,
           RemoteIMApprovalRequest.isValidToken(interaction.token),
           let rawAction = interaction.action,
           let action = RemoteIMApprovalAction(rawValue: rawAction),
           action == .approveOnce || action == .approvePrefix || action == .reject
        {
            return RemoteIMCloudMetadata(
                origin: origin,
                interaction: .approvalDecision(token: interaction.token, action: action)
            )
        }
        if interaction.kind == "approval-resolved",
           origin == .machine,
           interaction.actions == nil,
           interaction.action == nil,
           RemoteIMApprovalRequest.isValidToken(interaction.token),
           let outcome = interaction.outcome,
           ["approved", "rejected", "resolved", "auto-declined"].contains(outcome)
        {
            let action: RemoteIMApprovalAction = outcome == "auto-declined" ? .autoDeclined : .resolved
            return RemoteIMCloudMetadata(
                origin: origin,
                interaction: .approvalDecision(token: interaction.token, action: action)
            )
        }
        return nil
    }
}

public enum RemoteIMTimestampTextPolicy {
    public static func displayText(
        for date: Date,
        now: Date = Date(),
        calendar: Calendar = .current
    ) -> String {
        if calendar.isDate(date, inSameDayAs: now) {
            return timeText(for: date, calendar: calendar)
        }

        if let yesterday = calendar.date(byAdding: .day, value: -1, to: now),
           calendar.isDate(date, inSameDayAs: yesterday)
        {
            return "昨天 " + timeText(for: date, calendar: calendar)
        }

        let components = calendar.dateComponents([.month, .day], from: date)
        return "\(components.month ?? 0) 月 \(components.day ?? 0) 日 " +
            timeText(for: date, calendar: calendar)
    }

    private static func timeText(for date: Date, calendar: Calendar) -> String {
        let components = calendar.dateComponents([.hour, .minute], from: date)
        return String(
            format: "%02d:%02d",
            components.hour ?? 0,
            components.minute ?? 0
        )
    }
}

public enum RemoteIMMessageCopyPolicy {
    public static func selectionText(for message: RemoteIMMessage) -> String {
        let text = message.text.trimmingCharacters(in: .whitespacesAndNewlines)
        if !text.isEmpty {
            return text
        }

        if let attachment = message.videoAttachment {
            return "[视频消息] \(attachment.durationSeconds) 秒"
        }
        if let attachment = message.imageAttachment {
            return "[图片消息] \(fileName(from: attachment.localFilePath, fallback: "图片"))"
        }
        if let attachment = message.fileAttachment {
            return "[文件消息] \(attachment.fileName)"
        }
        if let attachment = message.voiceAttachment {
            return "[语音消息] \(attachment.durationSeconds) 秒"
        }
        return "[空消息]"
    }

    public static func fullText(
        for message: RemoteIMMessage,
        calendar: Calendar = .current
    ) -> String {
        var lines = [
            "发送人：\(message.fromUserID)",
            "接收人：\(message.toUserID)",
            "时间：\(formattedDate(message.createdAt, calendar: calendar))",
            "方向：\(directionText(message.direction))",
            "状态：\(statusText(message.status))",
            "类型：\(messageTypeText(message))",
            "内容：",
            selectionText(for: message)
        ]

        if let detail = attachmentDetail(message) {
            lines.append("附件：\(detail)")
        }
        return lines.joined(separator: "\n")
    }

    private static func formattedDate(_ date: Date, calendar: Calendar) -> String {
        let components = calendar.dateComponents(
            [.year, .month, .day, .hour, .minute, .second],
            from: date
        )
        return String(
            format: "%04d-%02d-%02d %02d:%02d:%02d",
            components.year ?? 0,
            components.month ?? 0,
            components.day ?? 0,
            components.hour ?? 0,
            components.minute ?? 0,
            components.second ?? 0
        )
    }

    private static func directionText(_ direction: RemoteIMMessageDirection) -> String {
        direction == .incoming ? "收到" : "发出"
    }

    private static func statusText(_ status: RemoteIMMessageStatus) -> String {
        switch status {
        case .pending:
            return "发送中"
        case .sent:
            return "已发送"
        case .received:
            return "已接收"
        case .failed:
            return "发送失败"
        }
    }

    private static func messageTypeText(_ message: RemoteIMMessage) -> String {
        if message.videoAttachment != nil { return "视频" }
        if message.imageAttachment != nil { return "图片" }
        if message.fileAttachment != nil { return "文件" }
        if message.voiceAttachment != nil { return "语音" }
        return "文本"
    }

    private static func attachmentDetail(_ message: RemoteIMMessage) -> String? {
        if let attachment = message.videoAttachment {
            var details = ["\(attachment.durationSeconds) 秒"]
            if attachment.width > 0, attachment.height > 0 {
                details.append("\(attachment.width) x \(attachment.height)")
            }
            if attachment.sizeBytes > 0 {
                details.append("\(attachment.sizeBytes) 字节")
            }
            return details.joined(separator: "，")
        }
        if let attachment = message.imageAttachment {
            var details = [fileName(from: attachment.localFilePath, fallback: "图片")]
            if let width = attachment.width, let height = attachment.height {
                details.append("\(width) x \(height)")
            }
            if let sizeBytes = attachment.sizeBytes {
                details.append("\(sizeBytes) 字节")
            }
            return details.joined(separator: "，")
        }
        if let attachment = message.fileAttachment {
            var details = [attachment.fileName]
            if !attachment.mimeType.isEmpty {
                details.append(attachment.mimeType)
            }
            if let sizeBytes = attachment.sizeBytes {
                details.append("\(sizeBytes) 字节")
            }
            return details.joined(separator: "，")
        }
        if let attachment = message.voiceAttachment {
            return "\(attachment.durationSeconds) 秒"
        }
        return nil
    }

    private static func fileName(from path: String, fallback: String) -> String {
        let fileName = URL(fileURLWithPath: path).lastPathComponent
        return fileName.isEmpty ? fallback : fileName
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

public struct RemoteIMVideoPreviewItem: Identifiable, Equatable {
    public let id: UUID
    public let localFilePath: String

    public init(id: UUID, localFilePath: String) {
        self.id = id
        self.localFilePath = localFilePath
    }
}

public enum RemoteIMVideoPreviewPolicy {
    public static func previewItem(for message: RemoteIMMessage) -> RemoteIMVideoPreviewItem? {
        guard let attachment = message.videoAttachment,
              !attachment.localPath.isEmpty,
              FileManager.default.fileExists(atPath: attachment.localPath)
        else {
            return nil
        }
        return RemoteIMVideoPreviewItem(id: message.id, localFilePath: attachment.localPath)
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

public struct RemoteIMContactGroup: Codable, Equatable, Hashable, Sendable, Identifiable {
    public var id: String { name }
    public let name: String
    public let sortOrder: Int

    public init(name: String, sortOrder: Int) {
        self.name = name.trimmingCharacters(in: .whitespacesAndNewlines)
        self.sortOrder = sortOrder
    }
}

public struct RemoteIMContact: Identifiable, Codable, Equatable, Hashable, Sendable {
    public var id: String { userID }
    public let userID: String
    public var displayName: String
    public var avatarURL: String?
    public var relation: RemoteIMContactRelation
    public var groupName: String

    public init(
        userID: String,
        displayName: String,
        avatarURL: String? = nil,
        relation: RemoteIMContactRelation = .friend,
        groupName: String = ""
    ) {
        self.userID = userID
        self.displayName = displayName
        self.avatarURL = avatarURL
        self.relation = .friend
        self.groupName = groupName.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private enum CodingKeys: String, CodingKey {
        case userID, displayName, avatarURL, relation, groupName
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        userID = try container.decode(String.self, forKey: .userID)
        displayName = try container.decode(String.self, forKey: .displayName)
        avatarURL = try container.decodeIfPresent(String.self, forKey: .avatarURL)
        relation = try container.decodeIfPresent(
            RemoteIMContactRelation.self,
            forKey: .relation
        ) ?? .friend
        groupName = try container.decodeIfPresent(String.self, forKey: .groupName)?
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
    }
}

public enum RemoteIMContactListItem: Equatable, Sendable, Identifiable {
    case group(name: String, memberCount: Int)
    case contact(RemoteIMContact, indented: Bool)

    public var id: String {
        switch self {
        case let .group(name, _): return "group:\(name)"
        case let .contact(contact, _): return "contact:\(contact.userID)"
        }
    }
}

public enum RemoteIMContactGroupDisplayPolicy {
    public static func items(
        groups: [RemoteIMContactGroup],
        contacts: [RemoteIMContact],
        collapsedGroupNames: Set<String>,
        query: String
    ) -> [RemoteIMContactListItem] {
        let needle = query.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        let searching = !needle.isEmpty
        var result: [RemoteIMContactListItem] = []
        for group in groups {
            let allMembers = contacts.filter { $0.groupName == group.name }
            let matchedMembers = allMembers.filter { matches($0, needle: needle) }
            if searching && matchedMembers.isEmpty { continue }
            result.append(.group(name: group.name, memberCount: allMembers.count))
            if searching || !collapsedGroupNames.contains(group.name) {
                result.append(contentsOf: matchedMembers.map { .contact($0, indented: true) })
            }
        }
        result.append(contentsOf: contacts
            .filter { $0.groupName.isEmpty && matches($0, needle: needle) }
            .map { .contact($0, indented: false) })
        return result
    }

    private static func matches(_ contact: RemoteIMContact, needle: String) -> Bool {
        needle.isEmpty
            || contact.displayName.lowercased().contains(needle)
            || contact.userID.lowercased().contains(needle)
    }
}

public enum RemoteIMBroadcastGroupSelectionState: Equatable, Sendable {
    case none
    case partial
    case all
}

public enum RemoteIMBroadcastSelectionPolicy {
    public static func uniqueRecipientIDs(_ rawIDs: [String]) -> [String] {
        var seen = Set<String>()
        return rawIDs.compactMap { raw in
            let clean = raw.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !clean.isEmpty, seen.insert(clean).inserted else { return nil }
            return clean
        }
    }

    public static func groupState(
        groupName: String,
        contacts: [RemoteIMContact],
        selectedUserIDs: Set<String>
    ) -> RemoteIMBroadcastGroupSelectionState {
        let members = contacts.filter { $0.groupName == groupName }
        let selectedCount = members.filter { selectedUserIDs.contains($0.userID) }.count
        if members.isEmpty || selectedCount == 0 { return .none }
        return selectedCount == members.count ? .all : .partial
    }

    public static func settingGroup(
        groupName: String,
        contacts: [RemoteIMContact],
        selectedUserIDs: Set<String>,
        selected: Bool
    ) -> Set<String> {
        var result = selectedUserIDs
        for contact in contacts where contact.groupName == groupName {
            if selected { result.insert(contact.userID) }
            else { result.remove(contact.userID) }
        }
        return result
    }
}

public struct RemoteIMBroadcastDeliveryTracker: Equatable, Sendable {
    public let total: Int
    public private(set) var failedUserIDs: [String] = []

    public init(total: Int) {
        self.total = max(0, total)
    }

    public mutating func record(userID: String, succeeded: Bool) {
        guard !succeeded else { return }
        let cleanUserID = userID.trimmingCharacters(in: .whitespacesAndNewlines)
        if !cleanUserID.isEmpty { failedUserIDs.append(cleanUserID) }
    }
}

public enum RemoteIMAvatarMonogramPolicy {
    public static func text(displayName: String, userID: String) -> String {
        let cleanUserID = userID.trimmingCharacters(in: .whitespacesAndNewlines)
        let cleanDisplayName = displayName.trimmingCharacters(in: .whitespacesAndNewlines)
        let hasNickname = !cleanDisplayName.isEmpty && cleanDisplayName != cleanUserID
        let source = hasNickname ? cleanDisplayName : cleanUserID
        guard !source.isEmpty else { return "M" }
        guard hasNickname else { return String(source.prefix(1)).uppercased() }

        let words = source
            .replacingOccurrences(of: "-", with: " ")
            .replacingOccurrences(of: "_", with: " ")
            .split(whereSeparator: { $0.isWhitespace })
        if words.count >= 2, let first = words.first, let last = words.last {
            return (String(first.prefix(1)) + String(last.prefix(1))).uppercased()
        }
        let containsNonASCII = source.unicodeScalars.contains { !$0.isASCII }
        return String(containsNonASCII ? source.suffix(2) : source.prefix(2)).uppercased()
    }
}

public enum RemoteIMMessageDirection: String, Codable, Equatable, Sendable {
    case incoming
    case outgoing
}

public enum RemoteIMMessageStatus: String, Codable, Equatable, Sendable {
    case pending
    case sent
    case received
    case failed
}

public struct RemoteIMVoiceAttachment: Codable, Equatable, Sendable {
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

public struct RemoteIMImageAttachment: Codable, Equatable, Sendable {
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

public struct RemoteIMVideoAttachment: Codable, Equatable, Sendable {
    public let localPath: String
    public let coverPath: String?
    public let durationSeconds: Int
    public let width: Int
    public let height: Int
    public let sizeBytes: Int64

    public init(
        localPath: String,
        coverPath: String? = nil,
        durationSeconds: Int,
        width: Int,
        height: Int,
        sizeBytes: Int64
    ) {
        self.localPath = localPath.trimmingCharacters(in: .whitespacesAndNewlines)
        let cleanCoverPath = coverPath?.trimmingCharacters(in: .whitespacesAndNewlines)
        self.coverPath = cleanCoverPath?.isEmpty == false ? cleanCoverPath : nil
        self.durationSeconds = max(0, durationSeconds)
        self.width = max(0, width)
        self.height = max(0, height)
        self.sizeBytes = max(0, sizeBytes)
    }
}

public enum RemoteIMVideoDownloadStage: String, Equatable, Sendable {
    case metadata
    case coverReady
    case videoReady
    case videoFailed
}

public enum RemoteIMVideoDownloadTrackingPolicy {
    public static func updatedKeys(
        current: Set<String>,
        key: String,
        stage: RemoteIMVideoDownloadStage,
        fileIsUsable: Bool
    ) -> Set<String> {
        var result = current
        switch stage {
        case .metadata:
            if fileIsUsable {
                result.remove(key)
            } else {
                result.insert(key)
            }
        case .coverReady:
            break
        case .videoReady, .videoFailed:
            result.remove(key)
        }
        return result
    }
}

public struct RemoteIMFileAttachment: Codable, Equatable, Sendable {
    public let localFilePath: String
    public let fileName: String
    public let mimeType: String
    public let remoteID: String?
    public let sizeBytes: Int?

    public init(
        localFilePath: String,
        fileName: String,
        mimeType: String,
        remoteID: String? = nil,
        sizeBytes: Int? = nil
    ) {
        self.localFilePath = localFilePath
        let cleanFileName = fileName.trimmingCharacters(in: .whitespacesAndNewlines)
        self.fileName = cleanFileName.isEmpty ? URL(fileURLWithPath: localFilePath).lastPathComponent : cleanFileName
        self.mimeType = mimeType.trimmingCharacters(in: .whitespacesAndNewlines)
        self.remoteID = remoteID
        self.sizeBytes = sizeBytes
    }
}

public struct RemoteIMMessage: Identifiable, Codable, Equatable, Sendable {
    public let id: UUID
    public var remoteID: String?
    public let fromUserID: String
    public let toUserID: String
    public let text: String
    public let voiceAttachment: RemoteIMVoiceAttachment?
    public let imageAttachment: RemoteIMImageAttachment?
    public let fileAttachment: RemoteIMFileAttachment?
    public var videoAttachment: RemoteIMVideoAttachment?
    public let approvalRequest: RemoteIMApprovalRequest?
    public let approvalDecision: RemoteIMApprovalDecision?
    public let direction: RemoteIMMessageDirection
    public var status: RemoteIMMessageStatus
    public var createdAt: Date

    public init(
        id: UUID = UUID(),
        remoteID: String? = nil,
        fromUserID: String,
        toUserID: String,
        text: String,
        voiceAttachment: RemoteIMVoiceAttachment? = nil,
        imageAttachment: RemoteIMImageAttachment? = nil,
        fileAttachment: RemoteIMFileAttachment? = nil,
        videoAttachment: RemoteIMVideoAttachment? = nil,
        approvalRequest: RemoteIMApprovalRequest? = nil,
        approvalDecision: RemoteIMApprovalDecision? = nil,
        direction: RemoteIMMessageDirection,
        status: RemoteIMMessageStatus,
        createdAt: Date
    ) {
        self.id = id
        self.remoteID = remoteID
        self.fromUserID = fromUserID
        self.toUserID = toUserID
        self.text = text
        self.voiceAttachment = voiceAttachment
        self.imageAttachment = imageAttachment
        self.fileAttachment = fileAttachment
        self.videoAttachment = videoAttachment
        self.approvalRequest = approvalRequest
        self.approvalDecision = approvalDecision
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

    public var isFileMessage: Bool {
        fileAttachment != nil
    }

    public var isVideoMessage: Bool {
        videoAttachment != nil
    }
}

public struct MasterChatState: Equatable {
    public let ownerUserID: String
    public private(set) var contacts: [RemoteIMContact]
    public private(set) var contactGroups: [RemoteIMContactGroup]
    public private(set) var messages: [RemoteIMMessage]
    public private(set) var selectedPeerID: String?
    private var conversationMessagesByPeerID: [String: [RemoteIMMessage]]
    private var messageIndexByID: [UUID: Int]
    private var messageIDByRemoteID: [String: UUID]

    public static func == (left: MasterChatState, right: MasterChatState) -> Bool {
        left.ownerUserID == right.ownerUserID &&
            left.contacts == right.contacts &&
            left.contactGroups == right.contactGroups &&
            left.messages == right.messages &&
            left.selectedPeerID == right.selectedPeerID
    }

    public init(ownerUserID: String) {
        self.ownerUserID = ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        self.contacts = []
        self.contactGroups = []
        self.messages = []
        self.selectedPeerID = nil
        self.conversationMessagesByPeerID = [:]
        self.messageIndexByID = [:]
        self.messageIDByRemoteID = [:]
    }

    public init(
        ownerUserID: String,
        contacts: [RemoteIMContact],
        contactGroups: [RemoteIMContactGroup] = [],
        messages: [RemoteIMMessage],
        selectedPeerID: String? = nil
    ) {
        self.ownerUserID = ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        self.contactGroups = Self.normalizedContactGroups(contactGroups)
        self.contacts = Self.normalizedContacts(
            contacts,
            ownerUserID: self.ownerUserID,
            groupNames: Set(self.contactGroups.map(\.name))
        )
        self.messages = Self.normalizedMessages(messages, ownerUserID: self.ownerUserID)
        self.conversationMessagesByPeerID = [:]
        self.messageIndexByID = [:]
        self.messageIDByRemoteID = [:]
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
        rebuildMessageIndexes()
    }

    private static func normalizedContacts(
        _ contacts: [RemoteIMContact],
        ownerUserID: String,
        groupNames: Set<String>
    ) -> [RemoteIMContact] {
        var normalizedContacts: [RemoteIMContact] = []
        for contact in contacts {
            let cleanUserID = contact.userID.trimmingCharacters(in: .whitespacesAndNewlines)
            guard RemoteIMPeerPolicy.isValidPeer(
                userID: cleanUserID,
                ownerUserID: ownerUserID
            ) else { continue }
            let cleanDisplayName = contact.displayName.trimmingCharacters(in: .whitespacesAndNewlines)
            let cleanAvatarURL = contact.avatarURL?.trimmingCharacters(in: .whitespacesAndNewlines)
            let normalizedContact = RemoteIMContact(
                userID: cleanUserID,
                displayName: cleanDisplayName.isEmpty ? cleanUserID : cleanDisplayName,
                avatarURL: cleanAvatarURL?.isEmpty == false ? cleanAvatarURL : nil,
                relation: .friend,
                groupName: groupNames.contains(contact.groupName) ? contact.groupName : ""
            )
            if let index = normalizedContacts.firstIndex(where: { $0.userID == cleanUserID }) {
                normalizedContacts[index] = normalizedContact
            } else {
                normalizedContacts.append(normalizedContact)
            }
        }
        return normalizedContacts
    }

    private static func normalizedContactGroups(
        _ groups: [RemoteIMContactGroup]
    ) -> [RemoteIMContactGroup] {
        var names = Set<String>()
        return groups
            .map { RemoteIMContactGroup(name: $0.name, sortOrder: $0.sortOrder) }
            .filter { !$0.name.isEmpty && names.insert($0.name).inserted }
            .sorted {
                $0.sortOrder == $1.sortOrder
                    ? $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending
                    : $0.sortOrder < $1.sortOrder
            }
    }

    private static func normalizedMessages(
        _ messages: [RemoteIMMessage],
        ownerUserID: String
    ) -> [RemoteIMMessage] {
        guard !ownerUserID.isEmpty else { return messages }
        return messages
            .filter { message in
                guard message.fromUserID == ownerUserID || message.toUserID == ownerUserID else {
                    return false
                }
                let peerUserID = message.fromUserID == ownerUserID
                    ? message.toUserID
                    : message.fromUserID
                return RemoteIMPeerPolicy.isValidPeer(
                    userID: peerUserID,
                    ownerUserID: ownerUserID
                )
            }
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
            guard RemoteIMPeerPolicy.isValidPeer(
                userID: cleanPeerID,
                ownerUserID: ownerUserID
            ) else { continue }
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
        displayName: String? = nil,
        avatarURL: String? = nil
    ) throws {
        let cleanUserID = userID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanUserID.isEmpty else { throw MasterChatStateError.blankUserID }
        guard cleanUserID != ownerUserID else {
            throw MasterChatStateError.selfContactNotAllowed
        }
        let cleanDisplayName = displayName?.trimmingCharacters(in: .whitespacesAndNewlines)
        let cleanAvatarURL = avatarURL?.trimmingCharacters(in: .whitespacesAndNewlines)
        if let index = contacts.firstIndex(where: { $0.userID == cleanUserID }) {
            var contact = contacts[index]
            if let cleanDisplayName,
               !cleanDisplayName.isEmpty,
               cleanDisplayName != cleanUserID || contact.displayName.isEmpty || contact.displayName == cleanUserID
            {
                contact.displayName = cleanDisplayName
            }
            if let cleanAvatarURL, !cleanAvatarURL.isEmpty {
                contact.avatarURL = cleanAvatarURL
            }
            contact.relation = .friend
            // profile refresh 不带本地 groupName，保留现值。
            contacts[index] = contact
        } else {
            contacts.append(
                RemoteIMContact(
                    userID: cleanUserID,
                    displayName: cleanDisplayName?.isEmpty == false ? cleanDisplayName! : cleanUserID,
                    avatarURL: cleanAvatarURL?.isEmpty == false ? cleanAvatarURL : nil,
                    relation: relation
                )
            )
        }
        if selectedPeerID == nil {
            selectedPeerID = cleanUserID
        }
    }

    @discardableResult
    public mutating func createContactGroup(name: String) -> Bool {
        let cleanName = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanName.isEmpty,
              !contactGroups.contains(where: { $0.name == cleanName }) else { return false }
        let nextOrder = (contactGroups.map(\.sortOrder).max() ?? -1) + 1
        contactGroups.append(RemoteIMContactGroup(name: cleanName, sortOrder: nextOrder))
        return true
    }

    @discardableResult
    public mutating func renameContactGroup(from: String, to: String) -> Bool {
        let oldName = from.trimmingCharacters(in: .whitespacesAndNewlines)
        let newName = to.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !newName.isEmpty,
              let index = contactGroups.firstIndex(where: { $0.name == oldName }) else { return false }
        if oldName == newName { return true }
        guard !contactGroups.contains(where: { $0.name == newName }) else { return false }
        contactGroups[index] = RemoteIMContactGroup(
            name: newName,
            sortOrder: contactGroups[index].sortOrder
        )
        for contactIndex in contacts.indices where contacts[contactIndex].groupName == oldName {
            contacts[contactIndex].groupName = newName
        }
        return true
    }

    @discardableResult
    public mutating func deleteContactGroup(name: String) -> Bool {
        let cleanName = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let index = contactGroups.firstIndex(where: { $0.name == cleanName }) else {
            return false
        }
        contactGroups.remove(at: index)
        for contactIndex in contacts.indices where contacts[contactIndex].groupName == cleanName {
            contacts[contactIndex].groupName = ""
        }
        return true
    }

    @discardableResult
    public mutating func setContactGroup(userID: String, groupName: String) -> Bool {
        let cleanUserID = userID.trimmingCharacters(in: .whitespacesAndNewlines)
        let cleanGroupName = groupName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let index = contacts.firstIndex(where: { $0.userID == cleanUserID }) else {
            return false
        }
        contacts[index].groupName = contactGroups.contains(where: { $0.name == cleanGroupName })
            ? cleanGroupName
            : ""
        return true
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
        rebuildMessageIndexes()
        if selectedPeerID == cleanUserID ||
            selectedPeerID.map({ selected in !contacts.contains(where: { $0.userID == selected }) }) == true
        {
            selectedPeerID = contacts.first?.userID
        }
    }

    public mutating func removeMessages(with userID: String) {
        let cleanUserID = userID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanUserID.isEmpty else { return }
        messages.removeAll { message in
            message.fromUserID == cleanUserID || message.toUserID == cleanUserID
        }
        rebuildMessageIndexes()
    }

    public mutating func selectPeer(userID: String) {
        let cleanUserID = userID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard RemoteIMPeerPolicy.isValidPeer(
            userID: cleanUserID,
            ownerUserID: ownerUserID
        ), contacts.contains(where: { $0.userID == cleanUserID }) else { return }
        selectedPeerID = cleanUserID
    }

    public func messages(with peerID: String) -> [RemoteIMMessage] {
        let cleanPeerID = peerID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanPeerID.isEmpty else { return [] }
        return conversationMessagesByPeerID[cleanPeerID] ?? []
    }

    public func latestMessage(with peerID: String) -> RemoteIMMessage? {
        let cleanPeerID = peerID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanPeerID.isEmpty else { return nil }
        return conversationMessagesByPeerID[cleanPeerID]?.last
    }

    public func messageCount(with peerID: String) -> Int {
        let cleanPeerID = peerID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanPeerID.isEmpty else { return 0 }
        return conversationMessagesByPeerID[cleanPeerID]?.count ?? 0
    }

    public func message(id: UUID) -> RemoteIMMessage? {
        guard let index = messageIndexByID[id] else { return nil }
        return messages[index]
    }

    public func message(remoteID: String?) -> RemoteIMMessage? {
        existingMessage(remoteID: remoteID)
    }

    /// Inserts a bounded history page or conversation summary into the in-memory working set.
    /// Existing messages always win because live delivery updates may be newer than an
    /// asynchronous SQLite read that started before them.
    public mutating func mergeMessages(_ incomingMessages: [RemoteIMMessage]) {
        let normalizedIncoming = Self.normalizedMessages(
            incomingMessages,
            ownerUserID: ownerUserID
        )
        guard !normalizedIncoming.isEmpty else { return }

        var indexByID: [UUID: Int] = [:]
        var indexByRemoteID: [String: Int] = [:]
        for (index, message) in messages.enumerated() {
            indexByID[message.id] = index
            if let remoteID = message.remoteID, !remoteID.isEmpty {
                indexByRemoteID[remoteID] = index
            }
        }

        for message in normalizedIncoming {
            if indexByID[message.id] != nil { continue }
            if let remoteID = message.remoteID,
               !remoteID.isEmpty,
               indexByRemoteID[remoteID] != nil
            {
                continue
            }
            indexByID[message.id] = messages.count
            if let remoteID = message.remoteID, !remoteID.isEmpty {
                indexByRemoteID[remoteID] = messages.count
            }
            messages.append(message)
        }

        messages.sort(by: Self.messageIsEarlier)
        Self.addMissingContacts(from: messages, ownerUserID: ownerUserID, contacts: &contacts)
        rebuildMessageIndexes()
    }

    private static func voiceDisplayText(durationSeconds: Int) -> String {
        "[语音消息 \(max(1, durationSeconds))s]"
    }

    private static func imageDisplayText(filePath: String) -> String {
        let fileName = URL(fileURLWithPath: filePath).lastPathComponent
        return fileName.isEmpty ? "[图片消息]" : "[图片消息] \(fileName)"
    }

    private static func videoDisplayText(durationSeconds: Int) -> String {
        "[视频消息 \(max(0, durationSeconds))s]"
    }

    private static func fileDisplayText(fileName: String, filePath: String) -> String {
        let cleanFileName = fileName.trimmingCharacters(in: .whitespacesAndNewlines)
        let fallbackName = URL(fileURLWithPath: filePath).lastPathComponent
        let displayName = cleanFileName.isEmpty ? fallbackName : cleanFileName
        return displayName.isEmpty ? "[文件消息]" : "[文件消息] \(displayName)"
    }

    private static func incomingDisplayText(_ text: String) -> String {
        var cleanText = text.trimmingCharacters(in: .whitespacesAndNewlines)
        let invisibleAICLIPrefix = "\u{2063}\u{200B}\u{200C}\u{200D}\u{2063}"
        if cleanText.hasPrefix(invisibleAICLIPrefix) {
            cleanText.removeFirst(invisibleAICLIPrefix.count)
            return cleanText.trimmingCharacters(in: .whitespacesAndNewlines)
        }
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
        guard let peerID = selectedPeerID, !peerID.isEmpty else {
            throw MasterChatStateError.noSelectedPeer
        }
        return try queueOutgoingText(to: peerID, text: text, now: now)
    }

    @discardableResult
    public mutating func queueOutgoingText(
        to peerID: String,
        text: String,
        now: Date = Date()
    ) throws -> RemoteIMMessage {
        let cleanText = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanText.isEmpty else { throw MasterChatStateError.blankMessage }
        let cleanPeerID = peerID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanPeerID.isEmpty else { throw MasterChatStateError.noSelectedPeer }
        let message = RemoteIMMessage(
            fromUserID: ownerUserID,
            toUserID: cleanPeerID,
            text: cleanText,
            direction: .outgoing,
            status: .pending,
            createdAt: now
        )
        appendMessage(message)
        return message
    }

    @discardableResult
    public mutating func queueOutgoingApprovalDecision(
        token: String,
        action: RemoteIMApprovalAction,
        now: Date = Date()
    ) throws -> RemoteIMMessage {
        guard action == .approveOnce || action == .approvePrefix || action == .reject else {
            throw MasterChatStateError.blankMessage
        }
        guard let decision = RemoteIMApprovalDecision(token: token, action: action) else {
            throw MasterChatStateError.blankMessage
        }
        guard let peerID = selectedPeerID, !peerID.isEmpty else {
            throw MasterChatStateError.noSelectedPeer
        }
        let message = RemoteIMMessage(
            fromUserID: ownerUserID,
            toUserID: peerID,
            text: action.decisionDisplayText,
            approvalDecision: decision,
            direction: .outgoing,
            status: .pending,
            createdAt: now
        )
        appendMessage(message)
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
        appendMessage(message)
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
        appendMessage(message)
        return message
    }

    @discardableResult
    public mutating func queueOutgoingVideo(
        filePath: String,
        coverPath: String?,
        durationSeconds: Int,
        width: Int,
        height: Int,
        sizeBytes: Int64,
        now: Date = Date()
    ) throws -> RemoteIMMessage {
        let cleanFilePath = filePath.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanFilePath.isEmpty else { throw MasterChatStateError.blankMessage }
        guard let peerID = selectedPeerID, !peerID.isEmpty else {
            throw MasterChatStateError.noSelectedPeer
        }
        let videoAttachment = RemoteIMVideoAttachment(
            localPath: cleanFilePath,
            coverPath: coverPath,
            durationSeconds: durationSeconds,
            width: width,
            height: height,
            sizeBytes: sizeBytes
        )
        let message = RemoteIMMessage(
            fromUserID: ownerUserID,
            toUserID: peerID,
            text: Self.videoDisplayText(durationSeconds: videoAttachment.durationSeconds),
            videoAttachment: videoAttachment,
            direction: .outgoing,
            status: .pending,
            createdAt: now
        )
        appendMessage(message)
        return message
    }

    @discardableResult
    public mutating func queueOutgoingFile(
        filePath: String,
        fileName: String,
        mimeType: String,
        sizeBytes: Int? = nil,
        now: Date = Date()
    ) throws -> RemoteIMMessage {
        let cleanFilePath = filePath.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanFilePath.isEmpty else { throw MasterChatStateError.blankMessage }
        guard let peerID = selectedPeerID, !peerID.isEmpty else {
            throw MasterChatStateError.noSelectedPeer
        }
        let fileAttachment = RemoteIMFileAttachment(
            localFilePath: cleanFilePath,
            fileName: fileName,
            mimeType: mimeType,
            sizeBytes: sizeBytes
        )
        let message = RemoteIMMessage(
            fromUserID: ownerUserID,
            toUserID: peerID,
            text: Self.fileDisplayText(
                fileName: fileAttachment.fileName,
                filePath: cleanFilePath
            ),
            fileAttachment: fileAttachment,
            direction: .outgoing,
            status: .pending,
            createdAt: now
        )
        appendMessage(message)
        return message
    }

    @discardableResult
    public mutating func receiveText(
        _ text: String,
        fromUserID: String,
        remoteID: String? = nil,
        approvalRequest: RemoteIMApprovalRequest? = nil,
        approvalDecision: RemoteIMApprovalDecision? = nil,
        now: Date = Date()
    ) -> RemoteIMMessage {
        if let existing = existingMessage(remoteID: remoteID) {
            return existing
        }
        let cleanFromUserID = fromUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        let message = RemoteIMMessage(
            remoteID: remoteID,
            fromUserID: cleanFromUserID,
            toUserID: ownerUserID,
            text: Self.incomingDisplayText(text),
            approvalRequest: approvalRequest,
            approvalDecision: approvalDecision,
            direction: .incoming,
            status: .received,
            createdAt: now
        )
        registerIncomingPeer(cleanFromUserID)
        appendMessage(message)
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
        if let existing = existingMessage(remoteID: remoteID) {
            return existing
        }
        let cleanFromUserID = fromUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        let voiceAttachment = RemoteIMVoiceAttachment(
            localFilePath: filePath.trimmingCharacters(in: .whitespacesAndNewlines),
            durationSeconds: durationSeconds,
            remoteID: remoteID
        )
        let message = RemoteIMMessage(
            remoteID: remoteID,
            fromUserID: cleanFromUserID,
            toUserID: ownerUserID,
            text: Self.voiceDisplayText(durationSeconds: voiceAttachment.durationSeconds),
            voiceAttachment: voiceAttachment,
            direction: .incoming,
            status: .received,
            createdAt: now
        )
        registerIncomingPeer(cleanFromUserID)
        appendMessage(message)
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
        if let existing = existingMessage(remoteID: remoteID) {
            return existing
        }
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
            remoteID: remoteID,
            fromUserID: cleanFromUserID,
            toUserID: ownerUserID,
            text: Self.imageDisplayText(filePath: cleanFilePath),
            imageAttachment: imageAttachment,
            direction: .incoming,
            status: .received,
            createdAt: now
        )
        registerIncomingPeer(cleanFromUserID)
        appendMessage(message)
        return message
    }

    @discardableResult
    public mutating func receiveVideo(
        filePath: String,
        coverFilePath: String?,
        durationSeconds: Int,
        width: Int,
        height: Int,
        sizeBytes: Int64,
        fromUserID: String,
        remoteID: String? = nil,
        now: Date = Date()
    ) -> RemoteIMMessage {
        let cleanFilePath = filePath.trimmingCharacters(in: .whitespacesAndNewlines)
        let cleanCoverPath = coverFilePath?.trimmingCharacters(in: .whitespacesAndNewlines)
        let attachment = RemoteIMVideoAttachment(
            localPath: cleanFilePath,
            coverPath: cleanCoverPath,
            durationSeconds: durationSeconds,
            width: width,
            height: height,
            sizeBytes: sizeBytes
        )

        if let existing = existingMessage(remoteID: remoteID),
           let index = messageIndexByID[existing.id]
        {
            let previousMessage = messages[index]
            let previousAttachment = previousMessage.videoAttachment
            messages[index].videoAttachment = RemoteIMVideoAttachment(
                localPath: cleanFilePath.isEmpty
                    ? previousAttachment?.localPath ?? ""
                    : cleanFilePath,
                coverPath: cleanCoverPath?.isEmpty == false
                    ? cleanCoverPath
                    : previousAttachment?.coverPath,
                durationSeconds: durationSeconds > 0
                    ? durationSeconds
                    : previousAttachment?.durationSeconds ?? 0,
                width: width > 0 ? width : previousAttachment?.width ?? 0,
                height: height > 0 ? height : previousAttachment?.height ?? 0,
                sizeBytes: sizeBytes > 0 ? sizeBytes : previousAttachment?.sizeBytes ?? 0
            )
            replaceCachedMessage(previous: previousMessage, updated: messages[index])
            return messages[index]
        }

        let cleanFromUserID = fromUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        let message = RemoteIMMessage(
            remoteID: remoteID,
            fromUserID: cleanFromUserID,
            toUserID: ownerUserID,
            text: Self.videoDisplayText(durationSeconds: attachment.durationSeconds),
            videoAttachment: attachment,
            direction: .incoming,
            status: .received,
            createdAt: now
        )
        registerIncomingPeer(cleanFromUserID)
        appendMessage(message)
        return message
    }

    @discardableResult
    public mutating func receiveFile(
        filePath: String,
        fromUserID: String,
        fileName: String,
        mimeType: String,
        remoteID: String? = nil,
        sizeBytes: Int? = nil,
        now: Date = Date()
    ) -> RemoteIMMessage {
        if let existing = existingMessage(remoteID: remoteID) {
            return existing
        }
        let cleanFromUserID = fromUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        let cleanFilePath = filePath.trimmingCharacters(in: .whitespacesAndNewlines)
        let fileAttachment = RemoteIMFileAttachment(
            localFilePath: cleanFilePath,
            fileName: fileName,
            mimeType: mimeType,
            remoteID: remoteID,
            sizeBytes: sizeBytes
        )
        let message = RemoteIMMessage(
            remoteID: remoteID,
            fromUserID: cleanFromUserID,
            toUserID: ownerUserID,
            text: Self.fileDisplayText(fileName: fileAttachment.fileName, filePath: cleanFilePath),
            fileAttachment: fileAttachment,
            direction: .incoming,
            status: .received,
            createdAt: now
        )
        registerIncomingPeer(cleanFromUserID)
        appendMessage(message)
        return message
    }

    public mutating func updateMessageStatus(
        id: UUID,
        status: RemoteIMMessageStatus
    ) throws {
        guard let index = messageIndexByID[id] else {
            throw MasterChatStateError.messageNotFound
        }
        let previousMessage = messages[index]
        messages[index].status = status
        replaceCachedMessage(previous: previousMessage, updated: messages[index])
    }

    public mutating func updateMessageDelivery(
        id: UUID,
        remoteID: String?,
        createdAt: Date?
    ) throws {
        guard let index = messageIndexByID[id] else {
            throw MasterChatStateError.messageNotFound
        }
        let previousMessage = messages[index]
        messages[index].status = .sent
        if let remoteID, !remoteID.isEmpty {
            messages[index].remoteID = remoteID
        }
        if let createdAt {
            messages[index].createdAt = createdAt
        }
        replaceCachedMessage(previous: previousMessage, updated: messages[index])
    }

    private func existingMessage(remoteID: String?) -> RemoteIMMessage? {
        guard let remoteID, !remoteID.isEmpty else { return nil }
        guard let messageID = messageIDByRemoteID[remoteID],
              let index = messageIndexByID[messageID]
        else {
            return nil
        }
        return messages[index]
    }

    private mutating func rebuildMessageIndexes() {
        conversationMessagesByPeerID.removeAll(keepingCapacity: true)
        messageIndexByID.removeAll(keepingCapacity: true)
        messageIDByRemoteID.removeAll(keepingCapacity: true)

        for (index, message) in messages.enumerated() {
            messageIndexByID[message.id] = index
            if let remoteID = message.remoteID, !remoteID.isEmpty,
               messageIDByRemoteID[remoteID] == nil
            {
                messageIDByRemoteID[remoteID] = message.id
            }
            if let peerID = conversationPeerID(for: message) {
                conversationMessagesByPeerID[peerID, default: []].append(message)
            }
        }

        for peerID in Array(conversationMessagesByPeerID.keys) {
            conversationMessagesByPeerID[peerID]?.sort(by: Self.messageIsEarlier)
        }
    }

    private mutating func appendMessage(_ message: RemoteIMMessage) {
        guard let peerID = conversationPeerID(for: message) else { return }
        messageIndexByID[message.id] = messages.count
        messages.append(message)
        if let remoteID = message.remoteID, !remoteID.isEmpty,
           messageIDByRemoteID[remoteID] == nil
        {
            messageIDByRemoteID[remoteID] = message.id
        }
        insertMessageInConversation(message, peerID: peerID)
    }

    private mutating func registerIncomingPeer(_ userID: String) {
        guard RemoteIMPeerPolicy.isValidPeer(
            userID: userID,
            ownerUserID: ownerUserID
        ) else { return }
        if !contacts.contains(where: { $0.userID == userID }) {
            contacts.append(
                RemoteIMContact(
                    userID: userID,
                    displayName: userID,
                    relation: .friend
                )
            )
        }
        if selectedPeerID == nil {
            selectedPeerID = userID
        }
    }

    private mutating func replaceCachedMessage(
        previous: RemoteIMMessage,
        updated: RemoteIMMessage
    ) {
        if let previousRemoteID = previous.remoteID,
           messageIDByRemoteID[previousRemoteID] == previous.id,
           previousRemoteID != updated.remoteID
        {
            messageIDByRemoteID[previousRemoteID] = nil
        }
        if let updatedRemoteID = updated.remoteID, !updatedRemoteID.isEmpty,
           messageIDByRemoteID[updatedRemoteID] == nil ||
            messageIDByRemoteID[updatedRemoteID] == previous.id
        {
            messageIDByRemoteID[updatedRemoteID] = updated.id
        }

        if let previousPeerID = conversationPeerID(for: previous) {
            conversationMessagesByPeerID[previousPeerID]?.removeAll { $0.id == previous.id }
            if conversationMessagesByPeerID[previousPeerID]?.isEmpty == true {
                conversationMessagesByPeerID[previousPeerID] = nil
            }
        }
        if let updatedPeerID = conversationPeerID(for: updated) {
            insertMessageInConversation(updated, peerID: updatedPeerID)
        }
    }

    private mutating func insertMessageInConversation(
        _ message: RemoteIMMessage,
        peerID: String
    ) {
        if let lastMessage = conversationMessagesByPeerID[peerID]?.last,
           Self.messageIsEarlier(lastMessage, message)
        {
            conversationMessagesByPeerID[peerID]?.append(message)
            return
        }
        if conversationMessagesByPeerID[peerID] == nil {
            conversationMessagesByPeerID[peerID] = [message]
            return
        }

        var conversation = conversationMessagesByPeerID[peerID] ?? []
        var lowerBound = 0
        var upperBound = conversation.count
        while lowerBound < upperBound {
            let midpoint = (lowerBound + upperBound) / 2
            if Self.messageIsEarlier(conversation[midpoint], message) {
                lowerBound = midpoint + 1
            } else {
                upperBound = midpoint
            }
        }
        conversation.insert(message, at: lowerBound)
        conversationMessagesByPeerID[peerID] = conversation
    }

    private func conversationPeerID(for message: RemoteIMMessage) -> String? {
        let peerUserID: String
        if message.fromUserID == ownerUserID {
            peerUserID = message.toUserID
        } else if message.toUserID == ownerUserID {
            peerUserID = message.fromUserID
        } else {
            return nil
        }
        guard RemoteIMPeerPolicy.isValidPeer(
            userID: peerUserID,
            ownerUserID: ownerUserID
        ) else { return nil }
        return peerUserID
    }

    private static func messageIsEarlier(
        _ left: RemoteIMMessage,
        _ right: RemoteIMMessage
    ) -> Bool {
        if left.createdAt == right.createdAt {
            return left.id.uuidString < right.id.uuidString
        }
        return left.createdAt < right.createdAt
    }
}
