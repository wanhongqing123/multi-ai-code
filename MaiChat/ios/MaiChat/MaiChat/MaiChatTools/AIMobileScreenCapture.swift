// 文件作用：直接截取 MaiChat 当前窗口，或在 iOS 27 真机通过用户开启的系统共享读取其他 App 最近画面。
// 两种来源独立；当前窗口截图不打开系统选择器，也不覆盖其他 App 的缓存。
import CoreImage
import CoreMedia
import Foundation
import ImageIO
#if canImport(ScreenCaptureKit) && !targetEnvironment(simulator)
@preconcurrency import ScreenCaptureKit
#endif
import SwiftUI
import UIKit
import UniformTypeIdentifiers

func aiSupportsWholeDeviceScreenCapture() -> Bool {
#if canImport(ScreenCaptureKit) && !targetEnvironment(simulator)
    if #available(iOS 27.0, *) { return true }
#endif
    return false
}

enum AIMobileScreenCaptureError: LocalizedError {
    case unsupported
    case unavailable
    case canceled
    case timedOut
    case noRecentFrame
    case notSharing
    case appNotActive
    case invalidOutput
    case captureFailed

    var code: String {
        switch self {
        case .unsupported, .unavailable: return "not_supported"
        case .canceled, .timedOut: return "canceled"
        case .noRecentFrame: return "not_found"
        case .notSharing, .appNotActive: return "not_configured"
        case .invalidOutput: return "invalid_input"
        case .captureFailed: return "internal"
        }
    }

    var errorDescription: String? {
        switch self {
        case .unsupported: return "整屏共享需要 iOS 27 或更新系统的真机"
        case .unavailable: return "这台设备当前不允许屏幕共享"
        case .canceled: return "已取消系统屏幕共享"
        case .timedOut: return "等待系统屏幕共享选择超时"
        case .noRecentFrame: return "尚无其他 App 的最近画面，请切到目标 App 停留片刻再返回。截 MaiChat 页面可直接使用 current_app。"
        case .notSharing: return "整屏共享尚未开启或已停止。截其他 App 前请重新开启共享；截 MaiChat 页面可直接使用 current_app。"
        case .appNotActive: return "请先返回 MaiChat 并收起系统面板，再截取当前页面。"
        case .invalidOutput: return "截图必须保存为当前会话工作区 screenshots 目录里的新 PNG 文件。"
        case .captureFailed: return "未能截取当前 MaiChat 页面，请稍后重试。"
        }
    }
}

enum AIMobileScreenCaptureTarget: String, Sendable {
    case currentApp = "current_app"
    case lastExternal = "last_external"
}

struct AIMobileCapturedFrame: Sendable {
    let source: AIMobileScreenCaptureTarget
    let ageMilliseconds: Int
}

@MainActor
final class AIMobileScreenCapture: ObservableObject {
    static let shared = AIMobileScreenCapture()

    @Published private(set) var isSharing = false
#if DEBUG
    private var smokeStarted = false
#endif
#if canImport(ScreenCaptureKit) && !targetEnvironment(simulator)
    private var driver: AnyObject?
#endif

    func start() async throws {
#if canImport(ScreenCaptureKit) && !targetEnvironment(simulator)
        guard #available(iOS 27.0, *) else { throw AIMobileScreenCaptureError.unsupported }
        let capture: AIiOS27ScreenCapture
        if let current = driver as? AIiOS27ScreenCapture {
            capture = current
        } else {
            capture = AIiOS27ScreenCapture { [weak self] sharing in
                self?.isSharing = sharing
            }
            driver = capture
        }
        try await capture.start()
#else
        throw AIMobileScreenCaptureError.unsupported
#endif
    }

    func stop() async {
#if canImport(ScreenCaptureKit) && !targetEnvironment(simulator)
        guard #available(iOS 27.0, *),
              let capture = driver as? AIiOS27ScreenCapture else { return }
        await capture.stop()
#endif
    }

    // Both ordinary UI callers and Agent tools use the same destination checks
    // and PNG writer. A current-app snapshot never opens the system picker or
    // reads/replaces the independent last-external-frame cache.
    func capture(to url: URL, workspace: URL,
                 target: AIMobileScreenCaptureTarget = .lastExternal,
                 delayMilliseconds: Int = 0) async throws -> AIMobileCapturedFrame {
        guard (0...30_000).contains(delayMilliseconds) else {
            throw AIMobileScreenCaptureError.invalidOutput
        }
        if delayMilliseconds > 0 {
            try await Task.sleep(for: .milliseconds(delayMilliseconds))
        }
        try Task.checkCancellation()
        let directory = workspace.standardizedFileURL.resolvingSymlinksInPath()
            .appendingPathComponent("screenshots", isDirectory: true)
        let output = url.standardizedFileURL
        guard workspace.isFileURL, output.isFileURL,
              directory.resolvingSymlinksInPath().path == directory.path,
              output.deletingLastPathComponent().resolvingSymlinksInPath().path == directory.path,
              output.pathExtension.lowercased() == "png",
              output.lastPathComponent.hasPrefix("mobile_screen_"),
              !FileManager.default.fileExists(atPath: output.path) else {
            throw AIMobileScreenCaptureError.invalidOutput
        }
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let png: Data
        let ageMilliseconds: Int
        switch target {
        case .currentApp:
            png = try currentAppPNG()
            ageMilliseconds = 0
        case .lastExternal:
            (png, ageMilliseconds) = try lastExternalFrame()
        }
        try Task.checkCancellation()
        try png.write(to: output, options: .atomic)
        var values = URLResourceValues()
        values.isExcludedFromBackup = true
        var saved = output
        do { try saved.setResourceValues(values) }
        catch {
            try? FileManager.default.removeItem(at: output)
            throw error
        }
        return AIMobileCapturedFrame(source: target, ageMilliseconds: ageMilliseconds)
    }

    private func currentAppPNG() throws -> Data {
        guard UIApplication.shared.applicationState == .active,
              let scene = UIApplication.shared.connectedScenes.compactMap({ $0 as? UIWindowScene })
                .first(where: { $0.activationState == .foregroundActive }),
              let window = scene.windows.first(where: {
                $0.isKeyWindow && !$0.isHidden && $0.windowLevel == .normal
              }), window.bounds.width > 0, window.bounds.height > 0 else {
            throw AIMobileScreenCaptureError.appNotActive
        }
        let format = UIGraphicsImageRendererFormat()
        format.scale = window.screen.scale
        format.opaque = true
        var rendered = false
        let image = UIGraphicsImageRenderer(size: window.bounds.size, format: format).image { _ in
            rendered = window.drawHierarchy(in: window.bounds, afterScreenUpdates: true)
        }
        guard rendered, let png = image.pngData(), !png.isEmpty, png.count <= 20_000_000 else {
            throw AIMobileScreenCaptureError.captureFailed
        }
        return png
    }

    private func lastExternalFrame() throws -> (Data, Int) {
#if canImport(ScreenCaptureKit) && !targetEnvironment(simulator)
        guard #available(iOS 27.0, *) else {
            throw AIMobileScreenCaptureError.unsupported
        }
        guard let capture = driver as? AIiOS27ScreenCapture else {
            throw AIMobileScreenCaptureError.notSharing
        }
        return try capture.latestFrame()
#else
        throw AIMobileScreenCaptureError.unsupported
#endif
    }

#if DEBUG
    // Opt-in, one-shot acceptance hook for a later agreed device test window.
    // It never touches session attachments or starts screen sharing.
    func captureCloudPageSmokeIfRequested() {
        guard !smokeStarted,
              ProcessInfo.processInfo.environment["MAICHAT_CURRENT_APP_CAPTURE_SMOKE"] == "YES"
        else { return }
        smokeStarted = true
        Task { @MainActor in
            let base = FileManager.default.urls(for: .applicationSupportDirectory,
                                                in: .userDomainMask)[0]
                .appendingPathComponent("MaiChat/ScreenCaptureSmoke", isDirectory: true)
                .appendingPathComponent(UUID().uuidString, isDirectory: true)
            let file = base.appendingPathComponent("screenshots/mobile_screen_current_app.png")
            let before = isSharing
            var result: [String: Any] = ["sharing_before": before]
            do {
                guard !before else { throw AIMobileScreenCaptureError.notSharing }
                let receipt = try await capture(to: file, workspace: base,
                    target: .currentApp, delayMilliseconds: 750)
                guard let image = UIImage(contentsOfFile: file.path)?.cgImage else {
                    throw AIMobileScreenCaptureError.captureFailed
                }
                result.merge(["ok": true, "source": receipt.source.rawValue,
                    "path": file.path, "width": image.width, "height": image.height,
                    "sharing_after": isSharing]) { _, new in new }
            } catch {
                result.merge(["ok": false, "error": error.localizedDescription,
                              "sharing_after": isSharing]) { _, new in new }
            }
            try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
            if let data = try? JSONSerialization.data(withJSONObject: result, options: [.sortedKeys]) {
                try? data.write(to: base.appendingPathComponent("result.json"), options: .atomic)
            }
        }
    }
#endif
}

#if targetEnvironment(simulator)
struct AICurrentAppScreenshotUITestRoot: View {
    @State private var result = "未运行"

    var body: some View {
        VStack(spacing: 24) {
            Text("MaiChat 当前页面截图").font(.title2.bold())
            RoundedRectangle(cornerRadius: 24).fill(Color.cyan)
                .overlay(Text("CURRENT APP ONLY").font(.title.bold()))
                .frame(height: 220)
            Button("验证截图") { Task { await verify() } }
                .accessibilityIdentifier("current-app-capture-run")
            Text(result).accessibilityIdentifier("current-app-capture-result")
                .font(.caption).textSelection(.enabled)
        }.padding(28)
    }

    @MainActor private func verify() async {
        let capture = AIMobileScreenCapture.shared
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("current-app-capture-" + UUID().uuidString, isDirectory: true)
        func output(_ suffix: String) -> URL {
            root.appendingPathComponent("screenshots/mobile_screen_" + suffix + ".png")
        }
        func require(_ condition: Bool, _ message: String) throws {
            if !condition { throw NSError(domain: "CurrentAppCaptureTest", code: 1,
                                          userInfo: [NSLocalizedDescriptionKey: message]) }
        }
        result = "运行中"
        do {
            try require(!capture.isSharing, "unexpected sharing before capture")
            let file = output("ordinary")
            let receipt = try await capture.capture(to: file, workspace: root,
                target: .currentApp, delayMilliseconds: 150)
            guard let bitmap = UIImage(contentsOfFile: file.path)?.cgImage else {
                throw AIMobileScreenCaptureError.captureFailed
            }
            try require(receipt.source == .currentApp && bitmap.width > 300 && bitmap.height > 500,
                        "ordinary capture did not produce current window pixels")
#if DEBUG
            AIAssistantModel.shared.installVideoToolTestWorkspace(root.path)
#endif
            let hostFile = output("host")
            let args: [String: Any] = ["output_path": hostFile.path,
                                      "target": "current_app", "delay_ms": 100]
            let encoded = String(data: try JSONSerialization.data(withJSONObject: args), encoding: .utf8)!
            let host = await AIMobileHostToolProvider.shared.execute(
                name: "mobile_capture_screen", argumentsJSON: encoded)
            guard let text = host.output, let data = text.data(using: .utf8),
                  let reply = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
                throw NSError(domain: "CurrentAppCaptureTest", code: 2,
                              userInfo: [NSLocalizedDescriptionKey: host.errorMessage ?? "missing host output"])
            }
            try require(reply["source"] as? String == "current_app" &&
                        reply["whole_device"] as? Bool == false &&
                        UIImage(contentsOfFile: hostFile.path) != nil && !capture.isSharing,
                        "host capture source or sharing contract failed")
            for invalid in ["\"target\":\"device\"", "\"delay_ms\":true", "\"delay_ms\":1.5",
                            "\"delay_ms\":30001", "\"extra\":1"] {
                let rejected = await AIMobileHostToolProvider.shared.execute(
                    name: "mobile_capture_screen",
                    argumentsJSON: "{\"output_path\":\"" + output("invalid").path + "\"," + invalid + "}")
                try require(rejected.errorCode == "invalid_input", "invalid host arguments accepted")
            }
            do {
                _ = try await capture.capture(to: file, workspace: root, target: .currentApp)
                throw NSError(domain: "CurrentAppCaptureTest", code: 3)
            } catch AIMobileScreenCaptureError.invalidOutput {}
            do {
                _ = try await capture.capture(to: root.appendingPathComponent("mobile_screen_escape.png"),
                                              workspace: root, target: .currentApp)
                throw NSError(domain: "CurrentAppCaptureTest", code: 4)
            } catch AIMobileScreenCaptureError.invalidOutput {}
            let canceledFile = output("canceled")
            let delayed = Task { @MainActor in
                try await capture.capture(to: canceledFile, workspace: root,
                    target: .currentApp, delayMilliseconds: 30_000)
            }
            delayed.cancel()
            do { _ = try await delayed.value; throw NSError(domain: "CurrentAppCaptureTest", code: 5) }
            catch is CancellationError {}
            try require(!FileManager.default.fileExists(atPath: canceledFile.path), "canceled PNG leaked")
            do {
                _ = try await capture.capture(to: output("external"), workspace: root)
                throw NSError(domain: "CurrentAppCaptureTest", code: 6)
            } catch AIMobileScreenCaptureError.unsupported {}
            try require(!capture.isSharing, "current app capture started sharing")
#if DEBUG
            capture.captureCloudPageSmokeIfRequested()
            try await Task.sleep(for: .milliseconds(1_000))
            let smokeBase = FileManager.default.urls(for: .applicationSupportDirectory,
                in: .userDomainMask)[0].appendingPathComponent("MaiChat/ScreenCaptureSmoke")
            let smokeFolders = try FileManager.default.contentsOfDirectory(
                at: smokeBase, includingPropertiesForKeys: [.creationDateKey])
                .sorted { $0.lastPathComponent < $1.lastPathComponent }
            var verifiedSmoke = false
            for folder in smokeFolders {
                let bytes = try Data(contentsOf: folder.appendingPathComponent("result.json"))
                let report = try JSONSerialization.jsonObject(with: bytes) as? [String: Any]
                if report?["ok"] as? Bool == true &&
                    report?["source"] as? String == "current_app" &&
                    report?["sharing_before"] as? Bool == false &&
                    report?["sharing_after"] as? Bool == false,
                   let imagePath = report?["path"] as? String,
                   UIImage(contentsOfFile: imagePath) != nil { verifiedSmoke = true }
            }
            try require(verifiedSmoke, "opt-in ordinary capture hook did not save a PNG/report")
#endif
            result = "PASS source=current_app sharing=false \(bitmap.width)x\(bitmap.height)\n" + file.path
        } catch { result = "FAIL " + error.localizedDescription }
    }
}
#endif

#if canImport(ScreenCaptureKit) && !targetEnvironment(simulator)
@available(iOS 27.0, *)
private final class AIiOS27ScreenFrameSink: NSObject, SCStreamOutput, @unchecked Sendable {
    private let lock = NSLock()
    private let renderer = CIContext()
    private var acceptBackgroundFrames = false
    private var lastEncodeUptime = 0.0
    private var lastFrameUptime = 0.0
    private var lastPNG: Data?

    func setBackground(_ background: Bool) {
        lock.lock()
        acceptBackgroundFrames = background
        lock.unlock()
    }

    func clear() {
        lock.lock()
        lastPNG = nil
        lastFrameUptime = 0
        lastEncodeUptime = 0
        acceptBackgroundFrames = false
        lock.unlock()
    }

    func latestFrame() throws -> (Data, Int) {
        lock.lock()
        let data = lastPNG
        let age = Int((ProcessInfo.processInfo.systemUptime - lastFrameUptime) * 1_000)
        lock.unlock()
        guard let data, age >= 0, age <= 300_000 else {
            throw AIMobileScreenCaptureError.noRecentFrame
        }
        return (data, age)
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer,
                of type: SCStreamOutputType) {
        guard type == .screen, CMSampleBufferIsValid(sampleBuffer),
              let pixels = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        let now = ProcessInfo.processInfo.systemUptime
        lock.lock()
        let shouldEncode = acceptBackgroundFrames && now - lastEncodeUptime >= 1
        if shouldEncode { lastEncodeUptime = now }
        lock.unlock()
        guard shouldEncode else { return }

        let image = CIImage(cvPixelBuffer: pixels)
        guard let bitmap = renderer.createCGImage(image, from: image.extent) else { return }
        let output = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(
            output, UTType.png.identifier as CFString, 1, nil) else { return }
        CGImageDestinationAddImage(destination, bitmap, nil)
        guard CGImageDestinationFinalize(destination), output.length > 0,
              output.length <= 20_000_000 else { return }
        lock.lock()
        if acceptBackgroundFrames {
            lastPNG = output as Data
            lastFrameUptime = now
        }
        lock.unlock()
    }
}

@available(iOS 27.0, *)
@MainActor
private final class AIiOS27ScreenCapture: NSObject {
    private let picker = SCContentSharingPicker.shared
    private let frameSink = AIiOS27ScreenFrameSink()
    private let frameQueue = DispatchQueue(label: "maichat.ios.screen-capture", qos: .utility)
    private let stateChanged: @MainActor (Bool) -> Void
    private lazy var delegate = AIiOS27ScreenCaptureDelegate(owner: self)
    private var stream: SCStream?
    private var pendingStart: CheckedContinuation<Void, Error>?
    private var startTimeout: Task<Void, Never>?
    private var observerAdded = false
    private var backgroundObserver: NSObjectProtocol?
    private var foregroundObserver: NSObjectProtocol?

    init(stateChanged: @escaping @MainActor (Bool) -> Void) {
        self.stateChanged = stateChanged
        super.init()
        backgroundObserver = NotificationCenter.default.addObserver(
            forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: .main
        ) { [weak self] _ in
            Task { @MainActor [weak self] in self?.frameSink.setBackground(true) }
        }
        foregroundObserver = NotificationCenter.default.addObserver(
            forName: UIApplication.willEnterForegroundNotification, object: nil, queue: .main
        ) { [weak self] _ in
            Task { @MainActor [weak self] in self?.frameSink.setBackground(false) }
        }
    }

    func start() async throws {
        trace("start-request")
        if stream != nil { return }
        if pendingStart != nil { throw AIMobileScreenCaptureError.unavailable }
        guard picker.isAvailable else { throw AIMobileScreenCaptureError.unavailable }
        var configuration = SCContentSharingPickerConfiguration()
        configuration.showsMicrophoneControl = false
        picker.defaultConfiguration = configuration
        if !observerAdded {
            picker.add(delegate)
            observerAdded = true
        }
        picker.isActive = true
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            pendingStart = continuation
            picker.present()
            startTimeout = Task { @MainActor [weak self] in
                do { try await Task.sleep(for: .seconds(120)) }
                catch { return }
                self?.finishStart(error: AIMobileScreenCaptureError.timedOut)
                if self?.stream == nil { self?.deactivatePicker() }
            }
        }
    }

    func didSelect(_ filter: SCContentFilter) async {
        trace("picker-selected")
        guard picker.isActive, pendingStart != nil || stream != nil else { return }
        let previous = stream
        stream = nil
        if let previous { try? await previous.stopCapture() }
        frameSink.clear()
        frameSink.setBackground(UIApplication.shared.applicationState == .background)
        do {
            let next = SCStream(filter: filter, configuration: SCStreamConfiguration(), delegate: delegate)
            try next.addStreamOutput(frameSink, type: .screen, sampleHandlerQueue: frameQueue)
            try await next.startCapture()
            guard picker.isActive else {
                try? await next.stopCapture()
                return
            }
            stream = next
            trace("stream-started")
            stateChanged(true)
            finishStart(error: nil)
        } catch {
            trace("stream-start-failed", error: error)
            finishStart(error: error)
            await stop()
        }
    }

    func stop() async {
        trace("stop-request")
        let previous = stream
        stream = nil
        frameSink.clear()
        stateChanged(false)
        picker.isActive = false
        if observerAdded {
            picker.remove(delegate)
            observerAdded = false
        }
        if let previous { try? await previous.stopCapture() }
        finishStart(error: AIMobileScreenCaptureError.canceled)
    }

    func didCancel() {
        trace("picker-canceled")
        if pendingStart != nil { finishStart(error: AIMobileScreenCaptureError.canceled) }
        if stream == nil { deactivatePicker() }
    }

    func didFail(_ error: Error) {
        trace("picker-failed", error: error)
        finishStart(error: error)
        if stream == nil { deactivatePicker() }
    }

    func didStop(_ stopped: SCStream, error: Error) {
        trace("stream-stopped", error: error)
        guard stream === stopped else { return }
        stream = nil
        frameSink.clear()
        stateChanged(false)
        deactivatePicker()
    }

    func latestFrame() throws -> (Data, Int) {
        trace("frame-request")
        guard stream != nil else { throw AIMobileScreenCaptureError.notSharing }
        return try frameSink.latestFrame()
    }

    private func finishStart(error: Error?) {
        guard let pendingStart else { return }
        self.pendingStart = nil
        startTimeout?.cancel()
        startTimeout = nil
        if let error { pendingStart.resume(throwing: error) }
        else { pendingStart.resume() }
    }

    private func deactivatePicker() {
        picker.isActive = false
        if observerAdded {
            picker.remove(delegate)
            observerAdded = false
        }
    }

    private func trace(_ event: String, error: Error? = nil) {
        let application = UIApplication.shared
        let window = application.connectedScenes.compactMap { $0 as? UIWindowScene }
            .flatMap(\.windows).first(where: \.isKeyWindow)
        var controller = window?.rootViewController
        while let presented = controller?.presentedViewController { controller = presented }
        var fields = ["stream_present": String(stream != nil),
                      "picker_active": String(picker.isActive),
                      "picker_available": String(picker.isAvailable),
                      "pending_start": String(pendingStart != nil),
                      "application_state": String(application.applicationState.rawValue),
                      "top_controller": controller.map { String(describing: type(of: $0)) } ?? "none"]
        if let error {
            let native = error as NSError
            fields["error_domain"] = native.domain
            fields["error_code"] = String(native.code)
            fields["error_message"] = native.localizedDescription
        }
        AppDiagnosticLog.shared.record(level: .info, category: "screen-capture",
                                       event: event, fields: fields)
    }
}

@available(iOS 27.0, *)
private final class AIiOS27ScreenCaptureDelegate: NSObject,
    SCContentSharingPickerObserver, SCStreamDelegate, @unchecked Sendable {
    private weak var owner: AIiOS27ScreenCapture?

    init(owner: AIiOS27ScreenCapture) { self.owner = owner }

    nonisolated func contentSharingPicker(_ picker: SCContentSharingPicker,
                                          didUpdateWith filter: SCContentFilter,
                                          for stream: SCStream?) {
        Task { @MainActor [weak owner] in await owner?.didSelect(filter) }
    }

    nonisolated func contentSharingPicker(_ picker: SCContentSharingPicker,
                                          didCancelFor stream: SCStream?) {
        Task { @MainActor [weak owner] in owner?.didCancel() }
    }

    nonisolated func contentSharingPickerStartDidFailWithError(_ error: Error) {
        Task { @MainActor [weak owner] in owner?.didFail(error) }
    }

    nonisolated func stream(_ stream: SCStream, didStopWithError error: Error) {
        Task { @MainActor [weak owner] in owner?.didStop(stream, error: error) }
    }
}
#endif
