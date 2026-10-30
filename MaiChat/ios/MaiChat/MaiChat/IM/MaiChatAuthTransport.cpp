// 文件作用：使用项目已有的 OpenSSL 版 libcurl 发送 iOS 账号服务的 GET/POST 请求。
// 安全边界：HTTPS 只信任 App 随包 CA，校验域名与证书；不跟随跳转，也不重试写入请求。
// 内存边界：响应最多 1 MiB，容纳分页前的完整好友列表；不记录账号令牌。
#include "MaiChatAuthTransport.h"
#include "MaiChatSystemProxy.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <string>
#include <thread>

#include <curl/curl.h>

namespace {

constexpr size_t kMaximumResponseBytes = 1024 * 1024;
constexpr size_t kMaximumHeaderBytes = 16 * 1024;
constexpr size_t kMaximumRequestBytes = 32 * 1024;
constexpr long kConnectTimeoutMilliseconds = 10 * 1000;
constexpr long kTotalTimeoutMilliseconds = 30 * 1000;
constexpr long kReadStallTimeoutSeconds = 20;

enum class AuthRoute { System, AliDoh, CloudflareDoh };

// 缓存只复用同一系统代理配置下验证过的线路，网络/代理切换会重新探测。
std::mutex routeMutex;
std::string cachedProxyKey;
AuthRoute cachedRoute = AuthRoute::System;
std::chrono::steady_clock::time_point routeExpiry;
#ifdef MAICHAT_AUTH_ROUTE_TEST
// 测试只替换网络结果，正式构建不暴露线路或业务发送的注入入口。
bool (*probeHook)(AuthRoute) = nullptr;
CURLcode (*businessHook)() = nullptr;
const char* testDohUrl = nullptr;
curl_slist* testConnectTo = nullptr;
#endif

struct ResponseCollector {
    uint8_t* bytes = nullptr;
    size_t length = 0;
    size_t headerLength = 0;
    bool limitExceeded = false;
};

size_t receiveResponse(char* contents, size_t size, size_t count, void* context) {
    auto* collector = static_cast<ResponseCollector*>(context);
    if (size != 0 && count > SIZE_MAX / size) {
        collector->limitExceeded = true;
        return 0;
    }
    const size_t length = size * count;
    if (length > kMaximumResponseBytes - collector->length) {
        collector->limitExceeded = true;
        return 0;
    }
    if (length > 0) {
        std::memcpy(collector->bytes + collector->length, contents, length);
        collector->length += length;
    }
    return length;
}

size_t receiveHeader(char*, size_t size, size_t count, void* context) {
    auto* collector = static_cast<ResponseCollector*>(context);
    if (size != 0 && count > SIZE_MAX / size) {
        collector->limitExceeded = true;
        return 0;
    }
    const size_t length = size * count;
    if (length > kMaximumHeaderBytes - collector->headerLength) {
        collector->limitExceeded = true;
        return 0;
    }
    collector->headerLength += length;
    return length;
}

bool containsHeaderSeparator(const char* value) {
    return value != nullptr && (std::strchr(value, '\r') != nullptr || std::strchr(value, '\n') != nullptr);
}

bool appendHeader(curl_slist** headers, const char* value) {
    curl_slist* updated = curl_slist_append(*headers, value);
    if (updated == nullptr) return false;
    *headers = updated;
    return true;
}

bool isValidRequest(const char* url, const char* method, const char* bearer, const uint8_t* body, size_t bodyLength,
                    const char* caBundlePath) {
    if (url == nullptr || method == nullptr || containsHeaderSeparator(bearer)) return false;
    if (bearer != nullptr) {
        if (std::strlen(bearer) > 8192) return false;
        for (const unsigned char* character = reinterpret_cast<const unsigned char*>(bearer); *character != 0;
             ++character) {
            if (*character < 33 || *character > 126) return false;
        }
    }
    constexpr char kProductionBase[] = "https://ichat.life/api/v1/";
    constexpr char kLocalBase[] = "http://127.0.0.1:";
    const bool secure = std::strncmp(url, kProductionBase, sizeof(kProductionBase) - 1) == 0;
    const bool localHttp =
        std::strncmp(url, kLocalBase, sizeof(kLocalBase) - 1) == 0 && std::strstr(url, "/v1/") != nullptr;
    if (!secure && !localHttp) return false;
    const char* query = std::strchr(url, '?');
    if (std::strlen(url) > 2048 || std::strstr(url, "..") != nullptr ||
        std::strpbrk(url, "#@") != nullptr || query != nullptr)
        return false;
    if (secure && (caBundlePath == nullptr || caBundlePath[0] == '\0')) return false;
    if (bodyLength > kMaximumRequestBytes || (bodyLength > 0 && body == nullptr)) return false;
    if (std::strcmp(method, "GET") == 0) return body == nullptr && bodyLength == 0;
    return std::strcmp(method, "POST") == 0;
}

// 每个 setopt 都核对返回值；任何配置失败都发生在发送 HTTP 请求之前。
bool configureRequest(CURL* curl, const char* url, const char* method, const uint8_t* body, size_t bodyLength,
                      const char* caBundlePath, const maiChatNetwork::SystemProxy& proxy,
                      ResponseCollector* collector, AuthRoute route = AuthRoute::System, long probeTimeoutMs = 0) {
    CURLcode setup = CURLE_OK;
    auto set = [&](CURLoption option, auto value) {
        if (setup == CURLE_OK) setup = curl_easy_setopt(curl, option, value);
    };
    set(CURLOPT_URL, url);
    set(CURLOPT_PROTOCOLS_STR, "http,https");
    set(CURLOPT_FOLLOWLOCATION, 0L);
    set(CURLOPT_MAXREDIRS, 0L);
    set(CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    set(CURLOPT_FRESH_CONNECT, 1L);
    set(CURLOPT_FORBID_REUSE, 1L);
    set(CURLOPT_SSL_SESSIONID_CACHE, 0L);
    set(CURLOPT_CONNECTTIMEOUT_MS, probeTimeoutMs > 0 ? probeTimeoutMs : kConnectTimeoutMilliseconds);
    set(CURLOPT_TIMEOUT_MS, probeTimeoutMs > 0 ? probeTimeoutMs : kTotalTimeoutMilliseconds);
    set(CURLOPT_LOW_SPEED_LIMIT, 1L);
    set(CURLOPT_LOW_SPEED_TIME, kReadStallTimeoutSeconds);
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_SSL_VERIFYPEER, 1L);
    set(CURLOPT_SSL_VERIFYHOST, 2L);
    set(CURLOPT_WRITEFUNCTION, receiveResponse);
    set(CURLOPT_WRITEDATA, collector);
    set(CURLOPT_HEADERFUNCTION, receiveHeader);
    set(CURLOPT_HEADERDATA, collector);
    set(CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(kMaximumResponseBytes));
    set(CURLOPT_ACCEPT_ENCODING, "identity");
    if (caBundlePath != nullptr) set(CURLOPT_CAINFO, caBundlePath);
    if (setup != CURLE_OK || !maiChatNetwork::configureSystemProxy(curl, proxy)) return false;
    if (route != AuthRoute::System) {
        // 只对账号域名使用 DoH，HTTPS URL、SNI 和证书校验仍使用原始域名。
        // 用户要求代理失效时可用直连备用；探测证明可用后才发送一次正式请求。
        set(CURLOPT_PROXY, "");
        set(CURLOPT_NOPROXY, "*");
        set(CURLOPT_DOH_URL, route == AuthRoute::AliDoh ?
            "https://223.5.5.5/dns-query" : "https://1.1.1.1/dns-query");
        set(CURLOPT_DOH_SSL_VERIFYPEER, 1L);
        set(CURLOPT_DOH_SSL_VERIFYHOST, 2L);
#ifdef MAICHAT_AUTH_ROUTE_TEST
        // 本机 TLS/DoH fixture 的端口由测试注入；生产构建没有这些覆盖项。
        if (testDohUrl) set(CURLOPT_DOH_URL, testDohUrl);
        if (testConnectTo) set(CURLOPT_CONNECT_TO, testConnectTo);
#endif
    }
    if (std::strcmp(method, "POST") == 0) {
        set(CURLOPT_POST, 1L);
        set(CURLOPT_POSTFIELDS, bodyLength == 0 ? static_cast<const void*>("") : body);
        set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(bodyLength));
    } else {
        set(CURLOPT_HTTPGET, 1L);
    }
    return setup == CURLE_OK;
}

bool buildHeaders(curl_slist** headers, const char* method, const char* bearer) {
    bool ready = appendHeader(headers, "Accept: application/json") &&
                 appendHeader(headers, "Cache-Control: no-store") && appendHeader(headers, "Expect:");
    if (ready && std::strcmp(method, "POST") == 0) ready = appendHeader(headers, "Content-Type: application/json");
    if (bearer == nullptr || bearer[0] == '\0') return ready;
    constexpr char kPrefix[] = "Authorization: Bearer ";
    const size_t length = sizeof(kPrefix) + std::strlen(bearer);
    auto* authorization = static_cast<char*>(std::malloc(length));
    if (authorization == nullptr) return false;
    std::memcpy(authorization, kPrefix, sizeof(kPrefix) - 1);
    std::strcpy(authorization + sizeof(kPrefix) - 1, bearer);
    if (ready) ready = appendHeader(headers, authorization);
    std::free(authorization);
    return ready;
}

// 探测只访问未携带令牌的账号接口，不能重放登录、验证码或刷新等写请求。
bool probeRoute(AuthRoute route, const maiChatNetwork::SystemProxy& proxy, const std::string& caPath,
                long timeoutMs) {
#ifdef MAICHAT_AUTH_ROUTE_TEST
    if (probeHook) return probeHook(route);
#endif
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    ResponseCollector collector;
    collector.bytes = static_cast<uint8_t*>(std::malloc(kMaximumResponseBytes));
    bool success = false;
    if (collector.bytes && configureRequest(curl, "https://ichat.life/api/v1/auth/me", "GET", nullptr, 0,
                                             caPath.c_str(), proxy, &collector, route, timeoutMs)) {
        const CURLcode code = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        success = code == CURLE_OK && !collector.limitExceeded && status == 401 && collector.length > 0;
    }
    std::free(collector.bytes);
    curl_easy_cleanup(curl);
    return success;
}

struct ProbeState {
    std::mutex mutex;
    std::condition_variable changed;
    int completed = 0;
    bool available = false;
    AuthRoute route = AuthRoute::System;
};

// DNS 清理可能超过 curl 超时：后台探测只持有拷贝，超时返回后不阻塞备用线路。
std::shared_ptr<ProbeState> launchProbes(const maiChatNetwork::SystemProxy& proxy,
                                         const std::string& caPath, bool system) {
    auto state = std::make_shared<ProbeState>();
    const auto start = [state, proxy, caPath](AuthRoute route, long timeout) {
        std::thread([state, proxy, caPath, route, timeout] {
            const bool ok = probeRoute(route, proxy, caPath, timeout);
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                ++state->completed;
                if (ok && !state->available) {
                    state->available = true;
                    state->route = route;
                }
            }
            state->changed.notify_one();
        }).detach();
    };
    if (system) start(AuthRoute::System, 2500);
    else {
        start(AuthRoute::AliDoh, 7000);
        start(AuthRoute::CloudflareDoh, 7000);
    }
    return state;
}

bool waitForProbe(const std::shared_ptr<ProbeState>& state, int count, long timeout, AuthRoute* route) {
    std::unique_lock<std::mutex> lock(state->mutex);
    state->changed.wait_for(lock, std::chrono::milliseconds(timeout),
                            [&] { return state->available || state->completed == count; });
    if (!state->available) return false;
    *route = state->route;
    return true;
}

bool chooseRoute(const maiChatNetwork::SystemProxy& proxy, bool proxyValid,
                 const std::string& caPath, AuthRoute* route) {
    const std::string key = proxyValid ?
        std::to_string(proxy.direct) + ":" + std::to_string(proxy.type) + ":" + proxy.host + ":" +
            std::to_string(proxy.port) : "unavailable";
    // 串行化选路，避免并发账号请求同时重复启动探测。
    std::lock_guard<std::mutex> lock(routeMutex);
    if (key == cachedProxyKey && std::chrono::steady_clock::now() < routeExpiry) {
        *route = cachedRoute;
        return true;
    }
    bool ok = proxyValid && waitForProbe(launchProbes(proxy, caPath, true), 1, 2500, route);
    if (!ok) ok = waitForProbe(launchProbes(proxy, caPath, false), 2, 7000, route);
    if (!ok) return false;
    cachedProxyKey = key;
    cachedRoute = *route;
    routeExpiry = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    return true;
}

}  // namespace

// 一次调用完成参数验证、系统代理选择、libcurl 初始化、传输和资源释放。
// 生产服务先用无凭据请求选路；业务 POST 使用全新连接且禁自动跳转，不进行应用层重试。
// 成功响应的数据所有权交给调用方，失败时 body 保持空值并通过 transportCode 报告。
extern "C" void maiChatAuthHttpPerform(const char* url, const char* method, const char* bearer,
                                       const uint8_t* requestBody, size_t requestLength, const char* caBundlePath,
                                       MaiChatAuthHttpResult* result) {
    if (result == nullptr) return;
    *result = {};
    if (!isValidRequest(url, method, bearer, requestBody, requestLength, caBundlePath)) {
        result->transportCode = -1;
        return;
    }
    // 先尝试系统代理；用户要求系统线路失败时允许账号服务走直连备选。
    maiChatNetwork::SystemProxy proxy;
    const bool proxyValid = maiChatNetwork::selectSystemProxy(url, &proxy);
    // libcurl 是进程级组件，只初始化一次；每个账号请求仍使用独立 easy handle。
    static std::once_flag curlInitialization;
    static CURLcode initializationResult = CURLE_FAILED_INIT;
    std::call_once(curlInitialization, [] { initializationResult = curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (initializationResult != CURLE_OK) {
        result->transportCode = -3;
        return;
    }
    AuthRoute route = AuthRoute::System;
    const bool production = std::strncmp(url, "https://ichat.life/api/v1/", 26) == 0;
    if (production && !chooseRoute(proxy, proxyValid, caBundlePath, &route)) {
        result->transportCode = static_cast<int>(CURLE_COULDNT_CONNECT);
        return;
    }
    if (!production && !proxyValid) {
        result->transportCode = -5;
        return;
    }
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        result->transportCode = -3;
        return;
    }
    // 在发送前分配固定上限的响应区，接收回调不会发生无上限内存增长。
    ResponseCollector collector;
    collector.bytes = static_cast<uint8_t*>(std::malloc(kMaximumResponseBytes));
    if (collector.bytes == nullptr) {
        result->transportCode = -4;
        curl_easy_cleanup(curl);
        return;
    }
    const bool configured =
        configureRequest(curl, url, method, requestBody, requestLength, caBundlePath, proxy, &collector, route);
    curl_slist* headers = nullptr;
    // 所有选项与 Header 设置成功才发送；Bearer 不进入错误字符串或日志。
    if (configured && buildHeaders(&headers, method, bearer) &&
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) == CURLE_OK) {
#ifdef MAICHAT_AUTH_ROUTE_TEST
        const CURLcode transferResult = businessHook ? businessHook() : curl_easy_perform(curl);
#else
        const CURLcode transferResult = curl_easy_perform(curl);
#endif
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result->statusCode);
        result->transportCode = collector.limitExceeded ? -2 : static_cast<int>(transferResult);
        if (production && transferResult != CURLE_OK) {
            // 请求可能已到达服务器；只让下次请求重新选路，绝不重放本次 POST。
            std::lock_guard<std::mutex> lock(routeMutex);
            routeExpiry = {};
        }
        if (result->transportCode == 0 && collector.length > 0) {
            result->responseBody = collector.bytes;
            result->responseLength = collector.length;
            collector.bytes = nullptr;
        }
    } else {
        result->transportCode = -4;
    }
    // 成功时 collector.bytes 已转移给 result，其余路径在本函数释放。
    std::free(collector.bytes);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

extern "C" void maiChatAuthHttpFreeResult(MaiChatAuthHttpResult* result) {
    if (result == nullptr) return;
    std::free(result->responseBody);
    *result = {};
}
