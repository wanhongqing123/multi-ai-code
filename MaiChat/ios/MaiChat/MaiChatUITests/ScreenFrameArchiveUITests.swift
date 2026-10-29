import XCTest

final class ScreenFrameArchiveUITests: XCTestCase {
    @MainActor
    func testRealSamplerSelectionUsesOriginalFramesAndCleanupKeepsReferences() throws {
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
        let screen = app.frame
        let portrait = app.images["ai-screen-frame-large-image"].frame
        XCTAssertEqual(portrait.minX, screen.minX, accuracy: 1.5)
        XCTAssertEqual(portrait.maxX, screen.maxX, accuracy: 1.5)
        XCTAssertEqual(portrait.minY, screen.minY, accuracy: 1.5)
        XCTAssertEqual(portrait.maxY, screen.maxY, accuracy: 1.5,
                       "屏幕同纵横比截图应适配完整viewport，不缩进safeArea形成四边黑框")
        XCTAssertFalse(app.statusBars.firstMatch.exists, "原图已有状态栏，预览不应叠加第二个系统状态栏")
        XCTAssertTrue(app.buttons["ai-screen-frame-large-close"].isHittable)
        let fullImage = XCTAttachment(screenshot: app.screenshot())
        fullImage.name = "screen-archive-decoded-preview"
        fullImage.lifetime = .keepAlways
        add(fullImage)
        app.buttons["ai-screen-frame-large-close"].tap()
        app.buttons["ai-screen-frame-preview-2"].tap()
        XCTAssertTrue(app.images["ai-screen-frame-large-image"].waitForExistence(timeout: 15))
        let landscape = app.images["ai-screen-frame-large-image"].frame
        XCTAssertEqual(landscape.width, screen.width, accuracy: 1.5)
        XCTAssertEqual(landscape.width / landscape.height, 320.0 / 180.0, accuracy: 0.02,
                       "横图保留完整比例，不能裁图或拉伸以填满竖屏")
        let landscapeImage = XCTAttachment(screenshot: app.screenshot())
        landscapeImage.name = "screen-archive-landscape-fit-preview"
        landscapeImage.lifetime = .keepAlways
        add(landscapeImage)
        app.buttons["ai-screen-frame-large-close"].tap()
        app.buttons["ai-screen-frame-select-0"].tap()
        app.buttons["ai-screen-frames-actions"].tap()
        XCTAssertTrue(app.buttons["清理未选图片"].exists)
        XCTAssertFalse(app.buttons["清理未选图片"].isEnabled,
                       "尚未加入草稿的勾选图片不能被清理操作悄悄删除")
        app.scrollViews["ai-screen-frames-grid"]
            .coordinate(withNormalizedOffset: CGVector(dx: 0.05, dy: 0.9)).tap()
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
        app.buttons["清理未选图片"].tap()
        XCTAssertTrue(app.buttons["screen-frames-fixture-verify"].waitForExistence(timeout: 10))
        app.buttons["screen-frames-fixture-verify"].tap()
        XCTAssertTrue(app.staticTexts["PASS: 原图无重复、独立引用安全、未选清理及会话回收通过"]
            .waitForExistence(timeout: 15))
        #else
        throw XCTSkip("仅使用隔离模拟器和生成的安全画面，不操作真机")
        #endif
    }
}
