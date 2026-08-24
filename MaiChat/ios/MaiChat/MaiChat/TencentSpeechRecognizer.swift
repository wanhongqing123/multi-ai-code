import AVFoundation
import Combine
import Foundation
import MaiChatCore
@preconcurrency import QCloudRealTime

/// 腾讯云实时语音识别。
///
/// 长按期间持续发布非稳态识别文本，松手后等待服务端的最终文本；SDK 使用内置录音器，
/// 与「直接发送语音」所用的 AVAudioRecorder 相互独立。
@MainActor
final class TencentRealtimeSpeechRecognizer: NSObject, ObservableObject,
    QCloudRealTimeRecognizerDelegate {

    @Published private(set) var isRecognizing = false
    private(set) var liveText = ""
    var onLiveTextUpdate: (@MainActor (String, UInt64) -> Void)?

    private let appId: String
    private let secretId: String
    private let secretKey: String
    private var recognizer: QCloudRealTimeRecognizer?
    private var recognizerID: ObjectIdentifier?
    private var stopContinuation: CheckedContinuation<String, Error>?
    private var stopTimeoutTask: Task<Void, Never>?
    private var sessionSequence: UInt64 = 0
    private var sessionStartedUptime: TimeInterval?
    private var firstTextUptime: TimeInterval?
    private var textUpdateCount = 0

    init(appId: String, secretId: String, secretKey: String) {
        self.appId = appId.trimmingCharacters(in: .whitespacesAndNewlines)
        self.secretId = secretId.trimmingCharacters(in: .whitespacesAndNewlines)
        self.secretKey = secretKey.trimmingCharacters(in: .whitespacesAndNewlines)
        super.init()
    }

    var isAvailable: Bool {
        !appId.isEmpty && !secretId.isEmpty && !secretKey.isEmpty
    }

    var diagnosticSessionID: UInt64 { sessionSequence }

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
        config.enableDetectVolume = false
        config.endRecognizeWhenDetectSilence = false
        config.endRecognizeWhenDetectSilenceAutoStop = false
        config.needvad = 1
        config.filterPunc = 0
        config.convertNumMode = 1
        config.requestTimeout = 20

        let nextRecognizer = QCloudRealTimeRecognizer(config: config)
        nextRecognizer.delegate = self
        recognizer = nextRecognizer
        recognizerID = ObjectIdentifier(nextRecognizer)
        sessionSequence &+= 1
        sessionStartedUptime = ProcessInfo.processInfo.systemUptime
        firstTextUptime = nil
        textUpdateCount = 0
        liveText = ""
        isRecognizing = true
        log(
            level: .info,
            event: "session-started",
            fields: ["session": String(sessionSequence)]
        )
        nextRecognizer.start()
    }

    func stop() async throws -> String {
        guard let recognizer, isRecognizing else {
            throw SpeechRecognitionError.service("实时语音识别尚未启动")
        }
        return try await withCheckedThrowingContinuation { continuation in
            stopContinuation = continuation
            log(
                level: .info,
                event: "stop-requested",
                fields: [
                    "session": String(sessionSequence),
                    "elapsed_ms": elapsedMilliseconds(),
                ]
            )
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
    }

    nonisolated func realTimeRecognizer(
        onSliceRecognize recognizer: QCloudRealTimeRecognizer,
        result: QCloudRealTimeResult
    ) {
        publish(resultText(from: result), recognizerID: ObjectIdentifier(recognizer))
    }

    nonisolated func realTimeRecognizer(
        onSegmentSuccessRecognize recognizer: QCloudRealTimeRecognizer,
        result: QCloudRealTimeResult
    ) {
        publish(resultText(from: result), recognizerID: ObjectIdentifier(recognizer))
    }

    nonisolated func realTimeRecognizerDidFinish(
        _ recognizer: QCloudRealTimeRecognizer,
        result: String
    ) {
        let finalText = result.trimmingCharacters(in: .whitespacesAndNewlines)
        let sourceID = ObjectIdentifier(recognizer)
        Task { @MainActor [weak self] in
            self?.finish(.success(finalText), sourceID: sourceID)
        }
    }

    nonisolated func realTimeRecognizerDidError(
        _ recognizer: QCloudRealTimeRecognizer,
        result: QCloudRealTimeResult
    ) {
        let message = [result.clientErrMessage, result.message]
            .first { !$0.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }
            ?? "实时语音识别失败"
        let sourceID = ObjectIdentifier(recognizer)
        Task { @MainActor [weak self] in
            self?.finish(
                .failure(SpeechRecognitionError.service(message)),
                sourceID: sourceID
            )
        }
    }

    nonisolated func realTimeRecognizerDidStartRecord(
        _ recognizer: QCloudRealTimeRecognizer,
        error: Error?
    ) {
        let sourceID = ObjectIdentifier(recognizer)
        Task { @MainActor [weak self] in
            guard let self, self.isCurrentRecognizer(sourceID) else { return }
            guard let error else {
                self.log(
                    level: .info,
                    event: "recording-started",
                    fields: ["session": String(self.sessionSequence)]
                )
                return
            }
            self.finish(
                .failure(SpeechRecognitionError.service(error.localizedDescription)),
                sourceID: sourceID
            )
        }
    }

    private nonisolated func resultText(from result: QCloudRealTimeResult) -> String {
        let recognized = result.recognizedText.trimmingCharacters(in: .whitespacesAndNewlines)
        if !recognized.isEmpty { return recognized }
        return result.text.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private nonisolated func publish(_ text: String, recognizerID: ObjectIdentifier) {
        guard !text.isEmpty else { return }
        Task { @MainActor [weak self] in
            guard let self, self.isCurrentRecognizer(recognizerID) else { return }
            guard self.liveText != text else { return }
            self.textUpdateCount += 1
            if self.firstTextUptime == nil {
                let now = ProcessInfo.processInfo.systemUptime
                self.firstTextUptime = now
                self.log(
                    level: .info,
                    event: "first-text-received",
                    fields: [
                        "session": String(self.sessionSequence),
                        "latency_ms": self.elapsedMilliseconds(at: now),
                        "characters": String(text.count),
                    ]
                )
            }
            self.liveText = text
            self.onLiveTextUpdate?(text, self.sessionSequence)
        }
    }

    private func finish(
        _ result: Result<String, Error>,
        sourceID: ObjectIdentifier? = nil
    ) {
        if let sourceID, !isCurrentRecognizer(sourceID) { return }
        let continuation = stopContinuation
        stopContinuation = nil
        stopTimeoutTask?.cancel()
        stopTimeoutTask = nil
        recognizer?.delegate = nil
        recognizer?.cancel()
        recognizer = nil
        recognizerID = nil
        isRecognizing = false
        deactivateAudioSession()

        let resultName: String
        switch result {
        case .success: resultName = "ok"
        case .failure: resultName = "failed"
        }
        log(
            level: resultName == "ok" ? .info : .warning,
            event: "session-finished",
            fields: [
                "session": String(sessionSequence),
                "result": resultName,
                "duration_ms": elapsedMilliseconds(),
                "text_updates": String(textUpdateCount),
            ]
        )
        sessionStartedUptime = nil

        switch result {
        case .success(let text):
            if !text.isEmpty {
                liveText = text
                onLiveTextUpdate?(text, sessionSequence)
            }
            continuation?.resume(returning: text)
        case .failure(let error):
            continuation?.resume(throwing: error)
        }
    }

    private func cancelCurrentSession() {
        let wasActive = recognizer != nil || isRecognizing
        recognizer?.delegate = nil
        recognizer?.cancel()
        recognizer = nil
        recognizerID = nil
        isRecognizing = false
        stopTimeoutTask?.cancel()
        stopTimeoutTask = nil
        deactivateAudioSession()
        if let continuation = stopContinuation {
            stopContinuation = nil
            continuation.resume(throwing: CancellationError())
        }
        if wasActive {
            log(
                level: .info,
                event: "session-cancelled",
                fields: [
                    "session": String(sessionSequence),
                    "duration_ms": elapsedMilliseconds(),
                    "text_updates": String(textUpdateCount),
                ]
            )
        }
        sessionStartedUptime = nil
    }

    private func isCurrentRecognizer(_ sourceID: ObjectIdentifier) -> Bool {
        isRecognizing && recognizerID == sourceID
    }

    private func elapsedMilliseconds(
        at uptime: TimeInterval = ProcessInfo.processInfo.systemUptime
    ) -> String {
        guard let sessionStartedUptime else { return "0" }
        return String(Int((max(0, uptime - sessionStartedUptime) * 1_000).rounded()))
    }

    private func deactivateAudioSession() {
        try? AVAudioSession.sharedInstance().setActive(
            false,
            options: .notifyOthersOnDeactivation
        )
    }

    private func log(
        level: DiagnosticLogLevel,
        event: String,
        fields: [String: String]
    ) {
        AppDiagnosticLog.shared.record(
            level: level,
            category: "asr",
            event: event,
            fields: fields
        )
    }

    private func requestRecordPermission() async -> Bool {
        await withCheckedContinuation { continuation in
            AVAudioSession.sharedInstance().requestRecordPermission { granted in
                continuation.resume(returning: granted)
            }
        }
    }
}
