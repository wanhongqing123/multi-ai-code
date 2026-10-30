import XCTest

final class ScreenFrameArchiveUITests: XCTestCase {
    /// 本机假账号和模拟器合成帧验证 Agent 相机流程，不请求真实相机或模型服务。
    @MainActor
    func testAgentCameraAutomaticallySamplesAndAddsSelectionToOriginalDraft() async throws {
        #if DEBUG && targetEnvironment(simulator)
        continueAfterFailure = false
        let base = "http://127.0.0.1:18592"
        let (status, response) = try await URLSession.shared.data(from: URL(string: base + "/test/status")!)
        XCTAssertEqual((response as? HTTPURLResponse)?.statusCode, 200)
        let fixture = try XCTUnwrap(JSONSerialization.jsonObject(with: status) as? [String: Any])
        XCTAssertTrue((fixture["macSessions"] as? [String] ?? []).contains("ses_mock_mac"))
        let (certificate, _) = try await URLSession.shared.data(from: URL(string: base + "/test/cert")!)
        let app = XCUIApplication()
        app.launchArguments = ["--agent-camera-ui-test"]
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] = "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18592"
        app.launchEnvironment["MAICHAT_LOCAL_UI_SOCKET_PORT"] = "18593"
        app.launchEnvironment["MAICHAT_LOCAL_UI_MODEL_PORT"] = "18594"
        app.launchEnvironment["MAICHAT_LOCAL_UI_CA_BASE64"] = certificate.base64EncodedString()
        app.launch()
        defer { app.terminate() }
        let assistant = app.buttons["创作中心"]
        XCTAssertTrue(assistant.waitForExistence(timeout: 35))
        if app.buttons["稍后"].exists { app.buttons["稍后"].tap() }
        assistant.tap()
        let race = app.buttons["ai-camera-fixture-race"]
        XCTAssertTrue(race.waitForExistence(timeout: 35))
        race.tap()
        let raceResult = app.staticTexts["ai-camera-fixture-race-result"]
        XCTAssertTrue(raceResult.waitForExistence(timeout: 5))
        XCTAssertEqual(raceResult.label, "相机请求代次通过")
        let open = app.buttons["ai-camera-fixture-open"]
        XCTAssertTrue(open.waitForExistence(timeout: 35))
        open.tap()
        let stop = app.buttons["ai-camera-stop"]
        XCTAssertTrue(stop.waitForExistence(timeout: 15))
        let threeFrames = app.staticTexts.matching(NSPredicate(
            format: "label MATCHES %@", ".*已拍摄 ([3-9]|[1-9][0-9]+) 张"
        )).firstMatch
        XCTAssertTrue(threeFrames.waitForExistence(timeout: 15), "两秒采样应连续保存至少三张图片")
        let recording = XCTAttachment(screenshot: app.screenshot())
        recording.name = "agent-camera-three-frames"
        recording.lifetime = .keepAlways
        add(recording)
        stop.tap()
        XCTAssertTrue(app.buttons["ai-screen-frame-select-1"].waitForExistence(timeout: 15))
        let grid = XCTAttachment(screenshot: app.screenshot())
        grid.name = "agent-camera-stopped-grid"
        grid.lifetime = .keepAlways
        add(grid)
        app.buttons["ai-screen-frame-select-0"].tap()
        app.buttons["ai-screen-frames-add"].tap()
        XCTAssertTrue(app.scrollViews["ai-pending-attachments"].waitForExistence(timeout: 15),
                      "选择图片后应回到原会话草稿")
        let archivedAttachment = app.buttons.matching(NSPredicate(format: "identifier BEGINSWITH %@",
            "ai-remove-attachment-.maichat/screen-frame-archive/")).firstMatch
        XCTAssertTrue(archivedAttachment.exists, "草稿应引用归档原图")
        XCTAssertTrue(open.exists)
        #else
        throw XCTSkip("相机合成帧验收只在 Debug 模拟器运行")
        #endif
    }

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
