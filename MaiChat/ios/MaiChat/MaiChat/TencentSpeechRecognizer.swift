import AVFoundation
import Combine
import Foundation
@preconcurrency import QCloudRealTime

/// 腾讯云实时语音识别。
///
/// 长按期间持续发布非稳态识别文本，松手后等待服务端的最终文本；SDK 使用内置录音器，
/// 与「直接发送语音」所用的 AVAudioRecorder 相互独立。
@MainActor
final class TencentRealtimeSpeechRecognizer: NSObject, ObservableObject,
    QCloudRealTimeRecognizerDelegate {

    @Published private(set) var liveText = ""
    @Published private(set) var meterLevel: CGFloat = 0
    @Published private(set) var isRecognizing = false

    private let appId: String
    private let secretId: String
    private let secretKey: String
    private var recognizer: QCloudRealTimeRecognizer?
    private var stopContinuation: CheckedContinuation<String, Error>?
    private var stopTimeoutTask: Task<Void, Never>?

    init(appId: String, secretId: String, secretKey: String) {
        self.appId = appId.trimmingCharacters(in: .whitespacesAndNewlines)
        self.secretId = secretId.trimmingCharacters(in: .whitespacesAndNewlines)
        self.secretKey = secretKey.trimmingCharacters(in: .whitespacesAndNewlines)
        super.init()
    }

    var isAvailable: Bool {
        !appId.isEmpty && !secretId.isEmpty && !secretKey.isEmpty
    }

    func start() async throws {
        guard isAvailable else { throw SpeechRecognitionError.unavailable }
        guard await requestRecordPermission() else {
            throw SpeechRecognitionError.service("没有麦克风权限")
        }
        try Task.checkCancellation()

        cancelCurrentSession()
        let audioSession = AVAudioSession.sharedInstance()
        try audioSession.setCategory(.record, mode: .measurement)
        try audioSession.setActive(true)

        let config = QCloudConfig(
            appId: appId,
            secretId: secretId,
            secretKey: secretKey,
            projectId: 0
        )
        config.engineType = "16k_zh"
        config.enableDetectVolume = true
        config.endRecognizeWhenDetectSilence = false
        config.endRecognizeWhenDetectSilenceAutoStop = false
        config.needvad = 1
        config.filterPunc = 0
        config.convertNumMode = 1
        config.requestTimeout = 20

        let nextRecognizer = QCloudRealTimeRecognizer(config: config)
        nextRecognizer.delegate = self
        recognizer = nextRecognizer
        liveText = ""
        meterLevel = 0
        isRecognizing = true
        nextRecognizer.start()
    }

    func stop() async throws -> String {
        guard let recognizer, isRecognizing else {
            throw SpeechRecognitionError.service("实时语音识别尚未启动")
        }
        return try await withCheckedThrowingContinuation { continuation in
            stopContinuation = continuation
            recognizer.stop()
            stopTimeoutTask?.cancel()
            stopTimeoutTask = Task { [weak self] in
                try? await Task.sleep(for: .seconds(1))
                guard !Task.isCancelled, let self else { return }
                // 个别 SDK/网络状态下 stop 后不会回调 DidFinish。屏幕上的实时文本已经
                // 是服务端返回的识别结果，用它兜底，避免界面无限停在“正在完成识别”。
                self.recognizer?.delegate = nil
                self.recognizer?.cancel()
                self.finish(.success(self.liveText))
            }
        }
    }

    func cancel() {
        cancelCurrentSession()
        liveText = ""
        meterLevel = 0
    }

    nonisolated func realTimeRecognizer(
        onSliceRecognize recognizer: QCloudRealTimeRecognizer,
        result: QCloudRealTimeResult
    ) {
        publish(resultText(from: result))
    }

    nonisolated func realTimeRecognizer(
        onSegmentSuccessRecognize recognizer: QCloudRealTimeRecognizer,
        result: QCloudRealTimeResult
    ) {
        publish(resultText(from: result))
    }

    nonisolated func realTimeRecognizerDidFinish(
        _ recognizer: QCloudRealTimeRecognizer,
        result: String
    ) {
        let finalText = result.trimmingCharacters(in: .whitespacesAndNewlines)
        Task { @MainActor [weak self] in
            self?.finish(.success(finalText))
        }
    }

    nonisolated func realTimeRecognizerDidError(
        _ recognizer: QCloudRealTimeRecognizer,
        result: QCloudRealTimeResult
    ) {
        let message = [result.clientErrMessage, result.message]
            .first { !$0.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }
            ?? "实时语音识别失败"
        Task { @MainActor [weak self] in
            self?.finish(.failure(SpeechRecognitionError.service(message)))
        }
    }

    nonisolated func realTimeRecognizerDidUpdateVolumeDB(
        _ recognizer: QCloudRealTimeRecognizer,
        volume: Float
    ) {
        let level: CGFloat
        if volume <= 0 {
            level = max(0, min(1, (CGFloat(volume) + 45) / 45))
        } else {
            level = max(0, min(1, CGFloat(volume) / 60))
        }
        Task { @MainActor [weak self] in
            guard let self else { return }
            self.meterLevel = self.meterLevel * 0.45 + level * 0.55
        }
    }

    nonisolated func realTimeRecognizerDidStartRecord(
        _ recognizer: QCloudRealTimeRecognizer,
        error: Error?
    ) {
        guard let error else { return }
        Task { @MainActor [weak self] in
            self?.finish(.failure(SpeechRecognitionError.service(error.localizedDescription)))
        }
    }

    private nonisolated func resultText(from result: QCloudRealTimeResult) -> String {
        let recognized = result.recognizedText.trimmingCharacters(in: .whitespacesAndNewlines)
        if !recognized.isEmpty { return recognized }
        return result.text.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private nonisolated func publish(_ text: String) {
        guard !text.isEmpty else { return }
        Task { @MainActor [weak self] in
            self?.liveText = text
        }
    }

    private func finish(_ result: Result<String, Error>) {
        let continuation = stopContinuation
        stopContinuation = nil
        stopTimeoutTask?.cancel()
        stopTimeoutTask = nil
        recognizer?.delegate = nil
        recognizer = nil
        isRecognizing = false
        meterLevel = 0

        switch result {
        case .success(let text):
            if !text.isEmpty {
                liveText = text
            }
            continuation?.resume(returning: text)
        case .failure(let error):
            continuation?.resume(throwing: error)
        }
    }

    private func cancelCurrentSession() {
        recognizer?.delegate = nil
        recognizer?.cancel()
        recognizer = nil
        isRecognizing = false
        meterLevel = 0
        stopTimeoutTask?.cancel()
        stopTimeoutTask = nil
        if let continuation = stopContinuation {
            stopContinuation = nil
            continuation.resume(throwing: CancellationError())
        }
    }

    private func requestRecordPermission() async -> Bool {
        await withCheckedContinuation { continuation in
            AVAudioSession.sharedInstance().requestRecordPermission { granted in
                continuation.resume(returning: granted)
            }
        }
    }
}
