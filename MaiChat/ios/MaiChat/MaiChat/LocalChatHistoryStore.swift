import Foundation
import MaiChatCore
import SQLite3

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

    func load(sdkAppID: Int?, ownerUserID: String) -> [RemoteIMMessage] {
        let cleanOwnerUserID = ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanOwnerUserID.isEmpty else { return [] }

        let databaseMessages = (try? loadFromDatabase(
            sdkAppID: sdkAppID,
            ownerUserID: cleanOwnerUserID
        )) ?? []
        guard let legacyHistory = loadLegacyHistory(
            sdkAppID: sdkAppID,
            ownerUserID: cleanOwnerUserID
        ) else {
            return databaseMessages
        }

        let mergedMessages = Self.mergedMessages(
            legacyHistory.messages,
            databaseMessages
        )
        do {
            try save(
                messages: mergedMessages,
                sdkAppID: sdkAppID,
                ownerUserID: cleanOwnerUserID
            )
            return mergedMessages
        } catch {
            return databaseMessages.isEmpty ? legacyHistory.messages : databaseMessages
        }
    }

    func save(
        messages: [RemoteIMMessage],
        sdkAppID: Int?,
        ownerUserID: String
    ) throws {
        let cleanOwnerUserID = ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanOwnerUserID.isEmpty else { return }

        try withDatabase { database in
            try execute(database, sql: "BEGIN IMMEDIATE TRANSACTION")
            do {
                try deleteMessages(
                    database,
                    sdkAppID: sdkAppID,
                    ownerUserID: cleanOwnerUserID
                )
                try insertMessages(
                    Self.deduplicatedMessages(messages),
                    into: database,
                    sdkAppID: sdkAppID,
                    ownerUserID: cleanOwnerUserID
                )
                try execute(database, sql: "COMMIT")
            } catch {
                try? execute(database, sql: "ROLLBACK")
                throw error
            }
        }

        try? fileManager.removeItem(
            at: legacyFileURL(sdkAppID: sdkAppID, ownerUserID: cleanOwnerUserID)
        )
    }

    private func loadFromDatabase(
        sdkAppID: Int?,
        ownerUserID: String
    ) throws -> [RemoteIMMessage] {
        try withDatabase { database in
            let statement = try prepare(
                database,
                sql: """
                SELECT id, from_user, to_user, text, direction, status, created_at,
                       voice_attachment, image_attachment, file_attachment
                FROM messages
                WHERE sdk_app_id = ? AND owner_user_id = ?
                ORDER BY created_at, id
                """
            )
            defer { sqlite3_finalize(statement) }
            try bindText(accountKey(for: sdkAppID), to: statement, at: 1, database: database)
            try bindText(ownerUserID, to: statement, at: 2, database: database)

            var messages: [RemoteIMMessage] = []
            while true {
                let result = sqlite3_step(statement)
                if result == SQLITE_DONE {
                    return messages
                }
                guard result == SQLITE_ROW else {
                    throw databaseError(database)
                }
                if let message = decodeMessage(from: statement) {
                    messages.append(message)
                }
            }
        }
    }

    private func deleteMessages(
        _ database: OpaquePointer,
        sdkAppID: Int?,
        ownerUserID: String
    ) throws {
        let statement = try prepare(
            database,
            sql: "DELETE FROM messages WHERE sdk_app_id = ? AND owner_user_id = ?"
        )
        defer { sqlite3_finalize(statement) }
        try bindText(accountKey(for: sdkAppID), to: statement, at: 1, database: database)
        try bindText(ownerUserID, to: statement, at: 2, database: database)
        guard sqlite3_step(statement) == SQLITE_DONE else {
            throw databaseError(database)
        }
    }

    private func insertMessages(
        _ messages: [RemoteIMMessage],
        into database: OpaquePointer,
        sdkAppID: Int?,
        ownerUserID: String
    ) throws {
        let statement = try prepare(
            database,
            sql: """
            INSERT INTO messages(
                sdk_app_id, owner_user_id, id, from_user, to_user, text,
                direction, status, created_at,
                voice_attachment, image_attachment, file_attachment
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """
        )
        defer { sqlite3_finalize(statement) }

        for message in messages {
            sqlite3_reset(statement)
            sqlite3_clear_bindings(statement)
            try bindText(accountKey(for: sdkAppID), to: statement, at: 1, database: database)
            try bindText(ownerUserID, to: statement, at: 2, database: database)
            try bindText(message.id.uuidString, to: statement, at: 3, database: database)
            try bindText(message.fromUserID, to: statement, at: 4, database: database)
            try bindText(message.toUserID, to: statement, at: 5, database: database)
            try bindText(message.text, to: statement, at: 6, database: database)
            try bindText(message.direction.rawValue, to: statement, at: 7, database: database)
            try bindText(message.status.rawValue, to: statement, at: 8, database: database)
            sqlite3_bind_double(statement, 9, message.createdAt.timeIntervalSince1970)
            try bindOptionalJSON(message.voiceAttachment, to: statement, at: 10, database: database)
            try bindOptionalJSON(message.imageAttachment, to: statement, at: 11, database: database)
            try bindOptionalJSON(message.fileAttachment, to: statement, at: 12, database: database)
            guard sqlite3_step(statement) == SQLITE_DONE else {
                throw databaseError(database)
            }
        }
    }

    private func decodeMessage(from statement: OpaquePointer) -> RemoteIMMessage? {
        guard let id = UUID(uuidString: textColumn(statement, at: 0)),
              let direction = RemoteIMMessageDirection(rawValue: textColumn(statement, at: 4)),
              let status = RemoteIMMessageStatus(rawValue: textColumn(statement, at: 5))
        else {
            return nil
        }

        return RemoteIMMessage(
            id: id,
            fromUserID: textColumn(statement, at: 1),
            toUserID: textColumn(statement, at: 2),
            text: textColumn(statement, at: 3),
            voiceAttachment: decodeOptionalJSON(
                RemoteIMVoiceAttachment.self,
                from: optionalTextColumn(statement, at: 7)
            ),
            imageAttachment: decodeOptionalJSON(
                RemoteIMImageAttachment.self,
                from: optionalTextColumn(statement, at: 8)
            ),
            fileAttachment: decodeOptionalJSON(
                RemoteIMFileAttachment.self,
                from: optionalTextColumn(statement, at: 9)
            ),
            direction: direction,
            status: status,
            createdAt: Date(timeIntervalSince1970: sqlite3_column_double(statement, 6))
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
        try execute(database, sql: "PRAGMA journal_mode = WAL")
        try execute(database, sql: "PRAGMA synchronous = NORMAL")
        try execute(
            database,
            sql: """
            CREATE TABLE IF NOT EXISTS messages (
                sdk_app_id TEXT NOT NULL,
                owner_user_id TEXT NOT NULL,
                id TEXT NOT NULL,
                from_user TEXT NOT NULL,
                to_user TEXT NOT NULL,
                text TEXT NOT NULL DEFAULT '',
                direction TEXT NOT NULL,
                status TEXT NOT NULL,
                created_at REAL NOT NULL,
                voice_attachment TEXT,
                image_attachment TEXT,
                file_attachment TEXT,
                PRIMARY KEY (sdk_app_id, owner_user_id, id)
            )
            """
        )
        try execute(
            database,
            sql: """
            CREATE INDEX IF NOT EXISTS idx_messages_account_time
            ON messages(sdk_app_id, owner_user_id, created_at, id)
            """
        )
        try execute(database, sql: "PRAGMA user_version = 1")
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

    private static func mergedMessages(
        _ legacyMessages: [RemoteIMMessage],
        _ databaseMessages: [RemoteIMMessage]
    ) -> [RemoteIMMessage] {
        deduplicatedMessages(legacyMessages + databaseMessages)
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
