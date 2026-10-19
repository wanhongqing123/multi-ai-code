// 文件作用：用本机假微信/QQ 凭据跑通 202 手机绑定与 200 已绑定登录界面。
import XCTest

final class SocialLoginUITests: XCTestCase {
    @MainActor func testLiveMaiChatAccountPasswordLoginShowsMaiChatId() throws {
        let environment = ProcessInfo.processInfo.environment
        guard let accountId = environment["MAICHAT_TEST_A_ID"],
              let password = environment["MAICHAT_TEST_A_PASSWORD"] else {
            throw XCTSkip("需要临时测试账号")
        }
        let app = XCUIApplication()
        app.launch()
        let field = app.textFields["account-phone"]
        XCTAssertTrue(field.waitForExistence(timeout: 20))
        app.buttons["密码登录"].tap()
        field.tap()
        field.typeText(accountId)
        let passwordField = app.secureTextFields["输入登录密码"]
        passwordField.tap()
        passwordField.typeText(password)
        app.buttons["account-minimum-age"].tap()
        app.buttons["account-agreement"].tap()
        app.buttons["account-submit"].tap()
        let me = app.buttons["我"]
        XCTAssertTrue(me.waitForExistence(timeout: 30))
        openAccountSettings(in: app)
        let accountValue = app.staticTexts.matching(NSPredicate(
            format: "label CONTAINS %@ AND label CONTAINS %@", "登录账号", accountId
        )).firstMatch
        XCTAssertTrue(accountValue.waitForExistence(timeout: 10))
        let screenshot = XCTAttachment(screenshot: app.screenshot())
        screenshot.name = "maichat-account-login-complete"
        screenshot.lifetime = .keepAlways
        add(screenshot)

        app.terminate()
        app.launch()
        XCTAssertTrue(me.waitForExistence(timeout: 30))
        openAccountSettings(in: app)
        let restored = XCTAttachment(screenshot: app.screenshot())
        restored.name = "maichat-account-id-after-relaunch"
        restored.lifetime = .keepAlways
        add(restored)
        XCTAssertTrue(accountValue.waitForExistence(timeout: 15))
    }

    @MainActor private func openAccountSettings(in app: XCUIApplication) {
        for _ in 0..<8 {
            if app.buttons["以后"].isHittable { app.buttons["以后"].tap() }
            if app.buttons["稍后"].isHittable { app.buttons["稍后"].tap() }
            if app.staticTexts["登录账号"].exists { return }
            let settings = app.buttons["设置"]
            if settings.isHittable {
                settings.tap()
            } else {
                app.coordinate(withNormalizedOffset: CGVector(dx: 0.85, dy: 0.925)).tap()
            }
        }
        XCTAssertTrue(app.staticTexts["登录账号"].waitForExistence(timeout: 10))
    }

    @MainActor func testLiveNicknameEditSurvivesRelaunch() throws {
        let environment = ProcessInfo.processInfo.environment
        guard let accountId = environment["MAICHAT_TEST_A_ID"],
              let password = environment["MAICHAT_TEST_A_PASSWORD"] else {
            throw XCTSkip("需要临时测试账号")
        }
        let app = XCUIApplication()
        app.launch()
        let field = app.textFields["account-phone"]
        XCTAssertTrue(field.waitForExistence(timeout: 20))
        app.buttons["密码登录"].tap()
        field.tap()
        field.typeText(accountId)
        let passwordField = app.secureTextFields["输入登录密码"]
        passwordField.tap()
        passwordField.typeText(password)
        app.buttons["account-minimum-age"].tap()
        app.buttons["account-agreement"].tap()
        app.buttons["account-submit"].tap()
        XCTAssertTrue(app.buttons["我"].waitForExistence(timeout: 30))
        openPersonalProfile(in: app)
        let edit = app.buttons["account-nickname-entry"]
        XCTAssertTrue(edit.waitForExistence(timeout: 10))
        edit.tap()
        let input = app.textFields["account-nickname-input"]
        XCTAssertTrue(input.waitForExistence(timeout: 10))
        input.tap()
        input.typeText("昵称测试")
        app.buttons["保存"].tap()
        let nickname = app.staticTexts.matching(NSPredicate(
            format: "label CONTAINS %@", "昵称测试"
        )).firstMatch
        XCTAssertTrue(nickname.waitForExistence(timeout: 15))
        let screenshot = XCTAttachment(screenshot: app.screenshot())
        screenshot.name = "maichat-nickname-after-edit"
        screenshot.lifetime = .keepAlways
        add(screenshot)

        app.terminate()
        app.launch()
        XCTAssertTrue(app.buttons["我"].waitForExistence(timeout: 30))
        openPersonalProfile(in: app)
        XCTAssertTrue(nickname.waitForExistence(timeout: 15))
    }

    @MainActor private func openPersonalProfile(in app: XCUIApplication) {
        for _ in 0..<8 {
            if app.buttons["以后"].isHittable { app.buttons["以后"].tap() }
            if app.buttons["稍后"].isHittable { app.buttons["稍后"].tap() }
            let profile = app.buttons["personal-profile-entry"]
            if profile.isHittable {
                profile.tap()
                return
            }
            app.coordinate(withNormalizedOffset: CGVector(dx: 0.85, dy: 0.925)).tap()
        }
        XCTFail("个人资料入口不可用")
    }

    @MainActor func testLiveFriendRequestUsesMaiChatAccountId() throws {
        guard let friendId = ProcessInfo.processInfo.environment["MAICHAT_TEST_B_ID"] else {
            throw XCTSkip("需要第二个临时测试账号")
        }
        let app = XCUIApplication()
        app.launch()
        if app.buttons["以后"].waitForExistence(timeout: 3) {
            app.buttons["以后"].tap()
        }
        let contacts = app.buttons["通讯录"]
        XCTAssertTrue(contacts.waitForExistence(timeout: 30))
        contacts.tap()
        let add = app.buttons["添加好友"]
        XCTAssertTrue(add.waitForExistence(timeout: 10))
        add.tap()
        let input = app.textFields["手机号或账号 ID"]
        XCTAssertTrue(input.waitForExistence(timeout: 10))
        input.tap()
        input.typeText(friendId)
        app.buttons["发送申请"].tap()
        XCTAssertFalse(input.waitForExistence(timeout: 3))
    }

    @MainActor func testLiveFriendListAndDeletion() throws {
        guard let friendId = ProcessInfo.processInfo.environment["MAICHAT_TEST_B_ID"] else {
            throw XCTSkip("需要第二个临时测试账号")
        }
        let app = XCUIApplication()
        app.launch()
        if app.buttons["以后"].waitForExistence(timeout: 3) {
            app.buttons["以后"].tap()
        }
        let contacts = app.buttons["通讯录"]
        XCTAssertTrue(contacts.waitForExistence(timeout: 30))
        contacts.tap()
        let friend = app.staticTexts[friendId].firstMatch
        XCTAssertTrue(friend.waitForExistence(timeout: 30))
        let screenshot = XCTAttachment(screenshot: app.screenshot())
        screenshot.name = "maichat-accepted-friend"
        screenshot.lifetime = .keepAlways
        add(screenshot)
        let row = app.cells.containing(.staticText, identifier: friendId).firstMatch
        XCTAssertTrue(row.exists)
        row.swipeLeft()
        app.buttons["删除"].tap()
        XCTAssertFalse(friend.waitForExistence(timeout: 5))
    }

    @MainActor func testLiveFriendProfileOpensBeforeChat() throws {
        let environment = ProcessInfo.processInfo.environment
        guard let accountId = environment["MAICHAT_TEST_A_ID"],
              let password = environment["MAICHAT_TEST_A_PASSWORD"],
              let friendId = environment["MAICHAT_TEST_B_ID"] else {
            throw XCTSkip("需要两个已建立好友关系的测试账号")
        }
        let app = XCUIApplication()
        app.launch()
        let field = app.textFields["account-phone"]
        XCTAssertTrue(field.waitForExistence(timeout: 20))
        app.buttons["密码登录"].tap()
        field.tap()
        field.typeText(accountId)
        let passwordField = app.secureTextFields["输入登录密码"]
        passwordField.tap()
        passwordField.typeText(password)
        app.buttons["account-minimum-age"].tap()
        app.buttons["account-agreement"].tap()
        app.buttons["account-submit"].tap()
        let contacts = app.buttons["通讯录"]
        XCTAssertTrue(contacts.waitForExistence(timeout: 30))
        if app.buttons["以后"].waitForExistence(timeout: 3) { app.buttons["以后"].tap() }
        if app.buttons["稍后"].waitForExistence(timeout: 5) { app.buttons["稍后"].tap() }
        contacts.tap()
        let friend = app.buttons["friend-profile-\(friendId)"]
        XCTAssertTrue(friend.waitForExistence(timeout: 30))
        friend.tap()
        XCTAssertTrue(app.staticTexts["好友资料"].waitForExistence(timeout: 10))
        XCTAssertTrue(app.staticTexts["friend-profile-account-id"].exists)
        let profileScreenshot = XCTAttachment(screenshot: app.screenshot())
        profileScreenshot.name = "maichat-friend-profile"
        profileScreenshot.lifetime = .keepAlways
        add(profileScreenshot)
        app.buttons["friend-profile-message"].tap()
        XCTAssertTrue(app.textViews["message-composer-text-view"].waitForExistence(timeout: 10))
    }

    @MainActor func testAccountPasswordModeAcceptsMaiChatId() throws {
        let app = XCUIApplication()
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = ""
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] = ""
        app.launch()
        let field = app.textFields["account-phone"]
        if !field.waitForExistence(timeout: 5) {
            let me = app.buttons["我"]
            XCTAssertTrue(me.waitForExistence(timeout: 10))
            me.tap()
            let settings = app.buttons["设置"]
            XCTAssertTrue(settings.waitForExistence(timeout: 10))
            settings.tap()
            app.buttons["切换账号"].tap()
        }
        XCTAssertTrue(field.waitForExistence(timeout: 20))
        app.buttons["密码登录"].tap()
        field.tap()
        field.typeText("test_account")
        let password = app.secureTextFields["输入登录密码"]
        XCTAssertTrue(password.exists)
        password.tap()
        password.typeText("test-account-password-123456")
        app.buttons["account-minimum-age"].tap()
        app.buttons["account-agreement"].tap()
        XCTAssertTrue(app.buttons["account-submit"].isEnabled)
        let screenshot = XCTAttachment(screenshot: app.screenshot())
        screenshot.name = "maichat-account-password-login"
        screenshot.lifetime = .keepAlways
        add(screenshot)
    }

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
        app.buttons["切换账号"].tap()
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
                app.buttons["切换账号"].tap()
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
