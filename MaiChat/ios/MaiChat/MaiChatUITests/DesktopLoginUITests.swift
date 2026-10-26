// 文件作用：用本机测试账号验证 iOS 扫码登录与会话替换的真实界面。
import XCTest

final class DesktopLoginUITests: XCTestCase {
    private struct LocalDesktopRequest: Decodable {
        let qrUri: String
    }

    func testPrivacyAndDeletionEntries() throws {
        let environment = ProcessInfo.processInfo.environment
        guard let phone = environment["MAICHAT_LOCAL_UI_PHONE"],
              let password = environment["MAICHAT_LOCAL_UI_PASSWORD"] else {
            throw XCTSkip("需要本机测试账号")
        }
        let app = launchAccountApp(phone: phone, password: password)
        XCTAssertTrue(app.buttons["我"].waitForExistence(timeout: 30))
        if app.buttons["稍后"].exists { app.buttons["稍后"].tap() }
        tapMeTab(in: app)
        let settings = app.buttons["设置"]
        if !settings.waitForExistence(timeout: 2) {
            app.coordinate(withNormalizedOffset: CGVector(dx: 0.85, dy: 0.955)).tap()
        }
        XCTAssertTrue(settings.waitForExistence(timeout: 10))
        settings.tap()
        dismissLocalIMError(in: app)
        let deletion = app.buttons["account-deletion-entry"]
        if !deletion.isHittable { app.swipeUp() }
        XCTAssertTrue(deletion.waitForExistence(timeout: 10))
        deletion.tap()
        XCTAssertTrue(app.textFields["account-deletion-confirm-id"]
            .waitForExistence(timeout: 10))
        XCTAssertTrue(app.buttons["account-deletion-submit"].exists)
        let screenshot = XCTAttachment(screenshot: app.screenshot())
        screenshot.name = "account-deletion-confirmation"
        screenshot.lifetime = .keepAlways
        add(screenshot)
    }

    func testLocalAccountDeletionShowsReceipt() throws {
        let environment = ProcessInfo.processInfo.environment
        guard let phone = environment["MAICHAT_LOCAL_UI_PHONE"],
              let password = environment["MAICHAT_LOCAL_UI_PASSWORD"] else {
            throw XCTSkip("需要本机测试账号")
        }
        let app = launchAccountApp(phone: phone, password: password)
        XCTAssertTrue(app.buttons["我"].waitForExistence(timeout: 30))
        if app.buttons["稍后"].exists { app.buttons["稍后"].tap() }
        tapMeTab(in: app)
        let settings = app.buttons["设置"]
        if !settings.waitForExistence(timeout: 2) {
            app.coordinate(withNormalizedOffset: CGVector(dx: 0.85, dy: 0.955)).tap()
        }
        XCTAssertTrue(settings.waitForExistence(timeout: 10))
        settings.tap()
        dismissLocalIMError(in: app)
        let deletion = app.buttons["account-deletion-entry"]
        if !deletion.isHittable { app.swipeUp() }
        XCTAssertTrue(deletion.waitForExistence(timeout: 10))
        deletion.tap()
        let accountID = app.staticTexts["account-deletion-current-id"].label
        XCTAssertFalse(accountID.isEmpty)
        let idField = app.textFields["account-deletion-confirm-id"]
        idField.tap()
        idField.typeText(accountID)
        app.swipeDown()
        let passwordField = app.secureTextFields["输入登录密码"]
        XCTAssertTrue(passwordField.waitForExistence(timeout: 10))
        passwordField.tap()
        passwordField.typeText(password + "\n")
        let submit = app.buttons["account-deletion-submit"]
        for _ in 0..<3 where !submit.exists {
            app.collectionViews.firstMatch.swipeUp()
        }
        XCTAssertTrue(submit.waitForExistence(timeout: 5))
        submit.tap()
        XCTAssertTrue(app.buttons["确认提交"].waitForExistence(timeout: 5))
        app.buttons["确认提交"].tap()
        XCTAssertTrue(app.staticTexts["注销申请已提交"].waitForExistence(timeout: 20))
        XCTAssertTrue(app.staticTexts["查询进度"].exists || app.buttons["查询进度"].exists)
        let screenshot = XCTAttachment(screenshot: app.screenshot())
        screenshot.name = "account-deletion-receipt"
        screenshot.lifetime = .keepAlways
        add(screenshot)
    }

    @MainActor func testDesktopLoginScannerEntry() async throws {
        let (phone, password) = try await localAccount()
        let app = launchAccountApp(phone: phone, password: password)
        let me = app.buttons["我"]
        XCTAssertTrue(me.waitForExistence(timeout: 30))
        dismissLocalIMError(in: app)
        openDesktopScanner(in: app)
        XCTAssertTrue(app.staticTexts["扫描好友二维码或电脑登录二维码"]
            .waitForExistence(timeout: 10))
        let scannerScreenshot = XCTAttachment(screenshot: app.screenshot())
        scannerScreenshot.name = "desktop-login-scanner"
        scannerScreenshot.lifetime = .keepAlways
        add(scannerScreenshot)
        app.buttons["关闭"].tap()
        XCTAssertTrue(app.buttons["im-message-actions"].waitForExistence(timeout: 5))
        tapMeTab(in: app)
        let settings = app.buttons["设置"]
        XCTAssertTrue(settings.waitForExistence(timeout: 5))
        settings.tap()
        XCTAssertFalse(app.buttons["desktop-login-entry"].exists)
        XCTAssertFalse(app.staticTexts["扫码登录电脑"].exists)
        let settingsScreenshot = XCTAttachment(screenshot: app.screenshot())
        settingsScreenshot.name = "desktop-login-removed-from-settings"
        settingsScreenshot.lifetime = .keepAlways
        add(settingsScreenshot)
    }

    @MainActor func testDesktopLoginConfirmationAndCancel() async throws {
        let (phone, password) = try await localAccount()
        let qr = try await makeDesktopRequest(platform: "mac")
        let app = launchAccountApp(phone: phone, password: password, qr: qr)
        let me = app.buttons["我"]
        XCTAssertTrue(me.waitForExistence(timeout: 30))
        dismissLocalIMError(in: app)
        openDesktopScanner(in: app)
        let simulatedScan = app.buttons["模拟识别二维码"]
        XCTAssertTrue(simulatedScan.waitForExistence(timeout: 5))
        simulatedScan.tap()
        XCTAssertTrue(app.staticTexts["desktop-login-code"].waitForExistence(timeout: 15))
        let confirmation = XCTAttachment(screenshot: app.screenshot())
        confirmation.name = "desktop-login-confirmation"
        confirmation.lifetime = .keepAlways
        add(confirmation)
        XCUIDevice.shared.orientation = .landscapeLeft
        defer { XCUIDevice.shared.orientation = .portrait }
        XCTAssertTrue(app.staticTexts["desktop-login-code"].waitForExistence(timeout: 10))
        let landscapeDeadline = Date().addingTimeInterval(5)
        while app.frame.width < app.frame.height && Date() < landscapeDeadline {
            try await Task.sleep(for: .milliseconds(100))
        }
        XCTAssertGreaterThan(app.frame.width, app.frame.height)
        try await Task.sleep(for: .seconds(1))
        XCTAssertTrue(app.staticTexts["desktop-login-code"].isHittable)
        XCTAssertTrue(app.buttons["desktop-login-approve"].isHittable)
        XCUIDevice.shared.orientation = .portrait
        let portraitDeadline = Date().addingTimeInterval(5)
        while app.frame.height < app.frame.width && Date() < portraitDeadline {
            try await Task.sleep(for: .milliseconds(100))
        }
        app.buttons["desktop-login-deny"].tap()
        XCTAssertTrue(app.staticTexts["已取消本次登录"].waitForExistence(timeout: 15))
        let cancelled = XCTAttachment(screenshot: app.screenshot())
        cancelled.name = "desktop-login-cancelled"
        cancelled.lifetime = .keepAlways
        add(cancelled)
        app.buttons["返回消息"].tap()
        XCTAssertTrue(app.buttons["im-message-actions"].waitForExistence(timeout: 5))
    }

    @MainActor func testDesktopLoginApproval() async throws {
        let (phone, password) = try await localAccount()
        let qr = try await makeDesktopRequest(platform: "windows")
        let app = launchAccountApp(phone: phone, password: password, qr: qr)
        let me = app.buttons["我"]
        XCTAssertTrue(me.waitForExistence(timeout: 30))
        dismissLocalIMError(in: app)
        openDesktopScanner(in: app)
        let simulatedScan = app.buttons["模拟识别二维码"]
        XCTAssertTrue(simulatedScan.waitForExistence(timeout: 5))
        simulatedScan.tap()
        XCTAssertTrue(app.staticTexts["desktop-login-code"].waitForExistence(timeout: 15))
        app.buttons["desktop-login-approve"].tap()
        XCTAssertTrue(app.staticTexts["已允许电脑登录"].waitForExistence(timeout: 15))
        let approved = XCTAttachment(screenshot: app.screenshot())
        approved.name = "desktop-login-approved"
        approved.lifetime = .keepAlways
        add(approved)
        app.buttons["返回消息"].tap()
        XCTAssertTrue(app.buttons["im-message-actions"].waitForExistence(timeout: 5))
    }

    @MainActor func testReplacedPhoneSessionReturnsToLogin() async throws {
        let environment = ProcessInfo.processInfo.environment
        guard let phone = environment["MAICHAT_LOCAL_UI_PHONE"],
              let password = environment["MAICHAT_LOCAL_UI_PASSWORD"] else {
            throw XCTSkip("需要本机测试账号")
        }
        let app = launchAccountApp(phone: phone, password: password)
        let me = app.buttons["我"]
        XCTAssertTrue(me.waitForExistence(timeout: 30))
        tapMeTab(in: app)
        let settings = app.buttons["设置"]
        XCTAssertTrue(settings.waitForExistence(timeout: 10))
        settings.tap()
        dismissLocalIMError(in: app)

        var request = URLRequest(url: URL(string: "http://127.0.0.1:18190/v1/auth/password/login")!)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONSerialization.data(withJSONObject: [
            "phone": phone, "password": password,
            "age_confirmed_14_plus": true,
            "device_slot": "ios_phone", "device_platform": "ios"
        ])
        let (_, response) = try await URLSession.shared.data(for: request)
        XCTAssertEqual((response as? HTTPURLResponse)?.statusCode, 200)

        XCUIDevice.shared.press(.home)
        app.activate()
        XCTAssertTrue(app.textFields["account-phone"].waitForExistence(timeout: 15))
        let explanation = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "账号已在另一台同类设备登录")
        ).firstMatch
        XCTAssertTrue(explanation.waitForExistence(timeout: 5))
        let login = XCTAttachment(screenshot: app.screenshot())
        login.name = "phone-session-replaced-login"
        login.lifetime = .keepAlways
        add(login)
    }

    @MainActor func testLegacyDeviceSlotRejectionClearsAccessAndRefreshSessions() async throws {
        let environment = ProcessInfo.processInfo.environment
        guard environment["MAICHAT_LOCAL_UI_DEVICE_SLOT_TEST"] == "1",
              let phone = environment["MAICHAT_LOCAL_UI_PHONE"],
              let password = environment["MAICHAT_LOCAL_UI_PASSWORD"] else {
            throw XCTSkip("需要严格设备槽本机测试服务")
        }
        let app = launchAccountApp(phone: phone, password: password)
        XCTAssertTrue(app.buttons["我"].waitForExistence(timeout: 30))
        try await forceLegacyDeviceSlot()
        XCUIDevice.shared.press(.home)
        app.activate()
        XCTAssertTrue(app.textFields["account-phone"].waitForExistence(timeout: 15))
        XCTAssertTrue(app.staticTexts["请更新 App 后重新登录"].waitForExistence(timeout: 10))

        app.terminate()
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] = password
        app.launch()
        XCTAssertTrue(app.buttons["我"].waitForExistence(timeout: 30))
        try await forceLegacyDeviceSlot()
        app.terminate()
        // 保留本机刷新令牌，不让本机自动登录钩子在第二次启动时新建会话。
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] = ""
        app.launch()
        XCTAssertTrue(app.textFields["account-phone"].waitForExistence(timeout: 15))
        XCTAssertTrue(app.staticTexts["请更新 App 后重新登录"].waitForExistence(timeout: 10))
        let login = XCTAttachment(screenshot: app.screenshot())
        login.name = "legacy-device-slot-refresh-rejected"
        login.lifetime = .keepAlways
        add(login)
    }

    private func launchAccountApp(
        phone: String, password: String, qr: String = ""
    ) -> XCUIApplication {
        let app = XCUIApplication()
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = phone
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] = password
        app.launchEnvironment["MAICHAT_LOCAL_UI_DESKTOP_QR"] = qr
        app.launch()
        // 本机模拟服务重启后，旧 Keychain 会话可在首次启动时被清理；
        // 重开即用新账号登录。
        if !app.buttons["我"].waitForExistence(timeout: 10),
           app.textFields["account-phone"].exists {
            app.terminate()
            app.launch()
        }
        return app
    }

    @MainActor private func localAccount() async throws -> (String, String) {
        let environment = ProcessInfo.processInfo.environment
        if let phone = environment["MAICHAT_LOCAL_UI_PHONE"],
           let password = environment["MAICHAT_LOCAL_UI_PASSWORD"] {
            return (phone, password)
        }
        guard let statusURL = URL(string: "http://127.0.0.1:18190/test/status"),
              let (_, response) = try? await URLSession.shared.data(from: statusURL),
              (response as? HTTPURLResponse)?.statusCode == 200 else {
            throw XCTSkip("需要本机测试账号服务")
        }
        return ("sync-ui-test", "local sync UI test passphrase")
    }

    private func dismissLocalIMError(in app: XCUIApplication) {
        let error = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "IM 操作失败")
        ).firstMatch
        if error.exists { error.tap() }
    }

    private func openDesktopScanner(in app: XCUIApplication) {
        let actions = app.buttons["im-message-actions"]
        XCTAssertTrue(actions.waitForExistence(timeout: 10))
        actions.tap()
        let scan = app.buttons["扫一扫"]
        XCTAssertTrue(scan.waitForExistence(timeout: 5))
        scan.tap()
    }

    /// SwiftUI TabView 的“我”在 iOS 26 无可点击 frame；按实际底栏中心进入个人页。
    private func tapMeTab(in app: XCUIApplication) {
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.85, dy: 0.925)).tap()
    }

    @MainActor private func makeDesktopRequest(platform: String) async throws -> String {
        let bytes = Data((0..<32).map { _ in UInt8.random(in: .min ... .max) })
        let challenge = bytes.base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
        var request = URLRequest(url: URL(string: "http://127.0.0.1:18190/v1/auth/desktop/request")!)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONSerialization.data(withJSONObject: [
            "platform": platform, "code_challenge": challenge
        ])
        let (data, response) = try await URLSession.shared.data(for: request)
        XCTAssertEqual((response as? HTTPURLResponse)?.statusCode, 200)
        let decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
        return try decoder.decode(LocalDesktopRequest.self, from: data).qrUri
    }

    @MainActor private func forceLegacyDeviceSlot() async throws {
        var request = URLRequest(url: URL(string: "http://127.0.0.1:18190/v1/test/force-legacy-slot")!)
        request.httpMethod = "POST"
        let (data, response) = try await URLSession.shared.data(for: request)
        XCTAssertEqual((response as? HTTPURLResponse)?.statusCode, 200)
        let value = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Int])
        XCTAssertGreaterThan(value["changed"] ?? 0, 0)
    }

}
