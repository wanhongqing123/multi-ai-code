import XCTest

final class CloudDriveDeviceReadOnlySmokeUITests: XCTestCase {
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
