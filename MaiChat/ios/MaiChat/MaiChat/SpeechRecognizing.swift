import Foundation

/// 语音转文字。
///
/// 单独抽一层协议，是为了把「凭证从哪来」挡在调用方之外：现在是 TencentASRCredentials 里的
/// 常量（仅够本机联调），将来换成 STS 临时密钥、Keychain，或改由桌面端代理转发，都只换实现，
/// ChatView 不用动。
protocol SpeechRecognizing: Sendable {
    /// 当前是否可用（比如凭证没填就不可用）。不可用时调用方应直接走原有的发语音路径。
    var isAvailable: Bool { get }

    /// 转写一段本地音频。失败抛错，由调用方决定是否回退成发语音消息。
    /// - Parameters:
    ///   - fileURL: 本地音频文件
    ///   - format: 音频格式标识，如 "m4a"（对应 AVAudioRecorder 的 kAudioFormatMPEG4AAC）
    func transcribe(fileURL: URL, format: String) async throws -> String
}

enum SpeechRecognitionError: LocalizedError {
    case unavailable
    case emptyAudio
    case audioTooLarge
    case unreadableAudio
    case service(String)

    var errorDescription: String? {
        switch self {
        case .unavailable: return "未配置语音识别凭证"
        case .emptyAudio: return "录音内容为空"
        case .audioTooLarge: return "录音超过 3MB，无法识别"
        case .unreadableAudio: return "读取录音失败"
        case .service(let message): return message
        }
    }
}

/// 腾讯云 ASR 凭证。
///
/// 留空即关闭语音识别，录音会照旧作为语音消息发出去，不会因此报错。
///
/// 这里刻意不提交真实密钥：仓库推到 GitHub，密钥一旦进 git 历史基本清不掉，
/// 而且会随每个安装包发出去。本机联调时直接把值填在下面即可，但**不要提交这次改动**；
/// 正式发布应改为 STS 临时密钥或经由桌面端代理转发。
enum TencentASRCredentials {
    static let appId = ""
    static let secretId = ""
    static let secretKey = ""

    static var isConfigured: Bool {
        !appId.isEmpty && !secretId.isEmpty && !secretKey.isEmpty
    }
}
