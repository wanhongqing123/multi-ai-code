import XCTest

/// Explicitly opt-in, disposable-account, real auth + private OSS canary.
/// The launcher stages a 0600 one-time credential file in a dedicated simulator.
/// This suite is skipped by every ordinary CI/test invocation.
final class CloudDriveProductionCanaryUITests: XCTestCase {
    private func requireOptIn() throws {
        guard ProcessInfo.processInfo.environment["MAICHAT_RUN_CLOUD_PROD_CANARY"] == "YES"
        else { throw XCTSkip("真实 OSS 云盘用例需要显式开启") }
    }

    private func expectedAccountID(_ role: String) throws -> String {
        let key = "MAICHAT_CLOUD_CANARY_\(role)_USER_ID"
        let value = try XCTUnwrap(ProcessInfo.processInfo.environment[key])
        XCTAssertFalse(value.isEmpty)
        return value
    }

    @MainActor private func openPersonalPage(_ app: XCUIApplication,
                                             accountID: String) -> Bool {
        let me = app.buttons["我"]
        guard me.waitForExistence(timeout: 40) else { return false }
        for _ in 0..<8 {
            let profile = app.buttons["personal-profile-entry"]
            if profile.waitForExistence(timeout: 4),
               profile.label.contains(accountID) ||
                    app.staticTexts[accountID].exists {
                return true
            }
            if (me.value as? String) != "已选择" { me.tap() }
        }
        return false
    }

    @MainActor private func dismissNotificationPrompt(_ app: XCUIApplication) {
        let later = app.buttons["稍后"]
        if later.waitForExistence(timeout: 2) { later.tap() }
    }

    @MainActor func testOwnerNormalAppUploadPlayDelete() throws {
        try requireOptIn()
        let accountID = try expectedAccountID("A")
        let app = XCUIApplication()
        app.launchArguments = ["--cloud-drive-production-canary-normal",
                               "--cloud-drive-ui-test"]
        app.launchEnvironment["MAICHAT_PROD_SYNC_TEST_ACCOUNT"] = "canary"
        app.launch()
        defer { app.terminate() }
        guard openPersonalPage(app, accountID: accountID) else {
            let image = XCTAttachment(screenshot: app.screenshot())
            image.name = "cloud-drive-canary-owner-account-gate"
            image.lifetime = .keepAlways
            add(image)
            XCTFail("A 的个人页未显示预期测试账号")
            return
        }
        dismissNotificationPrompt(app)
        let entry = app.buttons["my-cloud-drive-entry"]
        guard entry.waitForExistence(timeout: 20) else {
            XCTFail("A 的云盘入口未出现")
            return
        }
        entry.tap()
        XCTAssertTrue(app.staticTexts["云盘空间"].waitForExistence(timeout: 20))
        guard app.staticTexts["还没有图片或视频"].waitForExistence(timeout: 20) else {
            XCTFail("灰度 A 必须使用空的专用账号")
            return
        }
        let upload = app.buttons["cloud-drive-upload-sample"]
        XCTAssertTrue(upload.waitForExistence(timeout: 10))
        upload.tap()
        let video = app.buttons.matching(NSPredicate(
            format: "identifier BEGINSWITH %@", "cloud-drive-play-")).firstMatch
        XCTAssertTrue(video.waitForExistence(timeout: 120), "真实 OSS 上传未完成")
        let videoID = String(video.identifier.dropFirst("cloud-drive-play-".count))
        XCTAssertFalse(videoID.isEmpty)
        let uploaded = XCTAttachment(screenshot: app.screenshot())
        uploaded.name = "cloud-drive-canary-owner-uploaded"
        uploaded.lifetime = .keepAlways
        add(uploaded)
        video.tap()
        let close = app.buttons["ffplay-close"]
        XCTAssertTrue(close.waitForExistence(timeout: 60), "真实视频未进入播放器")
        close.tap()
        let menu = app.buttons["cloud-drive-video-menu-\(videoID)"]
        XCTAssertTrue(menu.waitForExistence(timeout: 10))
        menu.tap()
        app.buttons["删除"].tap()
        app.buttons["删除文件"].tap()
        XCTAssertTrue(app.staticTexts["还没有图片或视频"].waitForExistence(timeout: 30),
                      "删除后云盘仍显示测试视频")
    }

    @MainActor func testOwnerAgentTools() throws {
        try requireOptIn()
        let accountID = try expectedAccountID("A")
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--cloud-drive-ui-test",
                               "--cloud-drive-production-canary-tools"]
        app.launchEnvironment["MAICHAT_PROD_SYNC_TEST_ACCOUNT"] = "canary"
        app.launch()
        defer { app.terminate() }
        guard app.staticTexts["云盘灰度账号已登录"]
            .waitForExistence(timeout: 40),
              app.staticTexts[accountID].waitForExistence(timeout: 10) else {
            XCTFail("云盘 Agent 灰度账号 A 未登录")
            return
        }
        let run = app.buttons["cloud-drive-production-tool-run"]
        XCTAssertTrue(run.waitForExistence(timeout: 15))
        run.tap()
        XCTAssertTrue(app.staticTexts["云盘真实工具八项通过"]
            .waitForExistence(timeout: 150), "Agent 云盘工具真实路径失败")
    }

    @MainActor func testImageThumbnailPreviewDelete() throws {
        try requireOptIn()
        let accountID = try expectedAccountID("B")
        let app = XCUIApplication()
        app.launchArguments = ["--cloud-drive-production-canary-normal",
                               "--cloud-drive-ui-test"]
        app.launchEnvironment["MAICHAT_PROD_SYNC_TEST_ACCOUNT"] = "canary"
        app.launch()
        defer { app.terminate() }
        guard openPersonalPage(app, accountID: accountID) else {
            XCTFail("B 的个人页未显示预期测试账号")
            return
        }
        dismissNotificationPrompt(app)
        let entry = app.buttons["my-cloud-drive-entry"]
        XCTAssertTrue(entry.waitForExistence(timeout: 20))
        entry.tap()
        XCTAssertTrue(app.staticTexts["还没有图片或视频"].waitForExistence(timeout: 20),
                      "灰度 B 必须使用空的专用账号")
        let directUpload = app.buttons["cloud-drive-upload-image-sample"]
        if !directUpload.exists {
            let more = app.buttons["OverflowBarButtonItem"]
            XCTAssertTrue(more.waitForExistence(timeout: 10))
            more.tap()
        }
        let upload = directUpload.exists ? directUpload : app.buttons["上传图片样本"]
        guard upload.waitForExistence(timeout: 10) else {
            XCTFail("调试样本上传按钮未出现")
            return
        }
        upload.tap()
        let media = app.buttons.matching(NSPredicate(
            format: "identifier BEGINSWITH %@", "cloud-drive-play-")).firstMatch
        XCTAssertTrue(media.waitForExistence(timeout: 120), "真实 OSS JPEG 上传未完成")
        let id = String(media.identifier.dropFirst("cloud-drive-play-".count))
        XCTAssertEqual(media.label, "查看图片")
        let loaded = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "value == %@", "封面已加载"), object: media)
        XCTAssertEqual(XCTWaiter.wait(for: [loaded], timeout: 35), .completed,
                      "真实 OSS 图片缩略图未显示")
        let listed = XCTAttachment(screenshot: app.screenshot())
        listed.name = "cloud-drive-production-image-thumbnail"
        listed.lifetime = .keepAlways
        add(listed)
        media.tap()
        let close = app.buttons["cloud-drive-image-close"]
        XCTAssertTrue(close.waitForExistence(timeout: 60), "真实 JPEG 原图预览未打开")
        let preview = XCTAttachment(screenshot: app.screenshot())
        preview.name = "cloud-drive-production-image-preview"
        preview.lifetime = .keepAlways
        add(preview)
        close.tap()
        let menu = app.buttons["cloud-drive-video-menu-\(id)"]
        XCTAssertTrue(menu.waitForExistence(timeout: 10))
        menu.tap()
        app.buttons["删除"].tap()
        app.buttons["删除文件"].tap()
        XCTAssertTrue(app.staticTexts["还没有图片或视频"].waitForExistence(timeout: 30))
        XCTAssertTrue(app.staticTexts["0 B / 10 GB"].waitForExistence(timeout: 10))
    }

    @MainActor func testPeerEntryHidden() throws {
        try requireOptIn()
        let accountID = try expectedAccountID("B")
        let app = XCUIApplication()
        app.launchArguments = ["--cloud-drive-production-canary-normal"]
        app.launchEnvironment["MAICHAT_PROD_SYNC_TEST_ACCOUNT"] = "canary"
        app.launch()
        defer { app.terminate() }
        guard openPersonalPage(app, accountID: accountID) else {
            XCTFail("B 的个人页未显示预期测试账号")
            return
        }
        dismissNotificationPrompt(app)
        XCTAssertTrue(app.buttons["personal-profile-entry"].waitForExistence(timeout: 10))
        XCTAssertFalse(app.buttons["my-cloud-drive-entry"].waitForExistence(timeout: 8),
                       "未开通账号 B 不应看到云盘入口")
    }

    @MainActor func testPeerApiReturns404() throws {
        try requireOptIn()
        let accountID = try expectedAccountID("B")
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--cloud-drive-ui-test",
                               "--cloud-drive-production-canary-tools"]
        app.launchEnvironment["MAICHAT_PROD_SYNC_TEST_ACCOUNT"] = "canary"
        app.launch()
        defer { app.terminate() }
        guard app.staticTexts["云盘灰度账号已登录"]
            .waitForExistence(timeout: 40),
              app.staticTexts[accountID].waitForExistence(timeout: 10) else {
            XCTFail("云盘 B 灰度账号未登录")
            return
        }
        let probe = app.buttons["cloud-drive-production-peer-probe"]
        XCTAssertTrue(probe.waitForExistence(timeout: 15))
        probe.tap()
        XCTAssertTrue(app.staticTexts["云盘账号 B 接口返回 404"]
            .waitForExistence(timeout: 30))
    }
}
