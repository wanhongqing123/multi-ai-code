// 文件作用：定义账号 STS 临时凭据、进程内缓存与识别错误，隔离具体识别 SDK。
// 缓存绑定账号认证代际；具体 SDK 的会话与音频采集在实现类中。
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

/// 账号服务下发的腾讯 STS 临时凭据；只留在进程内存，音频由客户端直连腾讯。
struct MaiChatASRCredential: Decodable, Sendable {
    let appId: String
    let secretId: String
    let secretKey: String
    let token: String
    let expiresAt: Int

    /// 与服务端相同地提前 90 秒领取下一份凭据，避免录音启动时刚好过期。
    func isReusable(at date: Date = Date()) -> Bool {
        // STS SecretId 是 opaque 标识，合法值可含符号；SDK 负责 URL 编码，不能限制为字母数字。
        let secretIdBytes = secretId.utf8
        return !appId.isEmpty && appId.utf8.allSatisfy { (48...57).contains($0) } &&
            (8...1024).contains(secretIdBytes.count) &&
            secretIdBytes.allSatisfy { (33...126).contains($0) } &&
            !secretKey.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty &&
            !token.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty &&
            TimeInterval(expiresAt) > date.timeIntervalSince1970 + 90
    }
}

/// 缓存绑定账号认证代际；退出、重新登录和迟到的旧响应都不能复用上一轮凭据。
struct MaiChatASRCredentialCache {
    private var entry: (sessionVersion: UInt64, credential: MaiChatASRCredential)?

    mutating func credential(
        for sessionVersion: UInt64, at date: Date = Date()
    ) -> MaiChatASRCredential? {
        guard let entry, entry.sessionVersion == sessionVersion,
              entry.credential.isReusable(at: date) else {
            self.entry = nil
            return nil
        }
        return entry.credential
    }

    @discardableResult
    mutating func store(
        _ credential: MaiChatASRCredential, for expectedSessionVersion: UInt64,
        currentSessionVersion: UInt64, at date: Date = Date()
    ) -> Bool {
        // 旧请求返回时不碰新账号已经保存的凭据。
        guard expectedSessionVersion == currentSessionVersion,
              credential.isReusable(at: date) else { return false }
        entry = (expectedSessionVersion, credential)
        return true
    }

    mutating func clear() { entry = nil }
}
