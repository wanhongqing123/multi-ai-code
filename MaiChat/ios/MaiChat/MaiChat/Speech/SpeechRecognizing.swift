// 文件作用：定义语音识别的会话接口、回调数据和错误类型，隔离具体识别 SDK。
// 定义语音转文字的开始、停止、取消和增量回调契约。
// 界面只消费会话结果，具体 SDK 的音频采集在实现类中。
import Foundation

enum SpeechRecognitionError: LocalizedError {
    case unavailable
    case service(String)

    var errorDescription: String? {
        switch self {
        case .unavailable: return "未配置语音识别凭证"
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
    static let appId = "1304255122"
    static let secretId = "AKIDdVAmEVaSAB5oKcHk295pOksoW4Rav76u"
    static let secretKey = "g8TbVG1gQ0NYF3oSwISOFtKZaNxKWcXG"

    static var isConfigured: Bool {
        !appId.isEmpty && !secretId.isEmpty && !secretKey.isEmpty
    }
}
