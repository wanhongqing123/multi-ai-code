// 文件作用：用本机账号与假微信/QQ 凭据验证普通登录、手机绑定和相关 IM 界面。
import XCTest

final class SocialLoginUITests: XCTestCase {
    /// 在干净的模拟器安装中验证服务选择默认值及重启后的持久化；只使用本机假账号。
    @MainActor func testSpeechProviderSelectionSurvivesRelaunch() async throws {
        #if DEBUG && targetEnvironment(simulator)
        continueAfterFailure = false
        // 独立内存夹具使用专用端口，避免影响其他并发验收及其账号状态。
        let environment = ProcessInfo.processInfo.environment
        let authPort = environment["MAICHAT_ASR_UI_FIXTURE_AUTH_PORT"] ?? "18592"
        let socketPort = environment["MAICHAT_ASR_UI_FIXTURE_SOCKET_PORT"] ?? "18593"
        let modelPort = environment["MAICHAT_ASR_UI_FIXTURE_MODEL_PORT"] ?? "18594"
        let base = "http://127.0.0.1:\(authPort)"
        let statusURL = URL(string: base + "/test/status")!
        let (statusData, statusResponse) = try await URLSession.shared.data(from: statusURL)
        XCTAssertEqual((statusResponse as? HTTPURLResponse)?.statusCode, 200)
        let status = try XCTUnwrap(JSONSerialization.jsonObject(with: statusData) as? [String: Any])
        XCTAssertTrue((status["macSessions"] as? [String] ?? []).contains("ses_mock_mac"),
                      "需要独立的 Session sync 本机夹具")
        let certificateURL = URL(string: base + "/test/cert")!
        let (certificate, _) = try await URLSession.shared.data(from: certificateURL)

        let app = XCUIApplication()
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] = "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = authPort
        app.launchEnvironment["MAICHAT_LOCAL_UI_SOCKET_PORT"] = socketPort
        app.launchEnvironment["MAICHAT_LOCAL_UI_MODEL_PORT"] = modelPort
        app.launchEnvironment["MAICHAT_LOCAL_UI_CA_BASE64"] = certificate.base64EncodedString()
        defer { app.terminate() }

        func openSettings() {
            XCTAssertTrue(app.buttons["我的"].waitForExistence(timeout: 30))
            if app.buttons["稍后"].exists { app.buttons["稍后"].tap() }
            let imError = app.staticTexts.matching(NSPredicate(
                format: "label CONTAINS %@", "IM 操作失败"
            )).firstMatch
            if imError.waitForExistence(timeout: 2) { imError.tap() }
            app.buttons["我的"].tap()
            let settings = app.buttons["设置"]
            XCTAssertTrue(settings.waitForExistence(timeout: 10))
            settings.tap()
        }

        func assertProvider(_ expected: String, screenshot name: String) {
            let picker = app.descendants(matching: .any)
                .matching(identifier: "speech-recognition-provider").firstMatch
            XCTAssertTrue(picker.waitForExistence(timeout: 10))
            let label = picker.label + " " + (picker.value as? String ?? "")
            XCTAssertTrue(label.contains(expected) || picker.staticTexts[expected].exists,
                          "服务选择应显示 \(expected)，当前为 \(label)")
            let attachment = XCTAttachment(screenshot: app.screenshot())
            attachment.name = name
            attachment.lifetime = .keepAlways
            add(attachment)
        }

        func selectProvider(_ title: String) {
            let picker = app.descendants(matching: .any)
                .matching(identifier: "speech-recognition-provider").firstMatch
            picker.tap()
            let option = app.buttons[title]
            XCTAssertTrue(option.waitForExistence(timeout: 5))
            option.tap()
        }

        app.launch()
        openSettings()
        assertProvider("腾讯云", screenshot: "speech-provider-default-tencent")
        selectProvider("阿里云")
        assertProvider("阿里云", screenshot: "speech-provider-aliyun-selected")
        app.terminate()
        app.launch()
        openSettings()
        assertProvider("阿里云", screenshot: "speech-provider-aliyun-after-relaunch")
        selectProvider("腾讯云")
        assertProvider("腾讯云", screenshot: "speech-provider-tencent-restored")
        #else
        throw XCTSkip("服务选择界面验收需要 Debug 模拟器构建")
        #endif
    }

    /// 正常登录页只接本机假短信，验证输入验证码与切换号码不会绕过或误用短信冷却。
    @MainActor func testNormalPhoneLoginRetainsSmsCooldownAfterChangingPhone() async throws {
        #if DEBUG && targetEnvironment(simulator)
        continueAfterFailure = false
        let url = URL(string: "http://127.0.0.1:18590/test/status")!
        let (data, _) = try await URLSession.shared.data(from: url)
        let fixture = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
        XCTAssertEqual(fixture["fixture"] as? String, "maichat-login-sms-ui")
        let app = XCUIApplication()
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = ""
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] = ""
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18590"
        app.launch()
        defer { app.terminate() }
        let phone = app.textFields["account-phone"]
        XCTAssertTrue(phone.waitForExistence(timeout: 20))
        phone.tap()
        phone.typeText("13900000881")
        app.swipeUp()
        app.buttons["account-minimum-age"].tap()
        app.buttons["account-agreement"].tap()
        let submit = app.buttons["account-submit"]
        submit.tap()
        let code = app.textFields["account-code"]
        XCTAssertTrue(code.waitForExistence(timeout: 15))
        let resend = app.buttons["account-resend"]
        XCTAssertTrue(resend.label.contains("秒后重试"))
        XCTAssertFalse(resend.isEnabled)
        code.tap()
        code.typeText("123456")
        app.swipeUp()
        XCTAssertTrue(submit.isEnabled, "六位验证码可提交，短信冷却不应阻塞登录")
        let sent = XCTAttachment(screenshot: app.screenshot())
        sent.name = "phone-login-resend-disabled-code-submit-enabled"
        sent.lifetime = .keepAlways
        add(sent)
        for value in ["13900000882", "13900000881"] {
            phone.tap()
            phone.typeText(String(repeating: XCUIKeyboardKey.delete.rawValue, count: 11))
            phone.typeText(value)
            app.swipeUp()
            XCTAssertFalse(app.textFields["account-code"].exists)
            XCTAssertEqual(submit.isEnabled, value == "13900000882")
        }
        XCTAssertTrue(submit.label.contains("秒后重试"))
        let returned = XCTAttachment(screenshot: app.screenshot())
        returned.name = "phone-login-original-number-cooldown-preserved"
        returned.lifetime = .keepAlways
        add(returned)
        #else
        throw XCTSkip("本机假短信界面验收需要 Debug 模拟器构建")
        #endif
    }

    @MainActor func testGroupCreationTextAndPrivateChatIsolation() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--im-group-ui-test"]
        app.launch()

        let actions = app.buttons["im-message-actions"]
        XCTAssertTrue(actions.waitForExistence(timeout: 20))
        actions.tap()
        let createEntry = app.buttons["发起群聊"]
        XCTAssertTrue(createEntry.waitForExistence(timeout: 5))
        XCTAssertTrue(app.buttons["添加朋友"].exists)
        XCTAssertTrue(app.buttons["扫一扫"].exists)
        let menuScreenshot = XCTAttachment(screenshot: app.screenshot())
        menuScreenshot.name = "im-group-menu-three-actions"
        menuScreenshot.lifetime = .keepAlways
        add(menuScreenshot)
        createEntry.tap()

        let groupName = app.textFields["im-group-name"]
        let createButton = app.buttons["im-group-create"]
        XCTAssertTrue(groupName.waitForExistence(timeout: 5))
        XCTAssertFalse(createButton.isEnabled)
        let member = app.buttons["im-group-member-sim-friend-a"]
        XCTAssertTrue(member.exists)
        member.tap()
        groupName.tap()
        groupName.typeText("项目讨论")
        XCTAssertTrue(createButton.isEnabled)
        let pickerScreenshot = XCTAttachment(screenshot: app.screenshot())
        pickerScreenshot.name = "im-group-creation-selected-friend"
        pickerScreenshot.lifetime = .keepAlways
        add(pickerScreenshot)
        createButton.tap()

        let groupDraft = app.descendants(matching: .any)["im-group-draft"]
        XCTAssertTrue(groupDraft.waitForExistence(timeout: 8))
        XCTAssertTrue(app.staticTexts["项目讨论"].exists)
        XCTAssertTrue(app.staticTexts["2 位成员"].exists)
        XCTAssertFalse(app.buttons["添加附件"].exists)
        groupDraft.tap()
        groupDraft.typeText("群内测试消息")
        app.buttons["im-group-send"].tap()
        XCTAssertTrue(app.staticTexts["群内测试消息"].waitForExistence(timeout: 8))
        XCTAssertTrue(app.staticTexts["已收到群消息"].waitForExistence(timeout: 8))
        let conversationScreenshot = XCTAttachment(screenshot: app.screenshot())
        conversationScreenshot.name = "im-group-text-and-member-reply"
        conversationScreenshot.lifetime = .keepAlways
        add(conversationScreenshot)

        app.buttons["返回会话列表"].tap()
        let groupRow = app.buttons.matching(NSPredicate(
            format: "identifier BEGINSWITH %@", "im-group-conversation-"
        )).firstMatch
        XCTAssertTrue(groupRow.waitForExistence(timeout: 5))
        XCTAssertTrue(app.staticTexts["已收到群消息"].exists)
        let listScreenshot = XCTAttachment(screenshot: app.screenshot())
        listScreenshot.name = "im-group-and-private-conversation-list"
        listScreenshot.lifetime = .keepAlways
        add(listScreenshot)
        let privateChat = app.buttons["im-conversation-sim-friend-b"]
        XCTAssertTrue(privateChat.exists)
        privateChat.tap()
        XCTAssertTrue(app.staticTexts["群内测试消息"].waitForNonExistence(timeout: 5))
        XCTAssertTrue(app.staticTexts["已收到群消息"].waitForNonExistence(timeout: 5))
        let privateScreenshot = XCTAttachment(screenshot: app.screenshot())
        privateScreenshot.name = "im-private-chat-after-group"
        privateScreenshot.lifetime = .keepAlways
        add(privateScreenshot)
        app.buttons["返回会话列表"].tap()
        groupRow.tap()
        XCTAssertTrue(app.staticTexts["群内测试消息"].waitForExistence(timeout: 5))
    }

    @MainActor func testGroupPartialCreationOpensExistingGroup() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--im-group-activation-fail-ui-test"]
        app.launch()

        let actions = app.buttons["im-message-actions"]
        XCTAssertTrue(actions.waitForExistence(timeout: 20))
        actions.tap()
        app.buttons["发起群聊"].tap()
        let groupName = app.textFields["im-group-name"]
        XCTAssertTrue(groupName.waitForExistence(timeout: 5))
        app.buttons["im-group-member-sim-friend-a"].tap()
        groupName.tap()
        groupName.typeText("待恢复群聊")
        let createButton = app.buttons["im-group-create"]
        createButton.tap()

        let recover = app.buttons["im-group-open-recovered"]
        XCTAssertTrue(recover.waitForExistence(timeout: 8))
        XCTAssertFalse(createButton.isEnabled)
        XCTAssertFalse(app.descendants(matching: .any)["im-group-draft"].exists)
        let recoveryScreenshot = XCTAttachment(screenshot: app.screenshot())
        recoveryScreenshot.name = "im-group-created-activation-pending"
        recoveryScreenshot.lifetime = .keepAlways
        add(recoveryScreenshot)

        recover.tap()
        let groupDraft = app.descendants(matching: .any)["im-group-draft"]
        XCTAssertTrue(groupDraft.waitForExistence(timeout: 8))
        groupDraft.tap()
        groupDraft.typeText("恢复后的首条群消息")
        app.buttons["im-group-send"].tap()
        XCTAssertTrue(app.staticTexts["已收到群消息"].waitForExistence(timeout: 8))
        app.buttons["返回会话列表"].tap()
        actions.tap()
        app.buttons["发起群聊"].tap()
        XCTAssertTrue(app.textFields["im-group-name"].waitForExistence(timeout: 5))
        XCTAssertFalse(recover.exists)
    }

    @MainActor func testLeavingGroupClosesOpenGroupWithoutLeakingHistory() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--im-group-ui-test"]
        app.launch()
        openFixtureGroup(in: app, named: "退群前的群聊")
        let draft = app.descendants(matching: .any)["im-group-draft"]
        XCTAssertTrue(draft.waitForExistence(timeout: 8))
        draft.tap()
        draft.typeText("仅原群可见的消息")
        app.buttons["im-group-send"].tap()
        XCTAssertTrue(app.staticTexts["仅原群可见的消息"].waitForExistence(timeout: 8))

        app.buttons["fixture-drop-group"].tap()
        let actions = app.buttons["im-message-actions"]
        XCTAssertTrue(actions.waitForExistence(timeout: 8))
        XCTAssertTrue(actions.isHittable)
        XCTAssertFalse(app.descendants(matching: .any)["im-group-header"].exists)
        XCTAssertFalse(app.buttons.matching(NSPredicate(
            format: "identifier BEGINSWITH %@", "im-group-conversation-"
        )).firstMatch.exists)
        XCTAssertFalse(app.staticTexts["仅原群可见的消息"].exists)
        XCTAssertTrue(app.buttons["im-conversation-sim-friend-b"].exists)
    }

    @MainActor func testSwitchingAccountClosesOldGroupAndHistory() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--im-group-ui-test"]
        app.launch()
        openFixtureGroup(in: app, named: "旧账号群聊")
        let draft = app.descendants(matching: .any)["im-group-draft"]
        XCTAssertTrue(draft.waitForExistence(timeout: 8))
        draft.tap()
        draft.typeText("仅旧账号可见的消息")
        app.buttons["im-group-send"].tap()
        XCTAssertTrue(app.staticTexts["仅旧账号可见的消息"].waitForExistence(timeout: 8))

        app.buttons["fixture-switch-owner"].tap()
        let actions = app.buttons["im-message-actions"]
        XCTAssertTrue(actions.waitForExistence(timeout: 8))
        XCTAssertTrue(actions.isHittable)
        XCTAssertFalse(app.descendants(matching: .any)["im-group-header"].exists)
        XCTAssertFalse(app.buttons.matching(NSPredicate(
            format: "identifier BEGINSWITH %@", "im-group-conversation-"
        )).firstMatch.exists)
        XCTAssertFalse(app.staticTexts["仅旧账号可见的消息"].exists)
        XCTAssertTrue(app.staticTexts["暂无会话"].exists)
    }

    @MainActor private func openFixtureGroup(in app: XCUIApplication, named name: String) {
        let actions = app.buttons["im-message-actions"]
        XCTAssertTrue(actions.waitForExistence(timeout: 20))
        actions.tap()
        app.buttons["发起群聊"].tap()
        let groupName = app.textFields["im-group-name"]
        XCTAssertTrue(groupName.waitForExistence(timeout: 5))
        app.buttons["im-group-member-sim-friend-a"].tap()
        groupName.tap()
        groupName.typeText(name)
        app.buttons["im-group-create"].tap()
        XCTAssertTrue(app.staticTexts[name].waitForExistence(timeout: 8))
    }

    @MainActor func testNormalAppMessageHeaderAndAddFriendMenu() async throws {
        let fixtureURL = URL(string: "http://127.0.0.1:18190/test/status")!
        guard let response = try? await URLSession.shared.data(from: fixtureURL),
              (response.1 as? HTTPURLResponse)?.statusCode == 200 else {
            throw XCTSkip("需要本机测试账号服务")
        }
        let certificateURL = URL(string: "http://127.0.0.1:18190/test/cert")!
        let (certificate, _) = try await URLSession.shared.data(from: certificateURL)
        let app = XCUIApplication()
        app.launchArguments = ["--friend-qr-ui-test"]
        app.launchEnvironment["MAICHAT_FRIEND_QR_TEST_VALUE"] =
            "maichat://friend/add?user_id=mac-multi-ai-code"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] = "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18190"
        app.launchEnvironment["MAICHAT_LOCAL_UI_CA_BASE64"] = certificate.base64EncodedString()
        app.launch()

        let actions = app.buttons["im-message-actions"]
        XCTAssertTrue(actions.waitForExistence(timeout: 30))
        let notificationLater = app.buttons["稍后"]
        if notificationLater.waitForExistence(timeout: 10) {
            notificationLater.tap()
        }
        let imError = app.staticTexts.matching(NSPredicate(
            format: "label CONTAINS %@", "IM 操作失败"
        )).firstMatch
        if imError.waitForExistence(timeout: 2) {
            imError.tap()
        }
        XCTAssertTrue(app.staticTexts["消息"].exists)
        let header = XCTAttachment(screenshot: app.screenshot())
        header.name = "normal-app-im-message-header"
        header.lifetime = .keepAlways
        add(header)

        actions.tap()
        let addFriend = app.buttons["添加朋友"]
        XCTAssertTrue(addFriend.waitForExistence(timeout: 5))
        XCTAssertTrue(app.buttons["扫一扫"].exists)
        let menu = XCTAttachment(screenshot: app.screenshot())
        menu.name = "normal-app-im-add-friend-menu"
        menu.lifetime = .keepAlways
        add(menu)
        addFriend.tap()
        XCTAssertTrue(app.textFields["手机号或账号 ID"].waitForExistence(timeout: 5))
        app.buttons["取消"].tap()
        XCTAssertTrue(actions.isHittable)

        actions.tap()
        app.buttons["扫一扫"].tap()
        XCTAssertTrue(app.buttons["模拟识别二维码"].waitForExistence(timeout: 5))
        app.buttons["模拟识别二维码"].tap()
        let scannedAccount = app.textFields["手机号或账号 ID"]
        XCTAssertTrue(scannedAccount.waitForExistence(timeout: 5))
        XCTAssertEqual(scannedAccount.value as? String, "mac-multi-ai-code")
        app.buttons["取消"].tap()
        XCTAssertTrue(actions.isHittable)
    }

    @MainActor func testMessageAddFriendMenuOpensSharedDialog() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--im-add-friend-menu-ui-test",
                               "--friend-qr-ui-test"]
        app.launchEnvironment["MAICHAT_FRIEND_QR_TEST_VALUE"] =
            "maichat://friend/add?user_id=mac-multi-ai-code"
        app.launch()

        let actions = app.buttons["im-message-actions"]
        XCTAssertTrue(actions.waitForExistence(timeout: 15))
        XCTAssertTrue(app.staticTexts["消息"].exists)
        let headerScreenshot = XCTAttachment(screenshot: app.screenshot())
        headerScreenshot.name = "im-message-header"
        headerScreenshot.lifetime = .keepAlways
        add(headerScreenshot)
        actions.tap()
        let addFriend = app.buttons["添加朋友"]
        XCTAssertTrue(addFriend.waitForExistence(timeout: 5))
        XCTAssertTrue(app.buttons["扫一扫"].exists)
        let menuScreenshot = XCTAttachment(screenshot: app.screenshot())
        menuScreenshot.name = "im-message-add-friend-menu"
        menuScreenshot.lifetime = .keepAlways
        add(menuScreenshot)

        addFriend.tap()
        let accountField = app.textFields["手机号或账号 ID"]
        XCTAssertTrue(accountField.waitForExistence(timeout: 5))
        XCTAssertFalse(app.buttons["扫一扫"].exists)
        XCTAssertTrue(app.buttons["我的二维码"].exists)
        let dialogScreenshot = XCTAttachment(screenshot: app.screenshot())
        dialogScreenshot.name = "im-message-add-friend-dialog"
        dialogScreenshot.lifetime = .keepAlways
        add(dialogScreenshot)
        app.buttons["取消"].tap()
        XCTAssertFalse(accountField.exists)
        XCTAssertTrue(actions.isHittable)

        actions.tap()
        XCTAssertTrue(app.buttons["扫一扫"].waitForExistence(timeout: 5))
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.2, dy: 0.45)).tap()
        XCTAssertFalse(app.buttons["扫一扫"].exists)

        actions.tap()
        app.buttons["扫一扫"].tap()
        let simulatedScan = app.buttons["模拟识别二维码"]
        XCTAssertTrue(simulatedScan.waitForExistence(timeout: 5))
        let scannerScreenshot = XCTAttachment(screenshot: app.screenshot())
        scannerScreenshot.name = "im-message-friend-qr-scanner"
        scannerScreenshot.lifetime = .keepAlways
        add(scannerScreenshot)
        app.buttons["关闭"].tap()
        XCTAssertTrue(actions.waitForExistence(timeout: 5))
        XCTAssertFalse(accountField.exists)

        actions.tap()
        app.buttons["扫一扫"].tap()
        XCTAssertTrue(simulatedScan.waitForExistence(timeout: 5))
        simulatedScan.tap()
        XCTAssertTrue(accountField.waitForExistence(timeout: 5))
        XCTAssertEqual(accountField.value as? String, "mac-multi-ai-code")
        let routedFriend = XCTAttachment(screenshot: app.screenshot())
        routedFriend.name = "im-friend-code-routed"
        routedFriend.lifetime = .keepAlways
        add(routedFriend)
        app.buttons["取消"].tap()
        XCTAssertTrue(actions.isHittable)
    }

    @MainActor func testMessageScanRejectsUnrelatedQRCode() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--im-add-friend-menu-ui-test",
                               "--friend-qr-ui-test"]
        app.launchEnvironment["MAICHAT_FRIEND_QR_TEST_VALUE"] = "https://example.com/other"
        app.launch()

        let actions = app.buttons["im-message-actions"]
        XCTAssertTrue(actions.waitForExistence(timeout: 15))
        actions.tap()
        app.buttons["扫一扫"].tap()
        app.buttons["模拟识别二维码"].tap()
        XCTAssertTrue(app.staticTexts["这不是 MaiChat 好友或电脑登录二维码"]
            .waitForExistence(timeout: 5))
        XCTAssertFalse(app.textFields["手机号或账号 ID"].exists)
        app.buttons["重新扫描"].tap()
        XCTAssertFalse(app.staticTexts["这不是 MaiChat 好友或电脑登录二维码"].exists)
        app.buttons["关闭"].tap()
        XCTAssertTrue(actions.isHittable)
    }

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
        let me = app.buttons["我的"]
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
        XCTAssertTrue(app.buttons["我的"].waitForExistence(timeout: 30))
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
        XCTAssertTrue(app.buttons["我的"].waitForExistence(timeout: 30))
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
        let contacts = app.buttons["好友列表"]
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
        let contacts = app.buttons["好友列表"]
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
        let contacts = app.buttons["好友列表"]
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
            let me = app.buttons["我的"]
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
        let me = app.buttons["我的"]
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
        XCTAssertTrue(app.buttons["我的"].waitForExistence(timeout: 30))
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
        let me = app.buttons["我的"]
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
        XCTAssertTrue(app.buttons["我的"].waitForExistence(timeout: 30))
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
