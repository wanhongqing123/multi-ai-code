// 文件作用：用阿里官方 NUI SDK 实现实时转文字，麦克风 PCM 与 SDK 控制留在音频工作线程。
// 账号只下发短期 NLS token；事件回调不调用 SDK 控制接口、不记录凭据或音频。
@preconcurrency import AVFoundation
import Combine
import Foundation

struct AliyunSpeechEvent: Sendable {
    let json: String?
    let sentenceFinal: Bool
    let completed: Bool
    let error: String?
}

/// 测试可注入不联网的引擎，复用正式 start/stop/cancel 和代际检查。
protocol AliyunSpeechControlling: AnyObject, Sendable {
    func start() throws
    func stop() throws
    func close()
}

typealias AliyunSpeechEventHandler = @Sendable (AliyunSpeechEvent) -> Void
typealias AliyunSpeechEngineFactory = @Sendable (
    MaiChatAliyunASRCredential, @escaping AliyunSpeechEventHandler
) throws -> any AliyunSpeechControlling

/// 若创建期间取消使返回值被工作队列丢弃，仍把 native 资源交回音频线程释放。
private struct AliyunSpeechCleanup: @unchecked Sendable {
    let nui: NeoNui?
    let recorder: AVAudioEngine?
    let tapInstalled: Bool
    let audioActivated: Bool
    let pcm: AliyunPCMBuffer

    func release() {
        recorder?.stop()
        if tapInstalled { recorder?.inputNode.removeTap(onBus: 0) }
        pcm.clear()
        nui?.delegate = nil
        _ = nui?.nui_dialog_cancel(true)
        _ = nui?.nui_release()
        if audioActivated {
            try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
        }
    }
}

/// 所有可变 SDK/AVAudioEngine 状态只在生产 audioQueue 上访问；delegate 与 tap 仅提交事件。
private final class AliyunSpeechEngine: NSObject, @unchecked Sendable, NeoNuiSdkDelegate, AliyunSpeechControlling {
    private var nui: NeoNui?
    private var audioEngine: AVAudioEngine?
    private var tapInstalled = false
    private var audioActivated = false
    private var closed = false
    private var stopping = false
    private let pcm = AliyunPCMBuffer()
    private let event: @Sendable (AliyunSpeechEvent) -> Void

    deinit {
        if !closed {
            let cleanup = AliyunSpeechCleanup(nui: nui, recorder: audioEngine,
                tapInstalled: tapInstalled, audioActivated: audioActivated, pcm: pcm)
            RemoteIMBackgroundWork.audioQueue.async { cleanup.release() }
        }
    }

    init(credential: MaiChatAliyunASRCredential,
         event: @escaping @Sendable (AliyunSpeechEvent) -> Void) throws {
        self.event = event
        super.init()
        RemoteIMBackgroundWork.audioQueue.assertCurrent()
        let sdk = NeoNui()
        nui = sdk
        sdk.delegate = self
        do {
            // 初始化不激活麦克风；迟到的创建结果取消时不会停用下一提供商的音频会话。
            let parameters = try Self.json([
                "app_key": credential.appKey, "token": credential.token,
                "url": "wss://nls-gateway-cn-shanghai.aliyuncs.com/ws/v1",
                "device_id": UUID().uuidString, "service_mode": "1"])
            try Self.check(parameters.withCString {
                sdk.nui_initialize($0, logLevel: NUI_LOG_LEVEL_NONE, saveLog: false)
            }, action: "初始化")
            let options = try Self.json([
                "service_type": 4,
                "nls_config": ["sample_rate": 16_000, "sr_format": "opus",
                               "enable_intermediate_result": true,
                               "enable_punctuation_prediction": true,
                               "enable_inverse_text_normalization": true]])
            try Self.check(options.withCString { sdk.nui_set_params($0) }, action: "配置")
        } catch {
            sdk.delegate = nil
            _ = sdk.nui_release()
            nui = nil
            throw error
        }
    }

    func start() throws {
        RemoteIMBackgroundWork.audioQueue.assertCurrent()
        guard !closed, let nui else { throw CancellationError() }
        let session = AVAudioSession.sharedInstance()
        try session.setCategory(.record, mode: .measurement)
        try session.setActive(true)
        audioActivated = true
        // 按住开始后先准备 PCM，避免网络建连完成才冷启动麦克风丢掉开头的语音。
        try startCapture()
        try Self.check(nui.nui_dialog_start(MODE_P2T, dialogParam: "{}"), action: "启动")
    }

    func stop() throws {
        RemoteIMBackgroundWork.audioQueue.assertCurrent()
        guard !closed, let nui else { throw CancellationError() }
        stopping = true
        stopCapture()
        // force=false 要求服务端正常完成；不把 stop 的同步返回当作最终识别结果。
        try Self.check(nui.nui_dialog_cancel(false), action: "结束")
    }

    func close() {
        RemoteIMBackgroundWork.audioQueue.assertCurrent()
        guard !closed else { return }
        closed = true
        stopping = true
        stopCapture()
        pcm.clear()
        if let nui {
            nui.delegate = nil
            _ = nui.nui_dialog_cancel(true)
            _ = nui.nui_release()
            self.nui = nil
        }
        if audioActivated {
            try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
            audioActivated = false
        }
    }

    private func startCapture() throws {
        RemoteIMBackgroundWork.audioQueue.assertCurrent()
        guard !closed, !stopping, audioActivated, audioEngine == nil else { return }
        let engine = AVAudioEngine()
        let input = engine.inputNode
        let sourceFormat = input.outputFormat(forBus: 0)
        guard sourceFormat.sampleRate > 0, sourceFormat.channelCount > 0,
              let target = AVAudioFormat(commonFormat: .pcmFormatInt16, sampleRate: 16_000,
                                        channels: 1, interleaved: true),
              let converter = AVAudioConverter(from: sourceFormat, to: target) else {
            throw SpeechRecognitionError.service("无法创建阿里云语音录音格式")
        }
        let pcm = pcm, event = event
        pcm.beginInput()
        input.installTap(onBus: 0, bufferSize: 1024, format: sourceFormat) { buffer, _ in
            // 硬件常为 44.1/48kHz Float；显式转换，不能把 AAC 或硬件原始帧当作 NUI PCM。
            let capacity = AVAudioFrameCount(ceil(Double(buffer.frameLength) * 16_000 / sourceFormat.sampleRate)) + 32
            guard let output = AVAudioPCMBuffer(pcmFormat: target, frameCapacity: capacity) else { return }
            var supplied = false
            var conversionError: NSError?
            let status = converter.convert(to: output, error: &conversionError) { _, state in
                if supplied { state.pointee = .noDataNow; return nil }
                supplied = true
                state.pointee = .haveData
                return buffer
            }
            guard status != .error, conversionError == nil else {
                event(AliyunSpeechEvent(json: nil, sentenceFinal: false, completed: false,
                                       error: "阿里云语音录音转换失败"))
                return
            }
            guard output.frameLength > 0, let pointer = output.audioBufferList.pointee.mBuffers.mData else { return }
            if !pcm.append(Data(bytes: pointer, count: Int(output.frameLength) * 2)) {
                event(AliyunSpeechEvent(json: nil, sentenceFinal: false, completed: false,
                                       error: "阿里云语音供音超时，请重试"))
            }
        }
        audioEngine = engine
        tapInstalled = true
        engine.prepare()
        do { try engine.start() }
        catch { stopCapture(); throw error }
    }

    private func stopCapture() {
        pcm.stopInput()
        audioEngine?.stop()
        if tapInstalled { audioEngine?.inputNode.removeTap(onBus: 0); tapInstalled = false }
        audioEngine = nil
    }

    func onNuiNeedAudioData(_ audioData: UnsafeMutablePointer<CChar>!, length: Int32) -> Int32 {
        guard let audioData, length > 0 else { return 0 }
        return pcm.read(into: audioData, capacity: Int(length))
    }

    func onNuiAudioStateChanged(_ state: NuiAudioState) {
        // SDK 回调不可同步调用 SDK/释放；音频启停也排到现有串行工作线程。
        RemoteIMBackgroundWork.audioQueue.async { [weak self] in
            guard let self, !self.closed else { return }
            do {
                if state == STATE_OPEN { try self.startCapture() }
                else { self.stopCapture() }
            } catch {
                self.event(AliyunSpeechEvent(json: nil, sentenceFinal: false, completed: false,
                                            error: "阿里云语音录音启动失败"))
            }
        }
    }

    func onNuiEventCallback(_ nuiEvent: NuiCallbackEvent, dialog: Int,
                           kwsResult: UnsafePointer<CChar>!, asrResult: UnsafePointer<CChar>!,
                           ifFinish: Bool, retCode: Int32) {
        if nuiEvent == EVENT_ASR_ERROR || nuiEvent == EVENT_DIALOG_ERROR || nuiEvent == EVENT_MIC_ERROR {
            event(AliyunSpeechEvent(json: nil, sentenceFinal: false, completed: false,
                                   error: "阿里云语音识别失败（\(retCode)）"))
            return
        }
        let hasText = nuiEvent == EVENT_ASR_PARTIAL_RESULT || nuiEvent == EVENT_SENTENCE_END
        let json = hasText ? asrResult.map { String(cString: $0) } : nil
        if hasText || ifFinish || nuiEvent == EVENT_TRANSCRIBER_COMPLETE {
            event(AliyunSpeechEvent(json: json, sentenceFinal: nuiEvent != EVENT_ASR_PARTIAL_RESULT,
                                   completed: ifFinish || nuiEvent == EVENT_TRANSCRIBER_COMPLETE, error: nil))
        }
    }

    private static func json(_ value: [String: Any]) throws -> String {
        String(decoding: try JSONSerialization.data(withJSONObject: value), as: UTF8.self)
    }

    private static func check(_ code: NuiResultCode, action: String) throws {
        guard code == 0 else {
            throw SpeechRecognitionError.service("阿里云语音\(action)失败（\(code)）")
        }
    }
}

/// 与腾讯识别器相同的会话契约；输入栏继续共用长按浮层、编辑/发送和草稿状态。
@MainActor
final class AliyunRealtimeSpeechRecognizer: NSObject, ObservableObject {
    @Published private(set) var isRecognizing = false
    private(set) var liveText = ""
    var onLiveTextUpdate: (@MainActor (String, UInt64) -> Void)?
    var onFailure: (@MainActor (Error, UInt64) -> Void)?
    private var engine: (any AliyunSpeechControlling)?
    private let makeEngine: AliyunSpeechEngineFactory
    private let recordPermission: @MainActor () async -> Bool
    private var generation: UInt64 = 0
    private var transcript = AliyunSpeechTranscript()
    private var stopContinuation: CheckedContinuation<String, Error>?
    private var stopTimeout: Task<Void, Never>?
    var diagnosticSessionID: UInt64 { generation }

    override init() {
        makeEngine = { credential, handler in
            try AliyunSpeechEngine(credential: credential, event: handler)
        }
        recordPermission = {
            await withCheckedContinuation { continuation in
                AVAudioSession.sharedInstance().requestRecordPermission {
                    continuation.resume(returning: $0)
                }
            }
        }
        super.init()
    }

    /// 仅替换录音/供应商适配与授权查询；正式会话、结果处理和取消逻辑仍走同一实现。
    init(engineFactory: @escaping AliyunSpeechEngineFactory,
         permissionProvider: @escaping @MainActor () async -> Bool) {
        makeEngine = engineFactory
        recordPermission = permissionProvider
        super.init()
    }

    deinit {
        let engine = engine
        RemoteIMBackgroundWork.audioQueue.async { engine?.close() }
    }

    func start(diagnosticFields: [String: String],
               credentialProvider: @MainActor () async throws -> MaiChatAliyunASRCredential) async throws {
        cancel()
        let sequence = generation
        let permitted = await recordPermission()
        try Task.checkCancellation()
        guard generation == sequence else { throw CancellationError() }
        guard permitted else { throw SpeechRecognitionError.service("没有麦克风权限") }
        let credential = try await credentialProvider()
        try Task.checkCancellation()
        guard generation == sequence else { throw CancellationError() }
        guard credential.isReusable() else { throw SpeechRecognitionError.unavailable }
        let handler: @Sendable (AliyunSpeechEvent) -> Void = { [weak self] event in
            Task { @MainActor [weak self] in self?.receive(event, sequence: sequence) }
        }
        let factory = makeEngine
        let next = try await RemoteIMBackgroundWork.audio { try factory(credential, handler) }
        guard generation == sequence, !Task.isCancelled else {
            RemoteIMBackgroundWork.audioQueue.async { next.close() }
            throw CancellationError()
        }
        engine = next
        transcript = AliyunSpeechTranscript()
        liveText = ""
        isRecognizing = true
        do {
            try await RemoteIMBackgroundWork.audio { try next.start() }
            guard generation == sequence, isRecognizing else { throw CancellationError() }
        } catch {
            if generation == sequence { cancel() }
            else { RemoteIMBackgroundWork.audioQueue.async { next.close() } }
            throw error
        }
    }

    func stop() async throws -> String {
        guard let engine, isRecognizing else {
            throw SpeechRecognitionError.service("实时语音识别尚未启动")
        }
        guard stopContinuation == nil else {
            throw SpeechRecognitionError.service("语音识别正在完成，请稍候")
        }
        let sequence = generation
        return try await withCheckedThrowingContinuation { continuation in
            stopContinuation = continuation
            RemoteIMBackgroundWork.audioQueue.async { [weak self] in
                do { try engine.stop() }
                catch {
                    Task { @MainActor [weak self] in
                        guard let self, self.generation == sequence else { return }
                        self.finish(.failure(error))
                    }
                }
            }
            stopTimeout?.cancel()
            stopTimeout = Task { [weak self] in
                try? await Task.sleep(for: .seconds(10))
                guard !Task.isCancelled, let self, self.generation == sequence,
                      self.stopContinuation != nil else { return }
                self.finish(.failure(SpeechRecognitionError.service("等待阿里云语音识别完成超时，请重试")))
            }
        }
    }

    func cancel() {
        generation &+= 1
        stopTimeout?.cancel(); stopTimeout = nil
        let continuation = stopContinuation; stopContinuation = nil
        let previous = engine; engine = nil
        isRecognizing = false
        liveText = ""
        transcript.cancel()
        RemoteIMBackgroundWork.audioQueue.async { previous?.close() }
        continuation?.resume(throwing: CancellationError())
    }

    private func receive(_ event: AliyunSpeechEvent, sequence: UInt64) {
        guard generation == sequence, isRecognizing else { return }
        if let error = event.error { finish(.failure(SpeechRecognitionError.service(error))); return }
        do {
            if let json = event.json, !json.isEmpty {
                guard try transcript.apply(json: json, sentenceFinal: event.sentenceFinal) else { return }
                if liveText != transcript.text {
                    liveText = transcript.text
                    onLiveTextUpdate?(liveText, generation)
                }
            }
            if event.completed { finish(.success(transcript.complete())) }
        } catch { finish(.failure(error)) }
    }

    private func finish(_ result: Result<String, Error>) {
        let continuation = stopContinuation; stopContinuation = nil
        stopTimeout?.cancel(); stopTimeout = nil
        let previous = engine; engine = nil
        isRecognizing = false
        // 释放始终异步出 SDK 回调，不等待 UI，也不把超时/取消的部分文字提交。
        RemoteIMBackgroundWork.audioQueue.async { previous?.close() }
        switch result {
        case let .success(text):
            liveText = text
            continuation?.resume(returning: text)
        case let .failure(error):
            continuation?.resume(throwing: error)
            if continuation == nil { onFailure?(error, generation) }
        }
    }
}
