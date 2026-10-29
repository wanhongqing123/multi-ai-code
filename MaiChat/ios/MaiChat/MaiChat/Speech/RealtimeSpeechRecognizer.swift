// 文件作用：为两个输入入口选择语音识别服务，并统一管理一轮识别的生命周期。
import Combine
import Foundation

enum SpeechRecognitionProvider: String, CaseIterable {
    case tencent
    case aliyun

    static let defaultsKey = "maichat.speech.provider"

    var title: String {
        switch self {
        case .tencent: return "腾讯云"
        case .aliyun: return "阿里云"
        }
    }

    static func selected(defaults: UserDefaults = .standard) -> Self {
        defaults.string(forKey: defaultsKey).flatMap(Self.init(rawValue:)) ?? .tencent
    }
}

/// 每次长按固定服务商，设置变更从下一次识别开始生效。
@MainActor
final class RealtimeSpeechRecognizer: ObservableObject {
    @MainActor private enum Session {
        case tencent(TencentRealtimeSpeechRecognizer)
        case aliyun(AliyunRealtimeSpeechRecognizer)

        var isRecognizing: Bool {
            switch self {
            case .tencent(let recognizer): return recognizer.isRecognizing
            case .aliyun(let recognizer): return recognizer.isRecognizing
            }
        }

        func stop() async throws -> String {
            switch self {
            case .tencent(let recognizer): return try await recognizer.stop()
            case .aliyun(let recognizer): return try await recognizer.stop()
            }
        }

        func cancel() {
            switch self {
            case .tencent(let recognizer): recognizer.cancel()
            case .aliyun(let recognizer): recognizer.cancel()
            }
        }
    }

    @Published private(set) var isRecognizing = false
    var onLiveTextUpdate: (@MainActor (String, UInt64) -> Void)?
    var onFailure: (@MainActor (Error, UInt64) -> Void)?

    private var session: Session?
    private var sequence: UInt64 = 0

    func start(
        diagnosticFields: [String: String],
        tencentCredentialProvider: @MainActor () async throws -> MaiChatASRCredential,
        aliyunCredentialProvider: @MainActor () async throws -> MaiChatAliyunASRCredential
    ) async throws {
        cancel()
        let current = sequence
        let onText: @MainActor (String, UInt64) -> Void = { [weak self] text, _ in
            guard let self, self.sequence == current else { return }
            self.onLiveTextUpdate?(text, current)
        }
        let onError: @MainActor (Error, UInt64) -> Void = { [weak self] error, _ in
            guard let self, self.sequence == current else { return }
            self.isRecognizing = false
            self.onFailure?(error, current)
        }

        do {
            switch SpeechRecognitionProvider.selected() {
            case .tencent:
                let recognizer = TencentRealtimeSpeechRecognizer()
                recognizer.onLiveTextUpdate = onText
                recognizer.onFailure = onError
                session = .tencent(recognizer)
                try await recognizer.start(diagnosticFields: diagnosticFields,
                                           credentialProvider: tencentCredentialProvider)
            case .aliyun:
                let recognizer = AliyunRealtimeSpeechRecognizer()
                recognizer.onLiveTextUpdate = onText
                recognizer.onFailure = onError
                session = .aliyun(recognizer)
                try await recognizer.start(diagnosticFields: diagnosticFields,
                                           credentialProvider: aliyunCredentialProvider)
            }
            // 凭据领取或系统授权期间可能已取消或开始新一轮，旧结果不能覆盖新状态。
            guard sequence == current, !Task.isCancelled else { throw CancellationError() }
            isRecognizing = session?.isRecognizing ?? false
        } catch {
            if sequence == current {
                session?.cancel()
                session = nil
                isRecognizing = false
            }
            throw error
        }
    }

    func stop() async throws -> String {
        guard let session, isRecognizing else {
            throw SpeechRecognitionError.service("实时语音识别尚未启动")
        }
        let current = sequence
        defer {
            if sequence == current {
                // 停止后迟到的 SDK 回调不能影响已结束的输入手势。
                sequence &+= 1
                self.session = nil
                isRecognizing = false
            }
        }
        let text = try await session.stop()
        // 异步完成可能晚于账号切换或下一轮录音，不允许提交旧结果。
        guard sequence == current, !Task.isCancelled else { throw CancellationError() }
        return text
    }

    func cancel() {
        sequence &+= 1
        session?.cancel()
        session = nil
        isRecognizing = false
    }
}
