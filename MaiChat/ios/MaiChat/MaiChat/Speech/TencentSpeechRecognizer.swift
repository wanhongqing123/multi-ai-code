// 文件作用：实现腾讯实时语音识别与麦克风采集，向输入界面持续返回转写结果。
// 阅读边界：这里只处理识别会话与回调；悬浮层的显示和发送由页面状态决定。
// 封装腾讯实时 ASR 会话、麦克风权限和音频缓冲。
import AVFoundation
import Combine
import Foundation
import MaiChatCore
@preconcurrency import QCloudRealTime

/// SDK 实例在共用音频队列上创建、控制和释放。
private final class SpeechEngineHandle: @unchecked Sendable {
    private final class Storage: @unchecked Sendable {
        var engine: QCloudRealTimeRecognizer?
        init(_ engine: QCloudRealTimeRecognizer) { self.engine = engine }
        func shutdown() {
            RemoteIMBackgroundWork.audioQueue.assertCurrent()
            engine?.delegate = nil
            engine?.cancel()
            engine = nil
        }
    }
    let id: ObjectIdentifier
    private let storage: Storage
    private let cancellation = RemoteIMBackgroundWork.Cancellation()
    private init(_ engine: QCloudRealTimeRecognizer) {
        id = ObjectIdentifier(engine); storage = Storage(engine)
    }
    deinit {
        let storage = storage
        RemoteIMBackgroundWork.audioQueue.async { storage.shutdown() }
    }
    static func make(credential: MaiChatASRCredential) async throws -> SpeechEngineHandle {
        try await RemoteIMBackgroundWork.audio {
            // SDK 直接向腾讯发送音频；STS token 必须与本轮临时 SecretId/SecretKey 一起使用。
            let config = QCloudConfig(appId: credential.appId, secretId: credential.secretId,
                                      secretKey: credential.secretKey, token: credential.token,
                                      projectId: 0)
            // 普通引擎使用官方 standard SDK；V2 二进制会强制 result_mod=1，普通引擎拒绝该模式。
            config.engineType = "16k_zh"
            config.enableDetectVolume = false
            config.endRecognizeWhenDetectSilence = false
            config.endRecognizeWhenDetectSilenceAutoStop = false
            config.needvad = 1
            config.filterPunc = 0; config.convertNumMode = 1; config.requestTimeout = 20
            return SpeechEngineHandle(QCloudRealTimeRecognizer(config: config))
        }
    }
    @MainActor
    /// 申请麦克风、激活音频会话并启动实时识别。
    /// 每次开始分配新的会话代次；旧识别器回调不得更新当前转写悬浮层。
    func start(delegate: TencentRealtimeSpeechRecognizer) async throws -> (began: TimeInterval, returned: TimeInterval)
    {
        let storage = storage, cancellation = cancellation
        return try await withCheckedThrowingContinuation {
            (continuation: CheckedContinuation<(TimeInterval, TimeInterval), Error>) in
            RemoteIMBackgroundWork.audioQueue.async {
                do {
                    try cancellation.check()
                    guard let engine = storage.engine else { throw CancellationError() }
                    engine.delegate = delegate
                    let began = ProcessInfo.processInfo.systemUptime
                    engine.start()
                    continuation.resume(returning: (began, ProcessInfo.processInfo.systemUptime))
                } catch { continuation.resume(throwing: error) }
            }
        }
    }
    func stop(completion: @escaping @Sendable (Int, TimeInterval) -> Void) {
        let storage = storage
        RemoteIMBackgroundWork.audioQueue.async {
            let began = ProcessInfo.processInfo.systemUptime
            storage.engine?.stop()
            let returned = ProcessInfo.processInfo.systemUptime
            completion(Int((returned - began) * 1000), returned)
        }
    }
    func cancel() {
        cancellation.cancel()
        let storage = storage
        RemoteIMBackgroundWork.audioQueue.async { storage.shutdown() }
    }
}

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
    // 录音途中失败时还没有 stop 等待者，必须主动通知输入栏恢复草稿和错误状态。
    var onFailure: (@MainActor (Error, UInt64) -> Void)?

    private var recognizer: SpeechEngineHandle?
    private var recognizerID: ObjectIdentifier?
    private var stopContinuation: CheckedContinuation<String, Error>?
    private var stopTimeoutTask: Task<Void, Never>?
    private var sessionSequence: UInt64 = 0
    private var sessionStartedUptime: TimeInterval?
    private var firstTextUptime: TimeInterval?
    private var firstCallbackUptime: TimeInterval?
    private var textUpdateCount = 0
    // 在会话开始时固定诊断字段，使迟到的 SDK 回调仍归属原会话。
    private var diagnosticFields: [String: String] = [:]
    private var stopRequestedUptime: TimeInterval?
    // 必须保持串行：取消与快速重启依赖 deactivate/activate 严格按入队顺序执行。
    private nonisolated static let audioSessionQueue = RemoteIMBackgroundWork.audioQueue

    var diagnosticSessionID: UInt64 { sessionSequence }

    /// 开始一轮实时语音识别：核验凭据和麦克风权限，关闭旧会话并建立本轮音频回调。
    /// 每次启动递增会话序号，使迟到的旧识别结果不能写入新悬浮层；
    /// 任一步失败都抛出错误交给输入界面收起悬浮层并保留原草稿。
    func start(
        diagnosticFields: [String: String],
        credentialProvider: @MainActor () async throws -> MaiChatASRCredential
    ) async throws {
        cancelCurrentSession()
        self.diagnosticFields = diagnosticFields
        stopRequestedUptime = nil
        sessionSequence &+= 1
        let startingSequence = sessionSequence
        // 权限和领取等待也属于本轮会话；取消、退出或新一轮长按后不得迟到启动录音。
        guard await requestRecordPermission() else {
            throw SpeechRecognitionError.service("没有麦克风权限")
        }
        try Task.checkCancellation()
        guard sessionSequence == startingSequence else { throw CancellationError() }
        // 授权弹窗可停留很久，凭据在授权结束后领取，避免用户返回时已经过期。
        let credential = try await credentialProvider()
        try Task.checkCancellation()
        guard sessionSequence == startingSequence else { throw CancellationError() }
        guard credential.isReusable() else { throw SpeechRecognitionError.unavailable }
        sessionStartedUptime = ProcessInfo.processInfo.systemUptime
        let audioSessionStartedAt = ProcessInfo.processInfo.systemUptime
        let audioTiming: AudioActivationTiming
        do {
            audioTiming = try await Self.activateAudioSession()
            try Task.checkCancellation()
            guard sessionSequence == startingSequence else { throw CancellationError() }
        } catch {
            if sessionSequence == startingSequence {
                Self.deactivateAudioSession()
                sessionStartedUptime = nil
            }
            throw error
        }
        let audioSessionReadyAt = ProcessInfo.processInfo.systemUptime

        let nextRecognizer: SpeechEngineHandle
        do {
            nextRecognizer = try await SpeechEngineHandle.make(credential: credential)
        } catch {
            if sessionSequence == startingSequence { Self.deactivateAudioSession() }
            throw error
        }
        do {
            try Task.checkCancellation()
            guard sessionSequence == startingSequence else { throw CancellationError() }
        } catch {
            nextRecognizer.cancel()
            if sessionSequence == startingSequence { Self.deactivateAudioSession() }
            throw error
        }
        recognizer = nextRecognizer
        recognizerID = nextRecognizer.id
        firstTextUptime = nil
        firstCallbackUptime = nil
        textUpdateCount = 0
        liveText = ""
        isRecognizing = true
        let sdkStartBeganAt = ProcessInfo.processInfo.systemUptime
        let timing: (began: TimeInterval, returned: TimeInterval)
        do {
            timing = try await nextRecognizer.start(delegate: self)
            try Task.checkCancellation()
            guard isCurrentRecognizer(nextRecognizer.id) else { throw CancellationError() }
        } catch {
            if recognizerID == nextRecognizer.id { cancelCurrentSession() }
            else { nextRecognizer.cancel() }
            throw error
        }
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
                    from: timing.began, to: timing.returned
                ),
                "sdk_start_queue_ms": Self.milliseconds(from: sdkStartBeganAt, to: timing.began),
                "sdk_start_resume_ms": Self.milliseconds(from: timing.returned, to: sdkStartReturnedAt),
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
            let stoppingID = recognizer.id
            let fields = diagnosticFields
            let session = sessionSequence
            recognizer.stop { duration, returnedAt in
                Task { @MainActor in
                    AppDiagnosticLog.shared.record(level: .info, category: "asr", event: "sdk-stop-returned",
                        fields: fields.merging(["duration_ms": String(duration), "session": String(session),
                                                "executor": "audio-queue",
                            "ui_resume_ms": Self.milliseconds(
                                from: returnedAt, to: ProcessInfo.processInfo.systemUptime),
                        ]) { _, value in value })
                }
            }
            stopTimeoutTask?.cancel()
            stopTimeoutTask = Task { [weak self] in
                try? await Task.sleep(for: .seconds(10))
                guard !Task.isCancelled, let self, self.stopContinuation != nil,
                      self.isCurrentRecognizer(stoppingID) else { return }
                // stop 后的中间态仍可能修正；超时不能把部分文字当最终成功发送。
                self.finish(.failure(SpeechRecognitionError.service("等待语音识别完成超时，请重试")),
                            sourceID: stoppingID, completion: "stop-timeout")
            }
        }
    }

    func cancel() {
        cancelCurrentSession()
        // 即使 SDK 尚未创建，也使正在等待临时凭据或系统授权的 start 失效。
        sessionSequence &+= 1
        liveText = ""
    }

    nonisolated func realTimeRecognizer(
        onSliceRecognize recognizer: QCloudRealTimeRecognizer,
        result: QCloudRealTimeResult
    ) {
        publish(resultText(from: result), source: "slice",
                recognizerID: ObjectIdentifier(recognizer),
                callbackUptime: ProcessInfo.processInfo.systemUptime,
                callbackOnMainThread: Thread.isMainThread)
    }

    nonisolated func realTimeRecognizer(
        onSegmentSuccessRecognize recognizer: QCloudRealTimeRecognizer,
        result: QCloudRealTimeResult
    ) {
        publish(resultText(from: result), source: "segment",
                recognizerID: ObjectIdentifier(recognizer),
                callbackUptime: ProcessInfo.processInfo.systemUptime,
                callbackOnMainThread: Thread.isMainThread)
    }

    nonisolated func realTimeRecognizerDidFinish(
        _ recognizer: QCloudRealTimeRecognizer,
        result: String
    ) {
        let sourceID = ObjectIdentifier(recognizer)
        Task { @MainActor [weak self] in
            guard let self, self.isCurrentRecognizer(sourceID) else { return }
            // 只有 SDK 明确完成才成功；空结束通知保留最后一份已收到的服务端全文。
            let finalText = result.trimmingCharacters(in: .whitespacesAndNewlines)
            self.finish(.success(finalText.isEmpty ? self.liveText : finalText),
                        sourceID: sourceID, completion: "sdk-finished")
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

    /// standard SDK 在 recognizedText 内合并稳定句与当前句，宿主直接替换全文，避免重复追加。
    private nonisolated func resultText(from result: QCloudRealTimeResult) -> String {
        let fullText = result.recognizedText.trimmingCharacters(in: .whitespacesAndNewlines)
        return fullText.isEmpty
            ? result.text.trimmingCharacters(in: .whitespacesAndNewlines) : fullText
    }

    // callbackUptime 是进入代理回调的时刻，不是网络接收时刻；
    // 若回调在主线程，SDK 内部排队时间已包含在内，需结合前台 run-loop 采样判断。
    private nonisolated func publish(
        _ text: String,
        source: String,
        recognizerID: ObjectIdentifier,
        callbackUptime: TimeInterval,
        callbackOnMainThread: Bool
    ) {
        let enqueuedUptime = ProcessInfo.processInfo.systemUptime
        Task { @MainActor [weak self] in
            guard let self, self.isCurrentRecognizer(recognizerID) else { return }
            let processedUptime = ProcessInfo.processInfo.systemUptime
            if self.firstCallbackUptime == nil {
                self.firstCallbackUptime = callbackUptime
                self.log(level: .info, event: "first-callback", fields: [
                    "session": String(self.sessionSequence),
                    "source": source,
                    "characters": String(text.count),
                    "callback_latency_ms": self.elapsedMilliseconds(at: callbackUptime),
                    "main_actor_wait_ms": Self.milliseconds(from: enqueuedUptime,
                                                               to: processedUptime),
                ])
            }
            AppDiagnosticLog.shared.recordDuration(
                .asrMainActorWait, since: enqueuedUptime, until: processedUptime
            )
            // 静音空回包不是全文清空指令，保留已有稳定句；结束由 DidFinish 单独确认。
            guard !text.isEmpty, self.liveText != text else { return }
            self.textUpdateCount += 1
            if !text.isEmpty, self.firstTextUptime == nil {
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
                        "source": source,
                    ]
                )
            }
            self.liveText = text
            self.onLiveTextUpdate?(text, self.sessionSequence)
        }
    }

    /// 结束当前识别会话并完成等待中的转写结果。
    /// 只回收匹配会话的资源；成功只来自 SDK 明确完成，超时返回错误。
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
            if continuation == nil { onFailure?(error, sessionSequence) }
        }
    }

    private func cancelCurrentSession() {
        let wasActive = recognizer != nil || isRecognizing
        recognizer?.cancel()
        recognizer = nil
        recognizerID = nil
        isRecognizing = false
        stopTimeoutTask?.cancel()
        stopTimeoutTask = nil
        // 空闲时取消不能在下一次激活前排入 setActive(false)，
        // 否则会干扰应用内其他正在使用音频会话的功能。
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
