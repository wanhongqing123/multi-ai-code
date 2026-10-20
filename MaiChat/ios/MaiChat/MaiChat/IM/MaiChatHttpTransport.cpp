// 文件作用：让 iOS 自有头像和旧模型凭据共用 libcurl/OpenSSL 请求通道。
// 安全边界：HTTPS 校验证书和域名；代理无效即停止，不跳转或重试带凭据请求。
// 资源边界：响应按调用方上限接收，取消标记终止传输。
#include "MaiChatHttpTransport.h"
#include "MaiChatSystemProxy.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>

#include <curl/curl.h>
#include <strings.h>

struct MaiChatHttpCancellation {
    std::atomic<bool> canceled{false};
};

namespace {

constexpr size_t kMaximumUrlBytes = 4096;
constexpr size_t kMaximumJsonRequestBytes = 96 * 1024;
constexpr size_t kMaximumAvatarRequestBytes = 2 * 1000 * 1000;
constexpr size_t kMaximumSupportedResponseBytes = 4 * 1024 * 1024;
constexpr size_t kMaximumHeaderBytes = 16 * 1024;
constexpr size_t kMaximumRedirectLocationBytes = 4096;

struct ResponseCollector {
    // 只按收到的字节逐步扩容；任何 Content-Length 或实际响应都不能突破调用方上限。
    uint8_t* bytes = nullptr;
    size_t length = 0;
    size_t capacity = 0;
    size_t maximumBytes = 0;
    size_t headerBytes = 0;
    char redirectLocation[kMaximumRedirectLocationBytes + 1] = {};
    size_t redirectLocationLength = 0;
    bool bodyExceeded = false;
    bool headerExceeded = false;
    bool allocationFailed = false;
};

size_t receiveBody(char* bytes, size_t size, size_t count, void* context) {
    auto* collector = static_cast<ResponseCollector*>(context);
    if (size != 0 && count > SIZE_MAX / size) {
        collector->bodyExceeded = true;
        return 0;
    }
    const size_t length = size * count;
    if (length > collector->maximumBytes - collector->length) {
        collector->bodyExceeded = true;
        return 0;
    }
    const size_t required = collector->length + length;
    // 接收回调跨越 libcurl 的 C 栈，不得抛 C++ 异常；扩容失败以写回调返回 0 结束。
    if (required > collector->capacity) {
        const size_t grown = collector->capacity == 0 ? 4096 : collector->capacity * 2;
        const size_t nextCapacity = required > grown ? required :
            (grown > collector->maximumBytes ? collector->maximumBytes : grown);
        auto* storage = static_cast<uint8_t*>(std::realloc(collector->bytes, nextCapacity));
        if (storage == nullptr) {
            collector->allocationFailed = true;
            return 0;
        }
        collector->bytes = storage;
        collector->capacity = nextCapacity;
    }
    if (length > 0) std::memcpy(collector->bytes + collector->length, bytes, length);
    collector->length = required;
    return length;
}

size_t receiveHeader(char* bytes, size_t size, size_t count, void* context) {
    auto* collector = static_cast<ResponseCollector*>(context);
    if (size != 0 && count > SIZE_MAX / size) {
        collector->headerExceeded = true;
        return 0;
    }
    const size_t length = size * count;
    if (length > kMaximumHeaderBytes - collector->headerBytes) {
        collector->headerExceeded = true;
        return 0;
    }
    collector->headerBytes += length;
    if (length >= 5 && std::memcmp(bytes, "HTTP/", 5) == 0) {
        collector->redirectLocationLength = 0;
        collector->redirectLocation[0] = '\0';
    }
    // 只记录 Location 值，由 Swift 逐跳重新检查协议和系统代理后决定是否继续。
    constexpr size_t kHeaderNameLength = 9;
    if (length >= kHeaderNameLength && strncasecmp(bytes, "Location:", kHeaderNameLength) == 0) {
        size_t start = kHeaderNameLength;
        while (start < length && (bytes[start] == ' ' || bytes[start] == '\t')) ++start;
        size_t end = length;
        while (end > start && (bytes[end - 1] == '\r' || bytes[end - 1] == '\n' ||
                               bytes[end - 1] == ' ' || bytes[end - 1] == '\t')) --end;
        if (end - start > kMaximumRedirectLocationBytes) {
            collector->headerExceeded = true;
            return 0;
        }
        collector->redirectLocationLength = end - start;
        std::memcpy(collector->redirectLocation, bytes + start, collector->redirectLocationLength);
        collector->redirectLocation[collector->redirectLocationLength] = '\0';
    }
    return length;
}

int reportProgress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* cancellation = static_cast<MaiChatHttpCancellation*>(context);
    return cancellation->canceled.load(std::memory_order_relaxed) ? 1 : 0;
}

bool printableHeaderValue(const char* value) {
    if (value == nullptr || std::strlen(value) > 8192) return false;
    for (const unsigned char* next = reinterpret_cast<const unsigned char*>(value); *next != 0; ++next) {
        if (*next < 33 || *next > 126) return false;
    }
    return true;
}

// 使用 curl URL 解析器校验目标；原 URL 保持不变，避免破坏头像 CDN 的签名查询串。
bool validUrl(const char* value, bool* secure) {
    if (value == nullptr || std::strlen(value) > kMaximumUrlBytes || std::strpbrk(value, "\r\n") != nullptr)
        return false;
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
    if (parsed == nullptr || curl_url_set(parsed.get(), CURLUPART_URL, value, 0) != CURLUE_OK) return false;
    char* scheme = nullptr;
    char* host = nullptr;
    char* user = nullptr;
    char* password = nullptr;
    char* fragment = nullptr;
    const bool hasScheme = curl_url_get(parsed.get(), CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK;
    const bool hasHost = curl_url_get(parsed.get(), CURLUPART_HOST, &host, 0) == CURLUE_OK;
    const bool hasUser = curl_url_get(parsed.get(), CURLUPART_USER, &user, 0) == CURLUE_OK;
    const bool hasPassword = curl_url_get(parsed.get(), CURLUPART_PASSWORD, &password, 0) == CURLUE_OK;
    const bool hasFragment = curl_url_get(parsed.get(), CURLUPART_FRAGMENT, &fragment, 0) == CURLUE_OK;
    const bool accepted = hasScheme && hasHost && host[0] != '\0' && !hasUser && !hasPassword && !hasFragment &&
                          (std::strcmp(scheme, "https") == 0 || std::strcmp(scheme, "http") == 0);
    if (accepted) *secure = std::strcmp(scheme, "https") == 0;
    curl_free(scheme);
    curl_free(host);
    curl_free(user);
    curl_free(password);
    curl_free(fragment);
    return accepted;
}

bool validateRequest(const char* url, const char* method, const char* bearer, const char* contentType,
                     const uint8_t* requestBody, size_t requestLength, size_t maximumResponseBytes,
                     long timeoutMilliseconds, const char* caBundlePath) {
    bool secure = false;
    if (!validUrl(url, &secure) || method == nullptr || !printableHeaderValue(bearer) ||
        !printableHeaderValue(contentType) || maximumResponseBytes == 0 ||
        maximumResponseBytes > kMaximumSupportedResponseBytes || timeoutMilliseconds < 1000 ||
        timeoutMilliseconds > 900000 || (secure && (caBundlePath == nullptr || caBundlePath[0] == '\0')))
        return false;
    const bool get = std::strcmp(method, "GET") == 0;
    const bool post = std::strcmp(method, "POST") == 0;
    const bool put = std::strcmp(method, "PUT") == 0;
    if (!get && !post && !put) return false;
    // 历史头像可以使用公开 HTTP 地址；任何凭据或 JSON 写入都必须使用 HTTPS。
    if (!secure && (!get || bearer[0] != '\0')) return false;
    if (get) return requestLength == 0 && contentType[0] == '\0';
    if (put) return bearer[0] == '\0' && requestBody != nullptr &&
        requestLength > 0 && requestLength <= kMaximumAvatarRequestBytes &&
        (std::strcmp(contentType, "image/jpeg") == 0 ||
         std::strcmp(contentType, "image/png") == 0);
    return requestLength <= kMaximumJsonRequestBytes &&
           (requestLength == 0 || requestBody != nullptr) &&
           std::strcmp(contentType, "application/json") == 0;
}

bool addHeader(curl_slist** headers, const std::string& header) {
    curl_slist* next = curl_slist_append(*headers, header.c_str());
    if (next == nullptr) return false;
    *headers = next;
    return true;
}

bool configureCurl(CURL* curl, const char* url, const char* method, const char* bearer,
                   const char* contentType, const uint8_t* requestBody, size_t requestLength,
                   size_t maximumResponseBytes, long timeoutMilliseconds, const char* caBundlePath,
                   const maiChatNetwork::SystemProxy& proxy, MaiChatHttpCancellation* cancellation,
                   ResponseCollector* collector, curl_slist** headers) {
    CURLcode setup = CURLE_OK;
    auto set = [&](CURLoption option, auto value) {
        if (setup == CURLE_OK) setup = curl_easy_setopt(curl, option, value);
    };
    collector->maximumBytes = maximumResponseBytes;
    // 先配置安全边界和读取回调；任何选项失败都不能发送请求。
    set(CURLOPT_URL, url);
    set(CURLOPT_PROTOCOLS_STR, "http,https");
    set(CURLOPT_FOLLOWLOCATION, 0L);
    set(CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    set(CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    set(CURLOPT_TIMEOUT_MS, timeoutMilliseconds);
    set(CURLOPT_LOW_SPEED_LIMIT, 1L);
    set(CURLOPT_LOW_SPEED_TIME, 20L);
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_SSL_VERIFYPEER, 1L);
    set(CURLOPT_SSL_VERIFYHOST, 2L);
    set(CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    set(CURLOPT_ACCEPT_ENCODING, "identity");
    set(CURLOPT_WRITEFUNCTION, receiveBody);
    set(CURLOPT_WRITEDATA, collector);
    set(CURLOPT_HEADERFUNCTION, receiveHeader);
    set(CURLOPT_HEADERDATA, collector);
    set(CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(maximumResponseBytes));
    // 进度回调负责传递任务取消，不依赖 Swift 主线程轮询连接状态。
    set(CURLOPT_XFERINFOFUNCTION, reportProgress);
    set(CURLOPT_XFERINFODATA, cancellation);
    set(CURLOPT_NOPROGRESS, 0L);
    if (caBundlePath != nullptr && caBundlePath[0] != '\0') set(CURLOPT_CAINFO, caBundlePath);
    if (setup != CURLE_OK || !maiChatNetwork::configureSystemProxy(curl, proxy)) return false;

    if (std::strcmp(method, "POST") == 0) {
        set(CURLOPT_POST, 1L);
        set(CURLOPT_POSTFIELDS, requestLength == 0 ? static_cast<const void*>("") : requestBody);
        set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(requestLength));
    } else if (std::strcmp(method, "PUT") == 0) {
        set(CURLOPT_CUSTOMREQUEST, "PUT");
        set(CURLOPT_POSTFIELDS, requestBody);
        set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(requestLength));
    } else {
        set(CURLOPT_HTTPGET, 1L);
    }
    // 只发送白名单内的请求头；带令牌的 JSON POST 不允许跳转。
    const bool validHeaders = addHeader(headers, "Expect:") &&
                              (contentType[0] == '\0' ||
                               addHeader(headers, std::string("Content-Type: ") + contentType)) &&
                              (bearer[0] == '\0' ||
                               addHeader(headers, std::string("Authorization: Bearer ") + bearer));
    if (!validHeaders) return false;
    set(CURLOPT_HTTPHEADER, *headers);
    return setup == CURLE_OK;
}

}  // namespace

extern "C" MaiChatHttpCancellation* maiChatHttpCreateCancellation(void) {
    return new (std::nothrow) MaiChatHttpCancellation();
}

extern "C" void maiChatHttpCancel(MaiChatHttpCancellation* cancellation) {
    if (cancellation != nullptr) cancellation->canceled.store(true, std::memory_order_relaxed);
}

extern "C" void maiChatHttpFreeCancellation(MaiChatHttpCancellation* cancellation) {
    delete cancellation;
}

// 请求在 Swift 指定的后台队列执行；调用返回前释放 headers 和 easy handle。
extern "C" void maiChatHttpPerform(const char* url, const char* method, const char* bearer,
                                    const char* contentType, const uint8_t* requestBody, size_t requestLength,
                                    size_t maximumResponseBytes, long timeoutMilliseconds, const char* caBundlePath,
                                    MaiChatHttpCancellation* cancellation, MaiChatHttpResult* result) {
    if (result == nullptr) return;
    *result = {};
    // 先做请求与协议校验，再检查取消；出错时不创建网络连接。
    if (cancellation == nullptr || !validateRequest(url, method, bearer, contentType, requestBody, requestLength,
                                                    maximumResponseBytes, timeoutMilliseconds, caBundlePath)) {
        result->transportCode = -1;
        return;
    }
    if (cancellation->canceled.load(std::memory_order_relaxed)) {
        result->transportCode = -6;
        return;
    }
    maiChatNetwork::SystemProxy proxy;
    // 每个目标地址（包括头像重定向后的目标）重新解析一次 iOS 当前代理设置。
    if (!maiChatNetwork::selectSystemProxy(url, &proxy)) {
        result->transportCode = -5;
        return;
    }
    static std::once_flag initialization;
    static CURLcode initializationResult = CURLE_FAILED_INIT;
    std::call_once(initialization, [] { initializationResult = curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (initializationResult != CURLE_OK) {
        result->transportCode = -3;
        return;
    }
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (curl == nullptr) {
        result->transportCode = -3;
        return;
    }
    ResponseCollector collector;
    curl_slist* headers = nullptr;
    const bool configured = configureCurl(curl.get(), url, method, bearer, contentType, requestBody,
                                          requestLength, maximumResponseBytes, timeoutMilliseconds,
                                          caBundlePath, proxy, cancellation, &collector, &headers);
    if (!configured) {
        curl_slist_free_all(headers);
        result->transportCode = -4;
        return;
    }
    const CURLcode transfer = curl_easy_perform(curl.get());
    const CURLcode statusLookup = curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &result->statusCode);
    curl_slist_free_all(headers);
    // 非 2xx 的 JSON POST 如果只因正文过大中止，保留可信 HTTP 状态供业务判断 401/503。
    // 握手、连接、响应头和内存错误不能伪装为服务端响应；成功 2xx 仍按大小限制报错。
    const bool responseTooLarge = collector.bodyExceeded || transfer == CURLE_FILESIZE_EXCEEDED;
    const bool receivedHttpError = std::strcmp(method, "POST") == 0 && statusLookup == CURLE_OK &&
                                   result->statusCode >= 400 && result->statusCode <= 599;
    const bool sizeOnlyFailure = transfer == CURLE_FILESIZE_EXCEEDED ||
                                 (collector.bodyExceeded && transfer == CURLE_WRITE_ERROR);
    const bool keepErrorStatus = receivedHttpError && responseTooLarge && sizeOnlyFailure &&
                                 !collector.headerExceeded && !collector.allocationFailed;
    result->transportCode = keepErrorStatus ? 0 :
        (collector.headerExceeded || responseTooLarge ? -2 :
         (collector.allocationFailed ? -4 : static_cast<int>(transfer)));
    if (keepErrorStatus) {
        std::free(collector.bytes);
        collector.bytes = nullptr;
        collector.length = 0;
    }
    // Location 只作为下一跳的候选地址返回；此层始终禁止 libcurl 自动跟随跳转。
    if (result->transportCode == 0 && collector.redirectLocationLength > 0) {
        result->redirectLocation = static_cast<char*>(std::malloc(collector.redirectLocationLength + 1));
        if (result->redirectLocation == nullptr) result->transportCode = -4;
        else std::memcpy(result->redirectLocation, collector.redirectLocation,
                         collector.redirectLocationLength + 1);
    }
    // 成功时把缓冲区所有权移给 Swift；失败时本层释放，避免取消或超限后泄漏。
    if (result->transportCode == 0) {
        result->responseBody = collector.bytes;
        result->responseLength = collector.length;
    } else {
        std::free(collector.bytes);
    }
}

extern "C" int maiChatHttpProxyForURL(const char* url, char* result, size_t resultCapacity) {
    if (url == nullptr || result == nullptr || resultCapacity == 0) return 0;
    result[0] = '\0';
    maiChatNetwork::SystemProxy route;
    if (!maiChatNetwork::selectSystemProxy(url, &route)) return 0;
    if (route.direct) return 1;
    if (route.type != CURLPROXY_HTTP) return 0;
    const int count = std::snprintf(result, resultCapacity, "http://%s:%ld/", route.host, route.port);
    return count > 0 && static_cast<size_t>(count) < resultCapacity ? 1 : 0;
}

extern "C" void maiChatHttpFreeResult(MaiChatHttpResult* result) {
    if (result == nullptr) return;
    std::free(result->responseBody);
    std::free(result->redirectLocation);
    *result = {};
}
