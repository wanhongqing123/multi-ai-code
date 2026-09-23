import XCTest

final class AIAssistantUITests: XCTestCase {
    func testConversationStreamsAndStopsWithoutBlockingComposer() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let editor = app.descendants(matching: .any).matching(identifier: "ai-composer").firstMatch
        XCTAssertTrue(editor.waitForExistence(timeout: 15))
        editor.tap()
        editor.typeText("UI streaming check")
        app.buttons["发送"].tap()
        XCTAssertTrue(app.staticTexts["Mobile response ready"].waitForExistence(timeout: 15))
        XCTAssertTrue(app.buttons["发送"].waitForExistence(timeout: 5))
        XCTAssertTrue(app.staticTexts["Mobile response ready"].isHittable)
        editor.tap()
        editor.typeText("stop")
        app.buttons["发送"].tap()
        let stop = app.buttons["停止"]
        XCTAssertTrue(stop.waitForExistence(timeout: 5))
        stop.tap()
        XCTAssertTrue(app.buttons["发送"].waitForExistence(timeout: 8))
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
        XCTAssertTrue(app.descendants(matching: .any).matching(identifier: "ai-voice-input").firstMatch.exists)
        editor.tap()
        editor.typeText("dismiss keyboard")
        XCTAssertTrue(app.keyboards.firstMatch.waitForExistence(timeout: 3))

        app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.3)).tap()

        XCTAssertFalse(app.keyboards.firstMatch.waitForExistence(timeout: 2))
    }

    func testConversationDrawerContainsNewConversationAction() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()

        XCTAssertTrue(app.buttons["对话列表"].waitForExistence(timeout: 15))
        app.buttons["对话列表"].tap()

        XCTAssertTrue(app.staticTexts["对话"].waitForExistence(timeout: 3))
        XCTAssertTrue(app.buttons["新对话"].waitForExistence(timeout: 3))
        app.buttons["完成"].tap()
        XCTAssertFalse(app.buttons["新对话"].waitForExistence(timeout: 2))
    }
}
