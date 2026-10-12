import Foundation

struct MaiChatAuthChallenge: Decodable, Sendable {
    let challengeId: String
    let expiresIn: Int
    let resendAfter: Int
}

struct MaiChatAuthUser: Decodable, Sendable {
    let id: String
    let imUserId: String
}

struct MaiChatAuthSession: Decodable, Sendable {
    let accessToken: String
    let refreshToken: String
    let expiresIn: Int
    let user: MaiChatAuthUser
}

struct MaiChatIMCredential: Decodable, Sendable {
    let sdkAppId: Int
    let userId: String
    let userSig: String
    let expiresAt: Int
}

struct MaiChatSeedanceCredential: Decodable, Sendable {
    let apiKey: String
    let expiresAt: Int
}

struct MaiChatBailianCredential: Decodable, Sendable {
    let apiKey: String
    let expiresAt: Int
    let wanWorkspaceId: String
}

private struct MaiChatAuthServerError: Decodable {
    let error: String
}

enum MaiChatAuthError: LocalizedError {
    case invalidServerResponse
    case requestFailed(String)
    case sessionExpired
    case identityMismatch

    var errorDescription: String? {
        switch self {
        case .invalidServerResponse: return "登录服务返回的数据无效"
        case let .requestFailed(message):
            let translations = [
                "Invalid phone or purpose": "手机号格式不正确",
                "Please wait before requesting another code": "请求太频繁，请稍后再试",
                "Verification code could not be sent": "验证码发送失败，请稍后再试",
                "Invalid or expired verification code": "验证码错误或已过期",
                "Phone or password is incorrect": "手机号或密码错误",
                "Minimum age confirmation required": "请先确认已年满 14 周岁",
                "Please wait before trying again": "登录尝试过多，请稍后再试",
                "Password must be 15-128 characters": "密码须为 15–128 个字符",
                "Sign in again before changing the password": "请重新登录后再修改密码",
            ]
            return translations[message] ?? message
        case .sessionExpired: return "登录已过期，请重新用手机号登录"
        case .identityMismatch: return "当前 IM 账号与手机号账号不一致，请重新登录"
        }
    }
}

/// 统一账号的网络协议。刷新令牌只放在本机 Keychain；访问令牌只留在内存。
/// 手机号登录的 UserSig 只能由服务端签发，客户端不持有 IM 签名密钥。
actor MaiChatAuthClient {
    static let shared = MaiChatAuthClient()

    private let baseURL = URL(string: "https://ichat.life/api/v1/")!
    private let refreshStore = KeychainSecretStore(account: "maichat-account-refresh-token")
    private var accessToken: String?
    private var accessExpiresAt = Date.distantPast
    private var refreshTask: Task<MaiChatAuthSession, Error>?

    func requestCode(
        phone: String, purpose: String = "login", ageConfirmed: Bool
    ) async throws -> MaiChatAuthChallenge {
        try await request(
            "auth/sms/challenge", method: "POST",
            body: [
                "phone": phone, "purpose": purpose,
                "age_confirmed_14_plus": ageConfirmed
            ]
        )
    }

    func verifyCode(
        phone: String, challengeId: String, code: String, ageConfirmed: Bool
    ) async throws -> MaiChatAuthSession {
        let session: MaiChatAuthSession = try await request(
            "auth/sms/verify", method: "POST",
            body: [
                "phone": phone, "challenge_id": challengeId, "code": code,
                "age_confirmed_14_plus": ageConfirmed
            ]
        )
        try adopt(session)
        return session
    }

    func loginWithPassword(
        phone: String, password: String, ageConfirmed: Bool
    ) async throws -> MaiChatAuthSession {
        let session: MaiChatAuthSession = try await request(
            "auth/password/login", method: "POST",
            body: [
                "phone": phone, "password": password,
                "age_confirmed_14_plus": ageConfirmed
            ]
        )
        try adopt(session)
        return session
    }

    func setPassword(_ password: String) async throws {
        let token = try await currentAccessToken()
        let _: [String: Bool] = try await request(
            "auth/password/set", method: "POST",
            body: ["new_password": password], bearer: token
        )
    }

    func credentialIfAvailable(expectedUserID: String) async throws -> MaiChatIMCredential? {
        guard accessToken != nil || !refreshStore.readSecretKey().isEmpty else { return nil }
        let token = try await currentAccessToken()
        let credential: MaiChatIMCredential
        do {
            credential = try await request("im/credential", method: "GET", bearer: token)
        } catch MaiChatAuthError.requestFailed("Authentication required") {
            // 访问令牌与服务端时钟不同步时，只刷新一次，不改用本地 IM 签名。
            accessToken = nil
            let renewedToken = try await currentAccessToken()
            credential = try await request(
                "im/credential", method: "GET", bearer: renewedToken
            )
        }
        guard credential.userId == expectedUserID else { throw MaiChatAuthError.identityMismatch }
        return credential
    }

    func seedanceCredentialIfAvailable() async throws -> MaiChatSeedanceCredential? {
        guard accessToken != nil || !refreshStore.readSecretKey().isEmpty else { return nil }
        let token = try await currentAccessToken()
        do {
            return try await request("model/seedance-credential", method: "GET", bearer: token)
        } catch MaiChatAuthError.requestFailed("Authentication required") {
            accessToken = nil
            let renewed = try await currentAccessToken()
            return try await request("model/seedance-credential", method: "GET",
                                     bearer: renewed)
        }
    }

    func bailianCredentialIfAvailable() async throws -> MaiChatBailianCredential? {
        guard accessToken != nil || !refreshStore.readSecretKey().isEmpty else { return nil }
        let token = try await currentAccessToken()
        do {
            return try await request("model/bailian-credential", method: "GET", bearer: token)
        } catch MaiChatAuthError.requestFailed("Authentication required") {
            accessToken = nil
            let renewed = try await currentAccessToken()
            return try await request("model/bailian-credential", method: "GET",
                                     bearer: renewed)
        }
    }

    private func adopt(_ session: MaiChatAuthSession) throws {
        try refreshStore.saveSecretKey(session.refreshToken)
        accessToken = session.accessToken
        accessExpiresAt = Date().addingTimeInterval(TimeInterval(session.expiresIn - 30))
    }

    private func currentAccessToken() async throws -> String {
        if let accessToken, Date() < accessExpiresAt { return accessToken }
        if let refreshTask { return try await refreshTask.value.accessToken }
        let refreshToken = refreshStore.readSecretKey()
        guard !refreshToken.isEmpty else { throw MaiChatAuthError.sessionExpired }
        // 多个 IM 操作同时需要凭据时只能刷新一次；旧令牌被重复使用会撤销会话。
        let task = Task<MaiChatAuthSession, Error> {
            try await self.request("auth/refresh", method: "POST",
                                   body: ["refresh_token": refreshToken])
        }
        refreshTask = task
        defer { refreshTask = nil }
        do {
            let session = try await task.value
            try adopt(session)
            return session.accessToken
        } catch MaiChatAuthError.requestFailed("Invalid session") {
            accessToken = nil
            try? refreshStore.saveSecretKey("")
            throw MaiChatAuthError.sessionExpired
        } catch {
            // 临时网络失败保留刷新令牌，下次连接可继续重试。
            throw error
        }
    }

    private func request<T: Decodable & Sendable>(
        _ path: String,
        method: String,
        body: [String: Any]? = nil,
        bearer: String? = nil
    ) async throws -> T {
        var request = URLRequest(url: baseURL.appendingPathComponent(path))
        request.httpMethod = method
        request.timeoutInterval = 20
        request.setValue("application/json", forHTTPHeaderField: "Accept")
        request.setValue("no-store", forHTTPHeaderField: "Cache-Control")
        if let bearer {
            request.setValue("Bearer \(bearer)", forHTTPHeaderField: "Authorization")
        }
        if let body {
            request.setValue("application/json", forHTTPHeaderField: "Content-Type")
            request.httpBody = try JSONSerialization.data(withJSONObject: body)
        }
        let (data, response) = try await URLSession.shared.data(for: request)
        guard let response = response as? HTTPURLResponse else {
            throw MaiChatAuthError.invalidServerResponse
        }
        guard (200..<300).contains(response.statusCode) else {
            let message = try? JSONDecoder().decode(MaiChatAuthServerError.self, from: data).error
            throw MaiChatAuthError.requestFailed(message ?? "登录服务暂时不可用（\(response.statusCode)）")
        }
        let decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
        do { return try decoder.decode(T.self, from: data) }
        catch { throw MaiChatAuthError.invalidServerResponse }
    }
}
