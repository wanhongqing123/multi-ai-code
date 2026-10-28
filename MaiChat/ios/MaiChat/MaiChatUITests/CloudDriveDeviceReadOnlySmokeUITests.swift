import XCTest

final class CloudDriveDeviceReadOnlySmokeUITests: XCTestCase {
    @MainActor func testInstalledCloudFolderReadOnlyRoundTrip() throws {
        #if targetEnvironment(simulator)
        throw XCTSkip("此单次只读验收仅用于已连接真机")
        #else
        let environment = ProcessInfo.processInfo.environment
        guard environment["MAICHAT_RUN_CLOUD_FOLDER_READONLY"] == "YES"
        else { throw XCTSkip("需要显式开启约定的真机只读窗口") }
        continueAfterFailure = false
        let configuredName = environment["MAICHAT_CLOUD_FOLDER_NAME"]?
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        let folderName = configuredName.isEmpty ? "桂山岛之旅" : configuredName
        let app = XCUIApplication()
        app.launchEnvironment["MAICHAT_CURRENT_APP_CAPTURE_SMOKE"] = "YES"
        app.launch()
        XCTAssertTrue(app.buttons["云盘"].waitForExistence(timeout: 30))
        app.buttons["云盘"].tap()
        let folder = app.buttons.matching(NSPredicate(
            format: "identifier BEGINSWITH %@ AND label == %@",
            "cloud-drive-folder-", folderName)).firstMatch
        XCTAssertTrue(folder.waitForExistence(timeout: 45), "根目录应显示约定文件夹：\(folderName)")
        let root = XCTAttachment(screenshot: app.screenshot())
        root.name = "device-cloud-folder-root-readonly"
        root.lifetime = .keepAlways
        add(root)
        folder.tap()
        let first = app.buttons.matching(NSPredicate(
            format: "identifier BEGINSWITH %@", "cloud-drive-play-")).firstMatch
        XCTAssertTrue(first.waitForExistence(timeout: 45), "文件夹内应显示真实文件")
        let contents = XCTAttachment(screenshot: app.screenshot())
        contents.name = "device-cloud-folder-contents-readonly"
        contents.lifetime = .keepAlways
        add(contents)
        first.tap()
        let counter = app.staticTexts["cloud-drive-image-counter"]
        XCTAssertTrue(counter.waitForExistence(timeout: 20))
        let ready = XCTNSPredicateExpectation(predicate: NSPredicate(
            format: "value IN %@", ["图片已加载", "视频已播放"]), object: counter)
        XCTAssertEqual(XCTWaiter.wait(for: [ready], timeout: 60), .completed,
                       "应等到图片解码或视频实际首帧完成")
        app.buttons["cloud-drive-image-close"].tap()
        XCTAssertTrue(first.waitForExistence(timeout: 15))
        app.buttons["cloud-drive-folder-root"].tap()
        XCTAssertTrue(folder.waitForExistence(timeout: 30))
        XCTAssertFalse(app.buttons["cloud-drive-folder-up"].exists)
        #endif
    }

    @MainActor func testInstalledCloudImagesSwipeWithoutChangingFiles() throws {
        #if targetEnvironment(simulator)
        throw XCTSkip("此用例验收已连接真机上的真实云盘")
        #else
        guard ProcessInfo.processInfo.environment["MAICHAT_RUN_DEVICE_CLOUD_GALLERY"] == "YES"
        else { throw XCTSkip("真实账号云盘只读验收需要显式开启") }
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchEnvironment["MAICHAT_MEDIA_UI_TEST_STATE"] = "YES"
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
        XCTAssertFalse(app.buttons["cloud-drive-delete-\(id)"].exists,
                       "右滑不应揭露删除按钮")
        row.swipeLeft()
        XCTAssertTrue(app.buttons["cloud-drive-delete-\(id)"].waitForExistence(timeout: 5))
        XCTAssertGreaterThan(app.buttons["cloud-drive-delete-\(id)"].frame.midX,
                             row.frame.midX, "左滑删除按钮应位于文件行右侧")
        let revealed = XCTAttachment(screenshot: app.screenshot())
        revealed.name = "device-cloud-left-swipe-delete-revealed"
        revealed.lifetime = .keepAlways
        add(revealed)
        // Close the revealed action without invoking deletion or its confirmation.
        row.swipeRight()
        first.tap()
        let counter = app.staticTexts["cloud-drive-image-counter"]
        XCTAssertTrue(counter.waitForExistence(timeout: 45))
        XCTAssertTrue(counter.label.hasPrefix("1/"))
        let image = app.images["cloud-drive-image-preview"].firstMatch
        XCTAssertTrue(image.waitForExistence(timeout: 45))
        let save = app.buttons["cloud-drive-image-save"]
        let firstReady = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "enabled == true"), object: save)
        XCTAssertEqual(XCTWaiter.wait(for: [firstReady], timeout: 45), .completed,
                       "首张图片应已完成下载和解码")
        XCTAssertFalse(counter.label.hasPrefix("1/1 ·"), "需要至少两张现有图片验证翻页")
        app.swipeLeft()
        XCTAssertTrue(counter.label.hasPrefix("2/"), "左滑应显示第二张图片")
        let secondReady = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "enabled == true"), object: save)
        XCTAssertEqual(XCTWaiter.wait(for: [secondReady], timeout: 45), .completed,
                       "第二张图片应已完成下载和解码")
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

    @MainActor func testInstalledCloudVideoPlaysAndCanSwipeToAdjacentMedia() throws {
        #if targetEnvironment(simulator)
        throw XCTSkip("此用例验收已连接真机上的真实云盘视频")
        #else
        guard ProcessInfo.processInfo.environment["MAICHAT_RUN_DEVICE_CLOUD_GALLERY"] == "YES"
        else { throw XCTSkip("真实账号云盘只读验收需要显式开启") }
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchEnvironment["MAICHAT_MEDIA_UI_TEST_STATE"] = "YES"
        app.launch()
        defer { app.terminate() }
        XCTAssertTrue(app.buttons["云盘"].waitForExistence(timeout: 30))
        app.buttons["云盘"].tap()
        let candidates = app.buttons.matching(NSPredicate(
            format: "identifier BEGINSWITH %@ AND label == %@",
            "cloud-drive-play-", "播放视频"))
        var selected: XCUIElement?
        for _ in 0..<15 {
            selected = candidates.allElementsBoundByIndex.first(where: { $0.isHittable })
            if selected != nil { break }
            let more = app.buttons["cloud-drive-load-more"]
            if more.exists && more.isHittable { more.tap() }
            else { app.swipeUp() }
        }
        guard let selected else { throw XCTSkip("当前账号没有可见视频，混合播放另由隔离素材验收") }
        selected.tap()
        let counter = app.staticTexts["cloud-drive-image-counter"]
        XCTAssertTrue(counter.waitForExistence(timeout: 15))
        let ready = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "value BEGINSWITH %@", "video:ready"), object: counter)
        XCTAssertEqual(XCTWaiter.wait(for: [ready], timeout: 60), .completed,
                       "真实视频需要渲染出首帧")
        let caption = counter.label
        let index = Int(caption.components(separatedBy: "/")[0]) ?? 0
        XCTAssertGreaterThan(index, 0)
        let pause = app.buttons["ffplay-pause"]
        XCTAssertTrue(pause.exists)
        pause.tap()
        XCTAssertEqual(pause.label, "播放视频")
        pause.tap()
        XCTAssertEqual(pause.label, "暂停视频")
        let frame = XCTAttachment(screenshot: app.screenshot())
        frame.name = "device-cloud-video-first-frame"
        frame.lifetime = .keepAlways
        add(frame)
        if index > 1 { app.swipeRight() } else { app.swipeLeft() }
        let expectedIndex = index > 1 ? index - 1 : index + 1
        XCTAssertTrue(counter.label.hasPrefix("\(expectedIndex)/"), "视频应能滑到相邻媒体")
        if index > 1 { app.swipeLeft() } else { app.swipeRight() }
        XCTAssertTrue(counter.label.hasPrefix("\(index)/"))
        let returned = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "value BEGINSWITH %@", "video:ready"), object: counter)
        XCTAssertEqual(XCTWaiter.wait(for: [returned], timeout: 60), .completed,
                       "重新滑回视频需要建立可播放的新点播会话")
        app.buttons["cloud-drive-image-close"].tap()
        XCTAssertTrue(selected.waitForExistence(timeout: 10))
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
