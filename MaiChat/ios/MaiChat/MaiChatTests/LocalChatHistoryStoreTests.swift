import XCTest
@testable import MaiChatCore
import SQLite3

final class LocalChatHistoryStoreTests: XCTestCase {
    private struct LegacyStoredChatHistory: Codable {
        let schemaVersion: Int
        let sdkAppID: Int?
        let ownerUserID: String
        let messages: [RemoteIMMessage]
    }

    func testPersistsHistoryPerSDKAppIDAndOwnerUserID() throws {
        let directoryURL = FileManager.default.temporaryDirectory
            .appendingPathComponent("maichat-history-\(UUID().uuidString)", isDirectory: true)
        defer { try? FileManager.default.removeItem(at: directoryURL) }

        let store = LocalChatHistoryStore(baseDirectoryURL: directoryURL)
        let message = RemoteIMMessage(
            id: UUID(uuidString: "44444444-4444-4444-4444-444444444444")!,
            fromUserID: "mac-quark-pc",
            toUserID: "ios-master",
            text: "历史消息",
            direction: .incoming,
            status: .received,
            createdAt: Date(timeIntervalSince1970: 300)
        )

        try persist([message], in: store)

        XCTAssertEqual(
            try conversationMessages(in: store, peerUserID: "mac-quark-pc"),
            [message]
        )
        XCTAssertEqual(
            try conversationMessages(
                in: store,
                peerUserID: "mac-quark-pc",
                ownerUserID: "another-owner"
            ),
            []
        )
        XCTAssertEqual(
            try conversationMessages(
                in: store,
                peerUserID: "mac-quark-pc",
                sdkAppID: 9_999
            ),
            []
        )
        XCTAssertTrue(
            FileManager.default.fileExists(
                atPath: directoryURL.appendingPathComponent("messages.sqlite3").path
            )
        )
    }

    func testPersistsStructuredApprovalRequestWithoutParsingMessageText() throws {
        let directoryURL = makeTemporaryDirectoryURL()
        defer { try? FileManager.default.removeItem(at: directoryURL) }
        let store = LocalChatHistoryStore(baseDirectoryURL: directoryURL)
        let request = RemoteIMApprovalRequest(
            token: "approval-persisted-1",
            actions: [.approveOnce, .approvePrefix, .reject]
        )!
        let message = RemoteIMMessage(
            fromUserID: "mac-quark-pc",
            toUserID: "ios-master",
            text: "审批正文不包含任何斜杠命令",
            approvalRequest: request,
            direction: .incoming,
            status: .received,
            createdAt: Date(timeIntervalSince1970: 301)
        )

        try persist([message], in: store)

        XCTAssertEqual(
            try conversationMessages(in: store, peerUserID: "mac-quark-pc").first?.approvalRequest,
            request
        )
    }

    func testUpsertDeduplicatesMessagesByIDAndConversationDeleteIsExplicit() throws {
        let directoryURL = makeTemporaryDirectoryURL()
        defer { try? FileManager.default.removeItem(at: directoryURL) }

        let store = LocalChatHistoryStore(baseDirectoryURL: directoryURL)
        let messageID = UUID(uuidString: "55555555-5555-5555-5555-555555555555")!
        let pendingMessage = RemoteIMMessage(
            id: messageID,
            fromUserID: "ios-master",
            toUserID: "mac-quark-pc",
            text: "待发送",
            direction: .outgoing,
            status: .pending,
            createdAt: Date(timeIntervalSince1970: 400)
        )
        let sentMessage = RemoteIMMessage(
            id: messageID,
            fromUserID: "ios-master",
            toUserID: "mac-quark-pc",
            text: "已发送",
            direction: .outgoing,
            status: .sent,
            createdAt: Date(timeIntervalSince1970: 401)
        )

        try persist([pendingMessage, sentMessage], in: store)
        XCTAssertEqual(
            try conversationMessages(in: store, peerUserID: "mac-quark-pc"),
            [sentMessage]
        )

        _ = try store.applyMutations(
            [
                .removeConversation(
                    peerUserID: "mac-quark-pc",
                    sdkAppID: 1_600_148_979,
                    ownerUserID: "ios-master"
                ),
            ]
        )
        XCTAssertEqual(
            try conversationMessages(in: store, peerUserID: "mac-quark-pc"),
            []
        )
    }

    func testPersistenceOnlyUpsertsChangedMessagesWithoutDeletingUnloadedHistory() async throws {
        let directoryURL = makeTemporaryDirectoryURL()
        defer { try? FileManager.default.removeItem(at: directoryURL) }

        let store = LocalChatHistoryStore(baseDirectoryURL: directoryURL)
        let firstID = UUID(uuidString: "77777777-7777-7777-7777-777777777777")!
        let secondID = UUID(uuidString: "88888888-8888-8888-8888-888888888888")!
        let thirdID = UUID(uuidString: "99999999-9999-9999-9999-999999999999")!
        let first = RemoteIMMessage(
            id: firstID,
            fromUserID: "ios-master",
            toUserID: "mac-quark-pc",
            text: "第一条",
            direction: .outgoing,
            status: .pending,
            createdAt: Date(timeIntervalSince1970: 700)
        )
        let second = RemoteIMMessage(
            id: secondID,
            fromUserID: "mac-quark-pc",
            toUserID: "ios-master",
            text: "第二条",
            direction: .incoming,
            status: .received,
            createdAt: Date(timeIntervalSince1970: 701)
        )
        try persist([first, second], in: store)

        let persistence = store.makePersistence()

        var deliveredFirst = first
        deliveredFirst.status = .sent
        let third = RemoteIMMessage(
            id: thirdID,
            fromUserID: "mac-quark-pc",
            toUserID: "ios-master",
            text: "第三条",
            direction: .incoming,
            status: .received,
            createdAt: Date(timeIntervalSince1970: 702)
        )
        let changed = try await persistence.persist(
            mutations: [
                .upsert(
                    [deliveredFirst, third],
                    sdkAppID: 1_600_148_979,
                    ownerUserID: "ios-master"
                ),
            ]
        )
        XCTAssertEqual(changed.upsertedCount, 2)
        XCTAssertEqual(changed.removedCount, 0)
        XCTAssertEqual(changed.messageCount, 2)
        XCTAssertEqual(
            try conversationMessages(in: store, peerUserID: "mac-quark-pc"),
            [deliveredFirst, second, third]
        )
    }

    func testFirstIncrementalMutationMigratesLegacyHistoryBeforeWriting() async throws {
        let directoryURL = makeTemporaryDirectoryURL()
        defer { try? FileManager.default.removeItem(at: directoryURL) }
        try FileManager.default.createDirectory(at: directoryURL, withIntermediateDirectories: true)

        let legacyA = makeMessage(idSuffix: 21, peer: "peer-a", time: 21)
        let legacyB = makeMessage(idSuffix: 22, peer: "peer-b", time: 22)
        let newMessage = makeMessage(idSuffix: 23, peer: "peer-a", time: 23)
        let legacyURL = directoryURL.appendingPathComponent("1600148979__ios-master.json")
        let legacyHistory = LegacyStoredChatHistory(
            schemaVersion: 1,
            sdkAppID: 1_600_148_979,
            ownerUserID: "ios-master",
            messages: [legacyA, legacyB]
        )
        try JSONEncoder().encode(legacyHistory).write(to: legacyURL, options: .atomic)

        let store = LocalChatHistoryStore(baseDirectoryURL: directoryURL)
        let persistence = store.makePersistence()
        _ = try await persistence.persist(
            mutations: [
                .upsert(
                    [newMessage],
                    sdkAppID: 1_600_148_979,
                    ownerUserID: "ios-master"
                ),
            ]
        )

        XCTAssertEqual(
            try conversationMessages(in: store, peerUserID: "peer-a"),
            [legacyA, newMessage]
        )
        XCTAssertEqual(
            try conversationMessages(in: store, peerUserID: "peer-b"),
            [legacyB]
        )
        XCTAssertFalse(FileManager.default.fileExists(atPath: legacyURL.path))
    }

    func testLoadsConversationHistoryInStableCursorPages() async throws {
        let directoryURL = makeTemporaryDirectoryURL()
        defer { try? FileManager.default.removeItem(at: directoryURL) }
        let store = LocalChatHistoryStore(baseDirectoryURL: directoryURL)
        let messages = (0 ..< 121).map { index in
            RemoteIMMessage(
                id: UUID(uuidString: String(format: "00000000-0000-0000-0000-%012d", index + 1))!,
                fromUserID: index.isMultiple(of: 2) ? "ios-master" : "peer-a",
                toUserID: index.isMultiple(of: 2) ? "peer-a" : "ios-master",
                text: "message-\(index)",
                direction: index.isMultiple(of: 2) ? .outgoing : .incoming,
                status: index.isMultiple(of: 2) ? .sent : .received,
                createdAt: Date(timeIntervalSince1970: TimeInterval(index / 3))
            )
        }
        try persist(messages, in: store)
        let persistence = store.makePersistence()

        let newest = try await persistence.loadConversationPage(
            sdkAppID: 1_600_148_979,
            ownerUserID: "ios-master",
            peerUserID: "peer-a",
            before: nil,
            limit: 50
        )
        XCTAssertEqual(newest.messages.count, 50)
        XCTAssertTrue(newest.hasEarlierMessages)
        let middle = try await persistence.loadConversationPage(
            sdkAppID: 1_600_148_979,
            ownerUserID: "ios-master",
            peerUserID: "peer-a",
            before: LocalChatHistoryCursor(
                createdAt: try XCTUnwrap(newest.messages.first).createdAt,
                messageID: try XCTUnwrap(newest.messages.first).id
            ),
            limit: 50
        )
        XCTAssertEqual(middle.messages.count, 50)
        XCTAssertTrue(middle.hasEarlierMessages)
        let oldest = try await persistence.loadConversationPage(
            sdkAppID: 1_600_148_979,
            ownerUserID: "ios-master",
            peerUserID: "peer-a",
            before: LocalChatHistoryCursor(
                createdAt: try XCTUnwrap(middle.messages.first).createdAt,
                messageID: try XCTUnwrap(middle.messages.first).id
            ),
            limit: 50
        )
        XCTAssertEqual(oldest.messages.count, 21)
        XCTAssertFalse(oldest.hasEarlierMessages)

        let loaded = oldest.messages + middle.messages + newest.messages
        let expected = messages.sorted {
            if $0.createdAt != $1.createdAt { return $0.createdAt < $1.createdAt }
            return $0.id.uuidString < $1.id.uuidString
        }
        XCTAssertEqual(loaded, expected)
        XCTAssertEqual(Set(loaded.map(\.id)).count, 121)
    }

    func testLoadsOneLatestSummaryPerConversationAndDeletesOnlyRequestedPeer() async throws {
        let directoryURL = makeTemporaryDirectoryURL()
        defer { try? FileManager.default.removeItem(at: directoryURL) }
        let store = LocalChatHistoryStore(baseDirectoryURL: directoryURL)
        let peerAOld = makeMessage(idSuffix: 1, peer: "peer-a", time: 1)
        let peerANew = makeMessage(idSuffix: 2, peer: "peer-a", time: 3)
        let peerB = makeMessage(idSuffix: 3, peer: "peer-b", time: 2)
        try persist([peerAOld, peerANew, peerB], in: store)
        let persistence = store.makePersistence()

        let summaries = try await persistence.loadConversationSummaries(
            sdkAppID: 1_600_148_979,
            ownerUserID: "ios-master",
            peerUserIDs: ["peer-a", "peer-b"]
        )
        XCTAssertEqual(summaries, [peerANew, peerB])

        let result = try await persistence.persist(
            mutations: [
                .removeConversation(
                    peerUserID: "peer-a",
                    sdkAppID: 1_600_148_979,
                    ownerUserID: "ios-master"
                ),
            ]
        )
        XCTAssertEqual(result.removedCount, 2)
        XCTAssertEqual(
            try conversationMessages(in: store, peerUserID: "peer-a"),
            []
        )
        XCTAssertEqual(
            try conversationMessages(in: store, peerUserID: "peer-b"),
            [peerB]
        )
    }

    func testMigratesV2RowsToPeerCursorSchemaWithoutLosingMessages() async throws {
        let directoryURL = makeTemporaryDirectoryURL()
        defer { try? FileManager.default.removeItem(at: directoryURL) }
        try FileManager.default.createDirectory(at: directoryURL, withIntermediateDirectories: true)
        let databaseURL = directoryURL.appendingPathComponent("messages.sqlite3")
        var database: OpaquePointer?
        XCTAssertEqual(sqlite3_open(databaseURL.path, &database), SQLITE_OK)
        let openedDatabase = try XCTUnwrap(database)
        try executeSQLite(
            openedDatabase,
            sql: """
            PRAGMA user_version = 2;
            CREATE TABLE messages (
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
                PRIMARY KEY (sdk_app_id, owner_user_id, id)
            );
            INSERT INTO messages VALUES(
                '1600148979', 'ios-master', 'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa',
                NULL, 'peer-a', 'ios-master', 'incoming', 'incoming', 'received', 1,
                NULL, NULL, NULL
            );
            INSERT INTO messages VALUES(
                '1600148979', 'ios-master', 'bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb',
                NULL, 'ios-master', 'peer-a', 'outgoing', 'outgoing', 'sent', 2,
                NULL, NULL, NULL
            );
            """
        )
        XCTAssertEqual(sqlite3_close(openedDatabase), SQLITE_OK)
        database = nil

        let page = try await LocalChatHistoryStore(baseDirectoryURL: directoryURL)
            .makePersistence()
            .loadConversationPage(
                sdkAppID: 1_600_148_979,
                ownerUserID: "ios-master",
                peerUserID: "peer-a",
                before: nil,
                limit: 50
            )

        XCTAssertEqual(page.messages.map(\.text), ["incoming", "outgoing"])
        XCTAssertFalse(page.hasEarlierMessages)

        XCTAssertEqual(sqlite3_open(databaseURL.path, &database), SQLITE_OK)
        let migratedDatabase = try XCTUnwrap(database)
        defer { sqlite3_close(migratedDatabase) }
        var versionStatement: OpaquePointer?
        XCTAssertEqual(
            sqlite3_prepare_v2(migratedDatabase, "PRAGMA user_version", -1, &versionStatement, nil),
            SQLITE_OK
        )
        let openedVersionStatement = try XCTUnwrap(versionStatement)
        defer { sqlite3_finalize(openedVersionStatement) }
        XCTAssertEqual(sqlite3_step(openedVersionStatement), SQLITE_ROW)
        XCTAssertEqual(sqlite3_column_int(openedVersionStatement, 0), 5)

        var columnStatement: OpaquePointer?
        XCTAssertEqual(
            sqlite3_prepare_v2(
                migratedDatabase,
                "SELECT COUNT(*) FROM pragma_table_info('messages') WHERE name = 'video_attachment'",
                -1,
                &columnStatement,
                nil
            ),
            SQLITE_OK
        )
        let openedColumnStatement = try XCTUnwrap(columnStatement)
        defer { sqlite3_finalize(openedColumnStatement) }
        XCTAssertEqual(sqlite3_step(openedColumnStatement), SQLITE_ROW)
        XCTAssertEqual(sqlite3_column_int(openedColumnStatement, 0), 1)

        var approvalColumnStatement: OpaquePointer?
        XCTAssertEqual(
            sqlite3_prepare_v2(
                migratedDatabase,
                "SELECT COUNT(*) FROM pragma_table_info('messages') WHERE name = 'approval_request'",
                -1,
                &approvalColumnStatement,
                nil
            ),
            SQLITE_OK
        )
        let openedApprovalColumnStatement = try XCTUnwrap(approvalColumnStatement)
        defer { sqlite3_finalize(openedApprovalColumnStatement) }
        XCTAssertEqual(sqlite3_step(openedApprovalColumnStatement), SQLITE_ROW)
        XCTAssertEqual(sqlite3_column_int(openedApprovalColumnStatement, 0), 1)
    }

    func testConversationPageQueryUsesPeerTimeIndex() throws {
        let directoryURL = makeTemporaryDirectoryURL()
        defer { try? FileManager.default.removeItem(at: directoryURL) }
        let store = LocalChatHistoryStore(baseDirectoryURL: directoryURL)
        try persist([makeMessage(idSuffix: 41, peer: "peer-a", time: 41)], in: store)

        var database: OpaquePointer?
        XCTAssertEqual(
            sqlite3_open(directoryURL.appendingPathComponent("messages.sqlite3").path, &database),
            SQLITE_OK
        )
        let openedDatabase = try XCTUnwrap(database)
        defer { sqlite3_close(openedDatabase) }
        var statement: OpaquePointer?
        XCTAssertEqual(
            sqlite3_prepare_v2(
                openedDatabase,
                """
                EXPLAIN QUERY PLAN
                SELECT id FROM messages
                WHERE sdk_app_id = '1600148979'
                  AND owner_user_id = 'ios-master'
                  AND peer_user_id = 'peer-a'
                ORDER BY created_at DESC, id DESC
                LIMIT 51
                """,
                -1,
                &statement,
                nil
            ),
            SQLITE_OK
        )
        let openedStatement = try XCTUnwrap(statement)
        defer { sqlite3_finalize(openedStatement) }
        var details: [String] = []
        while sqlite3_step(openedStatement) == SQLITE_ROW {
            if let detail = sqlite3_column_text(openedStatement, 3) {
                details.append(String(cString: detail))
            }
        }
        XCTAssertTrue(
            details.contains(where: { $0.contains("idx_messages_account_peer_time") }),
            "Unexpected query plan: \(details)"
        )
    }

    func testPersistsMessageAttachments() throws {
        let directoryURL = makeTemporaryDirectoryURL()
        defer { try? FileManager.default.removeItem(at: directoryURL) }
        let voiceURL = RemoteIMMediaStorage.fileURL(
            category: .incomingVoices,
            fileName: "voice-\(UUID().uuidString).m4a"
        )
        let fileURL = RemoteIMMediaStorage.fileURL(
            category: .incomingFiles,
            fileName: "report-\(UUID().uuidString).md"
        )
        let videoURL = RemoteIMMediaStorage.fileURL(
            category: .incomingVideos,
            fileName: "video-\(UUID().uuidString).mp4"
        )
        let coverURL = RemoteIMMediaStorage.fileURL(
            category: .incomingVideoCovers,
            fileName: "video-\(UUID().uuidString).jpg"
        )

        let store = LocalChatHistoryStore(baseDirectoryURL: directoryURL)
        let messages = [
            RemoteIMMessage(
                remoteID: "sdk-file-message-1",
                fromUserID: "mac-quark-pc",
                toUserID: "ios-master",
                text: "[语音]",
                voiceAttachment: RemoteIMVoiceAttachment(
                    localFilePath: voiceURL.path,
                    durationSeconds: 3,
                    remoteID: "voice-1"
                ),
                direction: .incoming,
                status: .received,
                createdAt: Date(timeIntervalSince1970: 500)
            ),
            RemoteIMMessage(
                fromUserID: "mac-quark-pc",
                toUserID: "ios-master",
                text: "report.md",
                fileAttachment: RemoteIMFileAttachment(
                    localFilePath: fileURL.path,
                    fileName: "report.md",
                    mimeType: "text/markdown",
                    remoteID: "file-1",
                    sizeBytes: 128
                ),
                direction: .incoming,
                status: .received,
                createdAt: Date(timeIntervalSince1970: 501)
            ),
            RemoteIMMessage(
                remoteID: "sdk-video-message-1",
                fromUserID: "mac-quark-pc",
                toUserID: "ios-master",
                text: "[视频消息 18s]",
                videoAttachment: RemoteIMVideoAttachment(
                    localPath: videoURL.path,
                    coverPath: coverURL.path,
                    durationSeconds: 18,
                    width: 1920,
                    height: 1080,
                    sizeBytes: 8_192
                ),
                direction: .incoming,
                status: .received,
                createdAt: Date(timeIntervalSince1970: 502)
            )
        ]

        try persist(messages, in: store)

        XCTAssertEqual(
            try conversationMessages(in: store, peerUserID: "mac-quark-pc"),
            messages
        )
        XCTAssertEqual(
            try conversationMessages(in: store, peerUserID: "mac-quark-pc").first?.remoteID,
            "sdk-file-message-1"
        )
    }

    func testPersistsNewApplicationSupportImageAsRelativeReference() throws {
        let directoryURL = makeTemporaryDirectoryURL()
        defer { try? FileManager.default.removeItem(at: directoryURL) }

        let fileName = "persistent-image-\(UUID().uuidString).png"
        let persistentURL = RemoteIMMediaStorage.fileURL(category: .outgoingImages, fileName: fileName)
        let bytes = Data([0x89, 0x50, 0x4E, 0x47])
        try bytes.write(to: persistentURL)
        defer { try? FileManager.default.removeItem(at: persistentURL) }

        let message = RemoteIMMessage(
            remoteID: "legacy-cache-image",
            fromUserID: "ios-master",
            toUserID: "peer-a",
            text: "[图片消息] \(fileName)",
            imageAttachment: RemoteIMImageAttachment(
                localFilePath: persistentURL.path,
                width: 1,
                height: 1,
                sizeBytes: bytes.count
            ),
            direction: .outgoing,
            status: .sent,
            createdAt: Date(timeIntervalSince1970: 550)
        )

        let store = LocalChatHistoryStore(baseDirectoryURL: directoryURL)
        try persist([message], in: store)
        let loaded = try XCTUnwrap(
            conversationMessages(in: store, peerUserID: "peer-a").first
        )

        XCTAssertEqual(loaded.imageAttachment?.localFilePath, persistentURL.path)
        XCTAssertEqual(try Data(contentsOf: persistentURL), bytes)

        let databaseURL = directoryURL.appendingPathComponent("messages.sqlite3")
        var database: OpaquePointer?
        XCTAssertEqual(sqlite3_open(databaseURL.path, &database), SQLITE_OK)
        let openedDatabase = try XCTUnwrap(database)
        defer { sqlite3_close(openedDatabase) }
        var statement: OpaquePointer?
        XCTAssertEqual(
            sqlite3_prepare_v2(openedDatabase, "SELECT image_attachment FROM messages LIMIT 1", -1, &statement, nil),
            SQLITE_OK
        )
        let openedStatement = try XCTUnwrap(statement)
        defer { sqlite3_finalize(openedStatement) }
        XCTAssertEqual(sqlite3_step(openedStatement), SQLITE_ROW)
        let storedJSON = sqlite3_column_text(openedStatement, 0).map { String(cString: $0) } ?? ""
        let storedAttachment = try JSONDecoder().decode(
            RemoteIMImageAttachment.self,
            from: try XCTUnwrap(storedJSON.data(using: .utf8))
        )
        XCTAssertEqual(storedAttachment.localFilePath, "Outgoing/Images/\(fileName)")
        XCTAssertFalse(storedJSON.contains("/Library/Caches/"))
    }

    func testRejectsLegacyAbsoluteMediaPathWithoutMigration() throws {
        let fileName = "legacy-image-\(UUID().uuidString).png"
        let legacyDirectory = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("RemoteIMPickedImage", isDirectory: true)
        try FileManager.default.createDirectory(at: legacyDirectory, withIntermediateDirectories: true)
        let legacyURL = legacyDirectory.appendingPathComponent(fileName)
        let bytes = Data([1, 1, 1])
        try bytes.write(to: legacyURL)
        defer { try? FileManager.default.removeItem(at: legacyURL) }

        XCTAssertEqual(
            RemoteIMMediaStorage.persistentReference(for: legacyURL.path, category: .outgoingImages),
            ""
        )
        XCTAssertEqual(
            RemoteIMMediaStorage.resolvedPath(from: legacyURL.path, category: .outgoingImages),
            ""
        )
        XCTAssertEqual(try Data(contentsOf: legacyURL), bytes)
    }

    func testMigratesLegacyJSONHistoryIntoSQLite() throws {
        let directoryURL = makeTemporaryDirectoryURL()
        defer { try? FileManager.default.removeItem(at: directoryURL) }
        try FileManager.default.createDirectory(at: directoryURL, withIntermediateDirectories: true)

        let message = RemoteIMMessage(
            id: UUID(uuidString: "66666666-6666-6666-6666-666666666666")!,
            fromUserID: "mac-quark-pc",
            toUserID: "ios-master",
            text: "旧历史消息",
            direction: .incoming,
            status: .received,
            createdAt: Date(timeIntervalSince1970: 600)
        )
        let legacyURL = directoryURL.appendingPathComponent("1600148979__ios-master.json")
        let legacyHistory = LegacyStoredChatHistory(
            schemaVersion: 1,
            sdkAppID: 1_600_148_979,
            ownerUserID: "ios-master",
            messages: [message]
        )
        try JSONEncoder().encode(legacyHistory).write(to: legacyURL, options: .atomic)

        let store = LocalChatHistoryStore(baseDirectoryURL: directoryURL)
        XCTAssertEqual(
            try conversationMessages(in: store, peerUserID: "mac-quark-pc"),
            [message]
        )
        XCTAssertFalse(FileManager.default.fileExists(atPath: legacyURL.path))

        let reopenedStore = LocalChatHistoryStore(baseDirectoryURL: directoryURL)
        XCTAssertEqual(
            try conversationMessages(in: reopenedStore, peerUserID: "mac-quark-pc"),
            [message]
        )
    }

    func testLegacyMigrationDoesNotOverwriteNewerSQLiteMessageState() throws {
        let directoryURL = makeTemporaryDirectoryURL()
        defer { try? FileManager.default.removeItem(at: directoryURL) }
        try FileManager.default.createDirectory(at: directoryURL, withIntermediateDirectories: true)

        let messageID = UUID(uuidString: "abababab-abab-abab-abab-abababababab")!
        let store = LocalChatHistoryStore(baseDirectoryURL: directoryURL)
        let sentMessage = RemoteIMMessage(
            id: messageID,
            fromUserID: "ios-master",
            toUserID: "peer-a",
            text: "已发送",
            direction: .outgoing,
            status: .sent,
            createdAt: Date(timeIntervalSince1970: 700)
        )
        try persist([sentMessage], in: store)

        let stalePendingMessage = RemoteIMMessage(
            id: messageID,
            fromUserID: "ios-master",
            toUserID: "peer-a",
            text: "待发送",
            direction: .outgoing,
            status: .pending,
            createdAt: Date(timeIntervalSince1970: 699)
        )
        let legacyURL = directoryURL.appendingPathComponent("1600148979__ios-master.json")
        try JSONEncoder().encode(
            LegacyStoredChatHistory(
                schemaVersion: 1,
                sdkAppID: 1_600_148_979,
                ownerUserID: "ios-master",
                messages: [stalePendingMessage]
            )
        ).write(to: legacyURL, options: .atomic)

        XCTAssertEqual(
            try conversationMessages(in: store, peerUserID: "peer-a"),
            [sentMessage]
        )
        XCTAssertFalse(FileManager.default.fileExists(atPath: legacyURL.path))
    }

    func testOpeningFutureSchemaDoesNotDowngradeUserVersion() throws {
        let directoryURL = makeTemporaryDirectoryURL()
        defer { try? FileManager.default.removeItem(at: directoryURL) }
        let store = LocalChatHistoryStore(baseDirectoryURL: directoryURL)
        try persist([makeMessage(idSuffix: 51, peer: "peer-a", time: 51)], in: store)

        let databaseURL = directoryURL.appendingPathComponent("messages.sqlite3")
        var database: OpaquePointer?
        XCTAssertEqual(sqlite3_open(databaseURL.path, &database), SQLITE_OK)
        let openedDatabase = try XCTUnwrap(database)
        try executeSQLite(openedDatabase, sql: "PRAGMA user_version = 99")
        XCTAssertEqual(sqlite3_close(openedDatabase), SQLITE_OK)
        database = nil

        _ = try conversationMessages(in: store, peerUserID: "peer-a")

        XCTAssertEqual(sqlite3_open(databaseURL.path, &database), SQLITE_OK)
        let reopenedDatabase = try XCTUnwrap(database)
        defer { sqlite3_close(reopenedDatabase) }
        var statement: OpaquePointer?
        XCTAssertEqual(sqlite3_prepare_v2(reopenedDatabase, "PRAGMA user_version", -1, &statement, nil), SQLITE_OK)
        let openedStatement = try XCTUnwrap(statement)
        defer { sqlite3_finalize(openedStatement) }
        XCTAssertEqual(sqlite3_step(openedStatement), SQLITE_ROW)
        XCTAssertEqual(sqlite3_column_int(openedStatement, 0), 99)
    }

    func testEmptySummaryRequestStillMigratesLegacyHistory() async throws {
        let directoryURL = makeTemporaryDirectoryURL()
        defer { try? FileManager.default.removeItem(at: directoryURL) }
        try FileManager.default.createDirectory(at: directoryURL, withIntermediateDirectories: true)

        let message = makeMessage(idSuffix: 31, peer: "peer-a", time: 31)
        let legacyURL = directoryURL.appendingPathComponent("1600148979__ios-master.json")
        try JSONEncoder().encode(
            LegacyStoredChatHistory(
                schemaVersion: 1,
                sdkAppID: 1_600_148_979,
                ownerUserID: "ios-master",
                messages: [message]
            )
        ).write(to: legacyURL, options: .atomic)

        let store = LocalChatHistoryStore(baseDirectoryURL: directoryURL)
        let summaries = try await store.makePersistence().loadConversationSummaries(
            sdkAppID: 1_600_148_979,
            ownerUserID: "ios-master",
            peerUserIDs: []
        )

        XCTAssertTrue(summaries.isEmpty)
        XCTAssertFalse(FileManager.default.fileExists(atPath: legacyURL.path))
        XCTAssertEqual(
            try conversationMessages(in: store, peerUserID: "peer-a"),
            [message]
        )
    }

    private func makeTemporaryDirectoryURL() -> URL {
        FileManager.default.temporaryDirectory
            .appendingPathComponent("maichat-history-\(UUID().uuidString)", isDirectory: true)
    }

    private func persist(
        _ messages: [RemoteIMMessage],
        in store: LocalChatHistoryStore,
        sdkAppID: Int? = 1_600_148_979,
        ownerUserID: String = "ios-master"
    ) throws {
        _ = try store.applyMutations(
            [
                .upsert(
                    messages,
                    sdkAppID: sdkAppID,
                    ownerUserID: ownerUserID
                ),
            ]
        )
    }

    private func conversationMessages(
        in store: LocalChatHistoryStore,
        peerUserID: String,
        sdkAppID: Int? = 1_600_148_979,
        ownerUserID: String = "ios-master"
    ) throws -> [RemoteIMMessage] {
        try store.loadConversationPage(
            sdkAppID: sdkAppID,
            ownerUserID: ownerUserID,
            peerUserID: peerUserID,
            before: nil,
            limit: 1_000
        ).messages
    }

    private func executeSQLite(_ database: OpaquePointer, sql: String) throws {
        var errorPointer: UnsafeMutablePointer<CChar>?
        let result = sqlite3_exec(database, sql, nil, nil, &errorPointer)
        guard result == SQLITE_OK else {
            let message = errorPointer.map { String(cString: $0) } ?? "unknown SQLite error"
            sqlite3_free(errorPointer)
            throw NSError(
                domain: "LocalChatHistoryStoreTests.SQLite",
                code: Int(result),
                userInfo: [NSLocalizedDescriptionKey: message]
            )
        }
    }

    private func makeMessage(
        idSuffix: Int,
        peer: String,
        time: TimeInterval
    ) -> RemoteIMMessage {
        RemoteIMMessage(
            id: UUID(uuidString: String(format: "10000000-0000-0000-0000-%012d", idSuffix))!,
            fromUserID: peer,
            toUserID: "ios-master",
            text: "\(peer)-\(idSuffix)",
            direction: .incoming,
            status: .received,
            createdAt: Date(timeIntervalSince1970: time)
        )
    }
}
