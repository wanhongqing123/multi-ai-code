import Foundation
import MaiChatCore
import SQLite3

enum RemoteIMMediaStorage {
    enum Category: String {
        case incomingImages = "Incoming/Images"
        case outgoingImages = "Outgoing/Images"
        case incomingVideos = "Incoming/Videos"
        case outgoingVideos = "Outgoing/Videos"
        case incomingVideoCovers = "Incoming/VideoCovers"
        case outgoingVideoCovers = "Outgoing/VideoCovers"
        case incomingVoices = "Incoming/Voices"
        case outgoingVoices = "Outgoing/Voices"
        case incomingFiles = "Incoming/Files"
        case outgoingFiles = "Outgoing/Files"
    }

    static func fileURL(
        category: Category,
        stem: String,
        pathExtension: String
    ) -> URL {
        let cleanStem = sanitizedFileName(stem).trimmingCharacters(in: CharacterSet(charactersIn: "."))
        let cleanExtension = pathExtension
            .trimmingCharacters(in: .whitespacesAndNewlines)
            .trimmingCharacters(in: CharacterSet(charactersIn: "."))
        let fileName = cleanExtension.isEmpty
            ? (cleanStem.isEmpty ? UUID().uuidString : cleanStem)
            : "\(cleanStem.isEmpty ? UUID().uuidString : cleanStem).\(cleanExtension)"
        return fileURL(category: category, fileName: fileName)
    }

    static func fileURL(category: Category, fileName: String) -> URL {
        let directory = directoryURL(category: category)
        return directory.appendingPathComponent(sanitizedFileName(fileName))
    }

    static func persistentReference(for runtimePath: String, category: Category) -> String {
        let cleanPath = runtimePath.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanPath.isEmpty else { return "" }

        let sourceURL = URL(fileURLWithPath: cleanPath)
        let mediaRootPath = mediaRootURL().standardizedFileURL.path
        let standardizedPath = sourceURL.standardizedFileURL.path
        if standardizedPath.hasPrefix(mediaRootPath + "/") {
            let relativePath = String(standardizedPath.dropFirst(mediaRootPath.count + 1))
            return relativePath.hasPrefix(category.rawValue + "/") ? relativePath : ""
        }
        return ""
    }

    static func resolvedPath(from storedPath: String, category: Category) -> String {
        let cleanPath = storedPath.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanPath.isEmpty else { return "" }
        guard !cleanPath.hasPrefix("/"), cleanPath.hasPrefix(category.rawValue + "/") else {
            return ""
        }
        let resolvedURL = mediaRootURL().appendingPathComponent(cleanPath).standardizedFileURL
        let categoryRoot = directoryURL(category: category).standardizedFileURL.path
        guard resolvedURL.path.hasPrefix(categoryRoot + "/") else { return "" }
        return resolvedURL.path
    }

    static func partialDownloadURL(for finalURL: URL) -> URL {
        finalURL.appendingPathExtension("part")
    }

    private static func mediaRootURL() -> URL {
        let appSupport = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        let root = appSupport
            .appendingPathComponent("MaiChat", isDirectory: true)
            .appendingPathComponent("RemoteIMMedia", isDirectory: true)
        try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        var excludedRoot = root
        var values = URLResourceValues()
        values.isExcludedFromBackup = true
        try? excludedRoot.setResourceValues(values)
        return root
    }

    private static func directoryURL(category: Category) -> URL {
        let directory = category.rawValue
            .split(separator: "/")
            .reduce(mediaRootURL()) { partialURL, component in
                partialURL.appendingPathComponent(String(component), isDirectory: true)
            }
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return directory
    }

    private static func sanitizedFileName(_ value: String) -> String {
        let clean = value.trimmingCharacters(in: .whitespacesAndNewlines)
        let fallback = clean.isEmpty ? UUID().uuidString : clean
        return fallback
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: ":", with: "_")
    }
}

struct LocalChatHistoryAccount: Hashable, Sendable {
    let sdkAppID: Int?
    let ownerUserID: String
}

struct LocalChatHistorySaveResult: Sendable, Equatable {
    let messageCount: Int
    let upsertedCount: Int
    let removedCount: Int
    let durationMilliseconds: Int
}

struct LocalChatHistoryCursor: Sendable, Equatable {
    let createdAt: Date
    let messageID: UUID
}

struct LocalChatHistoryPage: Sendable, Equatable {
    let messages: [RemoteIMMessage]
    let hasEarlierMessages: Bool
}

enum LocalChatHistoryMutationOperation: Sendable {
    case upsert([RemoteIMMessage])
    case removeConversation(peerUserID: String)
}

struct LocalChatHistoryMutation: Sendable {
    let account: LocalChatHistoryAccount
    let operation: LocalChatHistoryMutationOperation

    static func upsert(
        _ messages: [RemoteIMMessage],
        sdkAppID: Int?,
        ownerUserID: String
    ) -> Self {
        Self(
            account: LocalChatHistoryAccount(
                sdkAppID: sdkAppID,
                ownerUserID: ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
            ),
            operation: .upsert(messages)
        )
    }

    static func removeConversation(
        peerUserID: String,
        sdkAppID: Int?,
        ownerUserID: String
    ) -> Self {
        Self(
            account: LocalChatHistoryAccount(
                sdkAppID: sdkAppID,
                ownerUserID: ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
            ),
            operation: .removeConversation(
                peerUserID: peerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
            )
        )
    }
}

final class LocalChatHistoryStore {
    private struct StoredChatHistory: Codable {
        let schemaVersion: Int
        let sdkAppID: Int?
        let ownerUserID: String
        let messages: [RemoteIMMessage]
    }

    private enum StoreError: LocalizedError {
        case database(String)

        var errorDescription: String? {
            switch self {
            case .database(let message):
                return "保存本地消息失败：\(message)"
            }
        }
    }

    private let baseDirectoryURL: URL
    private let fileManager: FileManager
    private let encoder: JSONEncoder
    private let decoder: JSONDecoder

    init(
        baseDirectoryURL: URL? = nil,
        fileManager: FileManager = .default,
        encoder: JSONEncoder = JSONEncoder(),
        decoder: JSONDecoder = JSONDecoder()
    ) {
        self.fileManager = fileManager
        self.encoder = encoder
        self.decoder = decoder
        self.baseDirectoryURL = baseDirectoryURL ?? Self.defaultBaseDirectoryURL(fileManager: fileManager)
    }

    func makePersistence() -> LocalChatHistoryPersistence {
        LocalChatHistoryPersistence(baseDirectoryURL: baseDirectoryURL)
    }

    /// Loads one bounded conversation window using a stable cursor.
    func loadConversationPage(
        sdkAppID: Int?,
        ownerUserID: String,
        peerUserID: String,
        before cursor: LocalChatHistoryCursor?,
        limit: Int
    ) throws -> LocalChatHistoryPage {
        let cleanOwnerUserID = ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        let cleanPeerUserID = peerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanOwnerUserID.isEmpty, !cleanPeerUserID.isEmpty, limit > 0 else {
            return LocalChatHistoryPage(messages: [], hasEarlierMessages: false)
        }
        try migrateLegacyHistoryIfNeeded(
            sdkAppID: sdkAppID,
            ownerUserID: cleanOwnerUserID
        )

        return try withDatabase { database in
            let cursorClause = cursor == nil
                ? ""
                : "AND (created_at < ? OR (created_at = ? AND id < ?))"
            let statement = try prepare(
                database,
                sql: """
                SELECT id, remote_id, from_user, to_user, text, direction, status, created_at,
                       voice_attachment, image_attachment, file_attachment, video_attachment,
                       approval_request, approval_decision
                FROM messages
                WHERE sdk_app_id = ? AND owner_user_id = ?
                  AND peer_user_id = ?
                  \(cursorClause)
                ORDER BY created_at DESC, id DESC
                LIMIT ?
                """
            )
            defer { sqlite3_finalize(statement) }
            try bindText(accountKey(for: sdkAppID), to: statement, at: 1, database: database)
            try bindText(cleanOwnerUserID, to: statement, at: 2, database: database)
            try bindText(cleanPeerUserID, to: statement, at: 3, database: database)

            var limitIndex: Int32 = 4
            if let cursor {
                sqlite3_bind_double(statement, 4, cursor.createdAt.timeIntervalSince1970)
                sqlite3_bind_double(statement, 5, cursor.createdAt.timeIntervalSince1970)
                try bindText(cursor.messageID.uuidString, to: statement, at: 6, database: database)
                limitIndex = 7
            }
            sqlite3_bind_int64(statement, limitIndex, Int64(limit + 1))

            var descendingMessages: [RemoteIMMessage] = []
            descendingMessages.reserveCapacity(limit + 1)
            while true {
                switch sqlite3_step(statement) {
                case SQLITE_ROW:
                    if let message = decodeMessage(from: statement) {
                        descendingMessages.append(message)
                    }
                case SQLITE_DONE:
                    let hasEarlierMessages = descendingMessages.count > limit
                    let messages = Array(descendingMessages.prefix(limit).reversed())
                    return LocalChatHistoryPage(
                        messages: messages,
                        hasEarlierMessages: hasEarlierMessages
                    )
                default:
                    throw databaseError(database)
                }
            }
        }
    }

    /// Returns at most one newest message per peer for the conversation list.
    func loadConversationSummaries(
        sdkAppID: Int?,
        ownerUserID: String,
        peerUserIDs: [String]
    ) throws -> [RemoteIMMessage] {
        let cleanOwnerUserID = ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        let cleanPeerUserIDs = Array(Set(peerUserIDs.map {
            $0.trimmingCharacters(in: .whitespacesAndNewlines)
        })).filter { !$0.isEmpty }
        guard !cleanOwnerUserID.isEmpty else { return [] }
        try migrateLegacyHistoryIfNeeded(
            sdkAppID: sdkAppID,
            ownerUserID: cleanOwnerUserID
        )
        guard !cleanPeerUserIDs.isEmpty else { return [] }

        return try withDatabase { database in
            let statement = try prepare(
                database,
                sql: """
                SELECT id, remote_id, from_user, to_user, text, direction, status, created_at,
                       voice_attachment, image_attachment, file_attachment, video_attachment,
                       approval_request, approval_decision
                FROM messages
                WHERE sdk_app_id = ? AND owner_user_id = ? AND peer_user_id = ?
                ORDER BY created_at DESC, id DESC
                LIMIT 1
                """
            )
            defer { sqlite3_finalize(statement) }
            var messages: [RemoteIMMessage] = []
            messages.reserveCapacity(cleanPeerUserIDs.count)
            for peerUserID in cleanPeerUserIDs {
                sqlite3_reset(statement)
                sqlite3_clear_bindings(statement)
                try bindText(accountKey(for: sdkAppID), to: statement, at: 1, database: database)
                try bindText(cleanOwnerUserID, to: statement, at: 2, database: database)
                try bindText(peerUserID, to: statement, at: 3, database: database)
                switch sqlite3_step(statement) {
                case SQLITE_ROW:
                    if let message = decodeMessage(from: statement) {
                        messages.append(message)
                    }
                case SQLITE_DONE:
                    continue
                default:
                    throw databaseError(database)
                }
            }
            return messages.sorted {
                if $0.createdAt != $1.createdAt { return $0.createdAt > $1.createdAt }
                return $0.id.uuidString > $1.id.uuidString
            }
        }
    }

    func containsMessage(
        remoteID: String,
        sdkAppID: Int?,
        ownerUserID: String
    ) throws -> Bool {
        let cleanRemoteID = remoteID.trimmingCharacters(in: .whitespacesAndNewlines)
        let cleanOwnerUserID = ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanRemoteID.isEmpty, !cleanOwnerUserID.isEmpty else { return false }
        try migrateLegacyHistoryIfNeeded(
            sdkAppID: sdkAppID,
            ownerUserID: cleanOwnerUserID
        )
        return try withDatabase { database in
            let statement = try prepare(
                database,
                sql: """
                SELECT 1 FROM messages
                WHERE sdk_app_id = ? AND owner_user_id = ? AND remote_id = ?
                LIMIT 1
                """
            )
            defer { sqlite3_finalize(statement) }
            try bindText(accountKey(for: sdkAppID), to: statement, at: 1, database: database)
            try bindText(cleanOwnerUserID, to: statement, at: 2, database: database)
            try bindText(cleanRemoteID, to: statement, at: 3, database: database)
            switch sqlite3_step(statement) {
            case SQLITE_ROW:
                return true
            case SQLITE_DONE:
                return false
            default:
                throw databaseError(database)
            }
        }
    }

    func applyMutations(
        _ mutations: [LocalChatHistoryMutation]
    ) throws -> LocalChatHistorySaveResult {
        let startedAt = ProcessInfo.processInfo.systemUptime
        let validMutations = mutations.filter { !$0.account.ownerUserID.isEmpty }
        guard !validMutations.isEmpty else {
            return LocalChatHistorySaveResult(
                messageCount: 0,
                upsertedCount: 0,
                removedCount: 0,
                durationMilliseconds: 0
            )
        }

        var upsertedCount = 0
        var removedCount = 0
        let accounts = Set(validMutations.map(\.account))
        for account in accounts {
            try migrateLegacyHistoryIfNeeded(
                sdkAppID: account.sdkAppID,
                ownerUserID: account.ownerUserID
            )
        }
        try withDatabase { database in
            try execute(database, sql: "BEGIN IMMEDIATE TRANSACTION")
            do {
                for mutation in validMutations {
                    let ownerUserID = mutation.account.ownerUserID
                    switch mutation.operation {
                    case .upsert(let messages):
                        let validMessages = Self.deduplicatedMessages(messages).filter {
                            $0.fromUserID == ownerUserID || $0.toUserID == ownerUserID
                        }
                        try insertMessages(
                            validMessages,
                            into: database,
                            sdkAppID: mutation.account.sdkAppID,
                            ownerUserID: ownerUserID
                        )
                        upsertedCount += validMessages.count
                    case .removeConversation(let peerUserID):
                        removedCount += try removeConversationMessages(
                            peerUserID: peerUserID,
                            from: database,
                            sdkAppID: mutation.account.sdkAppID,
                            ownerUserID: ownerUserID
                        )
                    }
                }
                try execute(database, sql: "COMMIT")
            } catch {
                try? execute(database, sql: "ROLLBACK")
                throw error
            }
        }

        for account in accounts {
            try? fileManager.removeItem(
                at: legacyFileURL(
                    sdkAppID: account.sdkAppID,
                    ownerUserID: account.ownerUserID
                )
            )
        }
        return LocalChatHistorySaveResult(
            messageCount: upsertedCount,
            upsertedCount: upsertedCount,
            removedCount: removedCount,
            durationMilliseconds: max(
                0,
                Int(((ProcessInfo.processInfo.systemUptime - startedAt) * 1_000).rounded())
            )
        )
    }

    private func removeConversationMessages(
        peerUserID: String,
        from database: OpaquePointer,
        sdkAppID: Int?,
        ownerUserID: String
    ) throws -> Int {
        let cleanPeerUserID = peerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanPeerUserID.isEmpty else { return 0 }
        let statement = try prepare(
            database,
            sql: """
            DELETE FROM messages
            WHERE sdk_app_id = ? AND owner_user_id = ?
              AND peer_user_id = ?
            """
        )
        defer { sqlite3_finalize(statement) }
        try bindText(accountKey(for: sdkAppID), to: statement, at: 1, database: database)
        try bindText(ownerUserID, to: statement, at: 2, database: database)
        try bindText(cleanPeerUserID, to: statement, at: 3, database: database)
        guard sqlite3_step(statement) == SQLITE_DONE else {
            throw databaseError(database)
        }
        return Int(sqlite3_changes(database))
    }

    private func insertMessages(
        _ messages: [RemoteIMMessage],
        into database: OpaquePointer,
        sdkAppID: Int?,
        ownerUserID: String,
        updateExisting: Bool = true
    ) throws {
        let conflictClause = updateExisting
            ? """
              DO UPDATE SET
                  remote_id = excluded.remote_id,
                  from_user = excluded.from_user,
                  to_user = excluded.to_user,
                  text = excluded.text,
                  direction = excluded.direction,
                  status = excluded.status,
                  created_at = excluded.created_at,
                  voice_attachment = excluded.voice_attachment,
                  image_attachment = excluded.image_attachment,
                  file_attachment = excluded.file_attachment,
                  video_attachment = excluded.video_attachment,
                  approval_request = excluded.approval_request,
                  approval_decision = excluded.approval_decision,
                  peer_user_id = excluded.peer_user_id
              """
            : "DO NOTHING"
        let statement = try prepare(
            database,
            sql: """
            INSERT INTO messages(
                sdk_app_id, owner_user_id, id, remote_id, from_user, to_user, text,
                direction, status, created_at,
                voice_attachment, image_attachment, file_attachment, video_attachment,
                approval_request, approval_decision, peer_user_id
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(sdk_app_id, owner_user_id, id) \(conflictClause)
            """
        )
        defer { sqlite3_finalize(statement) }

        for message in messages {
            sqlite3_reset(statement)
            sqlite3_clear_bindings(statement)
            let incoming = message.direction == .incoming
            let voiceCategory: RemoteIMMediaStorage.Category = incoming ? .incomingVoices : .outgoingVoices
            let imageCategory: RemoteIMMediaStorage.Category = incoming ? .incomingImages : .outgoingImages
            let fileCategory: RemoteIMMediaStorage.Category = incoming ? .incomingFiles : .outgoingFiles
            let videoCategory: RemoteIMMediaStorage.Category = incoming ? .incomingVideos : .outgoingVideos
            let coverCategory: RemoteIMMediaStorage.Category = incoming ? .incomingVideoCovers : .outgoingVideoCovers
            let persistedVoiceAttachment = message.voiceAttachment.map {
                RemoteIMVoiceAttachment(
                    localFilePath: RemoteIMMediaStorage.persistentReference(
                        for: $0.localFilePath,
                        category: voiceCategory
                    ),
                    durationSeconds: $0.durationSeconds,
                    remoteID: $0.remoteID
                )
            }
            let persistedImageAttachment = message.imageAttachment.map {
                RemoteIMImageAttachment(
                    localFilePath: RemoteIMMediaStorage.persistentReference(
                        for: $0.localFilePath,
                        category: imageCategory
                    ),
                    remoteID: $0.remoteID,
                    width: $0.width,
                    height: $0.height,
                    sizeBytes: $0.sizeBytes
                )
            }
            let persistedFileAttachment = message.fileAttachment.map {
                RemoteIMFileAttachment(
                    localFilePath: RemoteIMMediaStorage.persistentReference(
                        for: $0.localFilePath,
                        category: fileCategory
                    ),
                    fileName: $0.fileName,
                    mimeType: $0.mimeType,
                    remoteID: $0.remoteID,
                    sizeBytes: $0.sizeBytes
                )
            }
            let persistedVideoAttachment = message.videoAttachment.map {
                RemoteIMVideoAttachment(
                    localPath: RemoteIMMediaStorage.persistentReference(
                        for: $0.localPath,
                        category: videoCategory
                    ),
                    coverPath: $0.coverPath.map {
                        RemoteIMMediaStorage.persistentReference(for: $0, category: coverCategory)
                    },
                    durationSeconds: $0.durationSeconds,
                    width: $0.width,
                    height: $0.height,
                    sizeBytes: $0.sizeBytes
                )
            }
            try bindText(accountKey(for: sdkAppID), to: statement, at: 1, database: database)
            try bindText(ownerUserID, to: statement, at: 2, database: database)
            try bindText(message.id.uuidString, to: statement, at: 3, database: database)
            try bindOptionalText(message.remoteID, to: statement, at: 4, database: database)
            try bindText(message.fromUserID, to: statement, at: 5, database: database)
            try bindText(message.toUserID, to: statement, at: 6, database: database)
            try bindText(message.text, to: statement, at: 7, database: database)
            try bindText(message.direction.rawValue, to: statement, at: 8, database: database)
            try bindText(message.status.rawValue, to: statement, at: 9, database: database)
            sqlite3_bind_double(statement, 10, message.createdAt.timeIntervalSince1970)
            try bindOptionalJSON(persistedVoiceAttachment, to: statement, at: 11, database: database)
            try bindOptionalJSON(persistedImageAttachment, to: statement, at: 12, database: database)
            try bindOptionalJSON(persistedFileAttachment, to: statement, at: 13, database: database)
            try bindOptionalJSON(persistedVideoAttachment, to: statement, at: 14, database: database)
            try bindOptionalJSON(message.approvalRequest, to: statement, at: 15, database: database)
            try bindOptionalJSON(message.approvalDecision, to: statement, at: 16, database: database)
            let peerUserID = message.fromUserID == ownerUserID
                ? message.toUserID
                : message.fromUserID
            try bindText(peerUserID, to: statement, at: 17, database: database)
            guard sqlite3_step(statement) == SQLITE_DONE else {
                throw databaseError(database)
            }
        }
    }

    private func decodeMessage(from statement: OpaquePointer) -> RemoteIMMessage? {
        guard let id = UUID(uuidString: textColumn(statement, at: 0)),
              let direction = RemoteIMMessageDirection(rawValue: textColumn(statement, at: 5)),
              let status = RemoteIMMessageStatus(rawValue: textColumn(statement, at: 6))
        else {
            return nil
        }

        let incoming = direction == .incoming
        let voiceCategory: RemoteIMMediaStorage.Category = incoming ? .incomingVoices : .outgoingVoices
        let imageCategory: RemoteIMMediaStorage.Category = incoming ? .incomingImages : .outgoingImages
        let fileCategory: RemoteIMMediaStorage.Category = incoming ? .incomingFiles : .outgoingFiles
        let videoCategory: RemoteIMMediaStorage.Category = incoming ? .incomingVideos : .outgoingVideos
        let coverCategory: RemoteIMMediaStorage.Category = incoming ? .incomingVideoCovers : .outgoingVideoCovers

        let voiceAttachment = decodeOptionalJSON(
            RemoteIMVoiceAttachment.self,
            from: optionalTextColumn(statement, at: 8)
        ).map {
            RemoteIMVoiceAttachment(
                localFilePath: RemoteIMMediaStorage.resolvedPath(
                    from: $0.localFilePath,
                    category: voiceCategory
                ),
                durationSeconds: $0.durationSeconds,
                remoteID: $0.remoteID
            )
        }
        let imageAttachment = decodeOptionalJSON(
            RemoteIMImageAttachment.self,
            from: optionalTextColumn(statement, at: 9)
        ).map {
            RemoteIMImageAttachment(
                localFilePath: RemoteIMMediaStorage.resolvedPath(
                    from: $0.localFilePath,
                    category: imageCategory
                ),
                remoteID: $0.remoteID,
                width: $0.width,
                height: $0.height,
                sizeBytes: $0.sizeBytes
            )
        }
        let fileAttachment = decodeOptionalJSON(
            RemoteIMFileAttachment.self,
            from: optionalTextColumn(statement, at: 10)
        ).map {
            RemoteIMFileAttachment(
                localFilePath: RemoteIMMediaStorage.resolvedPath(
                    from: $0.localFilePath,
                    category: fileCategory
                ),
                fileName: $0.fileName,
                mimeType: $0.mimeType,
                remoteID: $0.remoteID,
                sizeBytes: $0.sizeBytes
            )
        }
        let videoAttachment = decodeOptionalJSON(
            RemoteIMVideoAttachment.self,
            from: optionalTextColumn(statement, at: 11)
        ).map {
            RemoteIMVideoAttachment(
                localPath: RemoteIMMediaStorage.resolvedPath(from: $0.localPath, category: videoCategory),
                coverPath: $0.coverPath.map {
                    RemoteIMMediaStorage.resolvedPath(from: $0, category: coverCategory)
                },
                durationSeconds: $0.durationSeconds,
                width: $0.width,
                height: $0.height,
                sizeBytes: $0.sizeBytes
            )
        }
        let approvalRequest = decodeOptionalJSON(
            RemoteIMApprovalRequest.self,
            from: optionalTextColumn(statement, at: 12)
        )
        let approvalDecision = decodeOptionalJSON(
            RemoteIMApprovalDecision.self,
            from: optionalTextColumn(statement, at: 13)
        )

        return RemoteIMMessage(
            id: id,
            remoteID: optionalTextColumn(statement, at: 1),
            fromUserID: textColumn(statement, at: 2),
            toUserID: textColumn(statement, at: 3),
            text: textColumn(statement, at: 4),
            voiceAttachment: voiceAttachment,
            imageAttachment: imageAttachment,
            fileAttachment: fileAttachment,
            videoAttachment: videoAttachment,
            approvalRequest: approvalRequest,
            approvalDecision: approvalDecision,
            direction: direction,
            status: status,
            createdAt: Date(timeIntervalSince1970: sqlite3_column_double(statement, 7))
        )
    }

    private func withDatabase<T>(_ operation: (OpaquePointer) throws -> T) throws -> T {
        try fileManager.createDirectory(
            at: baseDirectoryURL,
            withIntermediateDirectories: true
        )

        var database: OpaquePointer?
        let openResult = sqlite3_open_v2(
            databaseURL.path,
            &database,
            SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
            nil
        )
        guard openResult == SQLITE_OK, let database else {
            let message = database.map(databaseErrorMessage) ?? "无法打开数据库"
            if let database {
                sqlite3_close(database)
            }
            throw StoreError.database(message)
        }
        defer { sqlite3_close(database) }

        try migrate(database)
        return try operation(database)
    }

    private func migrate(_ database: OpaquePointer) throws {
        let previousSchemaVersion = try databaseUserVersion(database)
        try execute(database, sql: "PRAGMA journal_mode = WAL")
        try execute(database, sql: "PRAGMA synchronous = NORMAL")
        try execute(
            database,
            sql: """
            CREATE TABLE IF NOT EXISTS messages (
                sdk_app_id TEXT NOT NULL,
                owner_user_id TEXT NOT NULL,
                id TEXT NOT NULL,
                remote_id TEXT,
                from_user TEXT NOT NULL,
                to_user TEXT NOT NULL,
                text TEXT NOT NULL DEFAULT '',
                direction TEXT NOT NULL,
                status TEXT NOT NULL,
                created_at REAL NOT NULL,
                voice_attachment TEXT,
                image_attachment TEXT,
                file_attachment TEXT,
                video_attachment TEXT,
                approval_request TEXT,
                approval_decision TEXT,
                peer_user_id TEXT,
                PRIMARY KEY (sdk_app_id, owner_user_id, id)
            )
            """
        )
        if try !columnExists("remote_id", in: "messages", database: database) {
            try execute(database, sql: "ALTER TABLE messages ADD COLUMN remote_id TEXT")
        }
        if try !columnExists("peer_user_id", in: "messages", database: database) {
            try execute(database, sql: "ALTER TABLE messages ADD COLUMN peer_user_id TEXT")
        }
        if try !columnExists("video_attachment", in: "messages", database: database) {
            try execute(database, sql: "ALTER TABLE messages ADD COLUMN video_attachment TEXT")
        }
        if try !columnExists("approval_request", in: "messages", database: database) {
            try execute(database, sql: "ALTER TABLE messages ADD COLUMN approval_request TEXT")
        }
        if try !columnExists("approval_decision", in: "messages", database: database) {
            try execute(database, sql: "ALTER TABLE messages ADD COLUMN approval_decision TEXT")
        }
        if previousSchemaVersion < 3 {
            try execute(
                database,
                sql: """
                UPDATE messages
                SET peer_user_id = CASE
                    WHEN from_user = owner_user_id THEN to_user
                    ELSE from_user
                END
                WHERE peer_user_id IS NULL OR peer_user_id = ''
                """
            )
        }
        try execute(
            database,
            sql: """
            CREATE INDEX IF NOT EXISTS idx_messages_account_time
            ON messages(sdk_app_id, owner_user_id, created_at, id)
            """
        )
        try execute(
            database,
            sql: """
            CREATE INDEX IF NOT EXISTS idx_messages_account_remote_id
            ON messages(sdk_app_id, owner_user_id, remote_id)
            """
        )
        try execute(
            database,
            sql: """
            CREATE INDEX IF NOT EXISTS idx_messages_account_peer_time
            ON messages(sdk_app_id, owner_user_id, peer_user_id, created_at DESC, id DESC)
            """
        )
        if previousSchemaVersion < 6 {
            try execute(database, sql: "PRAGMA user_version = 6")
        }
    }

    private func databaseUserVersion(_ database: OpaquePointer) throws -> Int {
        let statement = try prepare(database, sql: "PRAGMA user_version")
        defer { sqlite3_finalize(statement) }
        guard sqlite3_step(statement) == SQLITE_ROW else {
            throw databaseError(database)
        }
        return Int(sqlite3_column_int(statement, 0))
    }

    private func execute(_ database: OpaquePointer, sql: String) throws {
        var errorPointer: UnsafeMutablePointer<CChar>?
        let result = sqlite3_exec(database, sql, nil, nil, &errorPointer)
        guard result == SQLITE_OK else {
            let message = errorPointer.map { String(cString: $0) } ?? databaseErrorMessage(database)
            sqlite3_free(errorPointer)
            throw StoreError.database(message)
        }
    }

    private func prepare(_ database: OpaquePointer, sql: String) throws -> OpaquePointer {
        var statement: OpaquePointer?
        guard sqlite3_prepare_v2(database, sql, -1, &statement, nil) == SQLITE_OK,
              let statement
        else {
            throw databaseError(database)
        }
        return statement
    }

    private func bindText(
        _ value: String,
        to statement: OpaquePointer,
        at index: Int32,
        database: OpaquePointer
    ) throws {
        let result = value.withCString { pointer in
            sqlite3_bind_text(
                statement,
                index,
                pointer,
                -1,
                unsafeBitCast(-1, to: sqlite3_destructor_type.self)
            )
        }
        guard result == SQLITE_OK else {
            throw databaseError(database)
        }
    }

    private func bindOptionalText(
        _ value: String?,
        to statement: OpaquePointer,
        at index: Int32,
        database: OpaquePointer
    ) throws {
        guard let value, !value.isEmpty else {
            guard sqlite3_bind_null(statement, index) == SQLITE_OK else {
                throw databaseError(database)
            }
            return
        }
        try bindText(value, to: statement, at: index, database: database)
    }

    private func columnExists(
        _ columnName: String,
        in tableName: String,
        database: OpaquePointer
    ) throws -> Bool {
        let statement = try prepare(database, sql: "PRAGMA table_info(\(tableName))")
        defer { sqlite3_finalize(statement) }
        while sqlite3_step(statement) == SQLITE_ROW {
            if textColumn(statement, at: 1) == columnName {
                return true
            }
        }
        return false
    }

    private func bindOptionalJSON<Value: Encodable>(
        _ value: Value?,
        to statement: OpaquePointer,
        at index: Int32,
        database: OpaquePointer
    ) throws {
        guard let value else {
            guard sqlite3_bind_null(statement, index) == SQLITE_OK else {
                throw databaseError(database)
            }
            return
        }
        let data = try encoder.encode(value)
        guard let json = String(data: data, encoding: .utf8) else {
            throw StoreError.database("附件数据编码失败")
        }
        try bindText(json, to: statement, at: index, database: database)
    }

    private func decodeOptionalJSON<Value: Decodable>(
        _ type: Value.Type,
        from json: String?
    ) -> Value? {
        guard let json, let data = json.data(using: .utf8) else { return nil }
        return try? decoder.decode(type, from: data)
    }

    private func textColumn(_ statement: OpaquePointer, at index: Int32) -> String {
        guard let value = sqlite3_column_text(statement, index) else { return "" }
        return String(cString: value)
    }

    private func optionalTextColumn(_ statement: OpaquePointer, at index: Int32) -> String? {
        guard sqlite3_column_type(statement, index) != SQLITE_NULL else { return nil }
        return textColumn(statement, at: index)
    }

    private func databaseError(_ database: OpaquePointer) -> StoreError {
        StoreError.database(databaseErrorMessage(database))
    }

    private func databaseErrorMessage(_ database: OpaquePointer) -> String {
        guard let message = sqlite3_errmsg(database) else { return "未知数据库错误" }
        return String(cString: message)
    }

    private func loadLegacyHistory(
        sdkAppID: Int?,
        ownerUserID: String
    ) -> StoredChatHistory? {
        let historyURL = legacyFileURL(sdkAppID: sdkAppID, ownerUserID: ownerUserID)
        guard let data = try? Data(contentsOf: historyURL),
              let history = try? decoder.decode(StoredChatHistory.self, from: data),
              history.ownerUserID == ownerUserID,
              history.sdkAppID == sdkAppID
        else {
            return nil
        }
        return history
    }

    private func migrateLegacyHistoryIfNeeded(
        sdkAppID: Int?,
        ownerUserID: String
    ) throws {
        guard let legacyHistory = loadLegacyHistory(
            sdkAppID: sdkAppID,
            ownerUserID: ownerUserID
        ) else { return }
        try withDatabase { database in
            try execute(database, sql: "BEGIN IMMEDIATE TRANSACTION")
            do {
                try insertMessages(
                    Self.deduplicatedMessages(legacyHistory.messages),
                    into: database,
                    sdkAppID: sdkAppID,
                    ownerUserID: ownerUserID,
                    updateExisting: false
                )
                try execute(database, sql: "COMMIT")
            } catch {
                try? execute(database, sql: "ROLLBACK")
                throw error
            }
        }
        try fileManager.removeItem(
            at: legacyFileURL(sdkAppID: sdkAppID, ownerUserID: ownerUserID)
        )
    }

    private func legacyFileURL(sdkAppID: Int?, ownerUserID: String) -> URL {
        let rawFileName = "\(sdkAppID.map(String.init) ?? "default")__\(ownerUserID)"
        let allowedCharacters = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "-_."))
        let fileName = rawFileName
            .addingPercentEncoding(withAllowedCharacters: allowedCharacters) ?? "history"
        return baseDirectoryURL.appendingPathComponent("\(fileName).json", isDirectory: false)
    }

    private var databaseURL: URL {
        baseDirectoryURL.appendingPathComponent("messages.sqlite3", isDirectory: false)
    }

    private func accountKey(for sdkAppID: Int?) -> String {
        sdkAppID.map(String.init) ?? "default"
    }

    private static func deduplicatedMessages(_ messages: [RemoteIMMessage]) -> [RemoteIMMessage] {
        var messagesByID: [UUID: RemoteIMMessage] = [:]
        for message in messages {
            messagesByID[message.id] = message
        }
        return messagesByID.values.sorted {
            if $0.createdAt != $1.createdAt {
                return $0.createdAt < $1.createdAt
            }
            return $0.id.uuidString < $1.id.uuidString
        }
    }

    private static func defaultBaseDirectoryURL(fileManager: FileManager) -> URL {
        if let applicationSupportURL = try? fileManager.url(
            for: .applicationSupportDirectory,
            in: .userDomainMask,
            appropriateFor: nil,
            create: true
        ) {
            return applicationSupportURL
                .appendingPathComponent("MaiChat", isDirectory: true)
                .appendingPathComponent("ChatHistory", isDirectory: true)
        }
        return fileManager.temporaryDirectory
            .appendingPathComponent("MaiChat", isDirectory: true)
            .appendingPathComponent("ChatHistory", isDirectory: true)
    }
}

actor LocalChatHistoryPersistence {
    private let store: LocalChatHistoryStore

    init(baseDirectoryURL: URL) {
        self.store = LocalChatHistoryStore(baseDirectoryURL: baseDirectoryURL)
    }

    func loadConversationPage(
        sdkAppID: Int?,
        ownerUserID: String,
        peerUserID: String,
        before cursor: LocalChatHistoryCursor?,
        limit: Int
    ) throws -> LocalChatHistoryPage {
        try store.loadConversationPage(
            sdkAppID: sdkAppID,
            ownerUserID: ownerUserID,
            peerUserID: peerUserID,
            before: cursor,
            limit: limit
        )
    }

    func loadConversationSummaries(
        sdkAppID: Int?,
        ownerUserID: String,
        peerUserIDs: [String]
    ) throws -> [RemoteIMMessage] {
        try store.loadConversationSummaries(
            sdkAppID: sdkAppID,
            ownerUserID: ownerUserID,
            peerUserIDs: peerUserIDs
        )
    }

    func containsMessage(
        remoteID: String,
        sdkAppID: Int?,
        ownerUserID: String
    ) throws -> Bool {
        try store.containsMessage(
            remoteID: remoteID,
            sdkAppID: sdkAppID,
            ownerUserID: ownerUserID
        )
    }

    func persist(
        mutations: [LocalChatHistoryMutation]
    ) throws -> LocalChatHistorySaveResult {
        try store.applyMutations(mutations)
    }
}
