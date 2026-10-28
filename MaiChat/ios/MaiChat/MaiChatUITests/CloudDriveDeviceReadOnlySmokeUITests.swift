import XCTest

final class CloudDriveDeviceReadOnlySmokeUITests: XCTestCase {
    @MainActor func testInstalledCloudImagesSwipeWithoutChangingFiles() throws {
        #if targetEnvironment(simulator)
        throw XCTSkip("此用例验收已连接真机上的真实云盘")
        #else
        guard ProcessInfo.processInfo.environment["MAICHAT_RUN_DEVICE_CLOUD_GALLERY"] == "YES"
        else { throw XCTSkip("真实账号云盘只读验收需要显式开启") }
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launch()
        defer { app.terminate() }
        XCTAssertTrue(app.buttons["云盘"].waitForExistence(timeout: 30))
        app.buttons["云盘"].tap()
        let first = app.buttons.matching(NSPredicate(
            format: "identifier BEGINSWITH %@ AND label == %@",
            "cloud-drive-play-", "查看图片")).firstMatch
        XCTAssertTrue(first.waitForExistence(timeout: 45), "需要账号现有的云盘图片")
        let id = String(first.identifier.dropFirst("cloud-drive-play-".count))
        XCTAssertFalse(app.buttons["cloud-drive-video-menu-\(id)"].exists)
        let row = app.otherElements["cloud-drive-row-\(id)"]
        XCTAssertTrue(row.exists)
        row.swipeRight()
        XCTAssertTrue(app.buttons["cloud-drive-delete-\(id)"].waitForExistence(timeout: 5))
        let revealed = XCTAttachment(screenshot: app.screenshot())
        revealed.name = "device-cloud-right-swipe-delete-revealed"
        revealed.lifetime = .keepAlways
        add(revealed)
        // Close the revealed action without invoking deletion or its confirmation.
        row.swipeLeft()
        first.tap()
        let counter = app.staticTexts["cloud-drive-image-counter"]
        XCTAssertTrue(counter.waitForExistence(timeout: 45))
        XCTAssertTrue(counter.label.hasPrefix("1/"))
        let image = app.images["cloud-drive-image-preview"].firstMatch
        XCTAssertTrue(image.waitForExistence(timeout: 45))
        XCTAssertFalse(counter.label.hasPrefix("1/1 ·"), "需要至少两张现有图片验证翻页")
        app.swipeLeft()
        XCTAssertTrue(counter.label.hasPrefix("2/"), "左滑应显示第二张图片")
        let second = XCTAttachment(screenshot: app.screenshot())
        second.name = "device-cloud-gallery-second-image"
        second.lifetime = .keepAlways
        add(second)
        app.swipeRight()
        XCTAssertTrue(counter.label.hasPrefix("1/"), "右滑应返回第一张图片")
        app.swipeRight()
        XCTAssertTrue(counter.label.hasPrefix("1/"), "第一张不能循环到末尾")
        app.buttons["cloud-drive-image-close"].tap()
        XCTAssertTrue(first.waitForExistence(timeout: 10), "关闭预览后应回到原文件列表")
        #endif
    }

    @MainActor func testInstalledAppStillOpensAccountAndMessages() throws {
        #if targetEnvironment(simulator)
        throw XCTSkip("此用例只检查已连接真机")
        #else
        guard ProcessInfo.processInfo.environment["MAICHAT_RUN_DEVICE_READONLY_SMOKE"] == "YES"
        else { throw XCTSkip("真机只读验收需要显式开启") }
        let app = XCUIApplication()
        app.launch()
        defer { app.terminate() }
        let messages = app.staticTexts["消息"]
        XCTAssertTrue(messages.waitForExistence(timeout: 30), "覆盖安装后消息页未打开")
        let initial = XCTAttachment(screenshot: app.screenshot())
        initial.name = "ios122-real-device-messages"
        initial.lifetime = .keepAlways
        add(initial)
        let me = app.buttons["我的"]
        XCTAssertTrue(me.waitForExistence(timeout: 20))
        me.tap()
        let profile = app.buttons["personal-profile-entry"]
        XCTAssertTrue(profile.waitForExistence(timeout: 20),
                      "覆盖安装后未看到原账号的个人资料入口")
        let personal = XCTAttachment(screenshot: app.screenshot())
        personal.name = "ios122-real-device-personal-page"
        personal.lifetime = .keepAlways
        add(personal)
        #endif
    }
}
