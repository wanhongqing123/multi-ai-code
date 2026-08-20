import Foundation
import QCloudOneSentence

/// 腾讯云「一句话识别」实现。
///
/// 选一句话识别而不是实时/录音文件识别，是因为 MaiChat 的用法就是「按住说一段、松手出结果」，
/// 正好落在它的适用范围（≤60 秒、≤3MB）。超出的走不了这个接口，这里当场拒绝并让调用方
/// 回退成发语音消息，而不是把一个必然失败的请求发出去。
final class TencentSpeechRecognizer: SpeechRecognizing {

    /// 一句话识别的硬上限：超过必然被服务端拒绝，本地先挡掉。
    private static let maxAudioBytes = 3 * 1024 * 1024
    /// 中文通用引擎。电话音质场景是 8k_zh；本端录音是 16kHz 单声道，走 16k_zh。
    private static let engineType = "16k_zh"

    private let appId: String
    private let secretId: String
    private let secretKey: String

    init(appId: String, secretId: String, secretKey: String) {
        self.appId = appId.trimmingCharacters(in: .whitespacesAndNewlines)
        self.secretId = secretId.trimmingCharacters(in: .whitespacesAndNewlines)
        self.secretKey = secretKey.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    var isAvailable: Bool {
        !appId.isEmpty && !secretId.isEmpty && !secretKey.isEmpty
    }

    func transcribe(fileURL: URL, format: String) async throws -> String {
        guard isAvailable else { throw SpeechRecognitionError.unavailable }

        let audioData: Data
        do {
            audioData = try Data(contentsOf: fileURL)
        } catch {
            throw SpeechRecognitionError.unreadableAudio
        }
        guard !audioData.isEmpty else { throw SpeechRecognitionError.emptyAudio }
        guard audioData.count <= Self.maxAudioBytes else { throw SpeechRecognitionError.audioTooLarge }

        let session = SentenceRecognitionSession(
            appId: appId,
            secretId: secretId,
            secretKey: secretKey
        )
        return try await session.recognize(
            audioData: audioData,
            voiceFormat: format,
            engineType: Self.engineType
        )
    }
}

/// 一次识别请求的生命周期。
///
/// SDK 的 delegate 是 weak，而回调要等网络往返才来——不自己持有的话这个对象在
/// 请求还在飞的时候就被释放，回调永远不触发，表现为「按了没反应」。所以这里在
/// 发起请求时把自己钉住，回调后再放开。
private final class SentenceRecognitionSession: NSObject, QCloudSentenceRecognizerDelegate {

    private let recognizer: QCloudSentenceRecognizer
    // SDK 的回调线程不确定，而 continuation 重复 resume 在 Swift 并发里是直接崩溃
    // （不是抛错），所以这两个字段的读写必须串行化。
    private let lock = NSLock()
    private var continuation: CheckedContinuation<String, Error>?
    private var selfReference: SentenceRecognitionSession?

    init(appId: String, secretId: String, secretKey: String) {
        recognizer = QCloudSentenceRecognizer(appId: appId, secretId: secretId, secretKey: secretKey)
        super.init()
        recognizer.delegate = self
    }

    func recognize(audioData: Data, voiceFormat: String, engineType: String) async throws -> String {
        try await withCheckedThrowingContinuation { continuation in
            lock.lock()
            self.continuation = continuation
            self.selfReference = self
            lock.unlock()
            let accepted = recognizer.recoginize(
                with: audioData,
                voiceFormat: voiceFormat,
                engSerViceType: engineType
            )
            // 返回 NO 表示本地参数校验就没过，此时不会有任何回调，必须在这里结束等待，
            // 否则 await 会永远挂住。
            if !accepted {
                finish(.failure(SpeechRecognitionError.service("语音识别参数校验失败")))
            }
        }
    }

    func oneSentenceRecognizerDidRecognize(
        _ recognizer: QCloudSentenceRecognizer,
        text: String?,
        error: Error?,
        resultData: [AnyHashable: Any]?
    ) {
        if let error {
            finish(.failure(SpeechRecognitionError.service(error.localizedDescription)))
            return
        }
        finish(.success(text?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""))
    }

    /// 回调可能来自任意线程，且理论上可能重入，这里保证 continuation 只被 resume 一次——
    /// 重复 resume 在 Swift 并发里是直接崩溃，不是报错。
    private func finish(_ result: Result<String, Error>) {
        lock.lock()
        guard let pending = continuation else {
            lock.unlock()
            return
        }
        continuation = nil
        selfReference = nil
        lock.unlock()

        switch result {
        case .success(let text): pending.resume(returning: text)
        case .failure(let error): pending.resume(throwing: error)
        }
    }
}
