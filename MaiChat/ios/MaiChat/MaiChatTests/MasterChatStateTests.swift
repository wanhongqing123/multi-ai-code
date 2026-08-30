import XCTest
@testable import MaiChatCore

final class MasterChatStateTests: XCTestCase {
    func testGitDiffDisplayPolicyOnlyRecognizesGeneratedHtmlArtifacts() {
        let diff = RemoteIMFileAttachment(
            localFilePath: "/tmp/repo.diff.html",
            fileName: "remote-im-diff-repo-\(String(repeating: "a", count: 64)).html",
            mimeType: "text/html"
        )
        let ordinary = RemoteIMFileAttachment(
            localFilePath: "/tmp/report.html",
            fileName: "report.html",
            mimeType: "text/html"
        )
        XCTAssertTrue(RemoteIMGitDiffDisplayPolicy.isGitDiff(diff))
        XCTAssertEqual(
            RemoteIMGitDiffDisplayPolicy.expectedSHA256(fileName: diff.fileName),
            String(repeating: "a", count: 64)
        )
        XCTAssertFalse(RemoteIMGitDiffDisplayPolicy.isGitDiff(ordinary))
    }

    func testNewMessageNotificationPolicyOnlySuppressesDuplicateOrVisibleForegroundConversation() {
        XCTAssertEqual(RemoteIMNewMessageNotificationPolicy.systemBadgeCount(totalUnreadCount: -1), 0)
        XCTAssertEqual(RemoteIMNewMessageNotificationPolicy.systemBadgeCount(totalUnreadCount: 0), 0)
        XCTAssertEqual(RemoteIMNewMessageNotificationPolicy.systemBadgeCount(totalUnreadCount: 58), 58)
        XCTAssertEqual(RemoteIMNewMessageNotificationPolicy.systemBadgeCount(totalUnreadCount: 1_608), 99)
        XCTAssertFalse(RemoteIMNewMessageNotificationPolicy.shouldNotify(
            wasInserted: false,
            isApplicationActive: false,
            visibleConversationUserID: nil,
            incomingUserID: "peer-a"
        ))
        XCTAssertFalse(RemoteIMNewMessageNotificationPolicy.shouldNotify(
            wasInserted: true,
            isApplicationActive: true,
            visibleConversationUserID: "peer-a",
            incomingUserID: "peer-a"
        ))
        XCTAssertTrue(RemoteIMNewMessageNotificationPolicy.shouldNotify(
            wasInserted: true,
            isApplicationActive: true,
            visibleConversationUserID: "peer-b",
            incomingUserID: "peer-a"
        ))
        XCTAssertTrue(RemoteIMNewMessageNotificationPolicy.shouldNotify(
            wasInserted: true,
            isApplicationActive: false,
            visibleConversationUserID: "peer-a",
            incomingUserID: "peer-a"
        ))
    }

    func testNewMessageNotificationPreviewDoesNotExposeAttachmentPath() {
        let image = RemoteIMMessage(
            fromUserID: "peer-a",
            toUserID: "ios-user",
            text: "[图片消息] private-photo.png",
            imageAttachment: RemoteIMImageAttachment(
                localFilePath: "/private/account/token/private-photo.png"
            ),
            direction: .incoming,
            status: .received,
            createdAt: Date(timeIntervalSince1970: 1)
        )
        let captioned = RemoteIMMessage(
            fromUserID: "peer-a",
            toUserID: "ios-user",
            text: "  请看这张图\n然后回复  ",
            imageAttachment: RemoteIMImageAttachment(localFilePath: "/tmp/photo.png"),
            direction: .incoming,
            status: .received,
            createdAt: Date(timeIntervalSince1970: 2)
        )
        let sensitiveFile = RemoteIMMessage(
            fromUserID: "peer-a",
            toUserID: "ios-user",
            text: "[文件消息] 2026年薪资表.xlsx",
            fileAttachment: RemoteIMFileAttachment(
                localFilePath: "/private/hr/2026年薪资表.xlsx",
                fileName: "2026年薪资表.xlsx",
                mimeType: "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"
            ),
            direction: .incoming,
            status: .received,
            createdAt: Date(timeIntervalSince1970: 3)
        )

        XCTAssertEqual(RemoteIMNewMessageNotificationPolicy.preview(for: image), "图片消息")
        XCTAssertFalse(RemoteIMNewMessageNotificationPolicy.preview(for: image).contains("/private"))
        XCTAssertEqual(
            RemoteIMNewMessageNotificationPolicy.preview(for: captioned),
            "请看这张图 然后回复"
        )
        XCTAssertEqual(RemoteIMNewMessageNotificationPolicy.preview(for: sensitiveFile), "文件消息")
        XCTAssertFalse(RemoteIMNewMessageNotificationPolicy.preview(for: sensitiveFile).contains("薪资"))
        XCTAssertEqual(
            RemoteIMNewMessageNotificationPolicy.aggregatedPreview(
                for: captioned,
                pendingCount: 3
            ),
            "3 条新消息：请看这张图 然后回复"
        )

        let longText = RemoteIMMessage(
            fromUserID: "peer-a",
            toUserID: "ios-user",
            text: String(repeating: "x", count: 200),
            direction: .incoming,
            status: .received,
            createdAt: Date(timeIntervalSince1970: 4)
        )
        let longPreview = RemoteIMNewMessageNotificationPolicy.preview(for: longText)
        XCTAssertEqual(longPreview.count, 81)
        XCTAssertTrue(longPreview.hasSuffix("…"))
    }
    func testExplicitRecipientSendDoesNotUseSelectedConversation() throws {
        var state = MasterChatState(ownerUserID: "ios-master")
        try state.upsertContact(userID: "alice", relation: .friend)
        try state.upsertContact(userID: "bob", relation: .friend)
        state.selectPeer(userID: "alice")

        try state.queueOutgoingText(to: "bob", text: "只发给 Bob")

        XCTAssertTrue(state.messages(with: "alice").isEmpty)
        XCTAssertEqual(state.messages(with: "bob").count, 1)
        XCTAssertEqual(state.messages(with: "bob").first?.toUserID, "bob")
    }

    func testBroadcastSelectionDeduplicatesAndTracksGroupTriState() {
        let contacts = [
            RemoteIMContact(userID: "alice", displayName: "Alice", groupName: "同事"),
            RemoteIMContact(userID: "amy", displayName: "Amy", groupName: "同事"),
            RemoteIMContact(userID: "bob", displayName: "Bob"),
        ]
        XCTAssertEqual(
            RemoteIMBroadcastSelectionPolicy.uniqueRecipientIDs(
                [" alice ", "", "bob", "alice", "  "]
            ),
            ["alice", "bob"]
        )
        var selected: Set<String> = ["alice", "bob"]
        XCTAssertEqual(
            RemoteIMBroadcastSelectionPolicy.groupState(
                groupName: "同事", contacts: contacts, selectedUserIDs: selected
            ),
            .partial
        )
        selected = RemoteIMBroadcastSelectionPolicy.settingGroup(
            groupName: "同事",
            contacts: contacts,
            selectedUserIDs: selected,
            selected: true
        )
        XCTAssertEqual(selected, ["alice", "amy", "bob"])
        XCTAssertEqual(
            RemoteIMBroadcastSelectionPolicy.groupState(
                groupName: "同事", contacts: contacts, selectedUserIDs: selected
            ),
            .all
        )
    }

    func testBroadcastDeliveryTrackerNamesEveryFailureWithoutSuccessNoise() {
        var tracker = RemoteIMBroadcastDeliveryTracker(total: 3)
        tracker.record(userID: "alice", succeeded: false)
        tracker.record(userID: "bob", succeeded: true)
        tracker.record(userID: " carol ", succeeded: false)

        XCTAssertEqual(tracker.total, 3)
        XCTAssertEqual(tracker.failedUserIDs, ["alice", "carol"])
    }

    func testBroadcastRecipientFilterDoesNotMutateSelectionAndKeepsGroupTotal() {
        let contacts = [
            RemoteIMContact(userID: "alice", displayName: "Alice", groupName: "同事"),
            RemoteIMContact(userID: "amy", displayName: "Amy", groupName: "同事"),
            RemoteIMContact(userID: "carol", displayName: "Carol", groupName: "家人"),
            RemoteIMContact(userID: "bob", displayName: "Bob"),
        ]
        var pickerState = RemoteIMBroadcastRecipientPickerState()
        pickerState.toggleGroup(groupName: "同事", contacts: contacts)
        pickerState.setFilterText("ali")

        let items = pickerState.visibleItems(
            groups: [
                RemoteIMContactGroup(name: "同事", sortOrder: 0),
                RemoteIMContactGroup(name: "家人", sortOrder: 1),
            ],
            contacts: contacts
        )

        XCTAssertEqual(pickerState.selectedUserIDs, ["alice", "amy"])
        XCTAssertEqual(items, [
            .group(name: "同事", memberCount: 2),
            .contact(contacts[0], indented: true),
        ])
        pickerState.setFilterText("")
        XCTAssertEqual(pickerState.selectedUserIDs, ["alice", "amy"])
    }

    func testContactGroupsRenameDeleteAndPreserveProfileAssignments() throws {
        var state = MasterChatState(ownerUserID: "ios-master")
        XCTAssertTrue(state.createContactGroup(name: " 同事 "))
        XCTAssertTrue(state.createContactGroup(name: "未分组"))
        XCTAssertFalse(state.createContactGroup(name: "同事"))
        XCTAssertFalse(state.createContactGroup(name: "  "))
        try state.upsertContact(userID: "mac-office", relation: .friend, displayName: "Mac")
        XCTAssertTrue(state.setContactGroup(userID: "mac-office", groupName: "同事"))

        try state.upsertContact(
            userID: "mac-office",
            relation: .friend,
            displayName: "办公室电脑",
            avatarURL: "https://example.com/avatar.png"
        )
        XCTAssertEqual(state.contacts.first?.groupName, "同事")
        XCTAssertTrue(state.renameContactGroup(from: "同事", to: "工作"))
        XCTAssertEqual(state.contacts.first?.groupName, "工作")
        XCTAssertFalse(state.renameContactGroup(from: "工作", to: "未分组"))
        XCTAssertTrue(state.deleteContactGroup(name: "工作"))
        XCTAssertEqual(state.contacts.first?.groupName, "")
        XCTAssertEqual(state.contactGroups.map(\.name), ["未分组"])
    }

    func testContactGroupInitializationSelfHealsDanglingAssignmentsAndSortsStably() {
        let state = MasterChatState(
            ownerUserID: "ios-master",
            contacts: [
                RemoteIMContact(userID: "a", displayName: "A", groupName: "幽灵组"),
                RemoteIMContact(userID: "b", displayName: "B", groupName: "同事"),
            ],
            contactGroups: [
                RemoteIMContactGroup(name: "家人", sortOrder: 2),
                RemoteIMContactGroup(name: "同事", sortOrder: 1),
            ],
            messages: []
        )

        XCTAssertEqual(state.contactGroups.map(\.name), ["同事", "家人"])
        XCTAssertEqual(state.contacts.first(where: { $0.userID == "a" })?.groupName, "")
        XCTAssertEqual(state.contacts.first(where: { $0.userID == "b" })?.groupName, "同事")
    }

    func testLegacyContactJSONDefaultsToUngrouped() throws {
        let data = Data(
            #"{"userID":"mac-office","displayName":"Mac","relation":"friend"}"#.utf8
        )
        let contact = try JSONDecoder().decode(RemoteIMContact.self, from: data)
        XCTAssertEqual(contact.groupName, "")
    }

    func testContactGroupDisplayKeepsEmptyGroupsAndSearchPiercesCollapse() {
        let groups = [
            RemoteIMContactGroup(name: "同事", sortOrder: 0),
            RemoteIMContactGroup(name: "空组", sortOrder: 1),
        ]
        let contacts = [
            RemoteIMContact(userID: "alice", displayName: "Alice", groupName: "同事"),
            RemoteIMContact(userID: "amy", displayName: "Amy", groupName: "同事"),
            RemoteIMContact(userID: "bob", displayName: "Bob"),
        ]

        XCTAssertEqual(
            RemoteIMContactGroupDisplayPolicy.items(
                groups: groups,
                contacts: contacts,
                collapsedGroupNames: ["同事"],
                query: ""
            ),
            [
                .group(name: "同事", memberCount: 2),
                .group(name: "空组", memberCount: 0),
                .contact(contacts[2], indented: false),
            ]
        )
        XCTAssertEqual(
            RemoteIMContactGroupDisplayPolicy.items(
                groups: groups,
                contacts: contacts,
                collapsedGroupNames: ["同事"],
                query: "ali"
            ),
            [
                .group(name: "同事", memberCount: 2),
                .contact(contacts[0], indented: true),
            ]
        )
    }

    func testApprovalRequestUsesVersionedActionsInsteadOfParsingMessageText() {
        let request = RemoteIMApprovalRequest(
            token: "approval-token-1",
            actions: [.approveOnce, .approvePrefix, .reject]
        )

        XCTAssertEqual(request?.actions.map(\.title), ["同意本次", "同意并记住", "拒绝"])
        XCTAssertEqual(RemoteIMApprovalAction.approveOnce.decisionDisplayText, "审批操作：同意本次")
        XCTAssertTrue(request?.allows(.approvePrefix) == true)
    }

    func testApprovalRequestRejectsInvalidTokensAndActionSets() {
        XCTAssertNil(RemoteIMApprovalRequest(token: "token-1", actions: [.approveOnce, .reject]))
        XCTAssertNil(RemoteIMApprovalRequest(token: "approval-", actions: [.approveOnce, .reject]))
        XCTAssertNil(RemoteIMApprovalRequest(token: "approval-good", actions: [.approveOnce]))
        XCTAssertNil(RemoteIMApprovalRequest(
            token: "approval-good",
            actions: [.approveOnce, .approveOnce, .reject]
        ))
        XCTAssertNil(RemoteIMApprovalRequest(
            token: "approval-good",
            actions: [.approveOnce, .reject, .autoDeclined]
        ))
    }

    func testIncomingApprovalRequestIsStoredOnTheMessage() {
        var state = MasterChatState(ownerUserID: "whq-iphone")
        let request = RemoteIMApprovalRequest(
            token: "approval-token-1",
            actions: [.approveOnce, .reject]
        )!
        let message = state.receiveText(
            "Codex 请求执行一条高风险命令",
            fromUserID: "mac-multi-ai-code",
            remoteID: "approval-message",
            approvalRequest: request,
            now: Date(timeIntervalSince1970: 91)
        )

        XCTAssertEqual(message.approvalRequest, request)
        XCTAssertEqual(state.messages.first?.approvalRequest, request)
    }

    func testOutgoingApprovalDecisionIsStoredOnTheMessage() throws {
        var state = MasterChatState(ownerUserID: "whq-iphone")
        try state.upsertContact(
            userID: "mac-multi-ai-code",
            relation: .friend,
            displayName: "Mac"
        )
        state.selectPeer(userID: "mac-multi-ai-code")

        let message = try state.queueOutgoingApprovalDecision(
            token: "approval-token-1",
            action: .approvePrefix,
            now: Date(timeIntervalSince1970: 92)
        )

        XCTAssertEqual(
            message.approvalDecision,
            RemoteIMApprovalDecision(token: "approval-token-1", action: .approvePrefix)
        )
        XCTAssertEqual(state.messages.first?.approvalDecision, message.approvalDecision)
    }

    func testApprovalCloudMetadataV2RoundTripsRequestsAndDecisions() {
        let request = RemoteIMApprovalRequest(
            token: "approval-wire-1",
            actions: [.approveOnce, .approvePrefix, .reject]
        )!
        let requestMetadata = RemoteIMCloudMetadata(
            origin: .machine,
            interaction: .approvalRequest(request)
        )
        let decisionMetadata = RemoteIMCloudMetadata(
            origin: .human,
            interaction: .approvalDecision(token: request.token, action: .approveOnce)
        )

        XCTAssertEqual(
            RemoteIMCloudMetadataCodec.decode(RemoteIMCloudMetadataCodec.encode(requestMetadata)),
            requestMetadata
        )
        XCTAssertEqual(
            RemoteIMCloudMetadataCodec.decode(RemoteIMCloudMetadataCodec.encode(decisionMetadata)),
            decisionMetadata
        )
        XCTAssertEqual(
            RemoteIMCloudMetadataCodec.decode(Data(
                #"{"namespace":"multi-ai-code","version":2,"origin":"machine","interaction":{"kind":"approval-request","token":"approval-wire-1","actions":["approve-once","approve-prefix","reject"]}}"#.utf8
            )),
            requestMetadata
        )
        XCTAssertEqual(
            RemoteIMCloudMetadataCodec.decode(Data(
                #"{"namespace":"multi-ai-code","version":2,"origin":"human","interaction":{"kind":"approval-decision","token":"approval-wire-1","action":"approve-once"}}"#.utf8
            )),
            decisionMetadata
        )
    }

    func testCaptionPlacementMetadataIsOptionalAndDoesNotBumpProtocolVersion() throws {
        let above = RemoteIMCloudMetadata(origin: .human, captionAbove: true)
        let aboveData = RemoteIMCloudMetadataCodec.encode(above)
        let aboveObject = try XCTUnwrap(
            JSONSerialization.jsonObject(with: aboveData) as? [String: Any]
        )
        XCTAssertEqual(aboveObject["version"] as? Int, 2)
        XCTAssertEqual(aboveObject["captionAbove"] as? Bool, true)
        XCTAssertEqual(RemoteIMCloudMetadataCodec.decode(aboveData), above)

        let defaultMetadata = RemoteIMCloudMetadata(origin: .human)
        let defaultData = RemoteIMCloudMetadataCodec.encode(defaultMetadata)
        let defaultObject = try XCTUnwrap(
            JSONSerialization.jsonObject(with: defaultData) as? [String: Any]
        )
        XCTAssertNil(defaultObject["captionAbove"])
        XCTAssertEqual(RemoteIMCloudMetadataCodec.decode(defaultData)?.captionAbove, false)
        XCTAssertEqual(
            RemoteIMCloudMetadataCodec.decode(Data(
                #"{"namespace":"multi-ai-code","version":2,"origin":"human"}"#.utf8
            ))?.captionAbove,
            false
        )
    }

    func testRemoteMessageDecodesLegacyHistoryWithoutCaptionPlacement() throws {
        let legacyJSON = #"{"id":"44444444-4444-4444-4444-444444444444","fromUserID":"mac","toUserID":"ios","text":"[图片消息] photo.png","direction":"incoming","status":"received","createdAt":0}"#
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .secondsSince1970
        let message = try decoder.decode(RemoteIMMessage.self, from: Data(legacyJSON.utf8))

        XCTAssertFalse(message.captionAbove)
        let encoded = try JSONEncoder().encode(message)
        let object = try XCTUnwrap(JSONSerialization.jsonObject(with: encoded) as? [String: Any])
        XCTAssertNil(object["captionAbove"])
    }

    func testApprovalCloudMetadataRejectsOldOrWrongDirectionProtocols() {
        XCTAssertNil(RemoteIMCloudMetadataCodec.decode(Data(
            #"{"namespace":"multi-ai-code","version":1,"origin":"machine"}"#.utf8
        )))
        XCTAssertNil(RemoteIMCloudMetadataCodec.decode(Data(
            #"{"namespace":"multi-ai-code","version":2,"origin":"human","interaction":{"kind":"approval-request","token":"approval-wire-1","actions":["approve-once","reject"]}}"#.utf8
        )))
        XCTAssertNil(RemoteIMCloudMetadataCodec.decode(Data(
            #"{"namespace":"multi-ai-code","version":2,"origin":"machine","interaction":{"kind":"approval-decision","token":"approval-wire-1","action":"approve-once"}}"#.utf8
        )))
    }

    func testApprovalResolvedMetadataBecomesAnAuthoritativeIncomingDecision() {
        let autoDeclined = RemoteIMCloudMetadataCodec.decode(Data(
            #"{"namespace":"multi-ai-code","version":2,"origin":"machine","interaction":{"kind":"approval-resolved","token":"approval-wire-1","outcome":"auto-declined"}}"#.utf8
        ))
        XCTAssertEqual(
            autoDeclined?.approvalDecision,
            RemoteIMApprovalDecision(token: "approval-wire-1", action: .autoDeclined)
        )
        XCTAssertNil(RemoteIMCloudMetadataCodec.decode(Data(
            #"{"namespace":"multi-ai-code","version":2,"origin":"human","interaction":{"kind":"approval-resolved","token":"approval-wire-1","outcome":"auto-declined"}}"#.utf8
        )))
    }

    func testPeerPolicyRejectsBlankAndCurrentLoginAccount() {
        XCTAssertFalse(RemoteIMPeerPolicy.isValidPeer(userID: "", ownerUserID: "whq-iphone"))
        XCTAssertFalse(
            RemoteIMPeerPolicy.isValidPeer(
                userID: " whq-iphone ",
                ownerUserID: "whq-iphone"
            )
        )
        XCTAssertTrue(
            RemoteIMPeerPolicy.isValidPeer(
                userID: "mac-multi-ai-code",
                ownerUserID: "whq-iphone"
            )
        )
    }

    func testInitializationDropsCurrentAccountContactAndSelfConversation() {
        let selfMessage = RemoteIMMessage(
            remoteID: "self-sync-message",
            fromUserID: "whq-iphone",
            toUserID: "whq-iphone",
            text: "这条是同账号多端同步，不是收到的消息",
            direction: .incoming,
            status: .received,
            createdAt: Date(timeIntervalSince1970: 100)
        )
        let peerMessage = RemoteIMMessage(
            remoteID: "peer-message",
            fromUserID: "mac-multi-ai-code",
            toUserID: "whq-iphone",
            text: "正常好友消息",
            direction: .incoming,
            status: .received,
            createdAt: Date(timeIntervalSince1970: 110)
        )

        let state = MasterChatState(
            ownerUserID: "whq-iphone",
            contacts: [
                RemoteIMContact(
                    userID: "whq-iphone",
                    displayName: "whq-iphone",
                    relation: .friend
                ),
                RemoteIMContact(
                    userID: "mac-multi-ai-code",
                    displayName: "mac-multi-ai-code",
                    relation: .friend
                ),
            ],
            messages: [selfMessage, peerMessage],
            selectedPeerID: "whq-iphone"
        )

        XCTAssertEqual(state.contacts.map(\.userID), ["mac-multi-ai-code"])
        XCTAssertEqual(state.messages, [peerMessage])
        XCTAssertEqual(state.selectedPeerID, "mac-multi-ai-code")
        XCTAssertTrue(state.messages(with: "whq-iphone").isEmpty)
    }

    func testCannotAddCurrentLoginAccountAsContact() {
        var state = MasterChatState(ownerUserID: "whq-iphone")

        XCTAssertThrowsError(try state.upsertFriend(userID: " whq-iphone ")) { error in
            XCTAssertEqual(error as? MasterChatStateError, .selfContactNotAllowed)
        }
        XCTAssertTrue(state.contacts.isEmpty)
    }

    func testSelfSyncedIncomingTextDoesNotCreateConversation() {
        var state = MasterChatState(ownerUserID: "whq-iphone")

        state.receiveText(
            "从另一台同账号设备同步过来的已发送消息",
            fromUserID: "whq-iphone",
            remoteID: "self-sync-message"
        )

        XCTAssertTrue(state.contacts.isEmpty)
        XCTAssertTrue(state.messages.isEmpty)
        XCTAssertNil(state.selectedPeerID)
    }

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
        XCTAssertEqual(reply.createdAt, Date(timeIntervalSince1970: 120))
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

    func testReceivesHiddenMarkedAICLIOutputWithoutProtocolMarker() throws {
        var state = MasterChatState(ownerUserID: "ios-master")
        let hiddenPrefix = "\u{2063}\u{200B}\u{200C}\u{200D}\u{2063}"

        let reply = state.receiveText(
            hiddenPrefix + "构建已通过",
            fromUserID: "mac-quark-pc",
            now: Date(timeIntervalSince1970: 122)
        )

        XCTAssertEqual(reply.text, "构建已通过")
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
        XCTAssertEqual(message.createdAt, Date(timeIntervalSince1970: 140))
        XCTAssertEqual(state.contacts.map(\.userID), ["mac-quark-pc"])
    }

    func testQueuesOutgoingImageMessageWithLocalAttachment() throws {
        var state = MasterChatState(ownerUserID: "ios-master")
        try state.upsertFriend(userID: "mac-quark-pc")
        state.selectPeer(userID: "mac-quark-pc")

        let message = try state.queueOutgoingImage(
            filePath: "/tmp/outgoing-photo.png",
            width: 640,
            height: 480,
            sizeBytes: 4096,
            now: Date(timeIntervalSince1970: 150)
        )

        XCTAssertEqual(message.text, "[图片消息] outgoing-photo.png")
        XCTAssertEqual(message.imageAttachment?.localFilePath, "/tmp/outgoing-photo.png")
        XCTAssertEqual(message.imageAttachment?.width, 640)
        XCTAssertEqual(message.imageAttachment?.height, 480)
        XCTAssertEqual(message.imageAttachment?.sizeBytes, 4096)
        XCTAssertEqual(message.direction, .outgoing)
        XCTAssertEqual(message.status, .pending)
        XCTAssertEqual(state.messages, [message])
    }

    func testReceivesImageMessageWithLocalAttachment() throws {
        var state = MasterChatState(ownerUserID: "ios-master")

        let message = state.receiveImage(
            filePath: "/tmp/incoming-photo.png",
            fromUserID: "mac-quark-pc",
            remoteID: "image-uuid",
            width: 320,
            height: 240,
            sizeBytes: 2048,
            now: Date(timeIntervalSince1970: 160)
        )

        XCTAssertEqual(message.text, "[图片消息] incoming-photo.png")
        XCTAssertEqual(message.imageAttachment?.localFilePath, "/tmp/incoming-photo.png")
        XCTAssertEqual(message.imageAttachment?.remoteID, "image-uuid")
        XCTAssertEqual(message.imageAttachment?.width, 320)
        XCTAssertEqual(message.imageAttachment?.height, 240)
        XCTAssertEqual(message.imageAttachment?.sizeBytes, 2048)
        XCTAssertEqual(message.direction, .incoming)
        XCTAssertEqual(message.status, .received)
        XCTAssertEqual(message.createdAt, Date(timeIntervalSince1970: 160))
        XCTAssertEqual(state.contacts.map(\.userID), ["mac-quark-pc"])
    }

    func testReceivesImageCaptionAndPreservesMetadataPlacement() throws {
        var state = MasterChatState(ownerUserID: "ios-master")

        let message = state.receiveImage(
            filePath: "/tmp/incoming-photo.png",
            fromUserID: "mac-quark-pc",
            remoteID: "image-caption-uuid",
            caption: "文字应该在图片上面",
            captionAbove: true,
            now: Date(timeIntervalSince1970: 161)
        )

        XCTAssertEqual(message.text, "文字应该在图片上面")
        XCTAssertTrue(message.captionAbove)
        XCTAssertNotNil(message.imageAttachment)
    }

    func testReceivesVideoProgressivelyWithoutDuplicatingMessage() throws {
        var state = MasterChatState(ownerUserID: "ios-master")

        let metadataMessage = state.receiveVideo(
            filePath: "/tmp/video-1.mp4",
            coverFilePath: nil,
            durationSeconds: 12,
            width: 1280,
            height: 720,
            sizeBytes: 4_096,
            fromUserID: "mac-quark-pc",
            remoteID: "video-uuid",
            now: Date(timeIntervalSince1970: 161)
        )
        let coverMessage = state.receiveVideo(
            filePath: "/tmp/video-1.mp4",
            coverFilePath: "/tmp/video-1.jpg",
            durationSeconds: 12,
            width: 1280,
            height: 720,
            sizeBytes: 4_096,
            fromUserID: "mac-quark-pc",
            remoteID: "video-uuid",
            now: Date(timeIntervalSince1970: 161)
        )

        XCTAssertEqual(state.messages.count, 1)
        XCTAssertEqual(metadataMessage.id, coverMessage.id)
        XCTAssertEqual(coverMessage.text, "[视频消息 12s]")
        XCTAssertEqual(coverMessage.videoAttachment?.localPath, "/tmp/video-1.mp4")
        XCTAssertEqual(coverMessage.videoAttachment?.coverPath, "/tmp/video-1.jpg")
        XCTAssertEqual(coverMessage.videoAttachment?.durationSeconds, 12)
        XCTAssertEqual(coverMessage.videoAttachment?.width, 1280)
        XCTAssertEqual(coverMessage.videoAttachment?.height, 720)
        XCTAssertEqual(coverMessage.videoAttachment?.sizeBytes, 4_096)
        XCTAssertEqual(state.message(remoteID: "video-uuid"), coverMessage)
    }

    func testQueuesOutgoingVideoWithCoverAndMetadata() throws {
        var state = MasterChatState(ownerUserID: "ios-master")
        try state.upsertFriend(userID: "mac-quark-pc")
        state.selectPeer(userID: "mac-quark-pc")

        let message = try state.queueOutgoingVideo(
            filePath: "/tmp/outgoing-video.mp4",
            coverPath: "/tmp/outgoing-video.jpg",
            durationSeconds: 9,
            width: 1920,
            height: 1080,
            sizeBytes: 12_345,
            now: Date(timeIntervalSince1970: 161.5)
        )

        XCTAssertEqual(message.text, "[视频消息 9s]")
        XCTAssertEqual(message.videoAttachment?.localPath, "/tmp/outgoing-video.mp4")
        XCTAssertEqual(message.videoAttachment?.coverPath, "/tmp/outgoing-video.jpg")
        XCTAssertEqual(message.videoAttachment?.durationSeconds, 9)
        XCTAssertEqual(message.videoAttachment?.width, 1920)
        XCTAssertEqual(message.videoAttachment?.height, 1080)
        XCTAssertEqual(message.videoAttachment?.sizeBytes, 12_345)
        XCTAssertEqual(message.direction, .outgoing)
        XCTAssertEqual(message.status, .pending)
        XCTAssertEqual(state.messages, [message])
    }

    func testVideoPreviewRequiresDownloadedLocalFile() throws {
        let fileURL = FileManager.default.temporaryDirectory
            .appendingPathComponent("maichat-video-\(UUID().uuidString).mp4")
        defer { try? FileManager.default.removeItem(at: fileURL) }
        let message = RemoteIMMessage(
            fromUserID: "mac-quark-pc",
            toUserID: "ios-master",
            text: "",
            videoAttachment: RemoteIMVideoAttachment(
                localPath: fileURL.path,
                durationSeconds: 5,
                width: 640,
                height: 360,
                sizeBytes: 3
            ),
            direction: .incoming,
            status: .received,
            createdAt: Date(timeIntervalSince1970: 162)
        )

        XCTAssertNil(RemoteIMVideoPreviewPolicy.previewItem(for: message))
        try Data([0, 1, 2]).write(to: fileURL)
        XCTAssertEqual(
            RemoteIMVideoPreviewPolicy.previewItem(for: message)?.localFilePath,
            fileURL.path
        )
    }

    func testReceivesMarkdownFileMessageWithLocalAttachment() throws {
        var state = MasterChatState(ownerUserID: "ios-master")

        let message = state.receiveFile(
            filePath: "/tmp/remote-im/report.md",
            fromUserID: "mac-quark-pc",
            fileName: "report.md",
            mimeType: "text/markdown",
            remoteID: "file-uuid",
            sizeBytes: 4096,
            now: Date(timeIntervalSince1970: 163)
        )

        XCTAssertEqual(message.text, "[文件消息] report.md")
        XCTAssertEqual(message.fileAttachment?.localFilePath, "/tmp/remote-im/report.md")
        XCTAssertEqual(message.fileAttachment?.fileName, "report.md")
        XCTAssertEqual(message.fileAttachment?.mimeType, "text/markdown")
        XCTAssertEqual(message.fileAttachment?.remoteID, "file-uuid")
        XCTAssertEqual(message.fileAttachment?.sizeBytes, 4096)
        XCTAssertTrue(message.isFileMessage)
        XCTAssertEqual(message.direction, .incoming)
        XCTAssertEqual(message.status, .received)
        XCTAssertEqual(message.createdAt, Date(timeIntervalSince1970: 163))
        XCTAssertEqual(state.contacts.map(\.userID), ["mac-quark-pc"])
    }

    func testQueuesOutgoingFileMessageWithLocalAttachment() throws {
        var state = MasterChatState(ownerUserID: "ios-master")
        try state.upsertFriend(userID: "mac-quark-pc")
        state.selectPeer(userID: "mac-quark-pc")

        let message = try state.queueOutgoingFile(
            filePath: "/tmp/report.pdf",
            fileName: "report.pdf",
            mimeType: "application/pdf",
            sizeBytes: 8192,
            now: Date(timeIntervalSince1970: 164)
        )

        XCTAssertEqual(message.text, "[文件消息] report.pdf")
        XCTAssertEqual(message.fileAttachment?.localFilePath, "/tmp/report.pdf")
        XCTAssertEqual(message.fileAttachment?.fileName, "report.pdf")
        XCTAssertEqual(message.fileAttachment?.mimeType, "application/pdf")
        XCTAssertEqual(message.fileAttachment?.sizeBytes, 8192)
        XCTAssertEqual(message.status, .pending)
    }

    func testClearsConversationMessagesWithoutRemovingContact() throws {
        var state = MasterChatState(ownerUserID: "ios-master")
        try state.upsertFriend(userID: "mac-quark-pc")
        state.selectPeer(userID: "mac-quark-pc")
        _ = try state.queueOutgoingText("hello")

        state.removeMessages(with: "mac-quark-pc")

        XCTAssertEqual(state.contacts.map(\.userID), ["mac-quark-pc"])
        XCTAssertEqual(state.selectedPeerID, "mac-quark-pc")
        XCTAssertTrue(state.messages.isEmpty)
    }

    func testDeduplicatesIncomingMessagesByRemoteID() {
        var state = MasterChatState(ownerUserID: "ios-master")

        let first = state.receiveText(
            "first",
            fromUserID: "mac-quark-pc",
            remoteID: "sdk-message-1",
            now: Date(timeIntervalSince1970: 165)
        )
        let duplicate = state.receiveText(
            "duplicate callback",
            fromUserID: "mac-quark-pc",
            remoteID: "sdk-message-1",
            now: Date(timeIntervalSince1970: 166)
        )

        XCTAssertEqual(first, duplicate)
        XCTAssertEqual(state.messages, [first])
    }

    func testAdoptsSDKReceiptForOutgoingMessage() throws {
        var state = MasterChatState(ownerUserID: "ios-master")
        try state.upsertFriend(userID: "mac-quark-pc")
        state.selectPeer(userID: "mac-quark-pc")
        let queued = try state.queueOutgoingText("hello")
        let serverDate = Date(timeIntervalSince1970: 167)

        try state.updateMessageDelivery(
            id: queued.id,
            remoteID: "sdk-message-2",
            createdAt: serverDate
        )

        XCTAssertEqual(state.messages.first?.remoteID, "sdk-message-2")
        XCTAssertEqual(state.messages.first?.createdAt, serverDate)
        XCTAssertEqual(state.messages.first?.status, .sent)
    }

    func testImagePreviewPolicyCreatesPreviewItemForImageMessage() {
        let messageID = UUID(uuidString: "22222222-2222-2222-2222-222222222222")!
        let message = RemoteIMMessage(
            id: messageID,
            fromUserID: "mac-quark-pc",
            toUserID: "ios-master",
            text: "[图片消息] incoming-photo.png",
            imageAttachment: RemoteIMImageAttachment(
                localFilePath: "/tmp/incoming-photo.png",
                remoteID: "image-uuid",
                width: 320,
                height: 240,
                sizeBytes: 2048
            ),
            direction: .incoming,
            status: .received,
            createdAt: Date(timeIntervalSince1970: 161)
        )

        let previewItem = RemoteIMImagePreviewPolicy.previewItem(for: message)

        XCTAssertEqual(previewItem?.id, messageID)
        XCTAssertEqual(previewItem?.localFilePath, "/tmp/incoming-photo.png")
    }

    func testImagePreviewPolicyIgnoresNonImageMessage() {
        let message = RemoteIMMessage(
            fromUserID: "mac-quark-pc",
            toUserID: "ios-master",
            text: "普通文本",
            direction: .incoming,
            status: .received,
            createdAt: Date(timeIntervalSince1970: 162)
        )

        XCTAssertNil(RemoteIMImagePreviewPolicy.previewItem(for: message))
    }

    func testPresenceStatusPolicyKeepsOnlyCurrentContacts() {
        let merged = RemoteIMPresenceStatusPolicy.merged(
            current: [
                "mac-online": .offline,
                "stale-contact": .online
            ],
            updates: [
                " mac-online ": .online,
                "mac-offline": .offline,
                "unknown-contact": .online,
                "": .online
            ],
            contactUserIDs: [
                "mac-online",
                "mac-offline"
            ]
        )

        XCTAssertEqual(merged, [
            "mac-online": .online,
            "mac-offline": .offline
        ])
        XCTAssertTrue(RemoteIMPresenceStatus.online.isOnline)
        XCTAssertFalse(RemoteIMPresenceStatus.offline.isOnline)
        XCTAssertFalse(RemoteIMPresenceStatus.unknown.isOnline)
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

    func testConversationMessagesAreReturnedChronologicallyAfterLiveUpdates() throws {
        var state = MasterChatState(ownerUserID: "ios-master")
        try state.upsertFriend(userID: "mac-quark-pc")
        state.selectPeer(userID: "mac-quark-pc")

        let newest = try state.queueOutgoingText(
            "最新消息",
            now: Date(timeIntervalSince1970: 300)
        )
        let oldest = state.receiveText(
            "最早消息",
            fromUserID: "mac-quark-pc",
            now: Date(timeIntervalSince1970: 100)
        )
        let middle = state.receiveText(
            "中间消息",
            fromUserID: "mac-quark-pc",
            now: Date(timeIntervalSince1970: 200)
        )

        XCTAssertEqual(state.messages(with: "mac-quark-pc"), [oldest, middle, newest])
        XCTAssertEqual(state.latestMessage(with: "mac-quark-pc"), newest)
        XCTAssertEqual(state.messageCount(with: "mac-quark-pc"), 3)
    }

    func testMergesPagedMessagesWithoutDuplicatingConversationSummary() {
        let summary = RemoteIMMessage(
            id: UUID(uuidString: "11111111-1111-1111-1111-111111111111")!,
            remoteID: "remote-summary",
            fromUserID: "mac-quark-pc",
            toUserID: "ios-master",
            text: "最新消息",
            direction: .incoming,
            status: .received,
            createdAt: Date(timeIntervalSince1970: 300)
        )
        let earlier = RemoteIMMessage(
            id: UUID(uuidString: "22222222-2222-2222-2222-222222222222")!,
            remoteID: "remote-earlier-page",
            fromUserID: "ios-master",
            toUserID: "mac-quark-pc",
            text: "更早消息",
            direction: .outgoing,
            status: .sent,
            createdAt: Date(timeIntervalSince1970: 100)
        )
        var state = MasterChatState(
            ownerUserID: "ios-master",
            contacts: [],
            messages: [summary]
        )

        state.mergeMessages([earlier, summary])

        XCTAssertEqual(state.messages(with: "mac-quark-pc"), [earlier, summary])
        XCTAssertEqual(state.messages.count, 2)
        XCTAssertEqual(state.contacts.map(\.userID), ["mac-quark-pc"])
    }

    func testHistoryMergeDoesNotRegressNewerInMemoryDeliveryState() throws {
        let messageID = UUID(uuidString: "33333333-3333-3333-3333-333333333333")!
        let stalePending = RemoteIMMessage(
            id: messageID,
            remoteID: "sdk-message",
            fromUserID: "ios-master",
            toUserID: "mac-quark-pc",
            text: "待发送",
            direction: .outgoing,
            status: .pending,
            createdAt: Date(timeIntervalSince1970: 200)
        )
        var currentSent = stalePending
        currentSent.status = .sent
        var state = MasterChatState(
            ownerUserID: "ios-master",
            contacts: [],
            messages: [currentSent]
        )

        state.mergeMessages([stalePending])

        XCTAssertEqual(state.messages(with: "mac-quark-pc"), [currentSent])
    }

    func testConversationIndexRemainsConsistentAfterDeliveryUpdatesAndRemoval() throws {
        var state = MasterChatState(ownerUserID: "ios-master")
        try state.upsertFriend(userID: "mac-quark-pc")
        try state.upsertFriend(userID: "mac-apollo-u3player")

        state.selectPeer(userID: "mac-quark-pc")
        let pending = try state.queueOutgoingText(
            "正在处理",
            now: Date(timeIntervalSince1970: 300)
        )
        let earlier = state.receiveText(
            "收到",
            fromUserID: "mac-quark-pc",
            remoteID: "remote-earlier",
            now: Date(timeIntervalSince1970: 100)
        )
        state.selectPeer(userID: "mac-apollo-u3player")
        let unrelated = try state.queueOutgoingText(
            "另一个会话",
            now: Date(timeIntervalSince1970: 200)
        )

        try state.updateMessageDelivery(
            id: pending.id,
            remoteID: "remote-pending",
            createdAt: Date(timeIntervalSince1970: 50)
        )

        XCTAssertEqual(state.messages(with: "mac-quark-pc").map(\.id), [pending.id, earlier.id])
        XCTAssertEqual(state.latestMessage(with: "mac-quark-pc")?.id, earlier.id)
        XCTAssertEqual(state.messageCount(with: "mac-quark-pc"), 2)
        XCTAssertEqual(state.messages(with: "mac-apollo-u3player"), [unrelated])

        state.removeMessages(with: "mac-quark-pc")

        XCTAssertEqual(state.messages(with: "mac-quark-pc"), [])
        XCTAssertEqual(state.messageCount(with: "mac-quark-pc"), 0)
        XCTAssertEqual(state.latestMessage(with: "mac-quark-pc"), nil)
        XCTAssertEqual(state.messages(with: "mac-apollo-u3player"), [unrelated])
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

    func testImageMessageHistoryRoundTripsThroughJSON() throws {
        let imageMessage = RemoteIMMessage(
            id: UUID(uuidString: "44444444-4444-4444-4444-444444444444")!,
            fromUserID: "mac-quark-pc",
            toUserID: "ios-master",
            text: "[图片消息] incoming-photo.png",
            imageAttachment: RemoteIMImageAttachment(
                localFilePath: "/tmp/incoming-photo.png",
                remoteID: "image-uuid",
                width: 320,
                height: 240,
                sizeBytes: 2048
            ),
            direction: .incoming,
            status: .received,
            createdAt: Date(timeIntervalSince1970: 230)
        )

        let data = try JSONEncoder().encode([imageMessage])
        let decoded = try JSONDecoder().decode([RemoteIMMessage].self, from: data)

        XCTAssertEqual(decoded, [imageMessage])
    }

    func testLegacySlaveContactsAreStoredAsFriends() throws {
        var state = MasterChatState(ownerUserID: "ios-master")

        try state.upsertFriend(userID: "ios-friend")
        try state.upsertSlave(userID: "mac-quark-pc")

        XCTAssertEqual(state.contacts.map(\.userID), ["ios-friend", "mac-quark-pc"])
        XCTAssertEqual(state.contacts.map(\.relation), [.friend, .friend])
    }

    func testAvatarMonogramPrefersNicknameAndFallsBackToUserID() {
        XCTAssertEqual(
            RemoteIMAvatarMonogramPolicy.text(
                displayName: "iPhone User",
                userID: "whq-iphone"
            ),
            "IU"
        )
        XCTAssertEqual(
            RemoteIMAvatarMonogramPolicy.text(
                displayName: "钟颖娟",
                userID: "whq-iphone"
            ),
            "颖娟"
        )
        XCTAssertEqual(
            RemoteIMAvatarMonogramPolicy.text(
                displayName: "whq-iphone",
                userID: "whq-iphone"
            ),
            "W"
        )
    }

    func testContactProfileUpdateKeepsUsefulMetadataWhenFallbackIsEmpty() throws {
        var state = MasterChatState(ownerUserID: "ios-master")
        try state.upsertContact(
            userID: "whq-iphone",
            relation: .friend,
            displayName: "iPhone User",
            avatarURL: "https://example.com/avatar.png"
        )

        try state.upsertContact(
            userID: "whq-iphone",
            relation: .friend,
            displayName: "whq-iphone",
            avatarURL: ""
        )

        XCTAssertEqual(state.contacts.first?.displayName, "iPhone User")
        XCTAssertEqual(state.contacts.first?.avatarURL, "https://example.com/avatar.png")
    }

    func testContactAvatarURLRoundTripsThroughJSON() throws {
        let contact = RemoteIMContact(
            userID: "whq-iphone",
            displayName: "iPhone User",
            avatarURL: "https://example.com/avatar.png"
        )

        let data = try JSONEncoder().encode(contact)
        let decoded = try JSONDecoder().decode(RemoteIMContact.self, from: data)

        XCTAssertEqual(decoded, contact)
    }

    func testDraftSubmitPolicyOnlySubmitsSingleReturnEvents() {
        XCTAssertTrue(RemoteIMDraftSubmitPolicy.shouldSubmit(replacementText: "\n"))
        XCTAssertTrue(RemoteIMDraftSubmitPolicy.shouldSubmit(replacementText: "\r\n"))
        XCTAssertFalse(RemoteIMDraftSubmitPolicy.shouldSubmit(replacementText: "hello"))
        XCTAssertFalse(RemoteIMDraftSubmitPolicy.shouldSubmit(replacementText: "\n\n"))
        XCTAssertFalse(
            RemoteIMDraftSubmitPolicy.shouldSubmit(
                replacementText: "## 标题\n\n- 第一项\n- 第二项\n"
            )
        )
    }

    func testTimestampTextMatchesDesktopDateRules() {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = TimeZone(secondsFromGMT: 8 * 60 * 60)!

        let now = calendar.date(from: DateComponents(
            year: 2026,
            month: 7,
            day: 6,
            hour: 10,
            minute: 0
        ))!
        let today = calendar.date(from: DateComponents(
            year: 2026,
            month: 7,
            day: 6,
            hour: 16,
            minute: 13
        ))!
        let yesterday = calendar.date(from: DateComponents(
            year: 2026,
            month: 7,
            day: 5,
            hour: 23,
            minute: 53
        ))!
        let older = calendar.date(from: DateComponents(
            year: 2026,
            month: 7,
            day: 4,
            hour: 14,
            minute: 18
        ))!

        XCTAssertEqual(
            RemoteIMTimestampTextPolicy.displayText(for: today, now: now, calendar: calendar),
            "16:13"
        )
        XCTAssertEqual(
            RemoteIMTimestampTextPolicy.displayText(for: yesterday, now: now, calendar: calendar),
            "昨天 23:53"
        )
        XCTAssertEqual(
            RemoteIMTimestampTextPolicy.displayText(for: older, now: now, calendar: calendar),
            "7 月 4 日 14:18"
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
            "请填写账号 ID"
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

    func testSavedAccountRestoresMainInterfaceAfterProcessRelaunch() {
        XCTAssertTrue(
            RemoteIMLoginCredentialPolicy.shouldRestoreSavedSession(userID: "ios-owner")
        )
        XCTAssertTrue(
            RemoteIMLoginCredentialPolicy.shouldRestoreSavedSession(userID: "  ios-owner  ")
        )
        XCTAssertFalse(
            RemoteIMLoginCredentialPolicy.shouldRestoreSavedSession(userID: "   ")
        )
        XCTAssertTrue(
            RemoteIMLoginCredentialPolicy.migratedReconnectOnLaunch(
                storedValue: nil,
                userID: "legacy-owner"
            )
        )
        XCTAssertFalse(
            RemoteIMLoginCredentialPolicy.shouldAutoConnectSavedSession(
                userID: "ios-owner",
                reconnectOnLaunch: false
            )
        )
        XCTAssertTrue(
            RemoteIMLoginCredentialPolicy.shouldAutoConnectSavedSession(
                userID: "ios-owner",
                reconnectOnLaunch: true
            )
        )
        XCTAssertFalse(
            RemoteIMLoginCredentialPolicy.shouldAutoConnectSavedSession(
                userID: "   ",
                reconnectOnLaunch: true
            )
        )
    }

    func testMessageListAutoScrollPolicyTargetsLatestMessageID() {
        let firstID = UUID(uuidString: "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa")!
        let latestID = UUID(uuidString: "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb")!
        let first = RemoteIMMessage(
            id: firstID,
            fromUserID: "ios-master",
            toUserID: "mac-quark-pc",
            text: "第一条",
            direction: .outgoing,
            status: .sent,
            createdAt: Date(timeIntervalSince1970: 100)
        )
        let latest = RemoteIMMessage(
            id: latestID,
            fromUserID: "mac-quark-pc",
            toUserID: "ios-master",
            text: "最新回复",
            direction: .incoming,
            status: .received,
            createdAt: Date(timeIntervalSince1970: 120)
        )

        XCTAssertEqual(
            MessageListAutoScrollPolicy.latestMessageID(from: [first, latest]),
            latestID
        )
    }

    func testMessageListAutoScrollPolicyIgnoresEmptyMessages() {
        XCTAssertNil(MessageListAutoScrollPolicy.latestMessageID(from: []))
    }

    func testVideoDownloadTrackingFollowsMetadataReadyAndFailureStages() {
        let key = "video-1"
        let downloading = RemoteIMVideoDownloadTrackingPolicy.updatedKeys(
            current: [],
            key: key,
            stage: .metadata,
            fileIsUsable: false
        )
        XCTAssertEqual(downloading, [key])
        XCTAssertEqual(
            RemoteIMVideoDownloadTrackingPolicy.updatedKeys(
                current: downloading,
                key: key,
                stage: .coverReady,
                fileIsUsable: false
            ),
            downloading
        )
        XCTAssertEqual(
            RemoteIMVideoDownloadTrackingPolicy.updatedKeys(
                current: downloading,
                key: key,
                stage: .videoReady,
                fileIsUsable: true
            ),
            []
        )
        XCTAssertEqual(
            RemoteIMVideoDownloadTrackingPolicy.updatedKeys(
                current: downloading,
                key: key,
                stage: .videoFailed,
                fileIsUsable: false
            ),
            []
        )
        XCTAssertEqual(
            RemoteIMVideoDownloadTrackingPolicy.updatedKeys(
                current: [],
                key: key,
                stage: .metadata,
                fileIsUsable: true
            ),
            []
        )
    }

    func testMessageCopyPolicyProvidesSelectableTextAndCompleteMetadata() {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = TimeZone(secondsFromGMT: 0)!
        let message = RemoteIMMessage(
            fromUserID: "mac-quark-pc",
            toUserID: "whq-iphone",
            text: "构建已经完成。",
            fileAttachment: RemoteIMFileAttachment(
                localFilePath: "/tmp/report.md",
                fileName: "report.md",
                mimeType: "text/markdown",
                sizeBytes: 2048
            ),
            direction: .incoming,
            status: .received,
            createdAt: Date(timeIntervalSince1970: 1_750_000_000)
        )

        XCTAssertEqual(RemoteIMMessageCopyPolicy.selectionText(for: message), "构建已经完成。")
        let fullText = RemoteIMMessageCopyPolicy.fullText(for: message, calendar: calendar)
        XCTAssertTrue(fullText.contains("发送人：mac-quark-pc"))
        XCTAssertTrue(fullText.contains("接收人：whq-iphone"))
        XCTAssertTrue(fullText.contains("方向：收到"))
        XCTAssertTrue(fullText.contains("状态：已接收"))
        XCTAssertTrue(fullText.contains("类型：文件"))
        XCTAssertTrue(fullText.contains("构建已经完成。"))
        XCTAssertTrue(fullText.contains("附件：report.md，text/markdown，2048 字节"))
    }

    func testMessageCopyPolicyPreservesMarkdownSource() {
        let markdown = """
        ## 标题

        **加粗**、`code` 和 [链接](https://example.com)

        ```swift
        print("hello")
        ```
        """
        let message = RemoteIMMessage(
            fromUserID: "mac-quark-pc",
            toUserID: "whq-iphone",
            text: markdown,
            direction: .incoming,
            status: .received,
            createdAt: Date(timeIntervalSince1970: 100)
        )

        XCTAssertEqual(RemoteIMMessageCopyPolicy.selectionText(for: message), markdown)
    }

    func testMessageCopyPolicyDescribesAttachmentOnlyMessages() {
        let message = RemoteIMMessage(
            fromUserID: "whq-iphone",
            toUserID: "mac-quark-pc",
            text: "",
            imageAttachment: RemoteIMImageAttachment(
                localFilePath: "/tmp/screenshot.png",
                width: 1170,
                height: 2532
            ),
            direction: .outgoing,
            status: .sent,
            createdAt: Date(timeIntervalSince1970: 100)
        )

        XCTAssertEqual(
            RemoteIMMessageCopyPolicy.selectionText(for: message),
            "[图片消息] screenshot.png"
        )
    }
}
