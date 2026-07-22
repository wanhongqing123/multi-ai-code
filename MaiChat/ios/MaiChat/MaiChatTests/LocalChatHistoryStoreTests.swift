import XCTest
@testable import MaiChatCore

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

        try store.save(
            messages: [message],
            sdkAppID: 1_600_148_979,
            ownerUserID: "ios-master"
        )

        XCTAssertEqual(
            store.load(sdkAppID: 1_600_148_979, ownerUserID: "ios-master"),
            [message]
        )
        XCTAssertEqual(
            store.load(sdkAppID: 1_600_148_979, ownerUserID: "another-owner"),
            []
        )
        XCTAssertTrue(
            FileManager.default.fileExists(
                atPath: directoryURL.appendingPathComponent("messages.sqlite3").path
            )
        )
    }

    func testReplacesAccountSnapshotAndDeduplicatesMessagesByID() throws {
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

        try store.save(
            messages: [pendingMessage, sentMessage],
            sdkAppID: 1_600_148_979,
            ownerUserID: "ios-master"
        )
        XCTAssertEqual(
            store.load(sdkAppID: 1_600_148_979, ownerUserID: "ios-master"),
            [sentMessage]
        )

        try store.save(
            messages: [],
            sdkAppID: 1_600_148_979,
            ownerUserID: "ios-master"
        )
        XCTAssertEqual(
            store.load(sdkAppID: 1_600_148_979, ownerUserID: "ios-master"),
            []
        )
    }

    func testPersistsMessageAttachments() throws {
        let directoryURL = makeTemporaryDirectoryURL()
        defer { try? FileManager.default.removeItem(at: directoryURL) }

        let store = LocalChatHistoryStore(baseDirectoryURL: directoryURL)
        let messages = [
            RemoteIMMessage(
                fromUserID: "mac-quark-pc",
                toUserID: "ios-master",
                text: "[语音]",
                voiceAttachment: RemoteIMVoiceAttachment(
                    localFilePath: "/tmp/voice.m4a",
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
                    localFilePath: "/tmp/report.md",
                    fileName: "report.md",
                    mimeType: "text/markdown",
                    remoteID: "file-1",
                    sizeBytes: 128
                ),
                direction: .incoming,
                status: .received,
                createdAt: Date(timeIntervalSince1970: 501)
            )
        ]

        try store.save(
            messages: messages,
            sdkAppID: 1_600_148_979,
            ownerUserID: "ios-master"
        )

        XCTAssertEqual(
            store.load(sdkAppID: 1_600_148_979, ownerUserID: "ios-master"),
            messages
        )
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
            store.load(sdkAppID: 1_600_148_979, ownerUserID: "ios-master"),
            [message]
        )
        XCTAssertFalse(FileManager.default.fileExists(atPath: legacyURL.path))

        let reopenedStore = LocalChatHistoryStore(baseDirectoryURL: directoryURL)
        XCTAssertEqual(
            reopenedStore.load(sdkAppID: 1_600_148_979, ownerUserID: "ios-master"),
            [message]
        )
    }

    private func makeTemporaryDirectoryURL() -> URL {
        FileManager.default.temporaryDirectory
            .appendingPathComponent("maichat-history-\(UUID().uuidString)", isDirectory: true)
    }
}
