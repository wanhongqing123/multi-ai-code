import XCTest
@testable import MultiAIIMCore

final class LocalChatHistoryStoreTests: XCTestCase {
    func testPersistsHistoryPerSDKAppIDAndOwnerUserID() throws {
        let directoryURL = FileManager.default.temporaryDirectory
            .appendingPathComponent("multi-aiim-history-\(UUID().uuidString)", isDirectory: true)
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
    }
}
