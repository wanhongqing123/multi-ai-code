// 文件作用：用本机假微信/QQ 凭据跑通 202 手机绑定与 200 已绑定登录界面。
import XCTest

final class SocialLoginUITests: XCTestCase {
    @MainActor func testWeChatBindingThenBoundLogin() async throws {
        try requireLocalSocialServer()
        let app = launchSocialApp(provider: "wechat", credential: "wechat-ui-code")
        try await bindPhone("13900000003", in: app)

        // 退出后再用相同已核验身份登录，服务端直接返回 200，不再要求绑定。
        app.terminate()
        app.launchEnvironment["MAICHAT_LOCAL_UI_SOCIAL_TEST"] = "0"
        app.launch()
        let me = app.buttons["我"]
        XCTAssertTrue(me.waitForExistence(timeout: 30))
        me.tap()
        let settings = app.buttons["设置"]
        if !settings.waitForExistence(timeout: 2) {
            app.coordinate(withNormalizedOffset: CGVector(dx: 0.85, dy: 0.955)).tap()
        }
        XCTAssertTrue(settings.waitForExistence(timeout: 10))
        settings.tap()
        app.buttons["切换手机号"].tap()
        XCTAssertTrue(app.textFields["account-phone"].waitForExistence(timeout: 15))
        app.terminate()
        app.launchEnvironment["MAICHAT_LOCAL_UI_SOCIAL_TEST"] = "1"
        app.launch()
        XCTAssertTrue(app.buttons["我"].waitForExistence(timeout: 30))
        XCTAssertFalse(app.textFields["social-bind-phone"].exists)
    }

    @MainActor func testQQBindingUsesSDKToken() async throws {
        try requireLocalSocialServer()
        let app = launchSocialApp(provider: "qq", credential: "qq-ui-token")
        try await bindPhone("13900000004", in: app)
    }

    @MainActor func testInvalidSocialCredentialKeepsLoginRecoverable() throws {
        try requireLocalSocialServer()
        let app = launchSocialApp(provider: "wechat", credential: "invalid-code")
        let error = app.staticTexts["第三方授权已失效，请重新授权"]
        XCTAssertTrue(error.waitForExistence(timeout: 25))
        XCTAssertTrue(app.textFields["account-phone"].exists)
        XCTAssertFalse(app.textFields["social-bind-phone"].exists)
        let failure = XCTAttachment(screenshot: app.screenshot())
        failure.name = "social-authorization-failed"
        failure.lifetime = .keepAlways
        add(failure)
    }

    @MainActor func testPhoneBindingLandscapeKeepsConsentScrollable() async throws {
        try requireLocalSocialServer()
        let app = launchSocialApp(provider: "wechat", credential: "wechat-ui-code")
        let phoneField = app.textFields["social-bind-phone"]
        XCTAssertTrue(phoneField.waitForExistence(timeout: 25))
        XCUIDevice.shared.orientation = .landscapeLeft
        defer { XCUIDevice.shared.orientation = .portrait }
        let deadline = Date().addingTimeInterval(5)
        while app.frame.width < app.frame.height && Date() < deadline {
            try await Task.sleep(for: .milliseconds(100))
        }
        XCTAssertGreaterThan(app.frame.width, app.frame.height)
        XCTAssertTrue(phoneField.exists)
        try await Task.sleep(for: .seconds(1))
    }

    @MainActor private func requireLocalSocialServer() throws {
        guard ProcessInfo.processInfo.environment["MAICHAT_LOCAL_UI_SOCIAL_TEST"] == "1" else {
            throw XCTSkip("需要本机假微信/QQ 核验服务")
        }
    }

    @MainActor private func launchSocialApp(
        provider: String, credential: String
    ) -> XCUIApplication {
        let app = XCUIApplication()
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "13900000001"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] = ""
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18191"
        app.launchEnvironment["MAICHAT_LOCAL_UI_SOCIAL_TEST"] = "0"
        app.launchEnvironment["MAICHAT_LOCAL_UI_SOCIAL_PROVIDER"] = provider
        app.launchEnvironment["MAICHAT_LOCAL_UI_SOCIAL_CREDENTIAL"] = credential
        app.launch()
        // 测试间共享模拟器；先从前一个账号退出，再让新页面接收本机 SDK 凭据。
        let me = app.buttons["我"]
        if me.waitForExistence(timeout: 5) {
            me.tap()
            let settings = app.buttons["设置"]
            if !settings.waitForExistence(timeout: 2) {
                app.coordinate(withNormalizedOffset: CGVector(dx: 0.85, dy: 0.955)).tap()
            }
            if settings.waitForExistence(timeout: 10) {
                settings.tap()
                app.buttons["切换手机号"].tap()
                _ = app.textFields["account-phone"].waitForExistence(timeout: 15)
            }
        }
        app.terminate()
        app.launchEnvironment["MAICHAT_LOCAL_UI_SOCIAL_TEST"] = "1"
        app.launch()
        return app
    }

    /// 一次真实 UI 操作验证发 social_bind 短信、填码和合并到原账号会话。
    @MainActor private func bindPhone(_ phone: String, in app: XCUIApplication) async throws {
        let phoneField = app.textFields["social-bind-phone"]
        XCTAssertTrue(phoneField.waitForExistence(timeout: 25))
        let opened = XCTAttachment(screenshot: app.screenshot())
        opened.name = "social-phone-binding-opened"
        opened.lifetime = .keepAlways
        add(opened)

        phoneField.tap()
        phoneField.typeText(phone)
        XCTAssertEqual(phoneField.value as? String, phone)
        app.swipeUp()
        app.buttons["social-bind-minimum-age"].tap()
        app.buttons["social-bind-agreement"].tap()
        let submit = app.buttons["social-bind-submit"]
        guard submit.isEnabled else {
            let blocked = XCTAttachment(screenshot: app.screenshot())
            blocked.name = "social-phone-binding-submit-disabled"
            blocked.lifetime = .keepAlways
            add(blocked)
            XCTFail("手机号与同意选项已填写，绑定短信按钮仍不可用")
            return
        }
        submit.tap()
        let codeField = app.textFields["social-bind-code"]
        XCTAssertTrue(codeField.waitForExistence(timeout: 15))
        let code = try await smsCode(for: phone)
        codeField.tap()
        codeField.typeText(code)
        XCTAssertTrue(submit.isEnabled)
        submit.tap()
        XCTAssertTrue(app.buttons["我"].waitForExistence(timeout: 30))
        let completed = XCTAttachment(screenshot: app.screenshot())
        completed.name = "social-phone-binding-completed"
        completed.lifetime = .keepAlways
        add(completed)
    }

    @MainActor private func smsCode(for phone: String) async throws -> String {
        var request = URLRequest(url: URL(string: "http://127.0.0.1:18191/v1/test/sms-code")!)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONSerialization.data(withJSONObject: ["phone": phone])
        let (data, response) = try await URLSession.shared.data(for: request)
        XCTAssertEqual((response as? HTTPURLResponse)?.statusCode, 200)
        let value = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: String])
        return try XCTUnwrap(value["code"])
    }
}
