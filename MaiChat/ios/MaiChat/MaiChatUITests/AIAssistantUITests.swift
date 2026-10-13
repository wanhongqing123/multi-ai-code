import XCTest

final class AIAssistantUITests: XCTestCase {
    func testTypingWhileAssistantStreamsRemainsResponsive() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-stream-typing-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let editor = app.descendants(matching: .any)
            .matching(identifier: "ai-composer").firstMatch
        XCTAssertTrue(editor.waitForExistence(timeout: 15))
        editor.tap()
        XCTAssertTrue(app.keyboards.firstMatch.waitForExistence(timeout: 5))
        let started = Date()
        editor.typeText("typing during streaming")
        XCTAssertLessThan(Date().timeIntervalSince(started), 15)
        XCTAssertTrue((editor.value as? String)?.contains("typing during streaming") == true)
    }

    func testEnteringLongAIConversationShowsLatestMessage() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-history-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let latest = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "历史消息 79")).firstMatch
        XCTAssertTrue(latest.waitForExistence(timeout: 15))
        let timeline = app.scrollViews.element(boundBy: 0)
        XCTAssertGreaterThanOrEqual(latest.frame.minY, timeline.frame.minY - 2,
                                    "The latest message must be inside the visible timeline")
        XCTAssertLessThanOrEqual(latest.frame.maxY, timeline.frame.maxY + 2)
        let editor = app.descendants(matching: .any)
            .matching(identifier: "ai-composer").firstMatch
        XCTAssertTrue(editor.waitForExistence(timeout: 5))
        editor.tap()
        XCTAssertTrue(app.keyboards.firstMatch.waitForExistence(timeout: 5))
        XCTAssertGreaterThanOrEqual(latest.frame.minY, timeline.frame.minY - 2)
        XCTAssertLessThanOrEqual(latest.frame.maxY, timeline.frame.maxY + 2,
                                 "Opening the keyboard must keep the latest message visible")
        timeline.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.2)).tap()
        let keyboardHidden = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "exists == false"),
            object: app.keyboards.firstMatch
        )
        XCTAssertEqual(XCTWaiter.wait(for: [keyboardHidden], timeout: 3), .completed)
        XCTAssertGreaterThanOrEqual(latest.frame.minY, timeline.frame.minY - 2)
        XCTAssertLessThanOrEqual(latest.frame.maxY, timeline.frame.maxY + 2,
                                 "Closing the keyboard must keep the latest message visible")
    }

    func testSwitchingConversationsOpensAtLatestMessage() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-session-switch-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        for title in ["B", "C"] {
            app.buttons["对话列表"].tap()
            let session = app.buttons["ai-conversation-switch-\(title)"]
            XCTAssertTrue(session.waitForExistence(timeout: 5))
            session.tap()
            let latest = app.staticTexts.matching(
                NSPredicate(format: "label CONTAINS %@", "\(title) 历史消息 79")).firstMatch
            XCTAssertTrue(latest.waitForExistence(timeout: 10))
            let hittable = XCTNSPredicateExpectation(
                predicate: NSPredicate(format: "hittable == true"), object: latest)
            XCTAssertEqual(XCTWaiter.wait(for: [hittable], timeout: 5), .completed)
            let timeline = app.scrollViews.element(boundBy: 0)
            XCTAssertGreaterThanOrEqual(latest.frame.minY, timeline.frame.minY - 2)
            XCTAssertLessThanOrEqual(latest.frame.maxY, timeline.frame.maxY + 2)
        }
    }

    func testLandscapeAssistantKeepsSessionSidebarBesideLatestMessage() throws {
        defer { XCUIDevice.shared.orientation = .portrait }
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-history-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        XCUIDevice.shared.orientation = .landscapeLeft
        let sidebar = app.descendants(matching: .any)
            .matching(identifier: "ai-session-drawer").firstMatch
        XCTAssertTrue(sidebar.waitForExistence(timeout: 15))
        let latest = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "历史消息 79")).firstMatch
        XCTAssertTrue(latest.waitForExistence(timeout: 15))
        XCTAssertLessThan(sidebar.frame.maxX, latest.frame.minX)
        XCTAssertTrue(latest.isHittable)
        let capture = XCTAttachment(screenshot: app.screenshot())
        capture.name = "ai-landscape-layout"
        capture.lifetime = .keepAlways
        add(capture)
    }

    func testLandscapeIMKeepsConversationListBesideChat() throws {
        defer { XCUIDevice.shared.orientation = .portrait }
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--im-landscape-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        XCUIDevice.shared.orientation = .landscapeLeft
        let conversation = app.buttons["im-conversation-im-test-peer"]
        let placeholder = app.descendants(matching: .any)
            .matching(identifier: "im-landscape-placeholder").firstMatch
        XCTAssertTrue(conversation.waitForExistence(timeout: 15))
        XCTAssertTrue(placeholder.waitForExistence(timeout: 15))
        conversation.tap()
        let menu = app.buttons["chatMoreMenu"]
        XCTAssertTrue(menu.waitForExistence(timeout: 15))
        XCTAssertTrue(conversation.isHittable)
        XCTAssertLessThan(conversation.frame.maxX, menu.frame.minX)
    }

    func testPortraitIMStillNavigatesIntoChat() throws {
        XCUIDevice.shared.orientation = .portrait
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--im-landscape-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let conversation = app.buttons["im-conversation-im-test-peer"]
        XCTAssertTrue(conversation.waitForExistence(timeout: 15))
        conversation.tap()
        XCTAssertTrue(app.buttons["chatMoreMenu"].waitForExistence(timeout: 15))
        XCTAssertFalse(conversation.isHittable)
    }

    func testToolCallsDoNotExposeReasoningContent() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--transcript-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let tools = app.buttons.matching(
            NSPredicate(format: "label CONTAINS %@", "工具调用 8 次"))
        XCTAssertFalse(app.buttons.matching(
            NSPredicate(format: "label CONTAINS %@", "思考过程")).firstMatch.exists)
        XCTAssertTrue(tools.firstMatch.waitForExistence(timeout: 15))
        XCTAssertEqual(tools.count, 1)
        XCTAssertFalse(app.staticTexts["检查视频"].exists)
        let answer = app.staticTexts["处理完成，结果已经准备好。"]
        XCTAssertTrue(answer.waitForExistence(timeout: 5))
        XCTAssertGreaterThan(answer.frame.minY, tools.firstMatch.frame.maxY)
    }

    func testVideoAttachmentAppearsAsPlayableMessageCard() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--video-bubble-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let bubble = app.buttons["agent-video-bubble"]
        XCTAssertTrue(bubble.waitForExistence(timeout: 15))
        XCTAssertTrue(app.staticTexts["ffplay-sample.mp4"].exists)
        bubble.tap()
        XCTAssertTrue(app.buttons["ffplay-close"].waitForExistence(timeout: 15))
        app.buttons["ffplay-close"].tap()
        XCTAssertTrue(bubble.waitForExistence(timeout: 3))
    }

    func testMentionedVideoFileOpensFromMessageBubble() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--text-media-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let bubble = app.buttons["agent-text-video-bubble"]
        XCTAssertTrue(bubble.waitForExistence(timeout: 15))
        XCTAssertTrue(bubble.label.contains("ffplay-sample.mp4"))
        XCTAssertGreaterThan(bubble.frame.height, 140,
                             "正文中的视频也应显示缩略图卡片，而不是一行文件名")
        bubble.tap()
        XCTAssertTrue(app.buttons["ffplay-close"].waitForExistence(timeout: 15))
        app.buttons["ffplay-close"].tap()
    }

    func testImagePreviewUsesTheWholeScreen() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-image-fullscreen-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let preview = app.images["agent-image-preview"]
        XCTAssertTrue(preview.waitForExistence(timeout: 15))
        XCTAssertTrue(app.buttons["关闭图片预览"].waitForExistence(timeout: 5))
        XCTAssertFalse(app.statusBars.firstMatch.exists,
                       "Full-screen image preview must hide the system status bar")
        let capture = XCTAttachment(screenshot: app.screenshot())
        capture.name = "ios-image-fullscreen"
        capture.lifetime = .keepAlways
        add(capture)
    }

    func testFfplayVideoPopupCanPauseSeekAndClose() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ffplay-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let pause = app.buttons["ffplay-pause"]
        XCTAssertTrue(pause.waitForExistence(timeout: 20))
        XCTAssertTrue(app.buttons["ffplay-share"].exists)
        XCTAssertTrue(app.buttons["ffplay-save"].exists)
        let speed = app.buttons["ffplay-speed"]
        XCTAssertTrue(speed.exists)
        speed.tap()
        XCTAssertTrue(app.buttons["ffplay-speed-0.5"].isHittable)
        XCTAssertFalse(app.alerts.firstMatch.exists)
        app.buttons["ffplay-speed-0.5"].tap()
        XCTAssertTrue(speed.label.contains("0.5"))
        pause.tap()
        XCTAssertTrue(app.buttons["ffplay-close"].isHittable)
        app.sliders["ffplay-seek"].adjust(toNormalizedSliderPosition: 0.5)
        let capture = XCTAttachment(screenshot: app.screenshot())
        capture.name = "ios-ffplay-metal-video"
        capture.lifetime = .keepAlways
        add(capture)
        app.buttons["ffplay-close"].tap()
        XCTAssertFalse(app.buttons["ffplay-close"].waitForExistence(timeout: 3))
    }

    func testAgentCanSeekPlayingVideoToAnAbsoluteSecond() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ffplay-seek-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let playbackTime = app.staticTexts["ffplay-time"]
        XCTAssertTrue(playbackTime.waitForExistence(timeout: 20))
        let ready = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label CONTAINS %@", " / 0:20"),
            object: playbackTime)
        XCTAssertEqual(XCTWaiter.wait(for: [ready], timeout: 15), .completed)
        app.buttons["agent-seek-six-seconds"].tap()
        XCTAssertTrue(app.buttons["定位已受理"].waitForExistence(timeout: 5))
        let sought = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label MATCHES %@", "0:0[6-8] / 0:20"),
            object: playbackTime)
        XCTAssertEqual(XCTWaiter.wait(for: [sought], timeout: 5), .completed,
                       "Agent seek must change the actual playback clock")
    }

    func testAgentCanOpenVideoFromAnAbsoluteSecond() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ffplay-start-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let playbackTime = app.staticTexts["ffplay-time"]
        XCTAssertTrue(playbackTime.waitForExistence(timeout: 20))
        let startedAtSix = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label MATCHES %@", "0:0[6-8] / 0:20"),
            object: playbackTime)
        XCTAssertEqual(XCTWaiter.wait(for: [startedAtSix], timeout: 15), .completed,
                       "start_seconds must set the initial playback position")
    }

    func testAgentOpenedDockedVideoCanReplayAndStartOver() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ffplay-agent-dock-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let result = app.staticTexts["agent-command-result"]
        XCTAssertTrue(result.waitForExistence(timeout: 20))
        let opened = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label == %@", "Agent 已打开视频"), object: result)
        XCTAssertEqual(XCTWaiter.wait(for: [opened], timeout: 15), .completed)
        let playbackTime = app.staticTexts["ffplay-time"]
        let advanced = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label MATCHES %@", "0:0[2-5] / 0:20"),
            object: playbackTime)
        XCTAssertEqual(XCTWaiter.wait(for: [advanced], timeout: 15), .completed)
        app.buttons["agent-replay"].tap()
        let replayed = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label MATCHES %@", "0:0[0-1] / 0:20"),
            object: playbackTime)
        XCTAssertEqual(XCTWaiter.wait(for: [replayed], timeout: 5), .completed)
        let advancedAgain = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label MATCHES %@", "0:0[2-5] / 0:20"),
            object: playbackTime)
        XCTAssertEqual(XCTWaiter.wait(for: [advancedAgain], timeout: 10), .completed)
        app.buttons["agent-start-over"].tap()
        let startedOver = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label MATCHES %@", "0:0[0-1] / 0:20"),
            object: playbackTime)
        XCTAssertEqual(XCTWaiter.wait(for: [startedOver], timeout: 5), .completed)
    }

    func testAgentOpenedDockedVideoCanChangeSoundTouchTempo() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ffplay-agent-dock-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let opened = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label == %@", "Agent 已打开视频"),
            object: app.staticTexts["agent-command-result"])
        XCTAssertEqual(XCTWaiter.wait(for: [opened], timeout: 20), .completed)
        let speed = app.buttons["ffplay-speed"]
        XCTAssertTrue(speed.waitForExistence(timeout: 15))
        app.buttons["agent-speed-half"].tap()
        let half = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label CONTAINS %@", "0.5"), object: speed)
        XCTAssertEqual(XCTWaiter.wait(for: [half], timeout: 10), .completed)
        app.buttons["agent-speed-double"].tap()
        let double = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label CONTAINS %@", "2.0"), object: speed)
        XCTAssertEqual(XCTWaiter.wait(for: [double], timeout: 10), .completed)
    }

    func testFfplaySaveKeepsPlayerAlive() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ffplay-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let save = app.buttons["ffplay-save"]
        XCTAssertTrue(save.waitForExistence(timeout: 20))
        save.tap()
        let status = app.staticTexts["ffplay-save-status"]
        XCTAssertTrue(status.waitForExistence(timeout: 20))
        XCTAssertEqual(status.label, "已保存到相册")
        XCTAssertTrue(app.buttons["ffplay-close"].exists,
                      "Saving must report a result without terminating the player")
    }

    func testComposerAcceptsTypingWithoutSendButton() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let editor = app.descendants(matching: .any).matching(identifier: "ai-composer").firstMatch
        XCTAssertTrue(editor.waitForExistence(timeout: 15))
        XCTAssertFalse(app.buttons["发送"].exists)
        editor.tap()
        editor.typeText("UI composer check")
        app.buttons["对话列表"].tap()
        XCTAssertTrue(app.staticTexts["对话"].waitForExistence(timeout: 3))
        app.buttons["完成"].tap()
        let capture = XCTAttachment(screenshot: app.screenshot())
        capture.name = "ios-ai-assistant"
        capture.lifetime = .keepAlways
        add(capture)
    }

    func testTappingConversationDismissesKeyboard() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()

        let editor = app.descendants(matching: .any).matching(identifier: "ai-composer").firstMatch
        XCTAssertTrue(editor.waitForExistence(timeout: 15))
        XCTAssertTrue(editor.label.contains("可按住转文字"))
        editor.tap()
        editor.typeText("dismiss keyboard")
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.3)).tap()

        let unfocused = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "hasKeyboardFocus == false"),
            object: editor
        )
        XCTAssertEqual(XCTWaiter.wait(for: [unfocused], timeout: 3), .completed)
    }

    func testAttachmentPanelMatchesMessageComposerActions() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()

        let more = app.buttons["展开更多功能"]
        XCTAssertTrue(more.waitForExistence(timeout: 15))
        more.tap()
        XCTAssertTrue(app.buttons["相册"].waitForExistence(timeout: 3))
        XCTAssertTrue(app.buttons["拍摄"].isHittable)
        XCTAssertTrue(app.buttons["文件"].isHittable)
        XCTAssertFalse(app.buttons["语音输入"].exists)

        app.buttons["收起更多功能"].tap()
        XCTAssertEqual(waitUntilNotHittable(app.buttons["相册"]), .completed)

        more.tap()
        XCTAssertTrue(app.buttons["相册"].waitForExistence(timeout: 3))
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.88, dy: 0.42)).tap()
        XCTAssertEqual(waitUntilNotHittable(app.buttons["相册"]), .completed)
    }

    func testModelChipOffersTextAndVisionModels() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()

        XCTAssertTrue(app.buttons["更多"].waitForExistence(timeout: 15))
        app.buttons["更多"].tap()
        let model = app.buttons["模型"]
        XCTAssertTrue(model.waitForExistence(timeout: 3))
        model.tap()
        let menu = app.descendants(matching: .any)
            .matching(identifier: "ai-model-menu").firstMatch
        XCTAssertTrue(menu.waitForExistence(timeout: 3))
        XCTAssertTrue(app.buttons["glm-5.3"].isHittable)
        XCTAssertTrue(app.buttons["glm-5.3-flash"].isHittable)
    }

    func testConversationDrawerContainsNewConversationAction() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()

        XCTAssertTrue(app.buttons["对话列表"].waitForExistence(timeout: 15))
        let swipeStart = app.coordinate(withNormalizedOffset: CGVector(dx: 0.01, dy: 0.5))
        let swipeEnd = app.coordinate(withNormalizedOffset: CGVector(dx: 0.72, dy: 0.5))
        swipeStart.press(forDuration: 0.05, thenDragTo: swipeEnd)
        XCTAssertTrue(app.staticTexts["对话"].waitForExistence(timeout: 3))
        XCTAssertTrue(app.buttons["新对话"].waitForExistence(timeout: 3))

        let drawer = app.descendants(matching: .any).matching(identifier: "ai-session-drawer").firstMatch
        XCTAssertTrue(drawer.waitForExistence(timeout: 3))
        drawer.swipeLeft()
        XCTAssertEqual(waitUntilNotHittable(app.buttons["新对话"]), .completed)

        app.buttons["对话列表"].tap()
        let firstSession = app.buttons.matching(
            NSPredicate(format: "identifier BEGINSWITH 'ai-conversation-'")
        ).firstMatch
        XCTAssertTrue(firstSession.waitForExistence(timeout: 3))
        firstSession.coordinate(withNormalizedOffset: CGVector(dx: 0.75, dy: 0.5)).tap()
        XCTAssertEqual(waitUntilNotHittable(app.buttons["新对话"]), .completed)

        app.buttons["对话列表"].tap()
        app.buttons["完成"].tap()
        XCTAssertEqual(waitUntilNotHittable(app.buttons["新对话"]), .completed)
    }

    func testPermissionUsesCompactMenuAndMoreOpensCustomModelSettings() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()

        XCTAssertTrue(app.buttons["更多"].waitForExistence(timeout: 15))
        app.buttons["更多"].tap()
        let permission = app.buttons["操作权限"]
        XCTAssertTrue(permission.waitForExistence(timeout: 3))
        permission.tap()

        let policyMenu = app.descendants(matching: .any)
            .matching(identifier: "ai-policy-menu").firstMatch
        XCTAssertTrue(policyMenu.waitForExistence(timeout: 3))
        XCTAssertLessThan(policyMenu.frame.width, app.frame.width * 0.75)
        app.buttons["完全访问"].tap()

        XCTAssertTrue(app.buttons["模型配置"].waitForExistence(timeout: 3))
        app.buttons["模型配置"].tap()

        let settings = app.descendants(matching: .any)
            .matching(identifier: "ai-model-settings").firstMatch
        XCTAssertTrue(settings.waitForExistence(timeout: 3))
        XCTAssertFalse(app.buttons["ai-primary-model-picker"].exists)
        XCTAssertFalse(app.textFields["ai-model-base-url"].exists)
        XCTAssertFalse(app.segmentedControls["ai-model-wire"].exists)
        XCTAssertTrue(app.textFields["ai-cloud-service-url"].exists)
        XCTAssertFalse(app.buttons["测试连接"].exists)
        XCTAssertFalse(app.staticTexts["DeepSeek API Key"].exists)
        XCTAssertTrue(app.buttons["关闭模型配置"].isHittable)
        let save = app.buttons["保存配置"]
        XCTAssertTrue(save.exists)
    }

    private func waitUntilNotHittable(_ element: XCUIElement) -> XCTWaiter.Result {
        let expectation = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "hittable == false"),
            object: element
        )
        return XCTWaiter.wait(for: [expectation], timeout: 3)
    }
}
