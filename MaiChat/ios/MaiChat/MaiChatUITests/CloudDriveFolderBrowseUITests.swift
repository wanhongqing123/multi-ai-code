import XCTest

final class CloudDriveFolderBrowseUITests: XCTestCase {
    @MainActor
    private func launchFixture() async throws -> XCUIApplication {
        let origin = "http://127.0.0.1:18449"
        guard let (certificate, response) = try? await URLSession.shared.data(
            from: URL(string: origin + "/test/cert")!),
              (response as? HTTPURLResponse)?.statusCode == 200 else {
            throw XCTSkip("需要隔离的本机云盘 TLS 服务")
        }
        var reset = URLRequest(url: URL(string: origin + "/test/reset")!)
        reset.httpMethod = "POST"
        let (_, resetResponse) = try await URLSession.shared.data(for: reset)
        XCTAssertEqual((resetResponse as? HTTPURLResponse)?.statusCode,204)
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--cloud-drive-ui-test",
                               "--cloud-drive-folder-browse-test"]
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] = "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18190"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_URL"] = "https://localhost:18448"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_CA_BASE64"] = certificate.base64EncodedString()
        app.launch()
        let prepare = app.buttons["cloud-folder-browse-prepare"]
        XCTAssertTrue(prepare.waitForExistence(timeout: 30))
        let enabled = XCTNSPredicateExpectation(predicate: NSPredicate(format: "enabled == true"),
                                               object: prepare)
        XCTAssertEqual(XCTWaiter.wait(for: [enabled],timeout: 30),.completed)
        prepare.tap()
        XCTAssertTrue(app.staticTexts["文件夹浏览样本已准备"].waitForExistence(timeout: 60))
        return app
    }

    @MainActor
    private func folder(_ name: String, in app: XCUIApplication) -> XCUIElement {
        app.buttons.matching(NSPredicate(format: "identifier BEGINSWITH %@ AND label == %@",
            "cloud-drive-folder-",name)).firstMatch
    }

    @MainActor
    private func breadcrumb(_ name: String, in app: XCUIApplication) -> XCUIElement {
        app.buttons.matching(NSPredicate(format: "identifier BEGINSWITH %@ AND label == %@",
            "cloud-drive-breadcrumb-",name)).firstMatch
    }

    @MainActor
    private func enterChild(in app: XCUIApplication) {
        let parent = revealFolder("旅行资料",in: app)
        parent.tap()
        let parentPath = breadcrumb("旅行资料",in: app)
        XCTAssertTrue(parentPath.waitForExistence(timeout: 15))
        XCTAssertTrue(parentPath.isHittable, "从长父目录进入长子目录后应回到导航顶部")
        XCTAssertTrue(app.buttons["cloud-drive-folder-root"].isHittable)
        let child = revealFolder("桂山岛之旅",in: app)
        child.tap()
    }

    @MainActor
    private func revealFolder(_ name: String, in app: XCUIApplication) -> XCUIElement {
        let item = folder(name,in: app)
        for _ in 0..<30 where !item.exists || !item.isHittable {
            app.scrollViews.firstMatch.swipeUp()
        }
        XCTAssertTrue(item.exists && item.isHittable, "目录行应可滚动到：\(name)")
        return item
    }

    @MainActor
    func testFolderNavigationScopesMediaAndSupportsEmptyRefreshAndRemoteRemoval() async throws {
        #if targetEnvironment(simulator)
        continueAfterFailure = false
        let app = try await launchFixture()
        defer { app.terminate() }
        let mediaID = "123e4567-e89b-42d3-a456-426614174000"
        XCTAssertFalse(app.buttons["cloud-drive-play-" + mediaID].exists,
                       "根目录不能混入子目录文件")
        XCTAssertFalse(app.buttons["新建文件夹"].exists)
        XCTAssertFalse(app.buttons["cloud-drive-folder-root"].exists,
                       "根目录不重复显示蓝色我的云盘路径")
        enterChild(in: app)
        let play = app.buttons["cloud-drive-play-" + mediaID]
        XCTAssertTrue(play.waitForExistence(timeout: 20))
        XCTAssertFalse(app.staticTexts["cloud-drive-folder-upload-notice"].exists)
        XCTAssertTrue(app.buttons["上传到根目录"].exists)
        let child = XCTAttachment(screenshot: app.screenshot())
        child.name = "ios-cloud-folder-child-media"
        child.lifetime = .keepAlways
        add(child)
        play.tap()
        let counter = app.staticTexts["cloud-drive-image-counter"]
        XCTAssertTrue(counter.waitForExistence(timeout: 20))
        XCTAssertTrue(counter.label.hasPrefix("1/1"), "预览只属于当前目录")
        let ready = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "value BEGINSWITH %@", "video:ready"),object: counter)
        XCTAssertEqual(XCTWaiter.wait(for: [ready],timeout: 60),.completed)
        app.buttons["cloud-drive-image-close"].tap()
        let parentPath = breadcrumb("旅行资料",in: app)
        XCTAssertTrue(parentPath.waitForExistence(timeout: 15))
        parentPath.tap()
        let returnedToParent = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "exists == false"),
            object: breadcrumb("桂山岛之旅",in: app))
        XCTAssertEqual(XCTWaiter.wait(for: [returnedToParent],timeout: 15),.completed)
        XCTAssertTrue(parentPath.waitForExistence(timeout: 15))
        XCTAssertTrue(parentPath.isHittable)
        XCTAssertFalse(play.exists)
        app.buttons["cloud-drive-folder-root"].tap()
        let empty = revealFolder("空文件夹",in: app)
        empty.tap()
        XCTAssertTrue(app.staticTexts["文件夹是空的"].waitForExistence(timeout: 15))
        app.scrollViews.firstMatch.swipeDown()
        XCTAssertTrue(app.staticTexts["文件夹是空的"].waitForExistence(timeout: 15))
        app.buttons["cloud-folder-browse-remove-empty"].tap()
        XCTAssertTrue(app.staticTexts["文件夹已不存在，请返回上级或根目录"]
            .waitForExistence(timeout: 20))
        XCTAssertFalse(app.staticTexts["云盘暂不可用"].exists)
        app.buttons["cloud-drive-folder-root"].tap()
        let returnedToRoot = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "exists == false"), object: app.buttons["cloud-drive-folder-root"])
        XCTAssertEqual(XCTWaiter.wait(for: [returnedToRoot],timeout: 15),.completed)
        XCTAssertFalse(folder("空文件夹",in: app).exists)
        #else
        throw XCTSkip("仅运行于隔离模拟器，不修改真机云盘")
        #endif
    }

    @MainActor
    func testFolderAndAccountSwitchDiscardInflightListsAndResetLoading() async throws {
        #if targetEnvironment(simulator)
        continueAfterFailure = false
        let app = try await launchFixture()
        defer { app.terminate() }
        enterChild(in: app)
        XCTAssertTrue(app.buttons["cloud-drive-play-123e4567-e89b-42d3-a456-426614174000"]
            .waitForExistence(timeout: 20))
        app.buttons["cloud-folder-browse-navigation-race"].tap()
        XCTAssertTrue(app.staticTexts["目录切换未混入旧文件"].waitForExistence(timeout: 20))
        XCTAssertFalse(app.buttons["cloud-drive-folder-root"].exists)
        XCTAssertFalse(app.buttons["cloud-drive-play-123e4567-e89b-42d3-a456-426614174000"].exists)
        app.buttons["cloud-folder-browse-account-race"].tap()
        XCTAssertTrue(app.staticTexts["切号加载状态已恢复且无旧目录"].waitForExistence(timeout: 30))
        XCTAssertTrue(app.staticTexts["还没有图片或视频"].waitForExistence(timeout: 15))
        XCTAssertFalse(folder("旅行资料",in: app).exists)
        XCTAssertFalse(app.buttons.matching(NSPredicate(format: "identifier BEGINSWITH %@",
            "cloud-drive-breadcrumb-")).firstMatch.exists)
        XCTAssertFalse(app.buttons["cloud-drive-folder-root"].exists)
        let switched = XCTAttachment(screenshot: app.screenshot())
        switched.name = "ios-cloud-folder-account-switch-root"
        switched.lifetime = .keepAlways
        add(switched)
        #else
        throw XCTSkip("仅运行于隔离模拟器，不切换真机账号")
        #endif
    }
}
