// 文件作用：在 iOS 27 及以上通过系统选择器共享整台设备的屏幕，并给 Agent 保存最近画面。
// 用户必须主动选择屏幕；共享期间只缓存切到其他 App 后的最近一帧，停止时清除。
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

    var code: String {
        switch self {
        case .unsupported, .unavailable: return "not_supported"
        case .canceled, .timedOut: return "canceled"
        case .noRecentFrame: return "not_found"
        }
    }

    var errorDescription: String? {
        switch self {
        case .unsupported: return "整屏共享需要 iOS 27 或更新系统的真机"
        case .unavailable: return "这台设备当前不允许屏幕共享"
        case .canceled: return "已取消系统屏幕共享"
        case .timedOut: return "等待系统屏幕共享选择超时"
        case .noRecentFrame: return "尚无其他 App 的最近画面，请切过去后再返回 MaiChat"
        }
    }
}

@MainActor
final class AIMobileScreenCapture: ObservableObject {
    static let shared = AIMobileScreenCapture()

    @Published private(set) var isSharing = false
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

    func saveLatestFrame(to url: URL) throws -> Int {
#if canImport(ScreenCaptureKit) && !targetEnvironment(simulator)
        guard #available(iOS 27.0, *),
              let capture = driver as? AIiOS27ScreenCapture else {
            throw AIMobileScreenCaptureError.unsupported
        }
        let (png, ageMilliseconds) = try capture.latestFrame()
        try png.write(to: url, options: .atomic)
        var values = URLResourceValues()
        values.isExcludedFromBackup = true
        var saved = url
        do {
            try saved.setResourceValues(values)
        } catch {
            try? FileManager.default.removeItem(at: url)
            throw error
        }
        return ageMilliseconds
#else
        throw AIMobileScreenCaptureError.unsupported
#endif
    }
}

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
        guard picker.isActive, pendingStart != nil || stream != nil else { return }
        let previous = stream
        stream = nil
        if let previous { try? await previous.stopCapture() }
        frameSink.clear()
        frameSink.setBackground(UIApplication.shared.applicationState != .active)
        do {
            let next = SCStream(filter: filter, configuration: SCStreamConfiguration(), delegate: delegate)
            try next.addStreamOutput(frameSink, type: .screen, sampleHandlerQueue: frameQueue)
            try await next.startCapture()
            guard picker.isActive else {
                try? await next.stopCapture()
                return
            }
            stream = next
            stateChanged(true)
            finishStart(error: nil)
        } catch {
            finishStart(error: error)
            await stop()
        }
    }

    func stop() async {
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
        if pendingStart != nil { finishStart(error: AIMobileScreenCaptureError.canceled) }
        if stream == nil { deactivatePicker() }
    }

    func didFail(_ error: Error) {
        finishStart(error: error)
        if stream == nil { deactivatePicker() }
    }

    func didStop(_ stopped: SCStream) {
        guard stream === stopped else { return }
        stream = nil
        frameSink.clear()
        stateChanged(false)
        deactivatePicker()
    }

    func latestFrame() throws -> (Data, Int) {
        guard stream != nil else { throw AIMobileScreenCaptureError.unavailable }
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
        Task { @MainActor [weak owner] in owner?.didStop(stream) }
    }
}
#endif
