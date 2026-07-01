import XCTest
@testable import MultiAIIMCore

final class MasterChatStateTests: XCTestCase {
    func testAddsTrustedFriendAndQueuesOutgoingMessage() throws {
        var state = MasterChatState(ownerUserID: "ios-master")

        try state.upsertSlave(userID: "mac-quark-pc", displayName: "Quark PC")
        state.selectPeer(userID: "mac-quark-pc")
        let message = try state.queueOutgoingText("帮我看下构建失败", now: Date(timeIntervalSince1970: 100))

        XCTAssertEqual(state.contacts.map(\.userID), ["mac-quark-pc"])
        XCTAssertEqual(state.contacts.first?.relation, .friend)
        XCTAssertEqual(state.selectedPeerID, "mac-quark-pc")
        XCTAssertEqual(message.toUserID, "mac-quark-pc")
        XCTAssertEqual(message.direction, .outgoing)
        XCTAssertEqual(message.status, .pending)
        XCTAssertEqual(state.messages, [message])
    }

    func testReceivesMarkdownReplyFromSelectedFriend() throws {
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

    func testQueuesOutgoingVoiceMessageWithPlayableAttachment() throws {
        var state = MasterChatState(ownerUserID: "ios-master")
        try state.upsertFriend(userID: "mac-quark-pc")
        state.selectPeer(userID: "mac-quark-pc")

        let message = try state.queueOutgoingVoice(
            filePath: "/tmp/remote-im-voice.m4a",
            durationSeconds: 6,
            now: Date(timeIntervalSince1970: 130)
        )

        XCTAssertEqual(message.text, "[语音消息 6s]")
        XCTAssertEqual(message.voiceAttachment?.localFilePath, "/tmp/remote-im-voice.m4a")
        XCTAssertEqual(message.voiceAttachment?.durationSeconds, 6)
        XCTAssertEqual(message.direction, .outgoing)
        XCTAssertEqual(message.status, .pending)
        XCTAssertEqual(state.messages, [message])
    }

    func testReceivesVoiceMessageWithPlayableAttachment() throws {
        var state = MasterChatState(ownerUserID: "ios-master")

        let message = state.receiveVoice(
            filePath: "/tmp/incoming-voice.m4a",
            durationSeconds: 4,
            fromUserID: "mac-quark-pc",
            remoteID: "voice-uuid",
            now: Date(timeIntervalSince1970: 140)
        )

        XCTAssertEqual(message.text, "[语音消息 4s]")
        XCTAssertEqual(message.voiceAttachment?.localFilePath, "/tmp/incoming-voice.m4a")
        XCTAssertEqual(message.voiceAttachment?.durationSeconds, 4)
        XCTAssertEqual(message.voiceAttachment?.remoteID, "voice-uuid")
        XCTAssertEqual(message.direction, .incoming)
        XCTAssertEqual(message.status, .received)
        XCTAssertEqual(state.contacts.map(\.userID), ["mac-quark-pc"])
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

    func testLegacySlaveContactsAreStoredAsFriends() throws {
        var state = MasterChatState(ownerUserID: "ios-master")

        try state.upsertFriend(userID: "ios-friend")
        try state.upsertSlave(userID: "mac-quark-pc")

        XCTAssertEqual(state.contacts.map(\.userID), ["ios-friend", "mac-quark-pc"])
        XCTAssertEqual(state.contacts.map(\.relation), [.friend, .friend])
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

    func testDefaultCredentialMatchesDesktopPreset() {
        XCTAssertEqual(RemoteIMCredentialDefaults.sdkAppID, 1_600_148_979)
        XCTAssertEqual(
            RemoteIMCredentialDefaults.userSigSecretKey,
            "aa18d554f5e4a235640745e98145e187977f87770b812b2b4f10ef032bd73861"
        )
    }

    func testDefaultCredentialFillsMissingCredentialPartsAsAPair() {
        XCTAssertEqual(
            RemoteIMCredentialDefaults.resolvedCredential(sdkAppID: nil, secretKey: ""),
            RemoteIMCredential(
                sdkAppID: RemoteIMCredentialDefaults.sdkAppID,
                userSigSecretKey: RemoteIMCredentialDefaults.userSigSecretKey
            )
        )
        XCTAssertEqual(
            RemoteIMCredentialDefaults.resolvedCredential(sdkAppID: 123, secretKey: ""),
            RemoteIMCredential(
                sdkAppID: RemoteIMCredentialDefaults.sdkAppID,
                userSigSecretKey: RemoteIMCredentialDefaults.userSigSecretKey
            )
        )
    }

    func testDefaultCredentialIgnoresCachedOrCustomCredential() {
        XCTAssertEqual(
            RemoteIMCredentialDefaults.resolvedCredential(
                sdkAppID: 1_400_704_311,
                secretKey: "8b897045d1ee4f067a745b1b6a3fb834d1bd4c5951de43282c21b945f98ec982"
            ),
            RemoteIMCredential(
                sdkAppID: RemoteIMCredentialDefaults.sdkAppID,
                userSigSecretKey: RemoteIMCredentialDefaults.userSigSecretKey
            )
        )
        XCTAssertEqual(
            RemoteIMCredentialDefaults.resolvedCredential(sdkAppID: 123, secretKey: "custom-secret"),
            RemoteIMCredential(
                sdkAppID: RemoteIMCredentialDefaults.sdkAppID,
                userSigSecretKey: RemoteIMCredentialDefaults.userSigSecretKey
            )
        )
    }

    func testRejectsBlankContactAndBlankOutgoingMessage() {
        var state = MasterChatState(ownerUserID: "ios-master")

        XCTAssertThrowsError(try state.upsertSlave(userID: "   ", displayName: "Blank"))
        XCTAssertThrowsError(try state.queueOutgoingText("   "))
    }

    func testInitialLoginRequiresOnlyUserIDBecauseCredentialIsFixed() {
        XCTAssertFalse(
            RemoteIMLoginCredentialPolicy.isComplete(
                sdkAppIDText: "1600148979",
                userID: "",
                secretKey: RemoteIMCredentialDefaults.userSigSecretKey
            )
        )
        XCTAssertTrue(
            RemoteIMLoginCredentialPolicy.isComplete(
                sdkAppIDText: "",
                userID: "ios-owner",
                secretKey: RemoteIMCredentialDefaults.userSigSecretKey
            )
        )
        XCTAssertTrue(
            RemoteIMLoginCredentialPolicy.isComplete(
                sdkAppIDText: "1600148979",
                userID: "ios-owner",
                secretKey: ""
            )
        )
        XCTAssertTrue(
            RemoteIMLoginCredentialPolicy.isComplete(
                sdkAppIDText: "1600148979",
                userID: "ios-owner",
                secretKey: RemoteIMCredentialDefaults.userSigSecretKey
            )
        )
    }

    func testInitialLoginValidationExplainsWhyLoginCannotStart() {
        XCTAssertNil(
            RemoteIMLoginCredentialPolicy.validationError(
                sdkAppIDText: "",
                userID: "ios-owner",
                secretKey: RemoteIMCredentialDefaults.userSigSecretKey
            )
        )
        XCTAssertEqual(
            RemoteIMLoginCredentialPolicy.validationError(
                sdkAppIDText: "1600148979",
                userID: "",
                secretKey: RemoteIMCredentialDefaults.userSigSecretKey
            ),
            "请填写 UserID"
        )
        XCTAssertNil(
            RemoteIMLoginCredentialPolicy.validationError(
                sdkAppIDText: "1600148979",
                userID: "ios-owner",
                secretKey: ""
            )
        )
        XCTAssertNil(
            RemoteIMLoginCredentialPolicy.validationError(
                sdkAppIDText: "1600148979",
                userID: "ios-owner",
                secretKey: RemoteIMCredentialDefaults.userSigSecretKey
            )
        )
    }
}
