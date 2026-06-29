import XCTest
@testable import MultiAIIMCore

final class MasterChatStateTests: XCTestCase {
    func testMasterAddsSlaveAndQueuesOutgoingMessage() throws {
        var state = MasterChatState(ownerUserID: "ios-master")

        try state.upsertSlave(userID: "mac-quark-pc", displayName: "Quark PC")
        state.selectPeer(userID: "mac-quark-pc")
        let message = try state.queueOutgoingText("帮我看下构建失败", now: Date(timeIntervalSince1970: 100))

        XCTAssertEqual(state.contacts.map(\.userID), ["mac-quark-pc"])
        XCTAssertEqual(state.contacts.first?.relation, .slave)
        XCTAssertEqual(state.selectedPeerID, "mac-quark-pc")
        XCTAssertEqual(message.toUserID, "mac-quark-pc")
        XCTAssertEqual(message.direction, .outgoing)
        XCTAssertEqual(message.status, .pending)
        XCTAssertEqual(state.messages, [message])
    }

    func testReceivesMarkdownReplyFromSelectedSlave() throws {
        var state = MasterChatState(ownerUserID: "ios-master")
        try state.upsertSlave(userID: "mac-quark-pc", displayName: "Quark PC")
        state.selectPeer(userID: "mac-quark-pc")

        let reply = state.receiveText(
            """
            ## 结果

            | 文件 | 状态 |
            | --- | --- |
            | build.log | 失败 |
            """,
            fromUserID: "mac-quark-pc",
            now: Date(timeIntervalSince1970: 120)
        )

        XCTAssertEqual(reply.fromUserID, "mac-quark-pc")
        XCTAssertEqual(reply.direction, .incoming)
        XCTAssertEqual(reply.status, .received)
        XCTAssertTrue(reply.text.contains("| 文件 | 状态 |"))
        XCTAssertEqual(state.messages, [reply])
    }

    func testUpdatesQueuedMessageStatusAfterDelivery() throws {
        var state = MasterChatState(ownerUserID: "ios-master")
        try state.upsertSlave(userID: "mac-quark-pc", displayName: "Quark PC")
        let queued = try state.queueOutgoingText("跑一下测试")

        try state.updateMessageStatus(id: queued.id, status: .sent)

        XCTAssertEqual(state.messages.first?.status, .sent)
    }

    func testFiltersMessagesByConversationPeer() throws {
        var state = MasterChatState(ownerUserID: "ios-master")
        try state.upsertSlave(userID: "mac-quark-pc")
        try state.upsertSlave(userID: "mac-apollo-u3player")

        state.selectPeer(userID: "mac-quark-pc")
        let quarkRequest = try state.queueOutgoingText("看一下 quark")
        let quarkReply = state.receiveText("quark 已处理", fromUserID: "mac-quark-pc")

        state.selectPeer(userID: "mac-apollo-u3player")
        let apolloRequest = try state.queueOutgoingText("看一下 apollo")

        XCTAssertEqual(state.messages(with: "mac-quark-pc"), [quarkRequest, quarkReply])
        XCTAssertEqual(state.messages(with: "mac-apollo-u3player"), [apolloRequest])
        XCTAssertEqual(state.latestMessage(with: "mac-quark-pc"), quarkReply)
    }

    func testAddsFriendAndSlaveContactsWithRelation() throws {
        var state = MasterChatState(ownerUserID: "ios-master")

        try state.upsertFriend(userID: "ios-friend")
        try state.upsertSlave(userID: "mac-quark-pc")

        XCTAssertEqual(state.contacts.map(\.userID), ["ios-friend", "mac-quark-pc"])
        XCTAssertEqual(state.contacts.map(\.relation), [.friend, .slave])
    }

    func testDraftSubmitPolicyConsumesTrailingReturn() {
        XCTAssertNil(RemoteIMDraftSubmitPolicy.textByConsumingTrailingReturn(from: "hello"))
        XCTAssertEqual(
            RemoteIMDraftSubmitPolicy.textByConsumingTrailingReturn(from: "hello\n"),
            "hello"
        )
        XCTAssertEqual(
            RemoteIMDraftSubmitPolicy.textByConsumingTrailingReturn(from: "hello\r\n"),
            "hello"
        )
    }

    func testRejectsBlankSlaveAndBlankOutgoingMessage() {
        var state = MasterChatState(ownerUserID: "ios-master")

        XCTAssertThrowsError(try state.upsertSlave(userID: "   ", displayName: "Blank"))
        XCTAssertThrowsError(try state.queueOutgoingText("   "))
    }
}
