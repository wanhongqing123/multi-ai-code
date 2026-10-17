// 文件作用：调用账号服务完成手机号登录、令牌刷新及移动端凭证获取。
// 阅读边界：本文件不保存长期模型密钥；安全存储由账号状态和系统存储层负责。
// 封装手机号验证、账号登录、刷新令牌及服务端 IM 凭据请求。
import Foundation
import UIKit

extension Notification.Name {
    static let maiChatAccountSessionReplaced = Notification.Name("MaiChatAccountSessionReplaced")
    static let maiChatAccountDeviceSlotRequired =
        Notification.Name("MaiChatAccountDeviceSlotRequired")
}

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

enum MaiChatSocialProvider: String, Sendable {
    case wechat
    case qq

    var displayName: String { self == .wechat ? "微信" : "QQ" }
}

/// 正式 SDK 适配器只把平台颁发的凭据交给账号服务，不在本机信任 openid。
enum MaiChatSocialCredential: Sendable {
    case wechatAuthorizationCode(String)
    case qqSDKAccessToken(String)

    var provider: MaiChatSocialProvider {
        switch self {
        case .wechatAuthorizationCode: return .wechat
        case .qqSDKAccessToken: return .qq
        }
    }
}

struct MaiChatSocialBindingIntent: Sendable {
    let provider: MaiChatSocialProvider
    let bindingToken: String
    let expiresIn: Int
}

enum MaiChatSocialLoginResult: Sendable {
    case signedIn(MaiChatAuthSession)
    case bindingRequired(MaiChatSocialBindingIntent)
}

private struct MaiChatSocialBindingResponse: Decodable, Sendable {
    let bindingToken: String
    let expiresIn: Int
    let phoneBindingRequired: Bool
}

struct MaiChatDesktopLoginRequestInfo: Decodable, Sendable {
    let platform: String
    let displayCode: String
    let status: String
    let expiresIn: Int

    var canDecide: Bool { status == "pending" || status == "scanned" }
}

private struct MaiChatCurrentAccount: Decodable, Sendable {
    let user: MaiChatAuthUser
}

private struct MaiChatDesktopLoginDecision: Decodable, Sendable {
    let status: String
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

struct MaiChatAccountModelKeys: Decodable, Sendable {
    let apiKeys: [String: String]
}

struct MaiChatMediaServiceAccess: Sendable {
    let baseURL: String
    let accountToken: String
}

private struct MaiChatAuthServerError: Decodable {
    let error: String
    let code: String?
}

enum MaiChatAuthError: LocalizedError {
    case invalidServerResponse
    case requestFailed(String)
    case sessionExpired
    case sessionReplaced
    case deviceSlotRequired
    case identityMismatch
    case desktopCodeMismatch
    case desktopRequestUnavailable

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
                "Desktop login request not found": "未找到这次电脑登录请求",
                "Desktop login request expired": "电脑登录二维码已过期，请刷新后重试",
                "Desktop login request already decided": "这次电脑登录请求已处理",
                "Mobile session required": "请先在手机或平板上登录",
                "Invalid social login request": "第三方登录凭据无效，请重试",
                "Social login is unavailable": "第三方登录暂不可用，请稍后重试",
                "Social credential is invalid": "第三方授权已失效，请重新授权",
                "Invalid social binding request": "手机号绑定请求无效，请重新授权",
                "Social binding expired": "绑定已过期，请重新使用第三方账号登录",
                "Social identity is already bound": "此第三方账号已绑定其他手机号",
                "Account already has this social provider": "此手机号已绑定另一个同类第三方账号",
                "Account unavailable": "账号暂不可用",
            ]
            return translations[message] ?? message
        case .sessionExpired: return "登录已过期，请重新用手机号登录"
        case .sessionReplaced: return "账号已在另一台同类设备登录，请重新登录"
        case .deviceSlotRequired: return "请更新 App 后重新登录"
        case .identityMismatch: return "当前 IM 账号与手机号账号不一致，请重新登录"
        case .desktopCodeMismatch: return "电脑上的验证码与此请求不一致，请重新扫码"
        case .desktopRequestUnavailable: return "这次电脑登录请求已处理，请刷新二维码后重试"
        }
    }
}

/// 统一账号的网络协议。刷新令牌只放在本机 Keychain；访问令牌只留在内存。
/// 手机号登录的 UserSig 只能由服务端签发，客户端不持有 IM 签名密钥。
actor MaiChatAuthClient {
    static let shared = MaiChatAuthClient()

    // UI 测试显式注入本机账号后才连接模拟服务；普通模拟器运行仍使用正式账号服务。
    #if DEBUG && targetEnvironment(simulator)
    private let baseURL: URL = {
        let environment = ProcessInfo.processInfo.environment
        guard environment["MAICHAT_LOCAL_UI_PHONE"] != nil else {
            return URL(string: "https://ichat.life/api/v1/")!
        }
        let configuredPort = Int(environment["MAICHAT_LOCAL_UI_AUTH_PORT"] ?? "")
        let port = configuredPort.flatMap { (1...65535).contains($0) ? $0 : nil } ?? 18190
        return URL(string: "http://127.0.0.1:\(port)/v1/")!
    }()
    #else
    private let baseURL = URL(string: "https://ichat.life/api/v1/")!
    #endif
    private let refreshStore = KeychainSecretStore(account: "maichat-account-refresh-token")
    private var accessToken: String?
    private var accessExpiresAt = Date.distantPast
    private var refreshTask: Task<MaiChatAuthSession, Error>?
    private var sessionVersion: UInt64 = 0

    private func mobileDeviceFields() async -> [String: Any] {
        let slot = await MainActor.run {
            UIDevice.current.userInterfaceIdiom == .pad ? "tablet" : "ios_phone"
        }
        return slot == "tablet"
            ? ["device_slot": slot, "device_platform": "ios"]
            : ["device_slot": slot]
    }

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
        let device = await mobileDeviceFields()
        let session: MaiChatAuthSession = try await request(
            "auth/sms/verify", method: "POST",
            body: [
                "phone": phone, "challenge_id": challengeId, "code": code,
                "age_confirmed_14_plus": ageConfirmed
            ].merging(device) { _, new in new }
        )
        sessionVersion &+= 1
        try adopt(session)
        return session
    }

    func loginWithPassword(
        phone: String, password: String, ageConfirmed: Bool
    ) async throws -> MaiChatAuthSession {
        let device = await mobileDeviceFields()
        let session: MaiChatAuthSession = try await request(
            "auth/password/login", method: "POST",
            body: [
                "phone": phone, "password": password,
                "age_confirmed_14_plus": ageConfirmed
            ].merging(device) { _, new in new }
        )
        sessionVersion &+= 1
        try adopt(session)
        return session
    }

    func resetPassword(
        phone: String, challengeId: String, code: String,
        newPassword: String, ageConfirmed: Bool
    ) async throws -> MaiChatAuthSession {
        let device = await mobileDeviceFields()
        let session: MaiChatAuthSession = try await request(
            "auth/password/reset", method: "POST",
            body: [
                "phone": phone, "challenge_id": challengeId, "code": code,
                "new_password": newPassword, "age_confirmed_14_plus": ageConfirmed
            ].merging(device) { _, new in new }
        )
        sessionVersion &+= 1
        try adopt(session)
        return session
    }

    /// 仅接受正式 SDK 适配器交来的微信 code 或 QQ token；服务端校验后决定登录或绑手机。
    func loginSocial(
        credential: MaiChatSocialCredential, ageConfirmed: Bool
    ) async throws -> MaiChatSocialLoginResult {
        let path: String
        let credentialField: String
        let value: String
        switch credential {
        case let .wechatAuthorizationCode(code):
            path = "auth/social/wechat/login"
            credentialField = "code"
            value = code
        case let .qqSDKAccessToken(token):
            path = "auth/social/qq/login"
            credentialField = "access_token"
            value = token
        }
        let device = await mobileDeviceFields()
        let (data, status) = try await requestData(
            path, method: "POST",
            body: [credentialField: value, "age_confirmed_14_plus": ageConfirmed]
                .merging(device) { _, new in new }
        )
        switch status {
        case 200:
            let session: MaiChatAuthSession = try decodeResponse(data)
            sessionVersion &+= 1
            try adopt(session)
            return .signedIn(session)
        case 202:
            let response: MaiChatSocialBindingResponse = try decodeResponse(data)
            guard response.phoneBindingRequired, response.expiresIn > 0,
                  response.bindingToken.range(
                    of: "^[A-Za-z0-9_-]{43}$", options: .regularExpression
                  ) != nil else { throw MaiChatAuthError.invalidServerResponse }
            return .bindingRequired(MaiChatSocialBindingIntent(
                provider: credential.provider,
                bindingToken: response.bindingToken,
                expiresIn: response.expiresIn
            ))
        default:
            throw MaiChatAuthError.invalidServerResponse
        }
    }

    /// 绑定 intent 已保存设备槽；短信只证明手机号归属，不由客户端提供 openid。
    func bindSocial(
        intent: MaiChatSocialBindingIntent, phone: String,
        challengeId: String, code: String, ageConfirmed: Bool
    ) async throws -> MaiChatAuthSession {
        let session: MaiChatAuthSession = try await request(
            "auth/social/bind", method: "POST",
            body: [
                "binding_token": intent.bindingToken,
                "phone": phone, "challenge_id": challengeId, "code": code,
                "age_confirmed_14_plus": ageConfirmed
            ]
        )
        sessionVersion &+= 1
        try adopt(session)
        return session
    }

    func desktopLoginRequestInfo(id: String, code: String) async throws
        -> MaiChatDesktopLoginRequestInfo {
        let token = try await currentAccessToken()
        guard id.count == 36, let requestID = UUID(uuidString: id)?.uuidString.lowercased() else {
            throw MaiChatAuthError.invalidServerResponse
        }
        let info: MaiChatDesktopLoginRequestInfo = try await request(
            "auth/desktop/request/\(requestID)", method: "GET", bearer: token
        )
        if !code.isEmpty && code != info.displayCode {
            throw MaiChatAuthError.desktopCodeMismatch
        }
        return info
    }

    /// 前台检查本机槽位仍归当前账号；网络失败保留本机状态，稍后重试。
    func validateCurrentSession(expectedUserID: String) async throws {
        let token = try await currentAccessToken()
        let account: MaiChatCurrentAccount
        do {
            account = try await request("auth/me", method: "GET", bearer: token)
        } catch MaiChatAuthError.requestFailed("Authentication required") {
            // 访问令牌提前失效时刷新一次；同槽替换的专用错误会直接向上抛出。
            accessToken = nil
            let renewedToken = try await currentAccessToken()
            account = try await request("auth/me", method: "GET", bearer: renewedToken)
        }
        guard account.user.imUserId == expectedUserID else {
            throw MaiChatAuthError.identityMismatch
        }
    }

    func approveDesktopLoginRequest(id: String, code: String) async throws {
        try await decideDesktopLoginRequest(id: id, code: code, approved: true)
    }

    func denyDesktopLoginRequest(id: String, code: String) async throws {
        try await decideDesktopLoginRequest(id: id, code: code, approved: false)
    }

    private func decideDesktopLoginRequest(
        id: String, code: String, approved: Bool
    ) async throws {
        let info = try await desktopLoginRequestInfo(id: id, code: code)
        guard info.canDecide else { throw MaiChatAuthError.desktopRequestUnavailable }
        let token = try await currentAccessToken()
        let path = approved ? "auth/desktop/approve" : "auth/desktop/deny"
        guard let requestID = UUID(uuidString: id)?.uuidString.lowercased() else {
            throw MaiChatAuthError.invalidServerResponse
        }
        let decision: MaiChatDesktopLoginDecision = try await request(
            path, method: "POST", body: ["request_id": requestID], bearer: token
        )
        guard decision.status == (approved ? "approved" : "denied") else {
            throw MaiChatAuthError.invalidServerResponse
        }
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

    func accountModelKeysIfAvailable() async throws -> MaiChatAccountModelKeys? {
        guard accessToken != nil || !refreshStore.readSecretKey().isEmpty else { return nil }
        let expectedSessionVersion = sessionVersion
        let token = try await currentAccessToken()
        let issued: MaiChatAccountModelKeys
        do {
            issued = try await request("model/api-keys", method: "GET", bearer: token)
        } catch MaiChatAuthError.requestFailed("Authentication required") {
            accessToken = nil
            let renewed = try await currentAccessToken()
            issued = try await request("model/api-keys", method: "GET", bearer: renewed)
        }
        guard sessionVersion == expectedSessionVersion else {
            throw MaiChatAuthError.sessionExpired
        }
        return issued
    }

    func mediaServiceAccessIfAvailable() async throws -> MaiChatMediaServiceAccess? {
        guard accessToken != nil || !refreshStore.readSecretKey().isEmpty else { return nil }
        let expectedSessionVersion = sessionVersion
        let token = try await currentAccessToken()
        guard sessionVersion == expectedSessionVersion,
              var components = URLComponents(url: baseURL, resolvingAgainstBaseURL: false)
        else { throw MaiChatAuthError.sessionExpired }
        components.path = "/maichat"
        components.query = nil
        components.fragment = nil
        guard let url = components.url else { throw MaiChatAuthError.invalidServerResponse }
        return MaiChatMediaServiceAccess(baseURL: url.absoluteString, accountToken: token)
    }

    /// 先撤销本机登录，保证 App 在切换过程中被系统结束时不会恢复旧账号。
    func discardLocalSession() -> String? {
        let token = accessToken
        sessionVersion &+= 1
        refreshTask?.cancel()
        refreshTask = nil
        accessToken = nil
        accessExpiresAt = .distantPast
        try? refreshStore.saveSecretKey("")
        return token
    }

    func revokeServerSession(_ token: String) async {
        guard !token.isEmpty else { return }
        let _: [String: Bool]? = try? await request(
            "auth/logout", method: "POST", body: [:], bearer: token
        )
    }

    private func adopt(_ session: MaiChatAuthSession) throws {
        try refreshStore.saveSecretKey(session.refreshToken)
        accessToken = session.accessToken
        accessExpiresAt = Date().addingTimeInterval(TimeInterval(session.expiresIn - 30))
    }

    private func currentAccessToken() async throws -> String {
        if let accessToken, Date() < accessExpiresAt { return accessToken }
        if let refreshTask {
            let version = sessionVersion
            let session = try await refreshTask.value
            guard sessionVersion == version else { throw MaiChatAuthError.sessionExpired }
            return session.accessToken
        }
        let refreshToken = refreshStore.readSecretKey()
        guard !refreshToken.isEmpty else { throw MaiChatAuthError.sessionExpired }
        let version = sessionVersion
        // 多个 IM 操作同时需要凭据时只能刷新一次；旧令牌被重复使用会撤销会话。
        let task = Task<MaiChatAuthSession, Error> {
            try await self.request("auth/refresh", method: "POST",
                                   body: ["refresh_token": refreshToken])
        }
        refreshTask = task
        defer { refreshTask = nil }
        do {
            let session = try await task.value
            guard sessionVersion == version else { throw MaiChatAuthError.sessionExpired }
            try adopt(session)
            return session.accessToken
        } catch MaiChatAuthError.requestFailed("Invalid session") {
            guard sessionVersion == version else { throw MaiChatAuthError.sessionExpired }
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
        let (data, _) = try await requestData(
            path, method: method, body: body, bearer: bearer
        )
        return try decodeResponse(data)
    }

    private func decodeResponse<T: Decodable & Sendable>(_ data: Data) throws -> T {
        let decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
        do { return try decoder.decode(T.self, from: data) }
        catch { throw MaiChatAuthError.invalidServerResponse }
    }

    /// 保留 HTTP 状态码，让社交登录安全地区分已登录(200)和待绑手机(202)。
    /// JSON 与账号状态仍由本层处理；libcurl 只负责在后台线程进行一次有证书校验的传输。
    private func requestData(
        _ path: String,
        method: String,
        body: [String: Any]? = nil,
        bearer: String? = nil
    ) async throws -> (Data, Int) {
        let requestedSessionVersion = sessionVersion
        let url = baseURL.appendingPathComponent(path)
        let requestBody = try body.map { try JSONSerialization.data(withJSONObject: $0) }
        let caBundlePath: String
        if url.scheme == "https" {
            guard let caBundle = Bundle.main.url(
                forResource: "cacert", withExtension: "pem", subdirectory: "MaiAgentCertificates"
            ) else { throw MaiChatAuthError.requestFailed("登录服务证书不可用") }
            caBundlePath = caBundle.path
        } else {
            // 只有 Debug 模拟器的显式本机 UI 测试会使用 HTTP；C++ 桥接也限制为 127.0.0.1。
            caBundlePath = ""
        }
        let (data, statusCode) = try await Self.performCurlRequest(
            url: url.absoluteString, method: method, bearer: bearer,
            body: requestBody ?? Data(), caBundlePath: caBundlePath
        )
        guard (200..<300).contains(statusCode) else {
            let serverError = try? JSONDecoder().decode(MaiChatAuthServerError.self, from: data)
            if statusCode == 401,
               serverError?.code == "SESSION_REPLACED" ||
               serverError?.code == "DEVICE_SLOT_REQUIRED" {
                guard sessionVersion == requestedSessionVersion else {
                    throw MaiChatAuthError.sessionExpired
                }
                _ = discardLocalSession()
                let replaced = serverError?.code == "SESSION_REPLACED"
                await MainActor.run {
                    NotificationCenter.default.post(
                        name: replaced ? .maiChatAccountSessionReplaced :
                            .maiChatAccountDeviceSlotRequired,
                        object: nil
                    )
                }
                throw replaced ? MaiChatAuthError.sessionReplaced :
                    MaiChatAuthError.deviceSlotRequired
            }
            throw MaiChatAuthError.requestFailed(serverError?.error ??
                "登录服务暂时不可用（\(statusCode)）")
        }
        return (data, statusCode)
    }

    /// libcurl 是同步接口，必须离开 actor 和主线程；每个调用只发送一次请求。
    private nonisolated static func performCurlRequest(
        url: String, method: String, bearer: String?, body: Data, caBundlePath: String
    ) async throws -> (Data, Int) {
        try Task.checkCancellation()
        return try await withCheckedThrowingContinuation { continuation in
            DispatchQueue.global(qos: .userInitiated).async {
                var response = MaiChatAuthHttpResult()
                defer { maiChatAuthHttpFreeResult(&response) }
                url.withCString { urlPointer in
                    method.withCString { methodPointer in
                        (bearer ?? "").withCString { bearerPointer in
                            caBundlePath.withCString { caBundlePointer in
                                body.withUnsafeBytes { bytes in
                                    maiChatAuthHttpPerform(
                                        urlPointer, methodPointer, bearerPointer,
                                        method == "GET" ? nil :
                                            bytes.bindMemory(to: UInt8.self).baseAddress,
                                        body.count, caBundlePointer, &response
                                    )
                                }
                            }
                        }
                    }
                }
                if response.transportCode != 0 {
                    continuation.resume(throwing: Self.transportError(response.transportCode))
                    return
                }
                guard (100...599).contains(response.statusCode) else {
                    continuation.resume(throwing: MaiChatAuthError.invalidServerResponse)
                    return
                }
                let data = response.responseBody.map {
                    Data(bytes: $0, count: response.responseLength)
                } ?? Data()
                continuation.resume(returning: (data, Int(response.statusCode)))
            }
        }
    }

    /// 给用户保留可操作的中文错误；诊断码用于区分网络、证书、代理和响应上限。
    private nonisolated static func transportError(_ code: Int32) -> MaiChatAuthError {
        let message: String
        switch code {
        case -5: message = "当前网络代理无法用于登录，请切换网络或代理配置"
        case -2, 63: message = "登录服务响应过大，请稍后再试"
        case 28: message = "连接登录服务超时，请检查网络后重试"
        case 60, 77, 58: message = "无法验证登录服务证书，请检查网络设置"
        case -1, -3, -4: message = "账号网络组件配置失败，请更新 App"
        default: message = "连接登录服务失败，请检查网络后重试"
        }
        return .requestFailed("\(message)（curl \(code)）")
    }
}
