// 文件作用：驱动真实 iOS 界面，验证创作中心和 IM 的关键操作及画面状态。
// 驱动真实 iOS 页面检查会话切换、媒体预览、语音层和滚动边界。
// 使用本机测试账号与无付费素材，并保留关键页面截图供人工验收。
import XCTest

final class AIAssistantUITests: XCTestCase {
    @MainActor func testCloudDriveAgentPlaybackCacheLifecycle() throws {
        #if targetEnvironment(simulator)
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--cloud-drive-ui-test"]
        app.launch()
        let lifecycle = app.buttons["cloud-drive-agent-cache-lifecycle"]
        XCTAssertTrue(lifecycle.waitForExistence(timeout: 25))
        lifecycle.tap()
        XCTAssertTrue(app.staticTexts["云盘播放关闭与切号通过"]
            .waitForExistence(timeout: 10))
        app.terminate()
        app.launch()
        let relaunch = app.buttons["cloud-drive-agent-cache-relaunch"]
        XCTAssertTrue(relaunch.waitForExistence(timeout: 20))
        relaunch.tap()
        XCTAssertTrue(app.staticTexts["云盘孤儿清理通过"].waitForExistence(timeout: 10))
        app.terminate()
        #else
        throw XCTSkip("云盘 Agent 播放缓存只在模拟器验证")
        #endif
    }

    @MainActor func testCloudDriveHostDisabledCode() async throws {
        #if targetEnvironment(simulator)
        let control = URL(string: "http://127.0.0.1:18449/test/cert")!
        guard let (certificate, response) = try? await URLSession.shared.data(from: control),
              (response as? HTTPURLResponse)?.statusCode == 200 else {
            throw XCTSkip("需要本机云盘 disabled TLS 测试服务")
        }
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--cloud-drive-ui-test"]
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] =
            "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18190"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_URL"] =
            "https://localhost:18448"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_CA_BASE64"] =
            certificate.base64EncodedString()
        app.launch()
        let trigger = app.buttons["cloud-drive-tool-smoke"]
        XCTAssertTrue(trigger.waitForExistence(timeout: 30))
        trigger.tap()
        XCTAssertTrue(app.staticTexts["工具列表错误: not_configured"]
            .waitForExistence(timeout: 15))
        app.terminate()
        #else
        throw XCTSkip("云盘错误码用例只运行于隔离模拟器")
        #endif
    }

    @MainActor func testCloudDriveHostPlayDelete() async throws {
        #if targetEnvironment(simulator)
        let control = URL(string: "http://127.0.0.1:18449/test/cert")!
        guard let (certificate, response) = try? await URLSession.shared.data(from: control),
              (response as? HTTPURLResponse)?.statusCode == 200 else {
            throw XCTSkip("需要本机云盘 TLS 测试服务")
        }
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--cloud-drive-ui-test"]
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] =
            "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18190"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_URL"] =
            "https://localhost:18448"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_CA_BASE64"] =
            certificate.base64EncodedString()
        app.launch()
        let directStart = app.buttons["cloud-drive-upload-sample"]
        if !directStart.exists {
            let more = app.buttons["OverflowBarButtonItem"]
            if more.waitForExistence(timeout: 10) { more.tap() }
        }
        let start = directStart.exists ? directStart : app.buttons["上传样本"]
        XCTAssertTrue(start.waitForExistence(timeout: 30))
        start.tap()
        XCTAssertTrue(app.buttons[
            "cloud-drive-play-123e4567-e89b-42d3-a456-426614174000"]
            .waitForExistence(timeout: 60))
        app.buttons["cloud-drive-tool-play-delete"].tap()
        let success = app.staticTexts["云盘工具已播放并删除"]
        let status = app.staticTexts.matching(
            NSPredicate(format: "label BEGINSWITH %@", "云盘工具")).firstMatch
        XCTAssertTrue(success.waitForExistence(timeout: 30),
                      "工具状态：\(status.exists ? status.label : "缺失")")
        let (rangeData, _) = try await URLSession.shared.data(
            from: URL(string: "http://127.0.0.1:18449/test/state")!)
        let rangeState = try XCTUnwrap(JSONSerialization.jsonObject(
            with: rangeData) as? [String: Any])
        XCTAssertGreaterThan(rangeState["range_requests"] as? Int ?? 0, 0,
                             "Agent 云盘视频应通过 HTTP Range 点播")
        XCTAssertTrue(app.staticTexts["还没有图片或视频"].waitForExistence(timeout: 15))
        app.terminate()
        #else
        throw XCTSkip("云盘工具宿主用例只运行于隔离模拟器")
        #endif
    }

    @MainActor func testCloudDriveHostImageOpenDelete() async throws {
        #if targetEnvironment(simulator)
        let control = URL(string: "http://127.0.0.1:18449/test/cert")!
        guard let (certificate, response) = try? await URLSession.shared.data(from: control),
              (response as? HTTPURLResponse)?.statusCode == 200 else {
            throw XCTSkip("需要本机云盘 TLS 测试服务")
        }
        var reset = URLRequest(url: URL(string: "http://127.0.0.1:18449/test/reset")!)
        reset.httpMethod = "POST"
        let (_, resetResponse) = try await URLSession.shared.data(for: reset)
        XCTAssertEqual((resetResponse as? HTTPURLResponse)?.statusCode, 204)
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--cloud-drive-ui-test"]
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] =
            "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18190"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_URL"] =
            "https://localhost:18448"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_CA_BASE64"] =
            certificate.base64EncodedString()
        app.launch()
        defer { app.terminate() }
        let run = app.buttons["cloud-drive-tool-image-open-delete"]
        XCTAssertTrue(run.waitForExistence(timeout: 30))
        run.tap()
        XCTAssertTrue(app.staticTexts["云盘图片工具已打开并删除"]
            .waitForExistence(timeout: 120))
        XCTAssertTrue(app.staticTexts["还没有图片或视频"].waitForExistence(timeout: 20))
        #else
        throw XCTSkip("云盘 Agent 图片用例只运行于隔离模拟器")
        #endif
    }

    @MainActor func testCloudDriveHostTools() async throws {
        #if targetEnvironment(simulator)
        let control = URL(string: "http://127.0.0.1:18449/test/cert")!
        guard let (certificate, response) = try? await URLSession.shared.data(from: control),
              (response as? HTTPURLResponse)?.statusCode == 200 else {
            throw XCTSkip("需要本机云盘 TLS 测试服务")
        }
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--cloud-drive-ui-test"]
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] =
            "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18190"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_URL"] =
            "https://localhost:18448"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_CA_BASE64"] =
            certificate.base64EncodedString()
        app.launch()
        let trigger = app.buttons["cloud-drive-tool-smoke"]
        XCTAssertTrue(trigger.waitForExistence(timeout: 30))
        trigger.tap()
        XCTAssertTrue(app.staticTexts["云盘工具已受理上传"].waitForExistence(timeout: 30))
        app.terminate()
        #else
        throw XCTSkip("云盘工具宿主用例只运行于隔离模拟器")
        #endif
    }

    @MainActor func testCloudDriveHostPauseAndAbort() async throws {
        #if targetEnvironment(simulator)
        let control = URL(string: "http://127.0.0.1:18449")!
        guard let (certificate, response) = try? await URLSession.shared.data(
            from: control.appendingPathComponent("test/cert")),
              (response as? HTTPURLResponse)?.statusCode == 200 else {
            throw XCTSkip("需要本机云盘 TLS 测试服务")
        }
        var reset = URLRequest(url: control.appendingPathComponent("test/reset"))
        reset.httpMethod = "POST"
        let (_, resetResponse) = try await URLSession.shared.data(for: reset)
        XCTAssertEqual((resetResponse as? HTTPURLResponse)?.statusCode, 204)

        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--cloud-drive-ui-test"]
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] =
            "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18190"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_URL"] =
            "https://localhost:18448"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_CA_BASE64"] =
            certificate.base64EncodedString()
        app.launch()
        let start = app.buttons["cloud-drive-upload-large-sample"]
        XCTAssertTrue(start.waitForExistence(timeout: 30))
        start.tap()
        let action = app.buttons["cloud-drive-tool-pause-abort"]
        XCTAssertTrue(action.waitForExistence(timeout: 20))
        var firstPartStored = false
        for _ in 0..<100 {
            let (data, _) = try await URLSession.shared.data(
                from: control.appendingPathComponent("test/state"))
            let state = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
            if (state["parts"] as? [Int] ?? []).contains(1) {
                firstPartStored = true
                break
            }
            try await Task.sleep(for: .milliseconds(100))
        }
        XCTAssertTrue(firstPartStored, "服务器应收到首个分片")
        action.tap()
        XCTAssertTrue(app.staticTexts["云盘工具暂停并取消通过"].waitForExistence(timeout: 30))
        let (data, _) = try await URLSession.shared.data(
            from: control.appendingPathComponent("test/state"))
        let state = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
        XCTAssertEqual(state["status"] as? String, "deleted",
                       "Agent 取消应同时删除 OSS 上传会话")
        app.terminate()
        #else
        throw XCTSkip("云盘工具宿主用例只运行于隔离模拟器")
        #endif
    }

    @MainActor func testCloudDriveDisabledHidesNormalAppEntry() async throws {
        #if targetEnvironment(simulator)
        let control = URL(string: "http://127.0.0.1:18449/test/cert")!
        guard let (certificate, response) = try? await URLSession.shared.data(from: control),
              (response as? HTTPURLResponse)?.statusCode == 200 else {
            throw XCTSkip("需要本机云盘 disabled TLS 测试服务")
        }
        let app = XCUIApplication()
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] =
            "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18190"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_URL"] =
            "https://localhost:18448"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_CA_BASE64"] =
            certificate.base64EncodedString()
        app.launch()
        let me = app.buttons["我"]
        XCTAssertTrue(me.waitForExistence(timeout: 35))
        me.tap()
        XCTAssertTrue(app.buttons["personal-profile-entry"].waitForExistence(timeout: 10))
        try await Task.sleep(for: .seconds(2))
        XCTAssertFalse(app.buttons["my-cloud-drive-entry"].exists,
                       "云盘服务明确关闭时不应显示可操作入口")
        app.terminate()
        #else
        throw XCTSkip("门控用例只运行于隔离模拟器")
        #endif
    }

    @MainActor func testCloudDrivePhotoKeepsOriginalName() async throws {
        #if targetEnvironment(simulator)
        let control = URL(string: "http://127.0.0.1:18449/test/cert")!
        guard let (certificate, response) = try? await URLSession.shared.data(from: control),
              (response as? HTTPURLResponse)?.statusCode == 200 else {
            throw XCTSkip("需要本机云盘 TLS 测试服务")
        }
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--cloud-drive-ui-test"]
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] =
            "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18190"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_URL"] =
            "https://localhost:18448"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_CA_BASE64"] =
            certificate.base64EncodedString()
        app.launch()
        let trigger = app.buttons["cloud-drive-photo-original-name"]
        XCTAssertTrue(trigger.waitForExistence(timeout: 30))
        trigger.tap()
        XCTAssertTrue(app.staticTexts["IMG_0042.mp4"].waitForExistence(timeout: 60),
                      "相册源名称应写入云盘列表，而非 staging UUID")
        app.terminate()
        #else
        throw XCTSkip("相册命名用例只运行于隔离模拟器")
        #endif
    }

    @MainActor func testCloudDriveCrashCleanupKeepsResumableSource() async throws {
        #if targetEnvironment(simulator)
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--cloud-drive-ui-test"]
        app.launch()
        let trigger = app.buttons["cloud-drive-cleanup-recovery"]
        XCTAssertTrue(trigger.waitForExistence(timeout: 30))
        trigger.tap()
        XCTAssertTrue(app.staticTexts["云盘缓存恢复通过"].waitForExistence(timeout: 10))
        app.terminate()
        #else
        throw XCTSkip("云盘缓存恢复用例只运行于隔离模拟器")
        #endif
    }

    @MainActor func testCloudDriveSlowPhotoImportRejectsAccountSwitch() async throws {
        #if targetEnvironment(simulator)
        let control = URL(string: "http://127.0.0.1:18449")!
        guard let (certificate, response) = try? await URLSession.shared.data(
            from: control.appendingPathComponent("test/cert")),
              (response as? HTTPURLResponse)?.statusCode == 200 else {
            throw XCTSkip("需要本机云盘 TLS 测试服务")
        }
        func sessions() async throws -> Int {
            let (data, _) = try await URLSession.shared.data(
                from: control.appendingPathComponent("test/state"))
            let state = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
            return try XCTUnwrap(state["sessions"] as? Int)
        }
        let before = try await sessions()
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--cloud-drive-ui-test"]
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] =
            "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18190"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_URL"] =
            "https://localhost:18448"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_CA_BASE64"] =
            certificate.base64EncodedString()
        app.launch()
        let trigger = app.buttons["cloud-drive-simulate-account-switch"]
        XCTAssertTrue(trigger.waitForExistence(timeout: 30))
        trigger.tap()
        XCTAssertTrue(app.staticTexts["已阻止跨账号上传"].waitForExistence(timeout: 10))
        let after = try await sessions()
        XCTAssertEqual(after, before,
                       "慢导入后切号不得向旧或新账号发起上传")
        app.terminate()
        #else
        throw XCTSkip("云盘切号用例只运行于隔离模拟器")
        #endif
    }

    @MainActor func testCloudDrivePauseRelaunchResumeReusesParts() async throws {
        #if targetEnvironment(simulator)
        let control = URL(string: "http://127.0.0.1:18449")!
        guard let (certificate, response) = try? await URLSession.shared.data(
            from: control.appendingPathComponent("test/cert")),
              (response as? HTTPURLResponse)?.statusCode == 200 else {
            throw XCTSkip("需要本机云盘 TLS 测试服务")
        }
        let (initialData, _) = try await URLSession.shared.data(
            from: control.appendingPathComponent("test/state"))
        let initialState = try XCTUnwrap(
            JSONSerialization.jsonObject(with: initialData) as? [String: Any])
        let initialSessions = try XCTUnwrap(initialState["sessions"] as? Int)
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--cloud-drive-ui-test"]
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] =
            "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18190"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_URL"] =
            "https://localhost:18448"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_CA_BASE64"] =
            certificate.base64EncodedString()
        app.launch()
        let start = app.buttons["cloud-drive-upload-large-sample"]
        XCTAssertTrue(start.waitForExistence(timeout: 30))
        start.tap()
        let pause = app.buttons["暂停"].firstMatch
        XCTAssertTrue(pause.waitForExistence(timeout: 20))
        var firstPartStored = false
        for _ in 0..<100 {
            let (data, _) = try await URLSession.shared.data(
                from: control.appendingPathComponent("test/state"))
            let state = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
            let parts = state["parts"] as? [Int] ?? []
            if parts.contains(1) { firstPartStored = true; break }
            try await Task.sleep(for: .milliseconds(100))
        }
        XCTAssertTrue(firstPartStored, "服务器应先收到第一分片")
        pause.tap()
        XCTAssertTrue(app.buttons["继续"].firstMatch.waitForExistence(timeout: 30))
        app.terminate()
        app.launch()
        let resume = app.buttons["继续"].firstMatch
        XCTAssertTrue(resume.waitForExistence(timeout: 30), "重启后应恢复未完成任务")
        let paused = XCTAttachment(screenshot: app.screenshot())
        paused.name = "cloud-drive-paused-after-relaunch"
        paused.lifetime = .keepAlways
        add(paused)
        app.buttons["cloud-drive-tool-resume"].tap()
        XCTAssertTrue(app.staticTexts["云盘工具已继续"].waitForExistence(timeout: 10))
        let video = app.buttons["cloud-drive-play-123e4567-e89b-42d3-a456-426614174000"]
        XCTAssertTrue(video.waitForExistence(timeout: 120))
        let (data, _) = try await URLSession.shared.data(
            from: control.appendingPathComponent("test/state"))
        let state = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
        XCTAssertEqual(state["sessions"] as? Int, initialSessions + 1,
                       "续传应复用原上传会话和已完成分片")
        XCTAssertEqual(state["parts"] as? [Int], [1, 2, 3])
        app.terminate()
        #else
        throw XCTSkip("云盘续传用例只运行于隔离模拟器")
        #endif
    }

    @MainActor func testCloudDriveLocalUploadPlayDelete() async throws {
        #if targetEnvironment(simulator)
        let control = URL(string: "http://127.0.0.1:18449/test/cert")!
        guard let (certificate, response) = try? await URLSession.shared.data(from: control),
              (response as? HTTPURLResponse)?.statusCode == 200 else {
            throw XCTSkip("需要本机云盘 TLS 测试服务")
        }
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--cloud-drive-ui-test"]
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] =
            "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18190"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_URL"] =
            "https://localhost:18448"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_CA_BASE64"] =
            certificate.base64EncodedString()
        addUIInterruptionMonitor(withDescription: "允许保存云盘视频") { alert in
            let allow = alert.buttons.matching(NSPredicate(
                format: "label CONTAINS[c] %@ OR label CONTAINS %@", "Allow", "允许")).firstMatch
            if allow.exists { allow.tap(); return true }
            return false
        }
        app.launch()
        let directUpload = app.buttons["cloud-drive-upload-sample"]
        if !directUpload.exists {
            let more = app.buttons["OverflowBarButtonItem"]
            if more.waitForExistence(timeout: 10) { more.tap() }
        }
        let upload = directUpload.exists ? directUpload : app.buttons["上传样本"]
        XCTAssertTrue(upload.waitForExistence(timeout: 30))
        upload.tap()
        let item = app.buttons["cloud-drive-play-123e4567-e89b-42d3-a456-426614174000"]
        XCTAssertTrue(item.waitForExistence(timeout: 90))
        let uploaded = XCTAttachment(screenshot: app.screenshot())
        uploaded.name = "cloud-drive-local-uploaded"
        uploaded.lifetime = .keepAlways
        add(uploaded)
        item.tap()
        XCTAssertTrue(app.buttons["ffplay-close"].waitForExistence(timeout: 60))
        let (rangeData, _) = try await URLSession.shared.data(
            from: URL(string: "http://127.0.0.1:18449/test/state")!)
        let rangeState = try XCTUnwrap(JSONSerialization.jsonObject(
            with: rangeData) as? [String: Any])
        XCTAssertGreaterThan(rangeState["range_requests"] as? Int ?? 0, 0,
                             "云盘视频应通过 HTTP Range 分片点播")
        let streaming = XCTAttachment(screenshot: app.screenshot())
        streaming.name = "cloud-drive-local-range-playback"
        streaming.lifetime = .keepAlways
        add(streaming)
        app.buttons["ffplay-close"].tap()
        let menu = app.buttons["cloud-drive-video-menu-123e4567-e89b-42d3-a456-426614174000"]
        XCTAssertTrue(menu.waitForExistence(timeout: 10))
        menu.tap()
        let saveMenu = XCTAttachment(screenshot: app.screenshot())
        saveMenu.name = "cloud-drive-explicit-save-menu"
        saveMenu.lifetime = .keepAlways
        add(saveMenu)
        app.buttons["保存到相册"].tap()
        let springboard = XCUIApplication(bundleIdentifier: "com.apple.springboard")
        if springboard.alerts.firstMatch.waitForExistence(timeout: 2) {
            app.tap() // Trigger the first-use Photos interruption monitor.
        }
        XCTAssertTrue(app.staticTexts["已保存到相册"].waitForExistence(timeout: 60))
        menu.tap()
        app.buttons["删除"].tap()
        app.buttons["删除文件"].tap()
        XCTAssertTrue(app.staticTexts["还没有图片或视频"].waitForExistence(timeout: 30))
        app.terminate()
        #else
        throw XCTSkip("云盘 UI 用例只运行于隔离模拟器")
        #endif
    }

    @MainActor func testCloudDriveLocalHlgVodPlayback() async throws {
        #if targetEnvironment(simulator)
        let control = URL(string: "http://127.0.0.1:18449/test/cert")!
        let (certificate, response) = try await URLSession.shared.data(from: control)
        XCTAssertEqual((response as? HTTPURLResponse)?.statusCode, 200)
        let (initialStateData, _) = try await URLSession.shared.data(
            from: URL(string: "http://127.0.0.1:18449/test/state")!)
        let initialState = try XCTUnwrap(JSONSerialization.jsonObject(
            with: initialStateData) as? [String: Any])
        guard initialState["status"] as? String == "ready",
              (initialState["size"] as? Int ?? 0) > 10_000_000 else {
            throw XCTSkip("仅在本地 TLS fixture 显式加载 HLG 原片时运行")
        }
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--cloud-drive-ui-test"]
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] =
            "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18190"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_URL"] =
            "https://localhost:18448"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_CA_BASE64"] =
            certificate.base64EncodedString()
        app.launch()
        defer { app.terminate() }
        let item = app.buttons["cloud-drive-play-123e4567-e89b-42d3-a456-426614174000"]
        XCTAssertTrue(item.waitForExistence(timeout: 30))
        item.tap()
        let close = app.buttons["ffplay-close"]
        XCTAssertTrue(close.waitForExistence(timeout: 30))
        try await Task.sleep(for: .seconds(3))
        XCTAssertTrue(app.buttons["ffplay-pause"].exists)
        let speed = app.buttons["ffplay-speed"]
        speed.tap()
        app.buttons["ffplay-speed-1.5"].tap()
        XCTAssertEqual(speed.label, "播放倍速 1.5×")
        try await Task.sleep(for: .seconds(2))
        let playing = XCTAttachment(screenshot: app.screenshot())
        playing.name = "cloud-drive-local-hlg-vod-frame"
        playing.lifetime = .keepAlways
        add(playing)
        close.tap()
        let (stateData, _) = try await URLSession.shared.data(
            from: URL(string: "http://127.0.0.1:18449/test/state")!)
        let state = try XCTUnwrap(JSONSerialization.jsonObject(
            with: stateData) as? [String: Any])
        XCTAssertGreaterThan(state["range_requests"] as? Int ?? 0, 0)
        #else
        throw XCTSkip("HLG VOD 用例只运行于隔离模拟器")
        #endif
    }

    @MainActor func testCloudDriveImageUploadPreviewDelete() async throws {
        #if targetEnvironment(simulator)
        let control = URL(string: "http://127.0.0.1:18449/test/cert")!
        guard let (certificate, response) = try? await URLSession.shared.data(from: control),
              (response as? HTTPURLResponse)?.statusCode == 200 else {
            throw XCTSkip("需要本机云盘 TLS 测试服务")
        }
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--cloud-drive-ui-test"]
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] =
            "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18190"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_URL"] =
            "https://localhost:18448"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_CA_BASE64"] =
            certificate.base64EncodedString()
        app.launch()
        defer { app.terminate() }
        let directUpload = app.buttons["cloud-drive-upload-image-sample"]
        if !directUpload.exists {
            let more = app.buttons["OverflowBarButtonItem"]
            if more.waitForExistence(timeout: 10) { more.tap() }
        }
        let upload = directUpload.exists ? directUpload : app.buttons["上传图片样本"]
        XCTAssertTrue(upload.waitForExistence(timeout: 30))
        upload.tap()
        let id = "123e4567-e89b-42d3-a456-426614174000"
        let image = app.buttons["cloud-drive-play-\(id)"]
        XCTAssertTrue(image.waitForExistence(timeout: 90))
        XCTAssertEqual(image.label, "查看图片")
        let uploaded = XCTAttachment(screenshot: app.screenshot())
        uploaded.name = "cloud-drive-image-uploaded"
        uploaded.lifetime = .keepAlways
        add(uploaded)
        image.tap()
        let close = app.buttons["cloud-drive-image-close"]
        XCTAssertTrue(close.waitForExistence(timeout: 60))
        let fullImage = XCTAttachment(screenshot: app.screenshot())
        fullImage.name = "cloud-drive-image-preview"
        fullImage.lifetime = .keepAlways
        add(fullImage)
        close.tap()
        let menu = app.buttons["cloud-drive-video-menu-\(id)"]
        XCTAssertTrue(menu.waitForExistence(timeout: 10))
        menu.tap()
        app.buttons["删除"].tap()
        app.buttons["删除文件"].tap()
        XCTAssertTrue(app.staticTexts["还没有图片或视频"].waitForExistence(timeout: 30))
        #else
        throw XCTSkip("云盘图片 UI 用例只运行于隔离模拟器")
        #endif
    }

    @MainActor func testCloudDriveNormalAppSettingsEntryAndEmptyState() async throws {
        #if targetEnvironment(simulator)
        let control = URL(string: "http://127.0.0.1:18449/test/cert")!
        guard let (certificate, response) = try? await URLSession.shared.data(from: control),
              (response as? HTTPURLResponse)?.statusCode == 200 else {
            throw XCTSkip("需要本机云盘 TLS 测试服务")
        }
        let app = XCUIApplication()
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] =
            "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18190"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_URL"] =
            "https://localhost:18448"
        app.launchEnvironment["MAICHAT_LOCAL_CLOUD_DRIVE_CA_BASE64"] =
            certificate.base64EncodedString()
        app.launch()
        let me = app.buttons["我"]
        XCTAssertTrue(me.waitForExistence(timeout: 35))
        me.tap()
        let entry = app.buttons["my-cloud-drive-entry"]
        XCTAssertTrue(entry.waitForExistence(timeout: 20))
        entry.tap()
        XCTAssertTrue(app.staticTexts["我的云盘"].waitForExistence(timeout: 10))
        XCTAssertTrue(app.staticTexts["云盘空间"].waitForExistence(timeout: 15))
        let ownVideo = app.buttons[
            "cloud-drive-play-123e4567-e89b-42d3-a456-426614174000"]
        XCTAssertTrue(app.staticTexts["还没有图片或视频"].waitForExistence(timeout: 3) ||
                      ownVideo.waitForExistence(timeout: 3))
        let fakeIMError = app.staticTexts.matching(
            NSPredicate(format: "label BEGINSWITH %@", "IM 操作失败")).firstMatch
        if fakeIMError.exists { fakeIMError.tap() }
        let attachment = XCTAttachment(screenshot: app.screenshot())
        attachment.name = "cloud-drive-normal-app-empty"
        attachment.lifetime = .keepAlways
        add(attachment)
        app.terminate()
        #else
        throw XCTSkip("云盘 UI 用例只运行于隔离模拟器")
        #endif
    }

    @MainActor func testVideoCircleNormalAppOpensPlaysAndReturns() async throws {
        #if targetEnvironment(simulator)
        let fixture = URL(string: "http://127.0.0.1:18290")!
        let (statusData, statusResponse) = try await URLSession.shared.data(
            from: fixture.appendingPathComponent("test/video-circle/status"))
        guard (statusResponse as? HTTPURLResponse)?.statusCode == 200,
              let object = try JSONSerialization.jsonObject(with: statusData) as? [String: Any],
              let posts = object["posts"] as? [[String: Any]],
              let postID = posts.first(where: { $0["status"] as? String == "approved" })?["id"]
                as? String else { throw XCTSkip("需要本机已审核测试视频") }
        let (certificate, _) = try await URLSession.shared.data(
            from: fixture.appendingPathComponent("test/cert"))
        let app = XCUIApplication()
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-viewer"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] = "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18290"
        app.launchEnvironment["MAICHAT_LOCAL_VIDEO_CIRCLE_URL"] = "https://localhost:18445"
        app.launchEnvironment["MAICHAT_LOCAL_VIDEO_CIRCLE_CA_BASE64"] =
            certificate.base64EncodedString()
        app.launch()
        XCTAssertTrue(app.buttons["创作中心"].waitForExistence(timeout: 35))
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.375, dy: 0.93)).tap()
        let entry = app.buttons["ai-video-circle-entry"]
        XCTAssertTrue(entry.waitForExistence(timeout: 35))
        let beforeTap = XCTAttachment(screenshot: app.screenshot())
        beforeTap.name = "video-circle-before-first-tap"
        beforeTap.lifetime = .keepAlways
        add(beforeTap)
        try await Task.sleep(for: .seconds(1))
        // The visible header control responds at this screen point; XCUITest's
        // synthesized element-center tap misses it despite reporting a 64pt frame.
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.82, dy: 0.10)).tap()
        guard app.otherElements["video-circle-screen"].waitForExistence(timeout: 5) else {
            let afterTap = XCTAttachment(screenshot: app.screenshot())
            afterTap.name = "video-circle-after-first-tap"
            afterTap.lifetime = .keepAlways
            add(afterTap)
            XCTFail("正常 App 首次点击后视频圈未打开；frame=\(entry.frame)")
            app.terminate()
            return
        }
        let play = app.buttons["video-circle-play-\(postID)"]
        XCTAssertTrue(play.waitForExistence(timeout: 15))
        let entered = XCTAttachment(screenshot: app.screenshot())
        entered.name = "video-circle-normal-app-opened"
        entered.lifetime = .keepAlways
        add(entered)
        play.tap()
        let closePlayer = app.buttons["ffplay-close"]
        XCTAssertTrue(closePlayer.waitForExistence(timeout: 120))
        let playback = XCTAttachment(screenshot: app.screenshot())
        playback.name = "video-circle-normal-app-playback"
        playback.lifetime = .keepAlways
        add(playback)
        closePlayer.tap()
        app.buttons["video-circle-close"].tap()
        XCTAssertTrue(entry.waitForExistence(timeout: 10))
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.82, dy: 0.10)).tap()
        XCTAssertTrue(play.waitForExistence(timeout: 15))
        app.buttons["video-circle-close"].tap()
        XCTAssertTrue(entry.waitForExistence(timeout: 10))
        app.terminate()
        #else
        throw XCTSkip("正常 App 视频圈 UI 用例只运行于隔离模拟器")
        #endif
    }

    @MainActor func testVideoCircleLocalTwoAccountPublishReviewPlaybackWithdraw() async throws {
        #if targetEnvironment(simulator)
        executionTimeAllowance = 600
        let fixture = URL(string: "http://127.0.0.1:18290")!
        let source = try XCTUnwrap(Bundle(for: Self.self).url(
            forResource: "video-circle-source", withExtension: "mp4"))
        XCTAssertGreaterThan(try Data(contentsOf: source).count, 0)
        let (certificate, certificateResponse) = try await URLSession.shared.data(
            from: fixture.appendingPathComponent("test/cert"))
        guard (certificateResponse as? HTTPURLResponse)?.statusCode == 200 else {
            throw XCTSkip("需要本机视频圈假 auth/OSS 服务")
        }

        func launch(account: String, normalApp: Bool = false) -> XCUIApplication {
            let app = XCUIApplication()
            if !normalApp {
                app.launchArguments = ["--ai-ui-test", "--video-circle-ui-test"]
            }
            app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = account
            app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] =
                "local sync UI test passphrase"
            app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18290"
            app.launchEnvironment["MAICHAT_LOCAL_VIDEO_CIRCLE_URL"] =
                "https://localhost:18445"
            app.launchEnvironment["MAICHAT_LOCAL_VIDEO_CIRCLE_CA_BASE64"] =
                certificate.base64EncodedString()
            app.launch()
            return app
        }
        func capture(_ app: XCUIApplication, _ name: String) {
            let attachment = XCTAttachment(screenshot: app.screenshot())
            attachment.name = name
            attachment.lifetime = .keepAlways
            add(attachment)
        }
        func fixtureStatus() async throws -> [String: Any] {
            let (data, response) = try await URLSession.shared.data(
                from: fixture.appendingPathComponent("test/video-circle/status"))
            XCTAssertEqual((response as? HTTPURLResponse)?.statusCode, 200)
            return try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
        }

        let baseline = try await fixtureStatus()
        let baselineIDs = Set(((baseline["posts"] as? [[String: Any]]) ?? [])
            .compactMap { $0["id"] as? String })
        let baselineObjects = baseline["objects"] as? Int ?? 0

        let author = launch(account: "sync-ui-test")
        let publish = author.buttons["ai-video-circle-publish-circle-sourced"]
        XCTAssertTrue(publish.waitForExistence(timeout: 40))
        XCTAssertFalse(author.buttons["ai-video-circle-publish-circle-legacy"].exists)
        capture(author, "circle-author-sourced-and-legacy-cards")
        publish.tap()
        let submit = author.buttons["video-circle-submit"]
        XCTAssertTrue(submit.waitForExistence(timeout: 10))
        XCTAssertTrue(author.staticTexts["AI 生成"].exists)
        capture(author, "circle-publication-confirmation")
        submit.tap()
        let mine = author.buttons["video-circle-mine-tab"]
        XCTAssertTrue(mine.waitForExistence(timeout: 180))
        mine.tap()
        XCTAssertTrue(author.staticTexts["待审核"].waitForExistence(timeout: 15))
        capture(author, "circle-author-pending")
        let pendingStatus = try await fixtureStatus()
        let pendingPosts = try XCTUnwrap(pendingStatus["posts"] as? [[String: Any]])
        let createdPost = try XCTUnwrap(pendingPosts.first {
            guard let id = $0["id"] as? String else { return false }
            return !baselineIDs.contains(id)
        })
        XCTAssertEqual(createdPost["status"] as? String, "pending")
        XCTAssertEqual(pendingStatus["objects"] as? Int, baselineObjects + 1)
        let postID = try XCTUnwrap(createdPost["id"] as? String)
        author.terminate()

        // A second logged-in account must not see a pending video.
        let viewer = launch(account: "sync-ui-viewer")
        let circleEntry = viewer.buttons["ai-video-circle-entry"]
        XCTAssertTrue(circleEntry.waitForExistence(timeout: 35))
        circleEntry.tap()
        XCTAssertTrue(viewer.staticTexts["还没有公开视频"].waitForExistence(timeout: 12))
        capture(viewer, "circle-viewer-pending-is-private")
        viewer.terminate()

        let approvalURL = fixture.appendingPathComponent("test/video-circle/approve")
        var approval = URLComponents(url: approvalURL, resolvingAgainstBaseURL: false)!
        approval.queryItems = [URLQueryItem(name: "post_id", value: postID)]
        let (_, approvalResponse) = try await URLSession.shared.data(from: try XCTUnwrap(approval.url))
        XCTAssertEqual((approvalResponse as? HTTPURLResponse)?.statusCode, 200)

        let approvedViewer = launch(account: "sync-ui-viewer")
        XCTAssertTrue(approvedViewer.buttons["ai-video-circle-entry"].waitForExistence(timeout: 35))
        approvedViewer.buttons["ai-video-circle-entry"].tap()
        let play = approvedViewer.buttons["video-circle-play-\(postID)"]
        XCTAssertTrue(play.waitForExistence(timeout: 20))
        capture(approvedViewer, "circle-viewer-approved-feed")
        play.tap()
        let closePlayer = approvedViewer.buttons["ffplay-close"]
        XCTAssertTrue(closePlayer.waitForExistence(timeout: 120))
        capture(approvedViewer, "circle-viewer-ffplay")
        closePlayer.tap()
        approvedViewer.terminate()

        // Exercise the same approved post from the normal, logged-in App shell.
        let normalViewer = launch(account: "sync-ui-viewer", normalApp: true)
        let creationCenter = normalViewer.buttons["创作中心"]
        XCTAssertTrue(creationCenter.waitForExistence(timeout: 35))
        normalViewer.coordinate(withNormalizedOffset: CGVector(dx: 0.375, dy: 0.93)).tap()
        let normalCircle = normalViewer.buttons["ai-video-circle-entry"]
        XCTAssertTrue(normalCircle.waitForExistence(timeout: 35))
        capture(normalViewer, "circle-normal-app-entry")
        normalViewer.coordinate(withNormalizedOffset: CGVector(dx: 0.82, dy: 0.10)).tap()
        let normalPlay = normalViewer.buttons["video-circle-play-\(postID)"]
        XCTAssertTrue(normalPlay.waitForExistence(timeout: 20))
        capture(normalViewer, "circle-normal-app-feed")
        normalPlay.tap()
        let normalClosePlayer = normalViewer.buttons["ffplay-close"]
        XCTAssertTrue(normalClosePlayer.waitForExistence(timeout: 120))
        capture(normalViewer, "circle-normal-app-playing")
        normalClosePlayer.tap()
        normalViewer.buttons["video-circle-close"].tap()
        XCTAssertTrue(normalViewer.buttons["ai-video-circle-entry"].waitForExistence(timeout: 10))
        normalViewer.coordinate(withNormalizedOffset: CGVector(dx: 0.82, dy: 0.10)).tap()
        XCTAssertTrue(normalViewer.buttons["video-circle-play-\(postID)"].waitForExistence(timeout: 15))
        normalViewer.buttons["video-circle-close"].tap()
        XCTAssertTrue(normalViewer.buttons["ai-video-circle-entry"].waitForExistence(timeout: 10))
        capture(normalViewer, "circle-normal-app-returned")
        normalViewer.terminate()

        let returningAuthor = launch(account: "sync-ui-test")
        XCTAssertTrue(returningAuthor.buttons["ai-video-circle-entry"].waitForExistence(timeout: 35))
        returningAuthor.buttons["ai-video-circle-entry"].tap()
        returningAuthor.buttons["video-circle-mine-tab"].tap()
        XCTAssertTrue(returningAuthor.staticTexts["已公开"].waitForExistence(timeout: 15))
        returningAuthor.buttons["管理作品"].tap()
        returningAuthor.buttons["撤回作品"].tap()
        returningAuthor.buttons["撤回作品"].tap()
        XCTAssertTrue(returningAuthor.staticTexts["已撤回"].waitForExistence(timeout: 20))
        XCTAssertFalse(returningAuthor.staticTexts["null"].exists)
        capture(returningAuthor, "circle-author-withdrawn")
        let withdrawnStatus = try await fixtureStatus()
        let withdrawnPosts = try XCTUnwrap(withdrawnStatus["posts"] as? [[String: Any]])
        XCTAssertEqual(withdrawnPosts.first { $0["id"] as? String == postID }?["status"] as? String,
                       "withdrawn")
        XCTAssertEqual(withdrawnStatus["objects"] as? Int, baselineObjects)
        returningAuthor.terminate()
        #else
        throw XCTSkip("视频圈双账号 UI 测试只运行于隔离模拟器")
        #endif
    }

    @MainActor func testIMCurlAttachmentDownloadLocalTLS() async throws {
        #if targetEnvironment(simulator)
        let certificateURL = try XCTUnwrap(URL(string: "http://127.0.0.1:18441/cert"))
        let (certificate, _) = try await URLSession.shared.data(from: certificateURL)
        XCTAssertFalse(certificate.isEmpty)
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--im-curl-download-ui-test"]
        app.launchEnvironment["MAICHAT_MEDIA_TEST_CA_BASE64"] = certificate.base64EncodedString()
        app.launch()
        let passed = app.staticTexts["im-media-all-ok"].waitForExistence(timeout: 45)
        let capture = XCTAttachment(screenshot: app.screenshot())
        capture.name = "ios-shared-curl-image-file-account-cancel-\(passed ? "passed" : "failed")"
        capture.lifetime = .keepAlways
        add(capture)
        XCTAssertTrue(passed, "iOS shared-curl transfer failed: \(app.staticTexts["im-media-result"].label)")
        #else
        throw XCTSkip("Local TLS attachment fixture runs in the iOS simulator")
        #endif
    }

    @MainActor func testRealDeviceDeepSeekReasoningEfforts() throws {
        #if targetEnvironment(simulator)
        throw XCTSkip("此用例只在已登录的真机上调用真实 DeepSeek")
        #else
        guard ProcessInfo.processInfo.environment["MAICHAT_REAL_DEEPSEEK_EFFORT_TEST"] == "1" else {
            throw XCTSkip("真实模型调用需显式启用")
        }
        executionTimeAllowance = 600
        let app = XCUIApplication()
        app.launch()
        guard app.buttons["创作中心"].waitForExistence(timeout: 30) else {
            let capture = XCTAttachment(screenshot: app.screenshot())
            capture.name = "real-deepseek-login-required"
            capture.lifetime = .keepAlways
            add(capture)
            XCTFail("真机尚未登录 MaiChat")
            return
        }
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.375, dy: 0.93)).tap()
        XCTAssertTrue(app.buttons["对话列表"].waitForExistence(timeout: 20))
        app.buttons["对话列表"].tap()
        let create = app.buttons["ai-session-drawer"]
        XCTAssertTrue(create.waitForExistence(timeout: 10))
        create.tap()
        let composer = app.textViews["ai-composer"]
        XCTAssertTrue(composer.waitForExistence(timeout: 15))

        for (title, marker) in [("简单", "LOWOK"), ("标准", "HIGHOK"), ("深度", "MAXOK")] {
            app.buttons["更多"].tap()
            let effort = app.buttons["思考强度"]
            XCTAssertTrue(effort.waitForExistence(timeout: 5))
            effort.tap()
            app.buttons[title].tap()
            let saved = XCTNSPredicateExpectation(
                predicate: NSPredicate(format: "value == %@", title), object: effort)
            XCTAssertEqual(XCTWaiter.wait(for: [saved], timeout: 10), .completed)

            // The first tap outside the actions menu dismisses its full-screen backdrop.
            app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.7)).tap()
            composer.tap()
            composer.typeText("请只回复这个字符串：\(marker)")
            let send = app.buttons["ai-send-draft"]
            XCTAssertTrue(send.waitForExistence(timeout: 10))
            send.tap()
            let answer = app.staticTexts[marker]
            let responded = answer.waitForExistence(timeout: 150)
            let capture = XCTAttachment(screenshot: app.screenshot())
            capture.name = "real-deepseek-\(title)-\(responded ? "success" : "failure")"
            capture.lifetime = .keepAlways
            add(capture)
            XCTAssertTrue(responded, "真实 DeepSeek 未返回预期的 \(marker)；请查看截图中的错误")
        }
        #endif
    }

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
        // A single flick stays within the first twenty rows. Repeated quick
        // flicks reach the page boundary while previous momentum is active.
        for _ in 0..<6 {
            start.press(forDuration: 0.05, thenDragTo: end)
        }
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
        XCTAssertTrue(echoed.waitForExistence(timeout: 1), "发送端应先显示自己的消息")

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

    @MainActor func testRemoteSessionCanBeCreatedAndDeletedOnFakeMac() async throws {
        let statusURL = URL(string: "http://127.0.0.1:18190/test/status")!
        guard let probe = try? await URLSession.shared.data(from: statusURL),
              (probe.1 as? HTTPURLResponse)?.statusCode == 200 else {
            throw XCTSkip("需要本机 Session 同步测试服务")
        }
        let (certificate, _) = try await URLSession.shared.data(from:
            URL(string: "http://127.0.0.1:18190/test/cert")!)
        let app = XCUIApplication()
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] = "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18190"
        app.launchEnvironment["MAICHAT_LOCAL_UI_CA_BASE64"] = certificate.base64EncodedString()
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        XCTAssertTrue(app.buttons["创作中心"].waitForExistence(timeout: 30))
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.375, dy: 0.93)).tap()
        XCTAssertTrue(app.buttons["对话列表"].waitForExistence(timeout: 20))
        let swipeStart = app.coordinate(withNormalizedOffset: CGVector(dx: 0.01, dy: 0.5))
        let swipeEnd = app.coordinate(withNormalizedOffset: CGVector(dx: 0.72, dy: 0.5))
        swipeStart.press(forDuration: 0.05, thenDragTo: swipeEnd)
        let drawer = app.scrollViews["ai-session-drawer"]
        let create = app.buttons["ai-remote-create-mac:localmacfixture"]
        for _ in 0..<8 where !create.isHittable { drawer.swipeUp() }
        XCTAssertTrue(create.waitForExistence(timeout: 30),
                      "在线 Mac 的设备标题应提供远端新建入口")
        let before = XCTAttachment(screenshot: app.screenshot())
        before.name = "remote-session-create-entry"
        before.lifetime = .keepAlways
        add(before)
        create.tap()
        let createdID = "mac:localmacfixture:ses_remote_created_1"
        let remote = app.buttons["ai-remote-conversation-\(createdID)"]
        swipeStart.press(forDuration: 0.05, thenDragTo: swipeEnd)
        XCTAssertTrue(remote.waitForExistence(timeout: 30),
                      "Mac 已创建的对话没有同步回 iPhone 目录")
        let remove = app.buttons["ai-remote-delete-\(createdID)"]
        XCTAssertTrue(remove.exists)
        remove.tap()
        app.buttons["删除对话"].tap()
        let alert = app.alerts["删除远端对话"]
        XCTAssertTrue(alert.waitForExistence(timeout: 5))
        let confirmation = XCTAttachment(screenshot: app.screenshot())
        confirmation.name = "remote-session-delete-confirmation"
        confirmation.lifetime = .keepAlways
        add(confirmation)
        alert.buttons["取消"].tap()
        XCTAssertTrue(remote.exists, "取消删除必须保留对话")
        remove.tap()
        app.buttons["删除对话"].tap()
        XCTAssertTrue(alert.waitForExistence(timeout: 5))
        alert.buttons["删除"].tap()
        let removed = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "exists == false"), object: remote)
        XCTAssertEqual(XCTWaiter.wait(for: [removed], timeout: 30), .completed)
        let (statusData, _) = try await URLSession.shared.data(from: statusURL)
        let status = try XCTUnwrap(JSONSerialization.jsonObject(with: statusData) as? [String: Any])
        XCTAssertFalse((status["macSessions"] as? [String] ?? [])
            .contains("ses_remote_created_1"))
    }

    @MainActor func testRemoteCreateToolOnMac() async throws {
        let statusURL = URL(string: "http://127.0.0.1:18190/test/status")!
        guard let probe = try? await URLSession.shared.data(from: statusURL),
              (probe.1 as? HTTPURLResponse)?.statusCode == 200 else {
            throw XCTSkip("需要本机 Session 同步测试服务")
        }
        let (certificate, _) = try await URLSession.shared.data(from:
            URL(string: "http://127.0.0.1:18190/test/cert")!)
        let app = XCUIApplication()
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] = "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18190"
        app.launchEnvironment["MAICHAT_LOCAL_UI_CA_BASE64"] = certificate.base64EncodedString()
        app.launchEnvironment["MAICHAT_LOCAL_UI_MODEL"] = "1"
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        XCTAssertTrue(app.buttons["创作中心"].waitForExistence(timeout: 30))
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.375, dy: 0.93)).tap()
        XCTAssertTrue(app.buttons["对话列表"].waitForExistence(timeout: 20))
        let start = app.coordinate(withNormalizedOffset: CGVector(dx: 0.01, dy: 0.5))
        let end = app.coordinate(withNormalizedOffset: CGVector(dx: 0.72, dy: 0.5))
        start.press(forDuration: 0.05, thenDragTo: end)
        XCTAssertTrue(app.buttons["ai-remote-create-mac:localmacfixture"]
            .waitForExistence(timeout: 30))
        app.scrollViews["ai-session-drawer"].swipeLeft()
        let composer = app.textViews["ai-composer"]
        XCTAssertTrue(composer.waitForExistence(timeout: 10))
        composer.tap()
        composer.typeText("remote-session-create-tool")
        app.buttons["ai-send-draft"].tap()
        XCTAssertTrue(app.staticTexts["REMOTE_CREATE_TOOL_DONE"].waitForExistence(timeout: 30))
        let (data, _) = try await URLSession.shared.data(from: statusURL)
        let status = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
        XCTAssertTrue((status["modelCalls"] as? [String] ?? [])
            .contains("remote_session_create"))
        let requests = status["modelRequests"] as? [[String: Any]] ?? []
        XCTAssertTrue(requests.contains(where: {
            ($0["toolOutputText"] as? String ?? "").contains("session_id")
        }), "Agent 工具未返回所属设备创建的 Session ID")
        XCTAssertTrue((status["macSessions"] as? [String] ?? [])
            .contains(where: { $0.hasPrefix("ses_remote_created_") }))
    }

    @MainActor func testRemoteDeleteToolRequiresApprovalOnIPhone() async throws {
        let statusURL = URL(string: "http://127.0.0.1:18190/test/status")!
        guard let probe = try? await URLSession.shared.data(from: statusURL),
              (probe.1 as? HTTPURLResponse)?.statusCode == 200 else {
            throw XCTSkip("需要本机 Session 同步测试服务")
        }
        let (certificate, _) = try await URLSession.shared.data(from:
            URL(string: "http://127.0.0.1:18190/test/cert")!)
        let app = XCUIApplication()
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] = "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18190"
        app.launchEnvironment["MAICHAT_LOCAL_UI_CA_BASE64"] = certificate.base64EncodedString()
        app.launchEnvironment["MAICHAT_LOCAL_UI_MODEL"] = "1"
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        XCTAssertTrue(app.buttons["创作中心"].waitForExistence(timeout: 30))
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.375, dy: 0.93)).tap()
        let composer = app.textViews["ai-composer"]
        XCTAssertTrue(composer.waitForExistence(timeout: 20))
        composer.tap()
        composer.typeText("remote-session-delete-tool")
        app.buttons["ai-send-draft"].tap()
        let approve = app.buttons["允许一次"]
        XCTAssertTrue(approve.waitForExistence(timeout: 30),
                      "远端删除工具应要求本次批准")
        XCTAssertFalse(app.buttons["本会话允许"].exists,
                       "删除远端对话不得整会话放行")
        let (beforeData, _) = try await URLSession.shared.data(from: statusURL)
        let before = try XCTUnwrap(
            JSONSerialization.jsonObject(with: beforeData) as? [String: Any])
        XCTAssertTrue((before["macSessions"] as? [String] ?? [])
            .contains("ses_mock_mac"), "批准前不得删除所属设备的对话")
        approve.tap()
        XCTAssertTrue(app.staticTexts["REMOTE_DELETE_TOOL_DONE"]
            .waitForExistence(timeout: 30))
        let (afterData, _) = try await URLSession.shared.data(from: statusURL)
        let after = try XCTUnwrap(
            JSONSerialization.jsonObject(with: afterData) as? [String: Any])
        XCTAssertFalse((after["macSessions"] as? [String] ?? [])
            .contains("ses_mock_mac"))
        XCTAssertTrue((after["modelCalls"] as? [String] ?? [])
            .contains("remote_session_delete"))
        let requests = after["modelRequests"] as? [[String: Any]] ?? []
        XCTAssertTrue(requests.contains(where: {
            ($0["toolOutputText"] as? String ?? "").contains("session_id")
        }), "Agent 删除工具未返回所属设备的确认结果")
    }

    @MainActor func testRemoteWorkspaceToolOnMac() async throws {
        let statusURL = URL(string: "http://127.0.0.1:18190/test/status")!
        guard let probe = try? await URLSession.shared.data(from: statusURL),
              (probe.1 as? HTTPURLResponse)?.statusCode == 200 else {
            throw XCTSkip("需要本机 Session 同步测试服务")
        }
        let (certificate, _) = try await URLSession.shared.data(from:
            URL(string: "http://127.0.0.1:18190/test/cert")!)
        let app = XCUIApplication()
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] = "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18190"
        app.launchEnvironment["MAICHAT_LOCAL_UI_CA_BASE64"] = certificate.base64EncodedString()
        app.launchEnvironment["MAICHAT_LOCAL_UI_MODEL"] = "1"
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        XCTAssertTrue(app.buttons["创作中心"].waitForExistence(timeout: 30))
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.375, dy: 0.93)).tap()
        let composer = app.textViews["ai-composer"]
        XCTAssertTrue(composer.waitForExistence(timeout: 20))
        composer.tap()
        composer.typeText("remote-session-workspace-tool")
        app.buttons["ai-send-draft"].tap()
        let approve = app.buttons["允许一次"]
        XCTAssertTrue(approve.waitForExistence(timeout: 30),
                      "远端 Workspace 工具应要求本次批准")
        let (beforeData, _) = try await URLSession.shared.data(from: statusURL)
        let before = try XCTUnwrap(
            JSONSerialization.jsonObject(with: beforeData) as? [String: Any])
        XCTAssertTrue((before["workspaceSwitches"] as? [[String: String]] ?? []).isEmpty,
                      "批准前不得执行远端 Workspace 切换")
        approve.tap()
        XCTAssertTrue(app.staticTexts["REMOTE_WORKSPACE_TOOL_DONE"]
            .waitForExistence(timeout: 30))
        let (afterData, _) = try await URLSession.shared.data(from: statusURL)
        let after = try XCTUnwrap(
            JSONSerialization.jsonObject(with: afterData) as? [String: Any])
        XCTAssertTrue((after["modelCalls"] as? [String] ?? [])
            .contains("remote_session_set_workspace"))
        XCTAssertTrue((after["workspaceSwitches"] as? [[String: String]] ?? [])
            .contains { $0["session_id"] == "ses_mock_mac" &&
                $0["workspace_path"] == "/tmp" })
        let requests = after["modelRequests"] as? [[String: Any]] ?? []
        XCTAssertTrue(requests.contains(where: {
            ($0["toolOutputText"] as? String ?? "").contains("workspace_path")
        }), "Agent 工具未返回目标设备的 Workspace")
    }

    @MainActor func testRemoteMacPermissionCanBeApprovedOnIPhone() async throws {
        let base = "http://127.0.0.1:18190"
        let statusURL = URL(string: base + "/test/status")!
        guard let probe = try? await URLSession.shared.data(from: statusURL),
              (probe.1 as? HTTPURLResponse)?.statusCode == 200 else {
            throw XCTSkip("需要本机 Session 同步测试服务")
        }
        let (certificate, _) = try await URLSession.shared.data(from:
            URL(string: base + "/test/cert")!)
        let (_, setupResponse) = try await URLSession.shared.data(from:
            URL(string: base + "/test/mac-permission")!)
        XCTAssertEqual((setupResponse as? HTTPURLResponse)?.statusCode, 200)
        let app = XCUIApplication()
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] = "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18190"
        app.launchEnvironment["MAICHAT_LOCAL_UI_CA_BASE64"] = certificate.base64EncodedString()
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        XCTAssertTrue(app.buttons["创作中心"].waitForExistence(timeout: 30))
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.375, dy: 0.93)).tap()
        let start = app.coordinate(withNormalizedOffset: CGVector(dx: 0.01, dy: 0.5))
        let end = app.coordinate(withNormalizedOffset: CGVector(dx: 0.72, dy: 0.5))
        start.press(forDuration: 0.05, thenDragTo: end)
        let remote = app.buttons["ai-remote-conversation-mac:localmacfixture:ses_mock_mac"]
        for _ in 0..<8 where !remote.isHittable {
            app.scrollViews["ai-session-drawer"].swipeUp()
        }
        XCTAssertTrue(remote.waitForExistence(timeout: 30))
        remote.tap()
        let approve = app.buttons["允许一次"]
        XCTAssertTrue(approve.waitForExistence(timeout: 30),
                      "Mac Agent 的审批应在远端 iPhone 会话中显示")
        XCTAssertFalse(app.buttons["本会话允许"].exists,
                       "删除远端 Session 只能逐次批准")
        approve.tap()
        var decisions: [String] = []
        for _ in 0..<20 {
            let (data, _) = try await URLSession.shared.data(from: statusURL)
            let status = try XCTUnwrap(
                JSONSerialization.jsonObject(with: data) as? [String: Any])
            decisions = status["macPermissionDecisions"] as? [String] ?? []
            if decisions.contains("approved") { break }
            try await Task.sleep(for: .milliseconds(500))
        }
        XCTAssertTrue(decisions.contains("approved"),
                      "iPhone 没有以统一协议回传批准决定")
    }

    @MainActor func testLiveIPhoneCreatesAndDeletesMacSession() async throws {
        guard ProcessInfo.processInfo.environment["MAICHAT_LIVE_SESSION_LIFECYCLE_TEST"] == "1" else {
            throw XCTSkip("仅在用户授权真实双端验收时运行")
        }
        let app = XCUIApplication()
        app.launch()
        let creationCenter = app.buttons["创作中心"]
        guard creationCenter.waitForExistence(timeout: 30) else {
            let capture = XCTAttachment(screenshot: app.screenshot())
            capture.name = "live-session-lifecycle-login-required"
            capture.lifetime = .keepAlways
            add(capture)
            XCTFail("iPhone 必须已登录真实 MaiChat 账号")
            return
        }
        creationCenter.tap()
        let conversations = app.buttons["对话列表"]
        guard conversations.waitForExistence(timeout: 20) else {
            XCTFail("打开创作中心后未出现对话列表")
            return
        }
        let drawer = app.descendants(matching: .any)
            .matching(identifier: "ai-session-drawer").firstMatch
        if !drawer.exists { conversations.tap() }
        guard drawer.waitForExistence(timeout: 5) else {
            XCTFail("会话侧栏或抽屉未打开")
            return
        }
        let list = drawer.scrollViews.firstMatch
        guard list.waitForExistence(timeout: 5) else {
            XCTFail("会话列表未出现")
            return
        }
        let create = app.buttons.matching(NSPredicate(
            format: "identifier BEGINSWITH %@", "ai-remote-create-mac:"
        )).firstMatch
        for _ in 0..<12 {
            if create.isHittable { break }
            list.swipeUp()
        }
        guard create.isHittable, create.isEnabled else {
            XCTFail("在线 Mac 的新建对话入口未出现在会话列表")
            return
        }
        let deviceID = String(create.identifier.dropFirst("ai-remote-create-".count))
        let macHeader = app.buttons["ai-session-device-\(deviceID)"]
        let prefix = "ai-remote-conversation-\(deviceID):"
        let rows = app.buttons.matching(NSPredicate(
            format: "identifier BEGINSWITH %@", prefix
        ))
        func macSessionCount() -> Int? {
            let countLabel = macHeader.label.components(separatedBy: "，").last ?? ""
            return Int(countLabel.components(separatedBy: " ").first ?? "")
        }
        guard macHeader.exists, let originalCount = macSessionCount() else {
            XCTFail("无法读取 Mac 会话数量；停止以免误删已有对话")
            return
        }
        var before = Set<String>()
        for _ in 0..<12 {
            before.formUnion(rows.allElementsBoundByIndex.map(\.identifier))
            if before.count == originalCount { break }
            list.swipeUp()
        }
        guard before.count == originalCount else {
            XCTFail("没有读全 Mac 原有会话；停止以免误删已有对话")
            return
        }
        for _ in 0..<12 {
            if create.isHittable { break }
            list.swipeDown()
        }
        guard create.isHittable else {
            XCTFail("无法重新定位 Mac 新建入口")
            return
        }
        create.tap()
        // 竖屏创建成功会关闭抽屉；横屏侧栏始终可见，点标题按钮反会将其收起。
        let wideLayout = app.frame.width >= 700 && app.frame.width > app.frame.height
        if !wideLayout {
            for _ in 0..<120 {
                if !drawer.exists { break }
                try await Task.sleep(for: .milliseconds(300))
            }
            guard !drawer.exists else {
                XCTFail("远端新建未完成或抽屉未关闭；停止删除操作")
                return
            }
            conversations.tap()
        }
        guard drawer.waitForExistence(timeout: 5) else {
            XCTFail("创建后会话列表未出现")
            return
        }
        for _ in 0..<50 {
            if macSessionCount() == originalCount + 1 { break }
            try await Task.sleep(for: .milliseconds(300))
        }
        guard macSessionCount() == originalCount + 1 else {
            XCTFail("Mac 创建后会话数量未增加一条；停止删除操作")
            return
        }
        var added = Set<String>()
        for _ in 0..<12 {
            added.formUnion(rows.allElementsBoundByIndex.map(\.identifier).filter {
                !before.contains($0)
            })
            if added.count == 1 { break }
            list.swipeUp()
        }
        guard added.count == 1, let created = added.first else {
            XCTFail("无法唯一确定刚创建的 Mac 会话；停止删除操作")
            return
        }
        let createdCapture = XCTAttachment(screenshot: app.screenshot())
        createdCapture.name = "live-iphone-created-mac-session"
        createdCapture.lifetime = .keepAlways
        add(createdCapture)
        let target = String(created.dropFirst("ai-remote-conversation-".count))
        let remove = app.buttons["ai-remote-delete-\(target)"]
        XCTAssertTrue(remove.waitForExistence(timeout: 5))
        remove.tap()
        app.buttons["删除对话"].tap()
        let alert = app.alerts["删除远端对话"]
        XCTAssertTrue(alert.waitForExistence(timeout: 5))
        alert.buttons["删除"].tap()
        let deleted = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "exists == false"),
            object: app.buttons[created])
        XCTAssertEqual(XCTWaiter.wait(for: [deleted], timeout: 30), .completed,
                       "Mac 没有删除刚才创建的测试 Session")
    }

    @MainActor func testFakeMacCreatesAndDeletesIPhoneSession() async throws {
        let statusURL = URL(string: "http://127.0.0.1:18190/test/status")!
        guard let probe = try? await URLSession.shared.data(from: statusURL),
              (probe.1 as? HTTPURLResponse)?.statusCode == 200 else {
            throw XCTSkip("需要本机 Session 同步测试服务")
        }
        let (certificate, _) = try await URLSession.shared.data(from:
            URL(string: "http://127.0.0.1:18190/test/cert")!)
        let app = XCUIApplication()
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] = "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18190"
        app.launchEnvironment["MAICHAT_LOCAL_UI_CA_BASE64"] = certificate.base64EncodedString()
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        XCTAssertTrue(app.buttons["创作中心"].waitForExistence(timeout: 30))
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.375, dy: 0.93)).tap()
        XCTAssertTrue(app.buttons["对话列表"].waitForExistence(timeout: 20))

        func issue(_ action: String, target: String = "") async throws -> String {
            var components = URLComponents(string: "http://127.0.0.1:18190/test/phone-lifecycle")!
            components.queryItems = [URLQueryItem(name: "action", value: action)]
            if !target.isEmpty {
                components.queryItems?.append(URLQueryItem(name: "target", value: target))
            }
            var request = URLRequest(url: try XCTUnwrap(components.url))
            request.httpMethod = "POST"
            let (data, response) = try await URLSession.shared.data(for: request)
            XCTAssertEqual((response as? HTTPURLResponse)?.statusCode, 200)
            let object = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
            return try XCTUnwrap(object["command_id"] as? String)
        }

        func awaitResult(_ commandID: String) async throws -> [String: Any] {
            for _ in 0..<60 {
                let (data, _) = try await URLSession.shared.data(from: statusURL)
                let status = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
                let results = status["phoneLifecycleResults"] as? [[String: Any]] ?? []
                if let result = results.first(where: { $0["command_id"] as? String == commandID }) {
                    return try XCTUnwrap(result["result"] as? [String: Any])
                }
                try await Task.sleep(for: .milliseconds(500))
            }
            XCTFail("iPhone 没有返回 Session 操作结果")
            return [:]
        }

        var createID = ""
        for _ in 0..<40 {
            let (data, _) = try await URLSession.shared.data(from: statusURL)
            let status = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
            if (status["slots"] as? [String] ?? []).contains("ios_phone") {
                createID = try await issue("session_create")
                break
            }
            try await Task.sleep(for: .milliseconds(500))
        }
        XCTAssertFalse(createID.isEmpty, "iPhone 没有上线")
        let createResult = try await awaitResult(createID)
        XCTAssertEqual(createResult["ok"] as? Bool, true)
        let sessionID = try XCTUnwrap(createResult["session_id"] as? String)
        XCTAssertFalse(sessionID.isEmpty)
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.01, dy: 0.5))
            .press(forDuration: 0.05,
                   thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: 0.72, dy: 0.5)))
        XCTAssertTrue(app.buttons["ai-conversation-\(sessionID)"].waitForExistence(timeout: 10))
        let temporaryIMError = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "IM 操作失败")
        ).firstMatch
        if temporaryIMError.exists { temporaryIMError.tap() }
        let created = XCTAttachment(screenshot: app.screenshot())
        created.name = "phone-owner-session-created-remotely"
        created.lifetime = .keepAlways
        add(created)

        var advertised = false
        for _ in 0..<30 {
            let (data, _) = try await URLSession.shared.data(from: statusURL)
            let status = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
            if (status["phoneSessions"] as? [String] ?? []).contains(sessionID) {
                advertised = true
                break
            }
            try await Task.sleep(for: .milliseconds(300))
        }
        XCTAssertTrue(advertised, "iPhone 创建后没有更新设备目录")
        let deleteID = try await issue("session_delete", target: sessionID)
        let deleteResult = try await awaitResult(deleteID)
        XCTAssertEqual(deleteResult["ok"] as? Bool, true)
        XCTAssertEqual(deleteResult["session_id"] as? String, sessionID)
        var removed = false
        for _ in 0..<30 {
            let (data, _) = try await URLSession.shared.data(from: statusURL)
            let status = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
            if !(status["phoneSessions"] as? [String] ?? []).contains(sessionID) {
                removed = true
                break
            }
            try await Task.sleep(for: .milliseconds(300))
        }
        XCTAssertTrue(removed, "iPhone 删除后目录仍包含原 Session")
    }

    @MainActor func testPhoneSessionPublishesSharedRoomMessages() async throws {
        let statusURL = URL(string: "http://127.0.0.1:18190/test/status")!
        guard let probe = try? await URLSession.shared.data(from: statusURL),
              (probe.1 as? HTTPURLResponse)?.statusCode == 200 else {
            throw XCTSkip("需要本机无付费 Session 同步服务")
        }
        let certificateURL = URL(string: "http://127.0.0.1:18190/test/cert")!
        let (certificate, _) = try await URLSession.shared.data(from: certificateURL)
        let app = XCUIApplication()
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] = "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18190"
        app.launchEnvironment["MAICHAT_LOCAL_UI_CA_BASE64"] = certificate.base64EncodedString()
        app.launchEnvironment["MAICHAT_LOCAL_UI_MODEL"] = "1"
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        XCTAssertTrue(app.buttons["创作中心"].waitForExistence(timeout: 30))
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.375, dy: 0.93)).tap()
        XCTAssertTrue(app.buttons["对话列表"].waitForExistence(timeout: 20))

        func sendFixture(_ path: String) async throws -> Int {
            var request = URLRequest(url: URL(string: "http://127.0.0.1:18190/test/\(path)")!)
            request.httpMethod = "POST"
            let (_, response) = try await URLSession.shared.data(for: request)
            return (response as? HTTPURLResponse)?.statusCode ?? 0
        }
        var subscribed = false
        for _ in 0..<40 {
            if try await sendFixture("phone-subscribe") == 200 {
                subscribed = true
                break
            }
            try await Task.sleep(for: .milliseconds(500))
        }
        XCTAssertTrue(subscribed, "iPhone 未上报本地会话")
        let promptStatus = try await sendFixture("phone-prompt")
        XCTAssertEqual(promptStatus, 200)

        var receivedReply = false
        var receivedCanonicalPrompt = false
        for _ in 0..<60 {
            let (data, _) = try await URLSession.shared.data(from: statusURL)
            let status = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
            let events = status["phoneEvents"] as? [[String: Any]] ?? []
            for event in events where event["kind"] as? String == "event" {
                let body = event["body"] as? [String: Any] ?? [:]
                guard let messages = body["messages"] as? [[String: Any]] else { continue }
                for message in messages {
                    let parts = message["parts"] as? [[String: Any]] ?? []
                    let text = parts.compactMap { $0["text"] as? String }.joined()
                    if message["role"] as? String == "assistant", text.contains("收到") {
                        receivedReply = true
                    }
                    if message["role"] as? String == "user", text == "你好" {
                        receivedCanonicalPrompt = true
                    }
                }
            }
            if receivedReply && receivedCanonicalPrompt { break }
            try await Task.sleep(for: .milliseconds(500))
        }
        XCTAssertTrue(receivedReply, "iPhone 未通过 WebSocket 推送 AI 回复")
        XCTAssertTrue(receivedCanonicalPrompt, "共享群消息中缺少用户的正式消息")
        let capture = XCTAttachment(screenshot: app.screenshot())
        capture.name = "phone-session-remote-prompt-and-reply"
        capture.lifetime = .keepAlways
        add(capture)
    }

    @MainActor func testCameraAgentCommandsReturnWithoutWaitingForShutter() async throws {
        let statusURL = URL(string: "http://127.0.0.1:18190/test/status")!
        guard let probe = try? await URLSession.shared.data(from: statusURL),
              (probe.1 as? HTTPURLResponse)?.statusCode == 200 else {
            throw XCTSkip("需要本机无付费模型测试服务")
        }
        let (certificate, _) = try await URLSession.shared.data(from:
            URL(string: "http://127.0.0.1:18190/test/cert")!)
        let app = XCUIApplication()
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] = "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18190"
        app.launchEnvironment["MAICHAT_LOCAL_UI_CA_BASE64"] = certificate.base64EncodedString()
        app.launchEnvironment["MAICHAT_LOCAL_UI_MODEL"] = "1"
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        XCTAssertTrue(app.buttons["创作中心"].waitForExistence(timeout: 30))
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.375, dy: 0.93)).tap()
        let composer = app.textViews["ai-composer"]
        XCTAssertTrue(composer.waitForExistence(timeout: 20))
        let send = app.buttons["ai-send-draft"]
        for (command, answer) in [("camera-open", "CAMERA_OPEN_DONE")] {
            composer.tap()
            composer.typeText(command)
            XCTAssertTrue(send.waitForExistence(timeout: 10))
            send.tap()
            XCTAssertTrue(app.staticTexts[answer].waitForExistence(timeout: 15),
                          "相机工具 \(command) 阻塞了后续 Agent 回答")
        }
        let cameraDismiss = app.buttons["DismissImagePickerButton"]
        XCTAssertTrue(cameraDismiss.waitForExistence(timeout: 10), "相机预览没有保持打开")
        let opened = XCTAttachment(screenshot: app.screenshot())
        opened.name = "agent-camera-open-before-remote-close"
        opened.lifetime = .keepAlways
        add(opened)
        var closeDelivered = false
        for _ in 0..<20 {
            var request = URLRequest(url: URL(string:
                "http://127.0.0.1:18190/test/phone-prompt?text=camera-close")!)
            request.httpMethod = "POST"
            let (_, response) = try await URLSession.shared.data(for: request)
            if (response as? HTTPURLResponse)?.statusCode == 200 {
                closeDelivered = true
                break
            }
            try await Task.sleep(for: .milliseconds(500))
        }
        XCTAssertTrue(closeDelivered, "远端设备未找到 iPhone 的 Session")
        for _ in 0..<30 {
            if composer.isHittable { break }
            try await Task.sleep(for: .milliseconds(500))
        }
        XCTAssertTrue(composer.isHittable, "Agent 的 close 没有关闭相机预览")
        XCTAssertFalse(cameraDismiss.exists)
        XCTAssertTrue(app.staticTexts["CAMERA_CLOSE_DONE"].waitForExistence(timeout: 15))
        let closed = XCTAttachment(screenshot: app.screenshot())
        closed.name = "agent-camera-closed-after-remote-command"
        closed.lifetime = .keepAlways
        add(closed)
        composer.tap()
        composer.typeText("camera-status")
        XCTAssertTrue(send.waitForExistence(timeout: 10))
        send.tap()
        XCTAssertTrue(app.staticTexts["CAMERA_STATUS_DONE"].waitForExistence(timeout: 15))
        let (data, _) = try await URLSession.shared.data(from: statusURL)
        let status = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
        let calls = status["modelCalls"] as? [String] ?? []
        XCTAssertTrue(calls.suffix(3).elementsEqual(["open", "close", "status"]))
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
        let detail = XCTAttachment(screenshot: app.screenshot())
        detail.name = "im-detail-status-and-header-background"
        detail.lifetime = .keepAlways
        add(detail)
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
        let paged = XCTAttachment(screenshot: app.screenshot())
        paged.name = "im-older-page-loaded-without-empty-viewport"
        paged.lifetime = .keepAlways
        add(paged)
        XCTAssertTrue(earlier.waitForExistence(timeout: 5),
                      "到达已加载页顶部后应开放上一页")
    }

    @MainActor func testFastHistorySwipeWithExpandedHistory() throws {
        XCUIDevice.shared.orientation = .portrait
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--im-large-history-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launchEnvironment["MAICHAT_IM_HISTORY_EXPANDED_PAGES"] = "1"
        app.launch()
        let conversation = app.buttons["im-conversation-im-test-peer"]
        XCTAssertTrue(conversation.waitForExistence(timeout: 15))
        conversation.tap()
        XCTAssertTrue(app.staticTexts["im-history-expanded-ready"]
            .waitForExistence(timeout: 15))

        let latest = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "IM 历史消息 2543")).firstMatch
        XCTAssertTrue(latest.waitForExistence(timeout: 15))
        let timeline = app.scrollViews.element(boundBy: 0)
        let laidOutRows = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "IM 历史消息 "))
        XCTAssertGreaterThanOrEqual(laidOutRows.count, 160,
                                    "长列表手势测试需要先打开足够多的历史页")
        timeline.swipeDown(velocity: .fast)
        let afterSwipe = XCTAttachment(screenshot: app.screenshot())
        afterSwipe.name = "im-fast-history-swipe-after-gesture"
        afterSwipe.lifetime = .keepAlways
        add(afterSwipe)
        Thread.sleep(forTimeInterval: 1.5)
        let inFlightShot = XCTAttachment(screenshot: app.screenshot())
        inFlightShot.name = "im-fast-history-swipe-after-1p5s"
        inFlightShot.lifetime = .keepAlways
        add(inFlightShot)
        Thread.sleep(forTimeInterval: 2.0)
        let afterSettling = XCTAttachment(screenshot: app.screenshot())
        afterSettling.name = "im-fast-history-swipe-after-3p5s"
        afterSettling.lifetime = .keepAlways
        add(afterSettling)
        XCTAssertTrue(timeline.exists)
    }

    @MainActor func testDatabaseHistoryLoadsOnePageAtTop() throws {
        XCUIDevice.shared.orientation = .portrait
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--im-large-history-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launchEnvironment["MAICHAT_IM_HISTORY_DATABASE"] = "1"
        app.launch()
        let count = app.staticTexts["im-history-page-count"]
        XCTAssertTrue(count.waitForExistence(timeout: 20))
        let conversation = app.buttons["im-conversation-im-test-peer"]
        XCTAssertTrue(conversation.waitForExistence(timeout: 15))
        conversation.tap()
        let latest = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", "IM 历史消息 2543")).firstMatch
        XCTAssertTrue(latest.waitForExistence(timeout: 15))
        XCTAssertEqual(count.label, "IM 已加载 20 条")

        let timeline = app.scrollViews.element(boundBy: 0)
        for _ in 0..<12 where count.label == "IM 已加载 20 条" {
            timeline.swipeDown(velocity: .fast)
        }
        Thread.sleep(forTimeInterval: 2.0)
        let capture = XCTAttachment(screenshot: app.screenshot())
        capture.name = "im-database-history-one-page-trigger-\(count.label)"
        capture.lifetime = .keepAlways
        add(capture)
        guard let number = Int(count.label.split(separator: " ").dropLast().last ?? "") else {
            XCTFail("历史页数未显示：\(count.label)")
            return
        }
        XCTAssertGreaterThan(number, 20, "必须至少到达顶部并读取一页数据库消息")
        XCTAssertLessThanOrEqual(number, 40,
                                 "只触发一次顶部翻页后不能由行 onAppear 连锁翻完数据库")
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
        XCTAssertFalse(app.buttons["完成"].exists)
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.96, dy: 0.5)).tap()
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
        XCTAssertTrue(app.buttons["音乐"].isHittable)
        XCTAssertFalse(app.buttons["语音输入"].exists)
        let musicEntry = XCTAttachment(screenshot: app.screenshot())
        musicEntry.name = "creative-center-local-music-entry"
        musicEntry.lifetime = .keepAlways
        add(musicEntry)

        app.buttons["收起更多功能"].tap()
        XCTAssertEqual(waitUntilNotHittable(app.buttons["相册"]), .completed)

        more.tap()
        XCTAssertTrue(app.buttons["相册"].waitForExistence(timeout: 3))
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.88, dy: 0.42)).tap()
        XCTAssertEqual(waitUntilNotHittable(app.buttons["相册"]), .completed)
    }

    func testLocalMusicOpensSystemFilePicker() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let more = app.buttons["展开更多功能"]
        XCTAssertTrue(more.waitForExistence(timeout: 15))
        more.tap()
        let music = app.buttons["音乐"]
        XCTAssertTrue(music.waitForExistence(timeout: 3))
        music.tap()
        XCTAssertTrue(app.buttons["系统音乐库"].waitForExistence(timeout: 3))
        let chooseFile = app.buttons["选择文件"]
        XCTAssertTrue(chooseFile.waitForExistence(timeout: 3))
        chooseFile.tap()
        let englishCancel = app.buttons["Cancel"]
        let chineseCancel = app.buttons["取消"]
        XCTAssertTrue(englishCancel.waitForExistence(timeout: 8) ||
                      chineseCancel.waitForExistence(timeout: 2))
        let picker = XCTAttachment(screenshot: app.screenshot())
        picker.name = "creative-center-system-music-picker"
        picker.lifetime = .keepAlways
        add(picker)
        let cancel = englishCancel.isHittable ? englishCancel : chineseCancel
        XCTAssertTrue(cancel.isHittable)
        cancel.tap()
        XCTAssertTrue(more.waitForExistence(timeout: 5))
    }

    func testLocalMusicPlayerControlsSilentWav() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-local-music-ui-test"]
        app.launch()
        let start = app.buttons["测试本地音乐"]
        XCTAssertTrue(start.waitForExistence(timeout: 10))
        start.tap()
        XCTAssertTrue(app.staticTexts["测试静音音乐"].waitForExistence(timeout: 5))
        XCTAssertTrue(app.buttons["暂停"].waitForExistence(timeout: 5))
        let playing = XCTAttachment(screenshot: app.screenshot())
        playing.name = "creative-center-local-music-playing"
        playing.lifetime = .keepAlways
        add(playing)
        app.buttons["暂停"].tap()
        XCTAssertTrue(app.buttons["播放"].waitForExistence(timeout: 3))
        app.buttons["停止"].tap()
        XCTAssertTrue(app.staticTexts["测试静音音乐"].exists)
    }

    func testMusicMiniPlayerKeepsChatAvailable() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-music-mini-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let mini = app.descendants(matching: .any)
            .matching(identifier: "ai-music-mini-player").firstMatch
        XCTAssertTrue(mini.waitForExistence(timeout: 15))
        XCTAssertTrue(app.descendants(matching: .any)
            .matching(identifier: "ai-composer").firstMatch.exists)
        let screenshot = XCTAttachment(screenshot: app.screenshot())
        screenshot.name = "creative-center-music-mini-player"
        screenshot.lifetime = .keepAlways
        add(screenshot)
        app.buttons["播放器"].tap()
        XCTAssertTrue(app.staticTexts["测试音乐"].waitForExistence(timeout: 5))
    }

    func testThinkingEffortOffersThreeDeepSeekLevels() throws {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()

        XCTAssertTrue(app.buttons["更多"].waitForExistence(timeout: 15))
        app.buttons["更多"].tap()
        let effort = app.buttons["思考强度"]
        XCTAssertTrue(effort.waitForExistence(timeout: 3))
        effort.tap()
        let menu = app.descendants(matching: .any)
            .matching(identifier: "ai-effort-menu").firstMatch
        XCTAssertTrue(menu.waitForExistence(timeout: 3))
        XCTAssertTrue(app.buttons["简单"].isHittable)
        XCTAssertTrue(app.buttons["标准"].isHittable)
        XCTAssertTrue(app.buttons["深度"].isHittable)
        let capture = XCTAttachment(screenshot: app.screenshot())
        capture.name = "deepseek-reasoning-effort-menu"
        capture.lifetime = .keepAlways
        add(capture)
        app.buttons["深度"].tap()
        let saved = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "value == %@", "深度"), object: effort)
        XCTAssertEqual(XCTWaiter.wait(for: [saved], timeout: 5), .completed)
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
        let drawerCapture = XCTAttachment(screenshot: app.screenshot())
        drawerCapture.name = "conversation-drawer-without-done-button"
        drawerCapture.lifetime = .keepAlways
        add(drawerCapture)
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
        XCTAssertFalse(app.buttons["完成"].exists)
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.96, dy: 0.5)).tap()
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

        XCTAssertTrue(app.buttons["思考强度"].exists)
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
