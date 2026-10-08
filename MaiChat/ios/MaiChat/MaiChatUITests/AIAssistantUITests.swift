import XCTest

final class AIAssistantUITests: XCTestCase {
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

    func testToolCallsAndReasoningCollapseWithoutMovingFinalAnswer() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--transcript-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let reasoning = app.buttons.matching(
            NSPredicate(format: "label CONTAINS %@", "思考过程"))
        let tools = app.buttons.matching(
            NSPredicate(format: "label CONTAINS %@", "工具调用 8 次"))
        XCTAssertTrue(reasoning.firstMatch.waitForExistence(timeout: 15))
        XCTAssertEqual(reasoning.count, 1)
        XCTAssertEqual(tools.count, 1)
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

    func testFfplayVideoPopupCanPauseSeekAndClose() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ffplay-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let pause = app.buttons["ffplay-pause"]
        XCTAssertTrue(pause.waitForExistence(timeout: 20))
        XCTAssertTrue(app.buttons["ffplay-share"].exists)
        XCTAssertTrue(app.buttons["ffplay-save"].exists)
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

    func testPermissionUsesCompactMenuAndMoreOpensCloudModelSettings() throws {
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
        let modelPicker = app.buttons["ai-primary-model-picker"]
        XCTAssertTrue(modelPicker.waitForExistence(timeout: 3))
        modelPicker.tap()
        let glm = app.buttons["GLM-5.3"]
        XCTAssertTrue(glm.waitForExistence(timeout: 3))
        glm.tap()
        XCTAssertFalse(app.textFields["ai-model-base-url"].exists)
        XCTAssertFalse(app.segmentedControls["ai-model-wire"].exists)
        modelPicker.tap()
        let deepSeek = app.buttons["DeepSeek V4.1 Flash"]
        XCTAssertTrue(deepSeek.waitForExistence(timeout: 3))
        deepSeek.tap()
        XCTAssertFalse(app.staticTexts["DeepSeek API Key"].exists)
        XCTAssertTrue(app.staticTexts[
            "Seedance、GLM、DeepSeek、Wan、Wan Workspace ID 和海螺密钥由云端服务统一同步。"
        ].waitForExistence(timeout: 3))
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
