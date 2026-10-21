// 文件作用：驱动真实 iOS 界面，验证创作中心和 IM 的关键操作及画面状态。
// 驱动真实 iOS 页面检查会话切换、媒体预览、语音层和滚动边界。
// 使用本机测试账号与无付费素材，并保留关键页面截图供人工验收。
import XCTest

final class AIAssistantUITests: XCTestCase {
    @MainActor func testConnectedDeviceShowsProductionSessionDirectory() throws {
        #if targetEnvironment(simulator)
        throw XCTSkip("此用例只验收已连接 iPhone 的正式账号路径")
        #else
        let app = XCUIApplication()
        app.launch()
        let launch = XCTAttachment(screenshot: app.screenshot())
        launch.name = "production-iphone-launch"
        launch.lifetime = .keepAlways
        add(launch)
        guard app.buttons["创作中心"].waitForExistence(timeout: 30) else {
            XCTFail("iPhone 尚未进入登录后的 MaiChat 主页面")
            return
        }
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.375, dy: 0.93)).tap()
        XCTAssertTrue(app.buttons["对话列表"].waitForExistence(timeout: 20))
        let start = app.coordinate(withNormalizedOffset: CGVector(dx: 0.01, dy: 0.5))
        let end = app.coordinate(withNormalizedOffset: CGVector(dx: 0.72, dy: 0.5))
        start.press(forDuration: 0.05, thenDragTo: end)
        XCTAssertTrue(app.staticTexts["对话"].waitForExistence(timeout: 5))
        let catalog = XCTAttachment(screenshot: app.screenshot())
        catalog.name = "production-iphone-session-directory"
        catalog.lifetime = .keepAlways
        add(catalog)
        XCTAssertTrue(app.buttons["ai-session-device-local"].exists)
        let drawer = app.scrollViews["ai-session-drawer"]
        let macGroup = app.buttons.matching(NSPredicate(
            format: "identifier BEGINSWITH %@", "ai-session-device-mac:"
        )).firstMatch
        for _ in 0..<8 where !macGroup.isHittable { drawer.swipeUp() }
        XCTAssertTrue(macGroup.waitForExistence(timeout: 10))
        let grouped = XCTAttachment(screenshot: app.screenshot())
        grouped.name = "production-iphone-device-groups"
        grouped.lifetime = .keepAlways
        add(grouped)
        #endif
    }

    @MainActor func testNormalAppReadsAndControlsLocalFakeMacSession() async throws {
        let fixture = URL(string: "http://127.0.0.1:18190/v1/session-sync/sessions")!
        let probe = try? await URLSession.shared.data(from: fixture)
        guard (probe?.1 as? HTTPURLResponse)?.statusCode == 401 else {
            throw XCTSkip("需要本机无付费 Session 同步服务")
        }
        let certificateURL = URL(string: "http://127.0.0.1:18190/test/cert")!
        let (certificate, certificateResponse) = try await URLSession.shared.data(from: certificateURL)
        XCTAssertEqual((certificateResponse as? HTTPURLResponse)?.statusCode, 200)
        let app = XCUIApplication()
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] = "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18190"
        app.launchEnvironment["MAICHAT_LOCAL_UI_CA_BASE64"] = certificate.base64EncodedString()
        app.launch()
        let creationCenter = app.buttons["创作中心"]
        guard creationCenter.waitForExistence(timeout: 30) else {
            XCTFail("测试账号未进入 MaiChat 主页面")
            return
        }
        // iOS 26 的底栏图标偶尔给出 {-1,-1} 的辅助功能命中点；点真实底栏位置。
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.375, dy: 0.93)).tap()
        let conversations = app.buttons["对话列表"]
        guard conversations.waitForExistence(timeout: 20) else {
            XCTFail("未打开创作中心")
            return
        }
        let swipeStart = app.coordinate(withNormalizedOffset: CGVector(dx: 0.01, dy: 0.5))
        let swipeEnd = app.coordinate(withNormalizedOffset: CGVector(dx: 0.72, dy: 0.5))
        swipeStart.press(forDuration: 0.05, thenDragTo: swipeEnd)
        XCTAssertTrue(app.staticTexts["对话"].waitForExistence(timeout: 5))
        let remote = app.buttons["ai-remote-conversation-mac:localmacfixture:ses_mock_mac"]
        let remoteDevice = app.buttons["ai-session-device-mac:localmacfixture"]
        XCTAssertTrue(remoteDevice.waitForExistence(timeout: 30))
        XCTAssertTrue(remoteDevice.label.contains("测试 Mac"))
        XCTAssertTrue(remoteDevice.label.contains("在线"))
        guard remote.waitForExistence(timeout: 30) else {
            XCTFail("本机假 Mac 会话未出现在列表")
            return
        }
        XCTAssertTrue(app.buttons["ai-session-device-local"].exists)
        let grouped = XCTAttachment(screenshot: app.screenshot())
        grouped.name = "normal-app-device-grouped-drawer"
        grouped.lifetime = .keepAlways
        add(grouped)
        remoteDevice.tap()
        XCTAssertFalse(remote.waitForExistence(timeout: 1))
        remoteDevice.tap()
        XCTAssertTrue(remote.waitForExistence(timeout: 5))
        remote.tap()
        let loading = XCTAttachment(screenshot: app.screenshot())
        loading.name = "normal-app-remote-mac-opening"
        loading.lifetime = .keepAlways
        add(loading)
        let history = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "这条历史来自本机假 Mac Session")
        ).firstMatch
        guard history.waitForExistence(timeout: 30) else {
            XCTFail("所属设备历史未返回")
            return
        }
        let opened = XCTAttachment(screenshot: app.screenshot())
        opened.name = "normal-app-remote-mac-history"
        opened.lifetime = .keepAlways
        add(opened)

        let composer = app.textViews["ai-composer"]
        XCTAssertTrue(composer.waitForExistence(timeout: 10))
        composer.tap()
        composer.typeText("从 iPhone 请求本机假 Mac")
        let send = app.buttons["ai-send-draft"]
        XCTAssertTrue(send.waitForExistence(timeout: 10))
        send.tap()
        let echoed = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "从 iPhone 请求本机假 Mac")
        ).firstMatch
        XCTAssertTrue(echoed.waitForExistence(timeout: 30))

        composer.tap()
        composer.typeText("请拒绝此操作")
        XCTAssertTrue(send.waitForExistence(timeout: 10))
        send.tap()
        XCTAssertTrue(app.staticTexts["所属设备拒绝了操作"].waitForExistence(timeout: 30))
        XCTAssertEqual(composer.value as? String, "请拒绝此操作")
        let rejected = XCTAttachment(screenshot: app.screenshot())
        rejected.name = "normal-app-remote-rejection-restores-draft"
        rejected.lifetime = .keepAlways
        add(rejected)
    }

    @MainActor func testProductionWebSocketReadsFakeMacHistory() throws {
        let environment = ProcessInfo.processInfo.environment
        guard let account = environment["MAICHAT_PROD_SYNC_TEST_ACCOUNT"],
              let password = environment["MAICHAT_PROD_SYNC_TEST_PASSWORD"] else {
            throw XCTSkip("需要无付费生产测试账号和假 Mac Session")
        }
        let app = XCUIApplication()
        app.launchEnvironment["MAICHAT_PROD_SYNC_TEST_ACCOUNT"] = account
        app.launch()
        let accountField = app.textFields["account-phone"]
        XCTAssertTrue(accountField.waitForExistence(timeout: 20), "请用隔离模拟器运行生产账号测试")
        app.buttons["密码登录"].tap()
        accountField.tap()
        accountField.typeText(account)
        let passwordField = app.secureTextFields["输入登录密码"]
        passwordField.tap()
        passwordField.typeText(password)
        app.buttons["account-minimum-age"].tap()
        app.buttons["account-agreement"].tap()
        app.buttons["account-submit"].tap()
        let creationCenter = app.buttons["创作中心"]
        guard creationCenter.waitForExistence(timeout: 30) else {
            XCTFail("生产测试账号未进入 MaiChat 主页面")
            return
        }
        let temporaryIMError = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "IM 操作失败")
        ).firstMatch
        if temporaryIMError.exists { temporaryIMError.tap() }
        creationCenter.tap()
        let entered = XCTAttachment(screenshot: app.screenshot())
        entered.name = "production-wss-iphone-simulator-after-tab"
        entered.lifetime = .keepAlways
        add(entered)
        guard app.buttons["对话列表"].waitForExistence(timeout: 20) else {
            XCTFail("生产账号未进入创作中心")
            return
        }
        let start = app.coordinate(withNormalizedOffset: CGVector(dx: 0.01, dy: 0.5))
        let end = app.coordinate(withNormalizedOffset: CGVector(dx: 0.72, dy: 0.5))
        start.press(forDuration: 0.05, thenDragTo: end)
        let remote = app.buttons["ai-remote-conversation-mac:prodwsfakemac:ses_prod_ws_qa"]
        let remoteDevice = app.buttons["ai-session-device-mac:prodwsfakemac"]
        XCTAssertTrue(remoteDevice.waitForExistence(timeout: 30))
        XCTAssertTrue(remoteDevice.label.contains("生产验收假 Mac"))
        XCTAssertTrue(remoteDevice.label.contains("在线"))
        guard remote.waitForExistence(timeout: 30) else {
            XCTFail("生产假 Mac Session 未出现在目录")
            return
        }
        let directory = XCTAttachment(screenshot: app.screenshot())
        directory.name = "production-wss-iphone-simulator-directory"
        directory.lifetime = .keepAlways
        add(directory)
        remote.tap()
        let history = app.staticTexts.matching(NSPredicate(
            format: "label CONTAINS %@", "这条历史来自生产 WebSocket 的无付费假 Mac"
        )).firstMatch
        XCTAssertTrue(history.waitForExistence(timeout: 30))
        let opened = XCTAttachment(screenshot: app.screenshot())
        opened.name = "production-wss-iphone-simulator-history"
        opened.lifetime = .keepAlways
        add(opened)

        app.buttons["返回主页面"].tap()
        app.buttons["我"].tap()
        app.buttons["设置"].tap()
        app.buttons["切换账号"].tap()
        XCTAssertTrue(accountField.waitForExistence(timeout: 15))
    }

    func testNormalAppKeepsConversationPageVisibleAcrossNavigation() throws {
        let environment = ProcessInfo.processInfo.environment
        guard let phone = environment["MAICHAT_LOCAL_UI_PHONE"],
              let password = environment["MAICHAT_LOCAL_UI_PASSWORD"] else {
            throw XCTSkip("需要本机测试账号，不能使用真实用户或付费模型")
        }
        let app = XCUIApplication()
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = phone
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] = password
        app.launch()

        let creationCenter = app.buttons["创作中心"]
        XCTAssertTrue(creationCenter.waitForExistence(timeout: 30))
        creationCenter.tap()
        XCTAssertTrue(app.staticTexts["创作中心"].waitForExistence(timeout: 20))
        let opened = XCTAttachment(screenshot: app.screenshot())
        opened.name = "normal-app-creation-center-opened"
        opened.lifetime = .keepAlways
        add(opened)

        let conversations = app.buttons["对话列表"]
        XCTAssertTrue(conversations.waitForExistence(timeout: 10))
        let temporaryIMError = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "IM 操作失败")).firstMatch
        if temporaryIMError.exists {
            temporaryIMError.tap()
        }
        conversations.tap()
        let drawerScrollView = app.scrollViews["ai-session-drawer"]
        let drawerVisible = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "hittable == true"), object: drawerScrollView)
        XCTAssertEqual(XCTWaiter.wait(for: [drawerVisible], timeout: 10), .completed)
        let drawer = XCTAttachment(screenshot: app.screenshot())
        drawer.name = "normal-app-conversation-drawer"
        drawer.lifetime = .keepAlways
        add(drawer)

        let existingConversation = app.buttons.matching(
            NSPredicate(format: "identifier BEGINSWITH %@", "ai-conversation-"))
            .element(boundBy: 1)
        XCTAssertTrue(existingConversation.waitForExistence(timeout: 5))
        existingConversation.tap()
        let composer = app.descendants(matching: .any)
            .matching(identifier: "ai-composer").firstMatch
        XCTAssertTrue(composer.waitForExistence(timeout: 10))
        let switched = XCTAttachment(screenshot: app.screenshot())
        switched.name = "normal-app-after-conversation-switch"
        switched.lifetime = .keepAlways
        add(switched)
    }

    func testVoiceOverlayInVideoAndConversationPreview() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--voice-conversation-preview"]
        app.launchEnvironment["MAICHAT_VOICE_DESIGN_VIDEO"] =
            "/tmp/maichat-voice-design-video.mov"
        app.launch()
        let transcript = app.staticTexts["voice-transcript"]
        XCTAssertTrue(transcript.waitForExistence(timeout: 15))
        RunLoop.current.run(until: Date().addingTimeInterval(2))
        let screenshot = XCTAttachment(screenshot: app.screenshot())
        screenshot.name = "voice-overlay-real-conversation-preview"
        screenshot.lifetime = .keepAlways
        add(screenshot)
    }

    func testCreationCenterGallerySwipesImageAndVideo() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-image-mixed-ui-test"]
        app.launch()
        let image = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "1/2 ·")).firstMatch
        XCTAssertTrue(image.waitForExistence(timeout: 15))
        let left = app.coordinate(withNormalizedOffset: CGVector(dx: 0.78, dy: 0.48))
        let right = app.coordinate(withNormalizedOffset: CGVector(dx: 0.22, dy: 0.48))
        left.press(forDuration: 0.05, thenDragTo: right)
        let video = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "2/2 ·")).firstMatch
        XCTAssertTrue(video.waitForExistence(timeout: 8))
        XCTAssertTrue(app.buttons["ffplay-pause"].waitForExistence(timeout: 8))
        right.press(forDuration: 0.05, thenDragTo: left)
        XCTAssertTrue(image.waitForExistence(timeout: 8))
    }

    func testVideoGallerySwipesIntoImageAndBack() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--mixed-gallery-ui-test"]
        app.launch()
        let first = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "1/3 ·")).firstMatch
        XCTAssertTrue(first.waitForExistence(timeout: 15))
        let left = app.coordinate(withNormalizedOffset: CGVector(dx: 0.78, dy: 0.48))
        let right = app.coordinate(withNormalizedOffset: CGVector(dx: 0.22, dy: 0.48))
        left.press(forDuration: 0.05, thenDragTo: right)
        let image = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "2/3 ·")).firstMatch
        XCTAssertTrue(image.waitForExistence(timeout: 8))
        let capture = XCTAttachment(screenshot: app.screenshot())
        capture.name = "video-to-image-gallery"
        capture.lifetime = .keepAlways
        add(capture)
        left.press(forDuration: 0.05, thenDragTo: right)
        let third = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "3/3 ·")).firstMatch
        XCTAssertTrue(third.waitForExistence(timeout: 8))
        XCTAssertTrue(app.buttons["ffplay-pause"].exists)
        right.press(forDuration: 0.05, thenDragTo: left)
        XCTAssertTrue(image.waitForExistence(timeout: 8))
    }

    func testIMGallerySwipesAcrossImagesAndVideo() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--im-image-fullscreen-ui-test"]
        app.launch()
        let bubble = app.buttons["remote-im-message-image"]
        XCTAssertTrue(bubble.waitForExistence(timeout: 15))
        for _ in 0..<3 where !bubble.isHittable { app.swipeUp() }
        bubble.tap()
        XCTAssertTrue(app.images["remote-im-image-preview"].waitForExistence(timeout: 10))
        let left = app.coordinate(withNormalizedOffset: CGVector(dx: 0.78, dy: 0.48))
        let right = app.coordinate(withNormalizedOffset: CGVector(dx: 0.22, dy: 0.48))
        left.press(forDuration: 0.05, thenDragTo: right)
        let second = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "2/3 ·")).firstMatch
        XCTAssertTrue(second.waitForExistence(timeout: 8))
        left.press(forDuration: 0.05, thenDragTo: right)
        let third = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "3/3 ·")).firstMatch
        XCTAssertTrue(third.waitForExistence(timeout: 8))
        XCTAssertTrue(app.buttons["ffplay-pause"].waitForExistence(timeout: 8))
        right.press(forDuration: 0.05, thenDragTo: left)
        XCTAssertTrue(second.waitForExistence(timeout: 8))
    }

    func testVideoPlayerSwipesBetweenConversationFiles() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ffplay-swipe-ui-test"]
        app.launch()
        let current = app.staticTexts["ffplay-current-video"]
        XCTAssertTrue(current.waitForExistence(timeout: 15))
        XCTAssertEqual(current.label, "ffplay-sample.mp4")
        let left = app.coordinate(withNormalizedOffset: CGVector(dx: 0.8, dy: 0.45))
        let right = app.coordinate(withNormalizedOffset: CGVector(dx: 0.2, dy: 0.45))
        left.press(forDuration: 0.05, thenDragTo: right)
        let next = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label == %@", "ffplay-seek-sample.mp4"),
            object: current)
        XCTAssertEqual(XCTWaiter.wait(for: [next], timeout: 8), .completed)
        XCTAssertTrue(app.buttons["ffplay-pause"].exists)
        let secondFrame = XCTAttachment(screenshot: app.screenshot())
        secondFrame.name = "video-after-left-swipe"
        secondFrame.lifetime = .keepAlways
        add(secondFrame)
        right.press(forDuration: 0.05, thenDragTo: left)
        let previous = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label == %@", "ffplay-sample.mp4"),
            object: current)
        XCTAssertEqual(XCTWaiter.wait(for: [previous], timeout: 8), .completed)
        let firstFrame = XCTAttachment(screenshot: app.screenshot())
        firstFrame.name = "video-after-right-swipe"
        firstFrame.lifetime = .keepAlways
        add(firstFrame)
    }

    func testTypingWhileAssistantStreamsRemainsResponsive() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-stream-typing-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let editor = app.descendants(matching: .any)
            .matching(identifier: "ai-composer").firstMatch
        XCTAssertTrue(editor.waitForExistence(timeout: 15))
        editor.tap()
        XCTAssertTrue(app.keyboards.firstMatch.waitForExistence(timeout: 5))
        let started = Date()
        editor.typeText("typing during streaming")
        XCTAssertLessThan(Date().timeIntervalSince(started), 15)
        XCTAssertTrue((editor.value as? String)?.contains("typing during streaming") == true)
    }

    func testEnteringLongAIConversationShowsLatestMessage() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-history-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let latest = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "历史消息 79")).firstMatch
        XCTAssertTrue(latest.waitForExistence(timeout: 15))
        let timeline = app.scrollViews.element(boundBy: 0)
        XCTAssertGreaterThanOrEqual(latest.frame.minY, timeline.frame.minY - 2,
                                    "The latest message must be inside the visible timeline")
        XCTAssertLessThanOrEqual(latest.frame.maxY, timeline.frame.maxY + 2)
        let editor = app.descendants(matching: .any)
            .matching(identifier: "ai-composer").firstMatch
        XCTAssertTrue(editor.waitForExistence(timeout: 5))
        editor.tap()
        XCTAssertTrue(app.keyboards.firstMatch.waitForExistence(timeout: 5))
        XCTAssertGreaterThanOrEqual(latest.frame.minY, timeline.frame.minY - 2)
        XCTAssertLessThanOrEqual(latest.frame.maxY, timeline.frame.maxY + 2,
                                 "Opening the keyboard must keep the latest message visible")
        timeline.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.2)).tap()
        let keyboardHidden = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "exists == false"),
            object: app.keyboards.firstMatch
        )
        XCTAssertEqual(XCTWaiter.wait(for: [keyboardHidden], timeout: 3), .completed)
        XCTAssertGreaterThanOrEqual(latest.frame.minY, timeline.frame.minY - 2)
        XCTAssertLessThanOrEqual(latest.frame.maxY, timeline.frame.maxY + 2,
                                 "Closing the keyboard must keep the latest message visible")
    }

    func testFastHistoryFlingsStopAtRenderedMessages() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-history-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let timeline = app.scrollViews.element(boundBy: 0)
        XCTAssertTrue(timeline.waitForExistence(timeout: 15))
        let first = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "历史消息 0")).firstMatch
        let latest = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "历史消息 79")).firstMatch
        for _ in 0..<16 where !first.isHittable { timeline.swipeDown() }
        XCTAssertTrue(first.isHittable, "快速上滑到已加载首页应停在真实消息")
        let top = XCTAttachment(screenshot: app.screenshot())
        top.name = "history-fast-fling-top"
        top.lifetime = .keepAlways
        add(top)
        for _ in 0..<16 where !latest.isHittable { timeline.swipeUp() }
        XCTAssertTrue(latest.isHittable, "快速下滑到底部应停在最新消息")
        let bottom = XCTAttachment(screenshot: app.screenshot())
        bottom.name = "history-fast-fling-bottom"
        bottom.lifetime = .keepAlways
        add(bottom)
    }

    func testSwitchingConversationsOpensAtLatestMessage() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-session-switch-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        for title in ["B", "C"] {
            app.buttons["对话列表"].tap()
            let session = app.buttons["ai-conversation-switch-\(title)"]
            XCTAssertTrue(session.waitForExistence(timeout: 5))
            session.tap()
            let latest = app.staticTexts.matching(
                NSPredicate(format: "label CONTAINS %@", "\(title) 历史消息 79")).firstMatch
            XCTAssertTrue(latest.waitForExistence(timeout: 10))
            let hittable = XCTNSPredicateExpectation(
                predicate: NSPredicate(format: "hittable == true"), object: latest)
            XCTAssertEqual(XCTWaiter.wait(for: [hittable], timeout: 5), .completed)
            let timeline = app.scrollViews.element(boundBy: 0)
            XCTAssertGreaterThanOrEqual(latest.frame.minY, timeline.frame.minY - 2)
            XCTAssertLessThanOrEqual(latest.frame.maxY, timeline.frame.maxY + 2)
        }
    }

    func testRemoteSessionShowsOwnerAndPreservesCurrentConversationWhenOffline() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-session-switch-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        app.buttons["对话列表"].tap()
        let remote = app.buttons["ai-remote-conversation-mac:fixturemac:remote-demo"]
        XCTAssertTrue(remote.waitForExistence(timeout: 10))
        XCTAssertTrue(app.buttons["ai-session-device-mac:fixturemac"].label.contains("离线"))
        let list = XCTAttachment(screenshot: app.screenshot())
        list.name = "remote-session-device-and-offline-state"
        list.lifetime = .keepAlways
        add(list)
        remote.tap()
        XCTAssertTrue(app.staticTexts["所属设备离线，暂时无法读取这段会话"]
            .waitForExistence(timeout: 5))
        XCTAssertTrue(app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "A 历史消息 79")
        ).firstMatch.exists)
    }

    func testMediaDraftStaysTogetherAcrossConversationSwitches() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-media-draft-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()

        let image = app.buttons.matching(
            NSPredicate(format: "label BEGINSWITH %@", "预览图片")
        ).firstMatch
        let video = app.buttons.matching(
            NSPredicate(format: "label BEGINSWITH %@", "预览视频")
        ).firstMatch
        let send = app.buttons["ai-send-draft"]
        XCTAssertTrue(image.waitForExistence(timeout: 10))
        XCTAssertTrue(video.exists)
        XCTAssertTrue(send.isEnabled, "只有媒体、未写说明时也能发送")
        XCTAssertFalse(app.buttons["关闭设置"].exists, "导入媒体不应自动发消息")

        let composer = app.textViews["ai-composer"]
        composer.tap()
        composer.typeText("请分析这张图片和视频")
        XCTAssertTrue(send.exists)
        XCTAssertTrue(image.exists)
        XCTAssertTrue(video.exists)
        let draft = XCTAttachment(screenshot: app.screenshot())
        draft.name = "media-draft-with-optional-description"
        draft.lifetime = .keepAlways
        add(draft)

        app.buttons["对话列表"].tap()
        app.buttons["ai-conversation-switch-B"].tap()
        XCTAssertFalse(image.exists, "其他会话不应显示原附件")
        XCTAssertFalse(send.exists, "空草稿不应显示发送按钮")
        app.buttons["对话列表"].tap()
        app.buttons["ai-conversation-empty-draft"].tap()
        XCTAssertTrue(app.buttons["预览图片 empty-draft-image.png"].waitForExistence(timeout: 5))
        XCTAssertFalse(video.exists, "空会话草稿与已有会话隔离")
        composer.tap()
        composer.typeText("空白草稿说明")
        XCTAssertTrue(composer.value as? String == "空白草稿说明")
        app.buttons["对话列表"].tap()
        app.buttons["ai-conversation-switch-A"].tap()
        XCTAssertTrue(image.waitForExistence(timeout: 5))
        XCTAssertTrue(video.exists)
        XCTAssertTrue(composer.value as? String == "请分析这张图片和视频")

        image.tap()
        XCTAssertTrue(app.buttons["关闭图片预览"].waitForExistence(timeout: 5))
        app.buttons["关闭图片预览"].tap()
        XCTAssertTrue(image.waitForExistence(timeout: 5))
        app.buttons["ai-remove-attachment-draft-image.png"].tap()
        XCTAssertFalse(image.exists)
        XCTAssertTrue(video.exists, "移除图片后视频仍在草稿中")
        video.tap()
        XCTAssertTrue(app.buttons["关闭视频"].waitForExistence(timeout: 10))
        app.buttons["关闭视频"].tap()
        XCTAssertTrue(video.waitForExistence(timeout: 5))
        XCTAssertTrue(video.isHittable)
    }

    func testLandscapeAssistantKeepsSessionSidebarBesideLatestMessage() throws {
        defer { XCUIDevice.shared.orientation = .portrait }
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-history-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        XCUIDevice.shared.orientation = .landscapeLeft
        let sidebar = app.descendants(matching: .any)
            .matching(identifier: "ai-session-drawer").firstMatch
        XCTAssertTrue(sidebar.waitForExistence(timeout: 15))
        let latest = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "历史消息 79")).firstMatch
        XCTAssertTrue(latest.waitForExistence(timeout: 15))
        XCTAssertLessThan(sidebar.frame.maxX, latest.frame.minX)
        XCTAssertTrue(latest.isHittable)
        let capture = XCTAttachment(screenshot: app.screenshot())
        capture.name = "ai-landscape-layout"
        capture.lifetime = .keepAlways
        add(capture)
    }

    func testLandscapeIMKeepsConversationListBesideChat() throws {
        defer { XCUIDevice.shared.orientation = .portrait }
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--im-landscape-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        XCUIDevice.shared.orientation = .landscapeLeft
        let conversation = app.buttons["im-conversation-im-test-peer"]
        let placeholder = app.descendants(matching: .any)
            .matching(identifier: "im-landscape-placeholder").firstMatch
        XCTAssertTrue(conversation.waitForExistence(timeout: 15))
        XCTAssertTrue(placeholder.waitForExistence(timeout: 15))
        conversation.tap()
        let menu = app.buttons["chatMoreMenu"]
        XCTAssertTrue(menu.waitForExistence(timeout: 15))
        XCTAssertTrue(conversation.isHittable)
        XCTAssertLessThan(conversation.frame.maxX, menu.frame.minX)
    }

    func testPortraitIMStillNavigatesIntoChat() throws {
        XCUIDevice.shared.orientation = .portrait
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--im-landscape-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let conversation = app.buttons["im-conversation-im-test-peer"]
        XCTAssertTrue(conversation.waitForExistence(timeout: 15))
        conversation.tap()
        XCTAssertTrue(app.buttons["chatMoreMenu"].waitForExistence(timeout: 15))
        XCTAssertFalse(conversation.isHittable)
    }

    func testLargeIMConversationShowsOnlyRenderedRecentPage() throws {
        XCUIDevice.shared.orientation = .portrait
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--im-large-history-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let conversation = app.buttons["im-conversation-im-test-peer"]
        XCTAssertTrue(conversation.waitForExistence(timeout: 15))
        conversation.tap()

        let latest = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "IM 历史消息 2543")).firstMatch
        XCTAssertTrue(latest.waitForExistence(timeout: 15))
        XCTAssertTrue(latest.isHittable)
        let renderedRows = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "IM 历史消息"))
        XCTAssertLessThanOrEqual(renderedRows.count, 20,
                                 "未翻页的 IM 时间线只能排版最近一页")
        let capture = XCTAttachment(screenshot: app.screenshot())
        capture.name = "im-2544-stored-20-rendered-latest-visible"
        capture.lifetime = .keepAlways
        add(capture)

        let timeline = app.scrollViews.element(boundBy: 0)
        let earlier = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "IM 历史消息 2504")).firstMatch
        for _ in 0..<8 where !earlier.exists { timeline.swipeDown() }
        XCTAssertTrue(earlier.waitForExistence(timeout: 5),
                      "到达已加载页顶部后应开放上一页")
        let paged = XCTAttachment(screenshot: app.screenshot())
        paged.name = "im-older-page-loaded-without-empty-viewport"
        paged.lifetime = .keepAlways
        add(paged)
    }

    func testSearchHitKeepsIMHistoryWindowBoundedAndReturnsToLatest() throws {
        XCUIDevice.shared.orientation = .portrait
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--im-search-focus-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()

        let target = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "IM 历史消息 100")).firstMatch
        XCTAssertTrue(target.waitForExistence(timeout: 15))
        let renderedRows = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "IM 历史消息"))
        XCTAssertLessThanOrEqual(renderedRows.count, 10,
                                 "搜索旧消息不能把之后的数千条历史同时交给列表")
        let focused = XCTAttachment(screenshot: app.screenshot())
        focused.name = "im-search-hit-bounded-window"
        focused.lifetime = .keepAlways
        add(focused)

        let latestButton = app.buttons["回到最新消息"]
        XCTAssertTrue(latestButton.waitForExistence(timeout: 5))
        latestButton.tap()
        let latest = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "IM 历史消息 2543")).firstMatch
        XCTAssertTrue(latest.waitForExistence(timeout: 10))
        XCTAssertTrue(latest.isHittable)
    }

    func testToolCallsDoNotExposeReasoningContent() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--transcript-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let tools = app.buttons.matching(
            NSPredicate(format: "label CONTAINS %@", "工具调用 8 次"))
        XCTAssertFalse(app.buttons.matching(
            NSPredicate(format: "label CONTAINS %@", "思考过程")).firstMatch.exists)
        XCTAssertTrue(tools.firstMatch.waitForExistence(timeout: 15))
        XCTAssertEqual(tools.count, 1)
        XCTAssertFalse(app.staticTexts["检查视频"].exists)
        let answer = app.staticTexts["处理完成，结果已经准备好。"]
        XCTAssertTrue(answer.waitForExistence(timeout: 5))
        XCTAssertGreaterThan(answer.frame.minY, tools.firstMatch.frame.maxY)
    }

    func testVideoAttachmentAppearsAsPlayableMessageCard() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--video-bubble-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let bubble = app.buttons["agent-video-bubble"]
        XCTAssertTrue(bubble.waitForExistence(timeout: 15))
        XCTAssertTrue(app.staticTexts["ffplay-sample.mp4"].exists)
        bubble.tap()
        XCTAssertTrue(app.buttons["ffplay-close"].waitForExistence(timeout: 15))
        app.buttons["ffplay-close"].tap()
        XCTAssertTrue(bubble.waitForExistence(timeout: 3))
    }

    func testMentionedVideoFileOpensFromMessageBubble() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--text-media-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let bubble = app.buttons["agent-text-video-bubble"]
        XCTAssertTrue(bubble.waitForExistence(timeout: 15))
        XCTAssertTrue(bubble.label.contains("ffplay-sample.mp4"))
        XCTAssertGreaterThan(bubble.frame.height, 140,
                             "正文中的视频也应显示缩略图卡片，而不是一行文件名")
        bubble.tap()
        XCTAssertTrue(app.buttons["ffplay-close"].waitForExistence(timeout: 15))
        app.buttons["ffplay-close"].tap()
    }

    func testImagePreviewUsesTheWholeScreen() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-image-fullscreen-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let preview = app.images["agent-image-preview"]
        XCTAssertTrue(preview.waitForExistence(timeout: 15))
        XCTAssertTrue(app.buttons["关闭图片预览"].waitForExistence(timeout: 5))
        XCTAssertFalse(app.statusBars.firstMatch.exists,
                       "Full-screen image preview must hide the system status bar")
        let capture = XCTAttachment(screenshot: app.screenshot())
        capture.name = "ios-image-fullscreen"
        capture.lifetime = .keepAlways
        add(capture)
    }

    func testIMImagePreviewCoversTheBottomOfTheWindow() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--im-image-fullscreen-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let open = app.buttons["remote-im-message-image"]
        XCTAssertTrue(open.waitForExistence(timeout: 15))
        for _ in 0..<3 where !open.isHittable { app.swipeUp() }
        XCTAssertTrue(open.isHittable)
        XCTAssertGreaterThan(open.frame.minY, app.frame.height * 0.1,
                             "The source bubble must be scrolled away from the top")
        let sourceCapture = XCTAttachment(screenshot: app.screenshot())
        sourceCapture.name = "im-image-source-after-scroll"
        sourceCapture.lifetime = .keepAlways
        add(sourceCapture)
        open.tap()
        let preview = app.images["remote-im-image-preview"]
        XCTAssertTrue(preview.waitForExistence(timeout: 15))
        let capture = XCTAttachment(screenshot: app.screenshot())
        capture.name = "im-image-modal-covers-window"
        capture.lifetime = .keepAlways
        add(capture)
        // 会话页底部之外也应属于图片预览，点击此处能关闭预览。
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.1, dy: 0.93)).tap()
        let closed = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "exists == false"), object: preview)
        XCTAssertEqual(XCTWaiter.wait(for: [closed], timeout: 5), .completed)
        XCTAssertEqual(app.staticTexts["im-source-frame-state"].label, "有效",
                       "The real IM image bubble must supply its current window frame")
        open.tap()
        XCTAssertTrue(preview.waitForExistence(timeout: 5))
        // 顶部状态栏原本所在的位置也必须由图片预览接管。
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.1, dy: 0.03)).tap()
        let closedFromTop = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "exists == false"), object: preview)
        XCTAssertEqual(XCTWaiter.wait(for: [closedFromTop], timeout: 5), .completed)
    }

    func testFfplayVideoPopupCanPauseSeekAndClose() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ffplay-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let pause = app.buttons["ffplay-pause"]
        XCTAssertTrue(pause.waitForExistence(timeout: 20))
        XCTAssertTrue(app.buttons["ffplay-share"].exists)
        XCTAssertTrue(app.buttons["ffplay-save"].exists)
        let speed = app.buttons["ffplay-speed"]
        XCTAssertTrue(speed.exists)
        speed.tap()
        XCTAssertTrue(app.buttons["ffplay-speed-0.5"].isHittable)
        XCTAssertFalse(app.alerts.firstMatch.exists)
        app.buttons["ffplay-speed-0.5"].tap()
        XCTAssertTrue(speed.label.contains("0.5"))
        pause.tap()
        XCTAssertTrue(app.buttons["ffplay-close"].isHittable)
        app.sliders["ffplay-seek"].adjust(toNormalizedSliderPosition: 0.5)
        let capture = XCTAttachment(screenshot: app.screenshot())
        capture.name = "ios-ffplay-metal-video"
        capture.lifetime = .keepAlways
        add(capture)
        app.buttons["ffplay-close"].tap()
        XCTAssertFalse(app.buttons["ffplay-close"].waitForExistence(timeout: 3))
    }

    func testAgentCanSeekPlayingVideoToAnAbsoluteSecond() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ffplay-seek-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let playbackTime = app.staticTexts["ffplay-time"]
        XCTAssertTrue(playbackTime.waitForExistence(timeout: 20))
        let ready = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label CONTAINS %@", " / 0:20"),
            object: playbackTime)
        XCTAssertEqual(XCTWaiter.wait(for: [ready], timeout: 15), .completed)
        app.buttons["agent-seek-six-seconds"].tap()
        XCTAssertTrue(app.buttons["定位已受理"].waitForExistence(timeout: 5))
        let sought = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label MATCHES %@", "0:0[6-8] / 0:20"),
            object: playbackTime)
        XCTAssertEqual(XCTWaiter.wait(for: [sought], timeout: 5), .completed,
                       "Agent seek must change the actual playback clock")
    }

    func testAgentCanOpenVideoFromAnAbsoluteSecond() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ffplay-start-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let playbackTime = app.staticTexts["ffplay-time"]
        XCTAssertTrue(playbackTime.waitForExistence(timeout: 20))
        let startedAtSix = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label MATCHES %@", "0:0[6-8] / 0:20"),
            object: playbackTime)
        XCTAssertEqual(XCTWaiter.wait(for: [startedAtSix], timeout: 15), .completed,
                       "start_seconds must set the initial playback position")
    }

    func testAgentOpenedDockedVideoCanReplayAndStartOver() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ffplay-agent-dock-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let result = app.staticTexts["agent-command-result"]
        XCTAssertTrue(result.waitForExistence(timeout: 20))
        let opened = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label == %@", "Agent 已打开视频"), object: result)
        XCTAssertEqual(XCTWaiter.wait(for: [opened], timeout: 15), .completed)
        let playbackTime = app.staticTexts["ffplay-time"]
        let advanced = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label MATCHES %@", "0:0[2-5] / 0:20"),
            object: playbackTime)
        XCTAssertEqual(XCTWaiter.wait(for: [advanced], timeout: 15), .completed)
        app.buttons["agent-replay"].tap()
        let replayed = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label MATCHES %@", "0:0[0-1] / 0:20"),
            object: playbackTime)
        XCTAssertEqual(XCTWaiter.wait(for: [replayed], timeout: 5), .completed)
        let advancedAgain = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label MATCHES %@", "0:0[2-5] / 0:20"),
            object: playbackTime)
        XCTAssertEqual(XCTWaiter.wait(for: [advancedAgain], timeout: 10), .completed)
        app.buttons["agent-start-over"].tap()
        let startedOver = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label MATCHES %@", "0:0[0-1] / 0:20"),
            object: playbackTime)
        XCTAssertEqual(XCTWaiter.wait(for: [startedOver], timeout: 5), .completed)
    }

    func testAgentOpenedDockedVideoCanChangeSoundTouchTempo() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ffplay-agent-dock-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let opened = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label == %@", "Agent 已打开视频"),
            object: app.staticTexts["agent-command-result"])
        XCTAssertEqual(XCTWaiter.wait(for: [opened], timeout: 20), .completed)
        let speed = app.buttons["ffplay-speed"]
        XCTAssertTrue(speed.waitForExistence(timeout: 15))
        app.buttons["agent-speed-half"].tap()
        let half = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label CONTAINS %@", "0.5"), object: speed)
        XCTAssertEqual(XCTWaiter.wait(for: [half], timeout: 10), .completed)
        app.buttons["agent-speed-double"].tap()
        let double = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label CONTAINS %@", "2.0"), object: speed)
        XCTAssertEqual(XCTWaiter.wait(for: [double], timeout: 10), .completed)
    }

    func testVoiceTranscriptionFloatsAboveDockedVideo() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--voice-docked-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let transcript = app.staticTexts["voice-transcript"]
        let cancel = app.buttons["voice-cancel"]
        let edit = app.buttons["voice-edit"]
        XCTAssertTrue(transcript.waitForExistence(timeout: 15))
        XCTAssertTrue(cancel.waitForExistence(timeout: 5))
        XCTAssertTrue(edit.waitForExistence(timeout: 5))
        XCTAssertGreaterThan(cancel.frame.minY, transcript.frame.maxY)
        XCTAssertGreaterThan(edit.frame.minY, transcript.frame.maxY)
        let input = app.buttons["voice-test-input"]
        XCTAssertTrue(input.exists)
        // 转写内容居中，操作目标独立靠近长按点；以实际控件坐标验收两块的间距。
        XCTAssertLessThan(abs(transcript.frame.midY - app.frame.midY), 100)
        let cancelGap = cancel.frame.minY - transcript.frame.maxY
        let editGap = edit.frame.minY - transcript.frame.maxY
        XCTAssertGreaterThan(cancelGap, 60)
        XCTAssertGreaterThan(editGap, 60)
        let inputToActions = input.frame.minY - max(cancel.frame.maxY, edit.frame.maxY)
        XCTAssertGreaterThan(inputToActions, 40)
        XCTAssertLessThan(inputToActions, 220,
                          "取消和编辑与输入框要留出空间，仍可向上拖到")
        let capture = XCTAttachment(screenshot: app.screenshot())
        capture.name = "voice-transcription-actions-above-composer"
        capture.lifetime = .keepAlways
        add(capture)
        input.tap()
        XCTAssertTrue(app.buttons["输入已触达"].exists,
                      "Topmost voice overlay must leave the bottom input touchable")
        cancel.tap()
        XCTAssertTrue(app.staticTexts["已取消"].exists)
        XCTAssertFalse(transcript.exists)
    }

    func testVoiceOverlayDismissKeepsConversationVisibleWithDockedVideo() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--voice-conversation-preview"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        if let videoPath = ProcessInfo.processInfo.environment["MAICHAT_VOICE_DESIGN_VIDEO"] {
            app.launchEnvironment["MAICHAT_VOICE_DESIGN_VIDEO"] = videoPath
        }
        app.launch()

        let transcript = app.staticTexts["voice-transcript"]
        XCTAssertTrue(transcript.waitForExistence(timeout: 15))
        let before = XCTAttachment(screenshot: app.screenshot())
        before.name = "voice-above-playing-video-and-conversation"
        before.lifetime = .keepAlways
        add(before)

        app.buttons["voice-cancel"].tap()
        XCTAssertFalse(transcript.exists)
        let latest = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "历史消息 79")).firstMatch
        XCTAssertTrue(latest.waitForExistence(timeout: 5))
        let after = XCTAttachment(screenshot: app.screenshot())
        after.name = "conversation-visible-after-voice-overlay-dismiss"
        after.lifetime = .keepAlways
        add(after)
    }

    func testFfplaySaveKeepsPlayerAlive() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ffplay-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let save = app.buttons["ffplay-save"]
        XCTAssertTrue(save.waitForExistence(timeout: 20))
        save.tap()
        let status = app.staticTexts["ffplay-save-status"]
        XCTAssertTrue(status.waitForExistence(timeout: 20))
        XCTAssertEqual(status.label, "已保存到相册")
        XCTAssertTrue(app.buttons["ffplay-close"].exists,
                      "Saving must report a result without terminating the player")
    }

    func testComposerAcceptsTypingWithoutSendButton() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let editor = app.descendants(matching: .any).matching(identifier: "ai-composer").firstMatch
        XCTAssertTrue(editor.waitForExistence(timeout: 15))
        XCTAssertFalse(app.buttons["发送"].exists)
        editor.tap()
        editor.typeText("UI composer check")
        app.buttons["对话列表"].tap()
        XCTAssertTrue(app.staticTexts["对话"].waitForExistence(timeout: 3))
        app.buttons["完成"].tap()
        let capture = XCTAttachment(screenshot: app.screenshot())
        capture.name = "ios-ai-assistant"
        capture.lifetime = .keepAlways
        add(capture)
    }

    func testTappingConversationDismissesKeyboard() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()

        let editor = app.descendants(matching: .any).matching(identifier: "ai-composer").firstMatch
        XCTAssertTrue(editor.waitForExistence(timeout: 15))
        XCTAssertTrue(editor.label.contains("可按住转文字"))
        editor.tap()
        editor.typeText("dismiss keyboard")
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.3)).tap()

        let unfocused = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "hasKeyboardFocus == false"),
            object: editor
        )
        XCTAssertEqual(XCTWaiter.wait(for: [unfocused], timeout: 3), .completed)
    }

    func testAttachmentPanelMatchesMessageComposerActions() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()

        let more = app.buttons["展开更多功能"]
        XCTAssertTrue(more.waitForExistence(timeout: 15))
        more.tap()
        XCTAssertTrue(app.buttons["相册"].waitForExistence(timeout: 3))
        XCTAssertTrue(app.buttons["拍摄"].isHittable)
        XCTAssertTrue(app.buttons["文件"].isHittable)
        XCTAssertFalse(app.buttons["语音输入"].exists)

        app.buttons["收起更多功能"].tap()
        XCTAssertEqual(waitUntilNotHittable(app.buttons["相册"]), .completed)

        more.tap()
        XCTAssertTrue(app.buttons["相册"].waitForExistence(timeout: 3))
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.88, dy: 0.42)).tap()
        XCTAssertEqual(waitUntilNotHittable(app.buttons["相册"]), .completed)
    }

    func testModelChipOffersTextAndVisionModels() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()

        XCTAssertTrue(app.buttons["更多"].waitForExistence(timeout: 15))
        app.buttons["更多"].tap()
        let model = app.buttons["模型"]
        XCTAssertTrue(model.waitForExistence(timeout: 3))
        model.tap()
        let menu = app.descendants(matching: .any)
            .matching(identifier: "ai-model-menu").firstMatch
        XCTAssertTrue(menu.waitForExistence(timeout: 3))
        XCTAssertTrue(app.buttons["glm-5.3"].isHittable)
        XCTAssertTrue(app.buttons["glm-5.3-flash"].isHittable)
    }

    func testConversationDrawerContainsNewConversationAction() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()

        XCTAssertTrue(app.buttons["对话列表"].waitForExistence(timeout: 15))
        let swipeStart = app.coordinate(withNormalizedOffset: CGVector(dx: 0.01, dy: 0.5))
        let swipeEnd = app.coordinate(withNormalizedOffset: CGVector(dx: 0.72, dy: 0.5))
        swipeStart.press(forDuration: 0.05, thenDragTo: swipeEnd)
        XCTAssertTrue(app.staticTexts["对话"].waitForExistence(timeout: 3))
        XCTAssertTrue(app.buttons["新对话"].waitForExistence(timeout: 3))

        let drawer = app.descendants(matching: .any).matching(identifier: "ai-session-drawer").firstMatch
        XCTAssertTrue(drawer.waitForExistence(timeout: 3))
        drawer.swipeLeft()
        XCTAssertEqual(waitUntilNotHittable(app.buttons["新对话"]), .completed)

        app.buttons["对话列表"].tap()
        let firstSession = app.buttons.matching(
            NSPredicate(format: "identifier BEGINSWITH 'ai-conversation-'")
        ).firstMatch
        XCTAssertTrue(firstSession.waitForExistence(timeout: 3))
        firstSession.coordinate(withNormalizedOffset: CGVector(dx: 0.75, dy: 0.5)).tap()
        XCTAssertEqual(waitUntilNotHittable(app.buttons["新对话"]), .completed)

        app.buttons["对话列表"].tap()
        app.buttons["完成"].tap()
        XCTAssertEqual(waitUntilNotHittable(app.buttons["新对话"]), .completed)
    }

    func testPermissionUsesCompactMenuWithoutModelSettingsEntry() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()

        XCTAssertTrue(app.buttons["更多"].waitForExistence(timeout: 15))
        app.buttons["更多"].tap()
        let permission = app.buttons["操作权限"]
        XCTAssertTrue(permission.waitForExistence(timeout: 3))
        permission.tap()

        let policyMenu = app.descendants(matching: .any)
            .matching(identifier: "ai-policy-menu").firstMatch
        XCTAssertTrue(policyMenu.waitForExistence(timeout: 3))
        XCTAssertLessThan(policyMenu.frame.width, app.frame.width * 0.75)
        app.buttons["完全访问"].tap()

        XCTAssertTrue(app.buttons["模型"].exists)
        XCTAssertTrue(app.buttons["清空当前对话"].exists)
        XCTAssertFalse(app.buttons["模型配置"].exists)
    }

    private func waitUntilNotHittable(_ element: XCUIElement) -> XCTWaiter.Result {
        let expectation = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "hittable == false"),
            object: element
        )
        return XCTWaiter.wait(for: [expectation], timeout: 3)
    }
}
