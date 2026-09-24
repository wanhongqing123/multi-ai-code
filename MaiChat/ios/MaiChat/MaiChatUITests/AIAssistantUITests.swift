import XCTest

final class AIAssistantUITests: XCTestCase {
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
    }

    func testModelChipOffersTextAndVisionModels() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()

        let model = app.buttons["切换模型"]
        XCTAssertTrue(model.waitForExistence(timeout: 15))
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

        let permission = app.buttons["操作权限"]
        XCTAssertTrue(permission.waitForExistence(timeout: 15))
        permission.tap()

        let policyMenu = app.descendants(matching: .any)
            .matching(identifier: "ai-policy-menu").firstMatch
        XCTAssertTrue(policyMenu.waitForExistence(timeout: 3))
        XCTAssertLessThan(policyMenu.frame.width, app.frame.width * 0.75)
        app.buttons["完全访问"].tap()

        app.buttons["更多"].tap()
        XCTAssertTrue(app.buttons["模型配置"].waitForExistence(timeout: 3))
        app.buttons["模型配置"].tap()

        let settings = app.descendants(matching: .any)
            .matching(identifier: "ai-model-settings").firstMatch
        XCTAssertTrue(settings.waitForExistence(timeout: 3))
        XCTAssertTrue(app.buttons["关闭模型配置"].isHittable)
        XCTAssertTrue(app.buttons["保存配置"].isHittable)
    }

    private func waitUntilNotHittable(_ element: XCUIElement) -> XCTWaiter.Result {
        let expectation = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "hittable == false"),
            object: element
        )
        return XCTWaiter.wait(for: [expectation], timeout: 3)
    }
}
