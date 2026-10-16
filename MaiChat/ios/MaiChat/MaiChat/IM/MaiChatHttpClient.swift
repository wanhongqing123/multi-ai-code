// 文件作用：为 iOS 自有头像和旧模型凭据请求提供统一的异步 curl 接口。
// 传输由 MaiChatHttpTransport.cpp 执行；本层负责证书路径、任务取消和响应所有权。
// 不把服务令牌写入日志，也不让同步的 curl 操作占用 SwiftUI 主线程。
import Foundation

struct MaiChatHttpResponse: Sendable {
    let body: Data
    let statusCode: Int
    let redirectLocation: String?
}

enum MaiChatHttpClientError: LocalizedError {
    case invalidRequest
    case certificateUnavailable
    case invalidResponse
    case transport(Int)

    var errorDescription: String? {
        switch self {
        case .invalidRequest: return "网络请求参数无效"
        case .certificateUnavailable: return "网络证书不可用"
        case .invalidResponse: return "服务器响应无效"
        case .transport(-2): return "服务器响应超过允许大小"
        case .transport(-5): return "当前网络代理不可用"
        case .transport: return "网络连接失败"
        }
    }
}

private final class MaiChatHttpCancellationHandle: @unchecked Sendable {
    let pointer: OpaquePointer

    init?() {
        guard let pointer = maiChatHttpCreateCancellation() else { return nil }
        self.pointer = pointer
    }

    deinit { maiChatHttpFreeCancellation(pointer) }

    func cancel() { maiChatHttpCancel(pointer) }
}

enum MaiChatHttpClient {
    /// URL 的原始查询串保持不变，避免改变头像 CDN 地址中的签名参数。
    /// curl 在后台队列执行，请求取消时共享原子标记会中断正在进行的传输。
    static func request(
        url: URL, method: String, bearer: String? = nil, contentType: String = "",
        body: Data = Data(), maximumResponseBytes: Int,
        timeoutMilliseconds: Int = 30_000, followSafeGetRedirects: Bool = false
    ) async throws -> MaiChatHttpResponse {
        // 只有公开头像 GET 可以逐跳重定向；带令牌的 JSON POST 永远只请求原始 URL。
        guard let components = URLComponents(url: url, resolvingAgainstBaseURL: false),
              ["http", "https"].contains(components.scheme?.lowercased() ?? ""),
              components.host?.isEmpty == false,
              components.user == nil, components.password == nil, components.fragment == nil,
              !followSafeGetRedirects ||
                  (method == "GET" && bearer == nil && contentType.isEmpty &&
                   body.isEmpty) else {
            throw MaiChatHttpClientError.invalidRequest
        }
        // curl 不读取 iOS 系统信任库，因此从 App 随包证书中显式提供 CA 路径。
        let certificatePath: String
        if components.scheme?.lowercased() == "https" {
            guard let certificate = Bundle.main.url(
                forResource: "cacert", withExtension: "pem", subdirectory: "MaiAgentCertificates"
            ) else { throw MaiChatHttpClientError.certificateUnavailable }
            certificatePath = certificate.path
        } else {
            certificatePath = ""
        }
        guard let cancellation = MaiChatHttpCancellationHandle() else {
            throw MaiChatHttpClientError.transport(-3)
        }
        try Task.checkCancellation()
        // 原生标记由异步请求持有到后台传输结束；取消时不会释放正在使用的 C++ 指针。
        var currentURL = url
        for redirectCount in 0...5 {
            try Task.checkCancellation()
            let requestURL = currentURL
            // 每跳都在独立后台任务完成，当前跳的取消会传到 libcurl 进度回调。
            let response = try await withTaskCancellationHandler {
                try await withCheckedThrowingContinuation {
                    (continuation: CheckedContinuation<MaiChatHttpResponse, Error>) in
                    DispatchQueue.global(qos: .utility).async {
                        var nativeResult = MaiChatHttpResult()
                        defer { maiChatHttpFreeResult(&nativeResult) }
                        let rawURL = requestURL.absoluteString
                        rawURL.withCString { urlPointer in
                            method.withCString { methodPointer in
                                (bearer ?? "").withCString { bearerPointer in
                                    contentType.withCString { typePointer in
                                        certificatePath.withCString { certificatePointer in
                                            body.withUnsafeBytes { bytes in
                                                maiChatHttpPerform(
                                                    urlPointer, methodPointer, bearerPointer, typePointer,
                                                    body.isEmpty ? nil :
                                                        bytes.bindMemory(to: UInt8.self).baseAddress,
                                                    body.count, maximumResponseBytes, timeoutMilliseconds,
                                                    certificatePointer, cancellation.pointer, &nativeResult
                                                )
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        if nativeResult.transportCode != 0 {
                            let code = nativeResult.transportCode
                            continuation.resume(throwing: code == -6 || code == 42
                                ? CancellationError() : MaiChatHttpClientError.transport(Int(code)))
                            return
                        }
                        guard (100...599).contains(nativeResult.statusCode) else {
                            continuation.resume(throwing: MaiChatHttpClientError.invalidResponse)
                            return
                        }
                        // 先复制响应再释放 C++ 缓冲区；Swift 调用方不会持有悬空地址。
                        let data = nativeResult.responseBody.map {
                            Data(bytes: $0, count: nativeResult.responseLength)
                        } ?? Data()
                        let location = nativeResult.redirectLocation.map { String(cString: $0) }
                        continuation.resume(returning: MaiChatHttpResponse(
                            body: data, statusCode: Int(nativeResult.statusCode),
                            redirectLocation: location
                        ))
                    }
                }
            } onCancel: {
                cancellation.cancel()
            }
            try Task.checkCancellation()
            // 只解析标准重定向状态；缺少 Location 时把原状态交给业务层处理。
            guard followSafeGetRedirects,
                  [301, 302, 303, 307, 308].contains(response.statusCode),
                  let location = response.redirectLocation else { return response }
            guard redirectCount < 5,
                  let nextURL = URL(string: location, relativeTo: requestURL)?.absoluteURL,
                  let next = URLComponents(url: nextURL, resolvingAgainstBaseURL: false),
                  next.scheme?.lowercased() == components.scheme?.lowercased(),
                  next.host?.isEmpty == false,
                  next.user == nil, next.password == nil, next.fragment == nil else {
                throw MaiChatHttpClientError.invalidResponse
            }
            // 下一跳重新进入 C++，重新解析该主机的 iOS 系统代理并验证 TLS 域名。
            currentURL = nextURL
        }
        throw MaiChatHttpClientError.invalidResponse
    }
}
