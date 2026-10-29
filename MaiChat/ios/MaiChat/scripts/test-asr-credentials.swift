// 文件作用：直接编译生产凭据类型，验证接口解码、刷新边界及账号代际隔离；不连接网络或麦克风。
// 运行：swiftc -swift-version 6 MaiChat/Speech/SpeechRecognizing.swift scripts/test-asr-credentials.swift -o /tmp/maichat-asr-credentials-test
import Foundation

@main
private struct ASRCredentialTests {
    private struct Failure: Error { let message: String }

    private static func require(_ condition: Bool, _ message: String) throws {
        guard condition else { throw Failure(message: message) }
    }

    private static func fixture(
        token: String = "test-session-token", expiresAt: Int = 2_000,
        secretId: String = "test-temporary-id"
    ) -> MaiChatASRCredential {
        MaiChatASRCredential(appId: "1234567890", secretId: secretId,
                            secretKey: "test-temporary-key", token: token, expiresAt: expiresAt)
    }

    static func main() throws {
        let now = Date(timeIntervalSince1970: 1_000)
        let decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
        let response = Data("""
            {"app_id":"1234567890","secret_id":"test-temporary-id",
             "secret_key":"test-temporary-key","token":"test-session-token","expires_at":2000}
            """.utf8)
        let decoded = try decoder.decode(MaiChatASRCredential.self, from: response)
        try require(decoded.appId == "1234567890" && decoded.token == "test-session-token" &&
                    decoded.expiresAt == 2_000, "Credential response contract changed")
        try require(decoded.isReusable(at: now), "Valid STS credential rejected")

        // 真实 STS 控制面返回过含符号的 68 字节 ID；这里只用独立合成值重现其格式边界。
        let opaqueSecretId = String(repeating: "x", count: 56) + "-._~+/=:@!$%"
        try require(opaqueSecretId.utf8.count == 68 &&
                    fixture(secretId: opaqueSecretId).isReusable(at: now),
                    "Opaque temporary SecretId with symbols was rejected")
        for validId in ["test-id!", String(repeating: "x", count: 1024)] {
            try require(fixture(secretId: validId).isReusable(at: now),
                        "Valid temporary SecretId length boundary was rejected")
        }
        for invalidId in [String(repeating: "x", count: 7), String(repeating: "x", count: 1025),
                          "test id!", "test-id\n", "test\u{00}id!", "test\u{7F}id!", "test-idé"] {
            try require(!fixture(secretId: invalidId).isReusable(at: now),
                        "Invalid temporary SecretId characters or length were accepted")
        }

        // token 是 STS 认证必填项，不能静默退回长期密钥形式。
        let missingToken = Data("""
            {"app_id":"1234567890","secret_id":"test-id","secret_key":"test-key","expires_at":2000}
            """.utf8)
        var rejectedMissingToken = false
        do { _ = try decoder.decode(MaiChatASRCredential.self, from: missingToken) }
        catch is DecodingError { rejectedMissingToken = true }
        try require(rejectedMissingToken, "Missing STS token was accepted")
        try require(!fixture(token: " \n").isReusable(at: now), "Empty STS token was accepted")
        try require(fixture(expiresAt: 1_091).isReusable(at: now), "Refresh started too early")
        try require(!fixture(expiresAt: 1_090).isReusable(at: now), "90-second refresh boundary missed")
        try require(!fixture(expiresAt: 999).isReusable(at: now), "Expired credential was accepted")

        var cache = MaiChatASRCredentialCache()
        try require(cache.store(decoded, for: 1, currentSessionVersion: 1, at: now),
                    "Current account credential was not cached")
        try require(cache.credential(for: 1, at: now)?.token == decoded.token,
                    "Current account could not reuse its credential")
        try require(cache.credential(for: 2, at: now) == nil, "Credential crossed account generations")
        let next = fixture(token: "test-next-account-token")
        try require(cache.store(next, for: 2, currentSessionVersion: 2, at: now),
                    "New account credential was not cached")
        try require(!cache.store(decoded, for: 1, currentSessionVersion: 2, at: now),
                    "Late response from previous account was stored")
        try require(cache.credential(for: 2, at: now)?.token == next.token,
                    "Late response overwrote the new account credential")
        try require(cache.credential(for: 2, at: Date(timeIntervalSince1970: 1_910)) == nil,
                    "Cache retained a credential inside its refresh window")
        try require(cache.store(next, for: 2, currentSessionVersion: 2, at: now),
                    "Fresh response was not cached")
        cache.clear()
        try require(cache.credential(for: 2, at: now) == nil, "Sign-out retained a credential")

        // NLS 使用独立的 AppKey/Token 合同，但复用相同的过期和账号代际缓存规则。
        let aliyunResponse = Data("""
            {"app_key":"test-project-key","token":"test-nls-token","expires_at":2000}
            """.utf8)
        let aliyun = try decoder.decode(MaiChatAliyunASRCredential.self, from: aliyunResponse)
        try require(aliyun.appKey == "test-project-key" && aliyun.token == "test-nls-token" &&
                    aliyun.isReusable(at: now), "Valid NLS response was rejected")
        let expiredAliyun = MaiChatAliyunASRCredential(appKey: "test-project-key",
                                                        token: "test-nls-token", expiresAt: 1_090)
        let malformedAliyun = MaiChatAliyunASRCredential(appKey: "test-project-key",
                                                          token: "bad\nvalue", expiresAt: 2_000)
        try require(!expiredAliyun.isReusable(at: now), "NLS refresh boundary was missed")
        try require(!malformedAliyun.isReusable(at: now), "Malformed NLS token was accepted")
        var aliyunCache = MaiChatAliyunASRCredentialCache()
        try require(aliyunCache.store(aliyun, for: 3, currentSessionVersion: 3, at: now),
                    "NLS credential was not cached")
        try require(!aliyunCache.store(aliyun, for: 3, currentSessionVersion: 4, at: now),
                    "Old NLS response crossed login generations")
        try require(aliyunCache.credential(for: 4, at: now) == nil,
                    "NLS cache crossed account generations")
        print("ASR credential contract, refresh and account isolation checks passed")
    }
}
