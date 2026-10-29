// 文件作用：定义账号 STS 临时凭据、进程内缓存和 V2 全文识别状态，隔离具体识别 SDK。
// 缓存绑定账号认证代际，V2 结果按全文修正；具体 SDK 的会话与音频采集在实现类中。
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

/// 腾讯实时 ASR V2 原始响应。只解析 V2 sentences，不依赖 V1 slice/segment 的本地拼接。
struct TencentASRV2Response: Decodable, Sendable {
    struct Sentence: Decodable, Sendable {
        let sentenceId: Int
        let sentenceType: Int
        let sentence: String

        var isValid: Bool { sentenceId >= 0 && (0...1).contains(sentenceType) }
    }

    struct Sentences: Decodable, Sendable {
        let voiceTextStr: String?
        let sentenceList: [Sentence]?
        // WebSocket V2 文档还定义单句更新形态，同样以 sentence_id 替换，不追加旧文本。
        let sentenceId: Int?
        let sentenceType: Int?
        let sentence: String?
    }

    let code: Int
    let voiceId: String?
    let final: Int?
    let sentences: Sentences?

    static func decode(_ json: String) throws -> Self {
        let decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
        do { return try decoder.decode(Self.self, from: Data(json.utf8)) }
        catch { throw TencentASRV2Error.invalidResponse }
    }
}

enum TencentASRV2Error: LocalizedError {
    case invalidResponse
    case service(Int)

    var errorDescription: String? {
        switch self {
        case .invalidResponse: return "语音识别返回的数据无效"
        case let .service(code): return "语音识别服务返回错误（\(code)）"
        }
    }
}

/// 保存一轮 V2 识别的最新全文；快照可修正已稳定句，空 final 包不会抹掉已收到的文字。
struct TencentASRV2Transcript {
    private(set) var text = ""
    private(set) var isFinal = false
    private(set) var isStable = false
    private var voiceId: String?
    private var sentencesByID: [Int: TencentASRV2Response.Sentence] = [:]
    private var isCancelled = false

    /// 返回 false 表示迟到的终态后回调或其他语音流，调用方不能据此更新界面或完成会话。
    mutating func apply(_ response: TencentASRV2Response) throws -> Bool {
        guard !isCancelled, !isFinal else { return false }
        if let incomingVoiceId = response.voiceId, !incomingVoiceId.isEmpty {
            if let voiceId, voiceId != incomingVoiceId { return false }
            voiceId = incomingVoiceId
        }
        guard response.code == 0 else { throw TencentASRV2Error.service(response.code) }
        guard response.final == nil || response.final == 0 || response.final == 1 else {
            throw TencentASRV2Error.invalidResponse
        }
        if let snapshot = response.sentences {
            if let list = snapshot.sentenceList {
                guard list.allSatisfy(\.isValid) else { throw TencentASRV2Error.invalidResponse }
                // sentence_list 是全文快照；包含重复 ID 时以最后一次修正为准。
                sentencesByID = Dictionary(list.map { ($0.sentenceId, $0) },
                                           uniquingKeysWith: { _, latest in latest })
                text = joinedSentences()
                isStable = sentencesByID.values.allSatisfy { $0.sentenceType == 1 }
            } else if let sentence = snapshot.sentence {
                guard let id = snapshot.sentenceId, let type = snapshot.sentenceType else {
                    throw TencentASRV2Error.invalidResponse
                }
                let update = TencentASRV2Response.Sentence(
                    sentenceId: id, sentenceType: type, sentence: sentence)
                guard update.isValid else { throw TencentASRV2Error.invalidResponse }
                sentencesByID[id] = update
                text = joinedSentences()
                isStable = sentencesByID.values.allSatisfy { $0.sentenceType == 1 }
            }
            if let fullText = snapshot.voiceTextStr {
                // 包括空字符串：这是服务端的权威全文，不能用旧文本覆盖纠正结果。
                text = fullText.trimmingCharacters(in: .whitespacesAndNewlines)
                if snapshot.sentenceList == nil && snapshot.sentence == nil {
                    sentencesByID = [:]
                    isStable = false
                }
            }
        }
        if response.final == 1 {
            isFinal = true
            isStable = true
        }
        return true
    }

    mutating func cancel() {
        isCancelled = true
        text = ""
        sentencesByID = [:]
    }

    private func joinedSentences() -> String {
        sentencesByID.keys.sorted().compactMap { sentencesByID[$0]?.sentence }
            .joined().trimmingCharacters(in: .whitespacesAndNewlines)
    }
}
