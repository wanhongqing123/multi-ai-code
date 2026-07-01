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

    func testReceivesAICLIOutputWithoutProtocolLabel() throws {
        var state = MasterChatState(ownerUserID: "ios-master")

        let reply = state.receiveText(
            """
            【AICLI 输出】
            ## 结果

            - **SDK 层**：`sdk-ios`
            - **核心层**：`MediaPlayer`
            """,
            fromUserID: "mac-quark-pc",
            now: Date(timeIntervalSince1970: 121)
        )

        XCTAssertFalse(reply.text.contains("AICLI 输出"))
        XCTAssertTrue(reply.text.hasPrefix("## 结果"))
        XCTAssertTrue(reply.text.contains("- **SDK 层**"))
    }

    func testChatDetailSwipeBackOnlyAcceptsLeftEdgeRightDrag() {
        XCTAssertTrue(
            ChatDetailSwipeBackPolicy.shouldReturnToConversationList(
                startX: 12,
                translationWidth: 92,
                translationHeight: 10
            )
        )
        XCTAssertFalse(
            ChatDetailSwipeBackPolicy.shouldReturnToConversationList(
                startX: 54,
                translationWidth: 120,
                translationHeight: 8
            )
        )
        XCTAssertFalse(
            ChatDetailSwipeBackPolicy.shouldReturnToConversationList(
                startX: 12,
                translationWidth: 34,
                translationHeight: 6
            )
        )
        XCTAssertFalse(
            ChatDetailSwipeBackPolicy.shouldReturnToConversationList(
                startX: 12,
                translationWidth: 110,
                translationHeight: 96
            )
        )
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

    func testRemovesContactAndConversationHistory() throws {
        var state = MasterChatState(ownerUserID: "ios-master")
        try state.upsertFriend(userID: "mac-quark-pc")
        try state.upsertFriend(userID: "mac-apollo-u3player")

        state.selectPeer(userID: "mac-quark-pc")
        let quarkRequest = try state.queueOutgoingText("看一下 quark")
        let quarkReply = state.receiveText("quark 已处理", fromUserID: "mac-quark-pc")

        state.selectPeer(userID: "mac-apollo-u3player")
        let apolloRequest = try state.queueOutgoingText("看一下 apollo")

        state.removeContactAndMessages(userID: " mac-quark-pc ")

        XCTAssertEqual(state.contacts.map(\.userID), ["mac-apollo-u3player"])
        XCTAssertEqual(state.messages, [apolloRequest])
        XCTAssertEqual(state.messages(with: "mac-quark-pc"), [])
        XCTAssertFalse(state.messages.contains(quarkRequest))
        XCTAssertFalse(state.messages.contains(quarkReply))
        XCTAssertEqual(state.selectedPeerID, "mac-apollo-u3player")
    }

    func testRestoresPersistedConversationMessages() throws {
        let incoming = RemoteIMMessage(
            id: UUID(uuidString: "11111111-1111-1111-1111-111111111111")!,
            fromUserID: "mac-quark-pc",
            toUserID: "ios-master",
            text: "处理完成",
            direction: .incoming,
            status: .received,
            createdAt: Date(timeIntervalSince1970: 200)
        )
        let outgoing = RemoteIMMessage(
            id: UUID(uuidString: "22222222-2222-2222-2222-222222222222")!,
            fromUserID: "ios-master",
            toUserID: "mac-quark-pc",
            text: "继续看一下",
            direction: .outgoing,
            status: .sent,
            createdAt: Date(timeIntervalSince1970: 210)
        )

        let restored = MasterChatState(
            ownerUserID: " ios-master ",
            contacts: [
                RemoteIMContact(userID: "mac-quark-pc", displayName: "Quark PC")
            ],
            messages: [incoming, outgoing],
            selectedPeerID: "mac-quark-pc"
        )

        XCTAssertEqual(restored.ownerUserID, "ios-master")
        XCTAssertEqual(restored.messages(with: "mac-quark-pc"), [incoming, outgoing])
        XCTAssertEqual(restored.latestMessage(with: "mac-quark-pc"), outgoing)
        XCTAssertEqual(restored.selectedPeerID, "mac-quark-pc")
    }

    func testMessageHistoryRoundTripsThroughJSON() throws {
        let voiceMessage = RemoteIMMessage(
            id: UUID(uuidString: "33333333-3333-3333-3333-333333333333")!,
            fromUserID: "mac-quark-pc",
            toUserID: "ios-master",
            text: "[语音消息 4s]",
            voiceAttachment: RemoteIMVoiceAttachment(
                localFilePath: "/tmp/incoming-voice.m4a",
                durationSeconds: 4,
                remoteID: "voice-uuid"
            ),
            direction: .incoming,
            status: .received,
            createdAt: Date(timeIntervalSince1970: 220)
        )

        let data = try JSONEncoder().encode([voiceMessage])
        let decoded = try JSONDecoder().decode([RemoteIMMessage].self, from: data)

        XCTAssertEqual(decoded, [voiceMessage])
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
