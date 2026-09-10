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
    // Frozen at session start so delayed SDK callbacks retain their original conversation.
    private var diagnosticFields: [String: String] = [:]
    private var stopRequestedUptime: TimeInterval?
    // 必须保持串行：取消与快速重启依赖 deactivate/activate 严格按入队顺序执行。
    private nonisolated static let audioSessionQueue = DispatchQueue(
        label: "com.kongshang.maichat.asr-audio-session",
        qos: .userInitiated
    )

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

    func start(diagnosticFields: [String: String]) async throws {
        guard isAvailable else { throw SpeechRecognitionError.unavailable }
        guard await requestRecordPermission() else {
            throw SpeechRecognitionError.service("没有麦克风权限")
        }
        try Task.checkCancellation()

        cancelCurrentSession()
        self.diagnosticFields = diagnosticFields
        stopRequestedUptime = nil
        sessionSequence &+= 1
        sessionStartedUptime = ProcessInfo.processInfo.systemUptime
        let audioSessionStartedAt = ProcessInfo.processInfo.systemUptime
        let audioTiming: AudioActivationTiming
        do {
            audioTiming = try await Self.activateAudioSession()
            try Task.checkCancellation()
        } catch {
            Self.deactivateAudioSession()
            sessionStartedUptime = nil
            throw error
        }
        let audioSessionReadyAt = ProcessInfo.processInfo.systemUptime

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
        firstTextUptime = nil
        textUpdateCount = 0
        liveText = ""
        isRecognizing = true
        let sdkStartBeganAt = ProcessInfo.processInfo.systemUptime
        nextRecognizer.start()
        let sdkStartReturnedAt = ProcessInfo.processInfo.systemUptime
        log(
            level: .info,
            event: "session-started",
            fields: [
                "session": String(sessionSequence),
                "audio_session_ms": Self.milliseconds(
                    from: audioSessionStartedAt,
                    to: audioSessionReadyAt
                ),
                "audio_queue_ms": Self.milliseconds(from: audioSessionStartedAt, to: audioTiming.began),
                "audio_category_ms": Self.milliseconds(from: audioTiming.began, to: audioTiming.configured),
                "audio_activate_ms": Self.milliseconds(from: audioTiming.configured, to: audioTiming.activated),
                "audio_resume_ms": Self.milliseconds(from: audioTiming.activated, to: audioSessionReadyAt),
                "sdk_start_call_ms": Self.milliseconds(
                    from: sdkStartBeganAt,
                    to: sdkStartReturnedAt
                ),
            ]
        )
    }

    func stop() async throws -> String {
        guard let recognizer, isRecognizing else {
            throw SpeechRecognitionError.service("实时语音识别尚未启动")
        }
        return try await withCheckedThrowingContinuation { continuation in
            stopContinuation = continuation
            stopRequestedUptime = ProcessInfo.processInfo.systemUptime
            log(
                level: .info,
                event: "stop-requested",
                fields: [
                    "session": String(sessionSequence),
                    "elapsed_ms": elapsedMilliseconds(),
                ]
            )
            let stopCallStarted = ProcessInfo.processInfo.systemUptime
            recognizer.stop()
            log(level: .info, event: "sdk-stop-returned", fields: [
                "duration_ms": Self.milliseconds(from: stopCallStarted, to: ProcessInfo.processInfo.systemUptime)
            ])
            stopTimeoutTask?.cancel()
            stopTimeoutTask = Task { [weak self] in
                try? await Task.sleep(for: .seconds(1))
                guard !Task.isCancelled, let self else { return }
                // 个别 SDK/网络状态下 stop 后不会回调 DidFinish。屏幕上的实时文本已经
                // 是服务端返回的识别结果，用它兜底，避免界面无限停在“正在完成识别”。
                self.recognizer?.delegate = nil
                self.recognizer?.cancel()
                self.finish(.success(self.liveText), completion: "stop-timeout-fallback")
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
        let callbackUptime = ProcessInfo.processInfo.systemUptime
        publish(
            resultText(from: result),
            recognizerID: ObjectIdentifier(recognizer),
            callbackUptime: callbackUptime,
            callbackOnMainThread: Thread.isMainThread
        )
    }

    nonisolated func realTimeRecognizer(
        onSegmentSuccessRecognize recognizer: QCloudRealTimeRecognizer,
        result: QCloudRealTimeResult
    ) {
        let callbackUptime = ProcessInfo.processInfo.systemUptime
        publish(
            resultText(from: result),
            recognizerID: ObjectIdentifier(recognizer),
            callbackUptime: callbackUptime,
            callbackOnMainThread: Thread.isMainThread
        )
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
        let callbackUptime = ProcessInfo.processInfo.systemUptime
        let callbackOnMainThread = Thread.isMainThread
        let sourceID = ObjectIdentifier(recognizer)
        Task { @MainActor [weak self] in
            guard let self, self.isCurrentRecognizer(sourceID) else { return }
            guard let error else {
                self.log(
                    level: .info,
                    event: "recording-started",
                    fields: [
                        "session": String(self.sessionSequence),
                        "callback_latency_ms": self.elapsedMilliseconds(at: callbackUptime),
                        "main_actor_wait_ms": Self.milliseconds(
                            from: callbackUptime, to: ProcessInfo.processInfo.systemUptime
                        ),
                        "callback_on_main": String(callbackOnMainThread),
                    ]
                )
                return
            }
            self.finish(
                .failure(SpeechRecognitionError.service(error.localizedDescription)),
                sourceID: sourceID
            )
        }
    }

    nonisolated func realTimeRecognizer(
        onFlowRecognizeStart recognizer: QCloudRealTimeRecognizer, voiceId: String, seq: Int
    ) {
        recordSDKStage("flow-started", recognizer: recognizer)
    }

    nonisolated func realTimeRecognizerDidStopRecord(_ recognizer: QCloudRealTimeRecognizer) {
        recordSDKStage("recording-stopped", recognizer: recognizer)
    }

    private nonisolated func recordSDKStage(
        _ event: String, recognizer: QCloudRealTimeRecognizer
    ) {
        let callbackUptime = ProcessInfo.processInfo.systemUptime
        let callbackOnMain = Thread.isMainThread
        let sourceID = ObjectIdentifier(recognizer)
        Task { @MainActor [weak self] in
            guard let self, self.isCurrentRecognizer(sourceID) else { return }
            self.log(level: .info, event: event, fields: [
                "callback_latency_ms": self.elapsedMilliseconds(at: callbackUptime),
                "callback_on_main": String(callbackOnMain),
                "main_actor_wait_ms": Self.milliseconds(
                    from: callbackUptime, to: ProcessInfo.processInfo.systemUptime
                ),
                "session": String(self.sessionSequence),
            ])
        }
    }

    private nonisolated func resultText(from result: QCloudRealTimeResult) -> String {
        let recognized = result.recognizedText.trimmingCharacters(in: .whitespacesAndNewlines)
        if !recognized.isEmpty { return recognized }
        return result.text.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    // callbackUptime is delegate entry, not a network receive timestamp. When
    // callbackOnMainThread is true, SDK-internal main-queue delay is already
    // included; interpret it alongside the foreground run-loop probe.
    private nonisolated func publish(
        _ text: String,
        recognizerID: ObjectIdentifier,
        callbackUptime: TimeInterval,
        callbackOnMainThread: Bool
    ) {
        guard !text.isEmpty else { return }
        let enqueuedUptime = ProcessInfo.processInfo.systemUptime
        Task { @MainActor [weak self] in
            guard let self, self.isCurrentRecognizer(recognizerID) else { return }
            let processedUptime = ProcessInfo.processInfo.systemUptime
            AppDiagnosticLog.shared.recordDuration(
                .asrMainActorWait, since: enqueuedUptime, until: processedUptime
            )
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
                        "callback_latency_ms": self.elapsedMilliseconds(at: callbackUptime),
                        "callback_on_main": String(callbackOnMainThread),
                        "callback_extract_ms": Self.milliseconds(
                            from: callbackUptime, to: enqueuedUptime
                        ),
                        "main_actor_wait_ms": Self.milliseconds(
                            from: enqueuedUptime, to: processedUptime
                        ),
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
        sourceID: ObjectIdentifier? = nil,
        completion: String = "sdk-callback"
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
        Self.deactivateAudioSession()

        let resultName: String
        var resultFields: [String: String] = [:]
        switch result {
        case .success: resultName = "ok"
        case .failure(let error):
            resultName = "failed"
            resultFields["code"] = String((error as NSError).code)
        }
        log(
            level: resultName == "ok" ? .info : .warning,
            event: "session-finished",
            fields: [
                "session": String(sessionSequence),
                "result": resultName,
                "completion": completion,
                "stop_wait_ms": stopRequestedUptime.map { Self.milliseconds(
                    from: $0, to: ProcessInfo.processInfo.systemUptime
                ) } ?? "0",
                "duration_ms": elapsedMilliseconds(),
                "text_updates": String(textUpdateCount),
            ].merging(resultFields) { _, result in result }
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
        // An idle cancel must not enqueue setActive(false) ahead of the next
        // activation, or interfere with another audio feature in the app.
        if wasActive || sessionStartedUptime != nil {
            Self.deactivateAudioSession()
        }
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

    private struct AudioActivationTiming: Sendable {
        let began: TimeInterval
        let configured: TimeInterval
        let activated: TimeInterval
    }

    private nonisolated static func activateAudioSession() async throws -> AudioActivationTiming {
        try await withCheckedThrowingContinuation {
            (continuation: CheckedContinuation<AudioActivationTiming, Error>) in
            audioSessionQueue.async {
                do {
                    let began = ProcessInfo.processInfo.systemUptime
                    let audioSession = AVAudioSession.sharedInstance()
                    try audioSession.setCategory(.record, mode: .measurement)
                    let configured = ProcessInfo.processInfo.systemUptime
                    try audioSession.setActive(true)
                    continuation.resume(returning: AudioActivationTiming(
                        began: began, configured: configured,
                        activated: ProcessInfo.processInfo.systemUptime
                    ))
                } catch {
                    continuation.resume(throwing: error)
                }
            }
        }
    }

    private nonisolated static func deactivateAudioSession() {
        audioSessionQueue.async {
            try? AVAudioSession.sharedInstance().setActive(
                false,
                options: .notifyOthersOnDeactivation
            )
        }
    }

    private nonisolated static func milliseconds(
        from start: TimeInterval,
        to end: TimeInterval
    ) -> String {
        String(Int((max(0, end - start) * 1_000).rounded()))
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
            fields: fields.merging(diagnosticFields) { _, context in context }
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
