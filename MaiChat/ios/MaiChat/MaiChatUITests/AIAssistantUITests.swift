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

    private func waitUntilNotHittable(_ element: XCUIElement) -> XCTWaiter.Result {
        let expectation = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "hittable == false"),
            object: element
        )
        return XCTWaiter.wait(for: [expectation], timeout: 3)
    }
}
