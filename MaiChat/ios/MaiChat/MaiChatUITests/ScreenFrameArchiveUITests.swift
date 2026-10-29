import XCTest

final class ScreenFrameArchiveUITests: XCTestCase {
    @MainActor
    func testRealSamplerSelectionStaysOnOriginalDraftAndDiscardKeepsCopies() throws {
        #if targetEnvironment(simulator)
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-screen-frames-ui-test"]
        app.launch()
        defer { app.terminate() }
        let prepare = app.buttons["screen-frames-fixture-prepare"]
        XCTAssertTrue(prepare.waitForExistence(timeout: 15))
        prepare.tap()
        XCTAssertTrue(app.staticTexts["采样已准备"].waitForExistence(timeout: 60))
        app.buttons["screen-frames-fixture-account"].tap()
        XCTAssertTrue(app.staticTexts["账号隔离通过"].waitForExistence(timeout: 15))

        app.buttons["screen-frames-fixture-open"].tap()
        XCTAssertTrue(app.staticTexts["ai-screen-frames-summary"].waitForExistence(timeout: 15))
        app.buttons["ai-screen-frame-preview-0"].tap()
        XCTAssertTrue(app.images["ai-screen-frame-large-image"].waitForExistence(timeout: 15),
                      "大图必须经过真实PNG验证和解码")
        let fullImage = XCTAttachment(screenshot: app.screenshot())
        fullImage.name = "screen-archive-decoded-preview"
        fullImage.lifetime = .keepAlways
        add(fullImage)
        app.buttons["ai-screen-frame-large-close"].tap()
        app.buttons["ai-screen-frame-select-0"].tap()
        app.buttons["ai-screen-frames-close"].tap()
        XCTAssertTrue(app.buttons["screen-frames-fixture-open-from-b"].waitForExistence(timeout: 10))

        // 关闭选择界面不丢档案，且从B打开仍只会写启动时的A草稿。
        app.buttons["screen-frames-fixture-open-from-b"].tap()
        XCTAssertTrue(app.buttons["ai-screen-frame-select-1"].waitForExistence(timeout: 15))
        app.buttons["ai-screen-frame-select-0"].tap()
        app.buttons["ai-screen-frame-select-1"].tap()
        XCTAssertEqual(app.staticTexts["ai-screen-frames-selection-count"].label, "已选 2 张")
        let grid = XCTAttachment(screenshot: app.screenshot())
        grid.name = "screen-archive-multiple-selection"
        grid.lifetime = .keepAlways
        add(grid)
        app.buttons["ai-screen-frames-add"].tap()
        XCTAssertTrue(app.staticTexts["A=2 B=0"].waitForExistence(timeout: 15))

        app.buttons["screen-frames-fixture-open"].tap()
        XCTAssertTrue(app.buttons["ai-screen-frames-actions"].waitForExistence(timeout: 10))
        app.buttons["ai-screen-frames-actions"].tap()
        app.buttons["清空本批"].tap()
        XCTAssertTrue(app.buttons["screen-frames-fixture-verify"].waitForExistence(timeout: 10))
        app.buttons["screen-frames-fixture-verify"].tap()
        XCTAssertTrue(app.staticTexts["PASS: 原会话2张、其他会话0张、档案已清空且副本可读"]
            .waitForExistence(timeout: 15))
        #else
        throw XCTSkip("仅使用隔离模拟器和生成的安全画面，不操作真机")
        #endif
    }
}
