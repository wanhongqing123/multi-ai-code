// 文件作用：Agent 相机预览按两秒间隔采样，停止后保留一批可供用户选择的图片。
import AVFoundation
import CoreImage
import ImageIO
import SwiftUI
import UniformTypeIdentifiers

/// 同一串行队列处理摄像头帧、采样和写入；停止等待队列排空后才封存批次。
private final class AIMobileCameraStream: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate,
    @unchecked Sendable {
    let session = AVCaptureSession()
    private let queue = DispatchQueue(label: "maichat.ios.agent-camera", qos: .userInitiated)
    private let renderer = CIContext()
    private let context: AIMobileScreenArchiveContext
    private let changed: @Sendable (AIMobileScreenArchiveBatch) -> Void
    private let failed: @Sendable (String) -> Void
    private var latest: CVPixelBuffer?
    private var timer: DispatchSourceTimer?
    private var startedAt = 0.0
    private var ended = false
    private var observers: [NSObjectProtocol] = []
    private var output: AVCaptureVideoDataOutput?
    #if DEBUG && targetEnvironment(simulator)
    private let synthetic = ProcessInfo.processInfo.arguments.contains("--agent-camera-ui-test")
    #endif

    init(context: AIMobileScreenArchiveContext,
         changed: @escaping @Sendable (AIMobileScreenArchiveBatch) -> Void,
         failed: @escaping @Sendable (String) -> Void) {
        self.context = context
        self.changed = changed
        self.failed = failed
    }

    func start() async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            queue.async { [self] in
                do {
                    guard !ended else { throw CancellationError() }
                    #if DEBUG && targetEnvironment(simulator)
                    if synthetic {
                        startedAt = ProcessInfo.processInfo.systemUptime
                        startTimer()
                        continuation.resume()
                        return
                    }
                    #endif
                    guard let camera = AVCaptureDevice.default(.builtInWideAngleCamera,
                                                               for: .video, position: .back)
                            ?? AVCaptureDevice.default(for: .video) else {
                        throw AIMobileLocalMediaRequestError.unavailable
                    }
                    let input = try AVCaptureDeviceInput(device: camera)
                    let output = AVCaptureVideoDataOutput()
                    output.alwaysDiscardsLateVideoFrames = true
                    output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String:
                        kCVPixelFormatType_32BGRA]
                    output.setSampleBufferDelegate(self, queue: queue)
                    session.beginConfiguration()
                    guard session.canAddInput(input), session.canAddOutput(output) else {
                        session.commitConfiguration()
                        throw AIMobileLocalMediaRequestError.unavailable
                    }
                    session.addInput(input)
                    session.addOutput(output)
                    self.output = output
                    session.sessionPreset = .high
                    // 保存帧和竖屏预览使用同一方向，避免相机画面进入附件后旋转。
                    if let connection = output.connection(with: .video),
                       connection.isVideoOrientationSupported { connection.videoOrientation = .portrait }
                    session.commitConfiguration()
                    let center = NotificationCenter.default
                    for name in [AVCaptureSession.wasInterruptedNotification,
                                 AVCaptureSession.runtimeErrorNotification] {
                        observers.append(center.addObserver(forName: name, object: session, queue: nil) {
                            [weak self] _ in self?.reportInterruption()
                        })
                    }
                    session.startRunning()
                    guard session.isRunning else { throw AIMobileLocalMediaRequestError.unavailable }
                    startedAt = ProcessInfo.processInfo.systemUptime
                    startTimer()
                    continuation.resume()
                } catch { continuation.resume(throwing: error) }
            }
        }
    }

    private func startTimer() {
        let source = DispatchSource.makeTimerSource(queue: queue)
        timer = source
        source.setEventHandler { [weak self] in self?.sample() }
        source.schedule(deadline: .now() + .seconds(2), leeway: .milliseconds(10))
        source.resume()
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer,
                       from connection: AVCaptureConnection) {
        guard !ended else { return }
        latest = CMSampleBufferGetImageBuffer(sampleBuffer)
    }

    private func sample() {
        guard !ended, timer != nil else { return }
        var nextDeadline = DispatchTime.now() + .milliseconds(2002)
        defer { timer?.schedule(deadline: nextDeadline, leeway: .milliseconds(5)) }
        #if DEBUG && targetEnvironment(simulator)
        if synthetic { latest = syntheticFrame() }
        #endif
        guard let latest else { return }
        // 以实际取得画面的时刻为基准，确保写入核心的 elapsed 也至少相隔两秒；
        // 单次 timer 和串行队列避免慢设备堆积旧画面。
        nextDeadline = DispatchTime.now() + .milliseconds(2002)
        let now = ProcessInfo.processInfo.systemUptime
        let capturedAt = Int64(Date().timeIntervalSince1970 * 1_000)
        let image = CIImage(cvPixelBuffer: latest)
        guard let bitmap = renderer.createCGImage(image, from: image.extent) else {
            terminate("相机图片处理失败"); return
        }
        let data = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(data, UTType.png.identifier as CFString, 1, nil)
        else { terminate("相机图片处理失败"); return }
        CGImageDestinationAddImage(destination, bitmap, nil)
        guard CGImageDestinationFinalize(destination), data.length > 0, data.length <= 20_000_000 else {
            terminate("相机图片保存失败"); return
        }
        let elapsed = Int64((now - startedAt) * 1_000)
        for attempt in 0..<201 {
            do {
                let (batch, limit, _) = try AIMobileScreenFrameArchive.shared.appendPNG(
                    data as Data, context: context, width: bitmap.width, height: bitmap.height,
                    capturedAtMS: capturedAt, elapsedMS: elapsed)
                changed(batch)
                if limit { terminate("已达到图片保存上限，已保留之前拍摄的图片") }
                return
            } catch let error as AIMobileScreenArchiveError where error.code == "rate_limited" {
                // 核心还按落盘时间限速；编码耗时变化时短暂重试同一帧，不积压新帧。
                if attempt == 200 { terminate("相机图片保存超时，已保留之前拍摄的图片"); return }
                Thread.sleep(forTimeInterval: 0.01)
            } catch { terminate(error.localizedDescription); return }
        }
    }

    private func terminate(_ message: String) {
        // 错误通知主线程前先停 timer，避免队列抢先再取一张。
        timer?.cancel()
        timer = nil
        failed(message)
    }

    #if DEBUG && targetEnvironment(simulator)
    /// 测试帧只在模拟器明确启用时生成，仍走正式采样、封存和选图流程。
    private func syntheticFrame() -> CVPixelBuffer? {
        var buffer: CVPixelBuffer?
        let attributes = [kCVPixelBufferCGImageCompatibilityKey: true,
                          kCVPixelBufferCGBitmapContextCompatibilityKey: true] as CFDictionary
        guard CVPixelBufferCreate(nil, 320, 480, kCVPixelFormatType_32BGRA,
                                  attributes, &buffer) == kCVReturnSuccess,
              let buffer else { return nil }
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        guard let pixels = CVPixelBufferGetBaseAddress(buffer),
              let graphics = CGContext(data: pixels, width: 320, height: 480,
                bitsPerComponent: 8, bytesPerRow: CVPixelBufferGetBytesPerRow(buffer),
                space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: CGBitmapInfo.byteOrder32Little.rawValue |
                    CGImageAlphaInfo.premultipliedFirst.rawValue) else { return nil }
        graphics.setFillColor(CGColor(red: 0.15, green: 0.55, blue: 0.85, alpha: 1))
        graphics.fill(CGRect(x: 0, y: 0, width: 320, height: 480))
        graphics.setFillColor(CGColor(red: 1, green: 1, blue: 1, alpha: 1))
        graphics.fill(CGRect(x: 50, y: 150, width: 220, height: 180))
        return buffer
    }
    #endif

    private func reportInterruption() {
        queue.async { [weak self] in
            guard let self, !self.ended else { return }
            self.terminate("相机已中断，已保留之前拍摄的图片")
        }
    }

    func setOrientation(_ orientation: AVCaptureVideoOrientation) {
        queue.async { [weak self] in
            guard let self, !self.ended, let connection = self.output?.connection(with: .video),
                  connection.isVideoOrientationSupported else { return }
            connection.videoOrientation = orientation
        }
    }

    func stop(reason: String) async throws -> AIMobileScreenArchiveBatch {
        try await withCheckedThrowingContinuation { continuation in
            queue.async { [self] in
                guard !ended else {
                    continuation.resume(throwing: CancellationError()); return
                }
                ended = true
                timer?.cancel(); timer = nil; latest = nil
                for observer in observers { NotificationCenter.default.removeObserver(observer) }
                observers.removeAll()
                if session.isRunning { session.stopRunning() }
                do { continuation.resume(returning: try AIMobileScreenFrameArchive.shared.finishBlocking(
                    context: context, reason: reason)) }
                catch { continuation.resume(throwing: error) }
            }
        }
    }
}

/// 只持有当前会话，所有回调都验证代次，避免旧相机写入新会话或更新新界面。
@MainActor
final class AIMobileCameraCapture: ObservableObject {
    static let shared = AIMobileCameraCapture()
    @Published private(set) var count = 0
    @Published private(set) var isRunning = false
    @Published private(set) var error = ""
    private(set) var archiveContext: AIMobileScreenArchiveContext?
    private var stream: AIMobileCameraStream?
    private var stoppingTask: Task<AIMobileScreenArchiveBatch, Error>?
    private var generation = UUID()
    var previewSession: AVCaptureSession? { stream?.session }

    func setOrientation(_ orientation: AVCaptureVideoOrientation) {
        stream?.setOrientation(orientation)
    }

    func start(context: AIMobileScreenArchiveContext) async throws {
        guard stream == nil, archiveContext == nil else { throw AIMobileLocalMediaRequestError.unavailable }
        stoppingTask = nil
        let id = UUID(); generation = id; count = 0; error = ""
        archiveContext = context
        let created = AIMobileCameraStream(context: context, changed: { [weak self] batch in
            Task { @MainActor in
                guard let self, self.generation == id else { return }
                self.count = batch.frameCount
            }
        }, failed: { [weak self] message in
            Task { @MainActor in
                guard let self, self.generation == id else { return }
                self.error = message
            }
        })
        stream = created
        do {
            try await created.start()
            guard generation == id, !Task.isCancelled else { throw CancellationError() }
            isRunning = true
        } catch {
            // 调用方统一收尾；若用户同时关闭，两个调用者等待同一封存任务。
            throw error
        }
    }

    func stop(reason: String) async throws -> AIMobileScreenArchiveBatch? {
        if let stoppingTask { return try await stoppingTask.value }
        let id = UUID(); generation = id; isRunning = false
        guard let current = stream else { return nil }
        stream = nil
        let task = Task { try await current.stop(reason: reason) }
        stoppingTask = task
        defer { if generation == id { archiveContext = nil } }
        return try await task.value
    }
}

struct AIMobileAgentCameraPreview: UIViewRepresentable {
    let session: AVCaptureSession
    let orientationChanged: (AVCaptureVideoOrientation) -> Void
    func makeUIView(context: Context) -> CameraPreviewView {
        let view = CameraPreviewView()
        view.orientationChanged = orientationChanged
        view.layerView.videoGravity = .resizeAspectFill
        view.layerView.session = session
        return view
    }
    func updateUIView(_ view: CameraPreviewView, context: Context) {
        view.layerView.session = session
        view.orientationChanged = orientationChanged
        view.updateOrientation()
    }

    final class CameraPreviewView: UIView {
        override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }
        var layerView: AVCaptureVideoPreviewLayer { layer as! AVCaptureVideoPreviewLayer }
        var orientationChanged: ((AVCaptureVideoOrientation) -> Void)?
        override func layoutSubviews() {
            super.layoutSubviews()
            updateOrientation()
        }
        func updateOrientation() {
            let orientation: AVCaptureVideoOrientation
            switch window?.windowScene?.interfaceOrientation {
            case .landscapeLeft: orientation = .landscapeLeft
            case .landscapeRight: orientation = .landscapeRight
            case .portraitUpsideDown: orientation = .portraitUpsideDown
            default: orientation = .portrait
            }
            if let connection = layerView.connection, connection.isVideoOrientationSupported {
                connection.videoOrientation = orientation
            }
            orientationChanged?(orientation)
        }
    }
}
