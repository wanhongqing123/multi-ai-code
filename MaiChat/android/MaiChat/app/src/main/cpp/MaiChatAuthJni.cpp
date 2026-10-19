// 文件作用：用 App 已链接的 libcurl/OpenSSL 执行 Android 自有的一次性 HTTP 请求。
// 账号、头像和云凭据共用受限的传输核心；各自的 URL 和业务错误仍由 Java 层处理。
// 传输核心负责系统 CA/代理参数、响应上限、超时和禁止 POST 自动重放。
#include <curl/curl.h>
#include <jni.h>

#include <cstring>
#include <exception>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>
#include <strings.h>

namespace {

constexpr size_t kMaxBodyBytes = 1024 * 1024;
constexpr size_t kMaxHeaderBytes = 16 * 1024;
constexpr size_t kMaxRequestBytes = 96 * 1024;
constexpr size_t kMaxCaBytes = 2 * 1024 * 1024;
constexpr size_t kMaxRedirectLocationBytes = 2048;

enum FailureKind {
    kNoFailure = 0,
    kTimeout = 1,
    kCertificate = 2,
    kTooLarge = 3,
    kSetup = 4,
    kHeaderTooLarge = 5,
};

struct CurlHandle {
    CURL* value = curl_easy_init();
    ~CurlHandle() {
        if (value != nullptr) curl_easy_cleanup(value);
    }
};

struct CurlHeaders {
    curl_slist* value = nullptr;
    ~CurlHeaders() {
        if (value != nullptr) curl_slist_free_all(value);
    }

    bool append(const std::string& line) {
        curl_slist* next = curl_slist_append(value, line.c_str());
        if (next == nullptr) return false;
        value = next;
        return true;
    }
};

struct Transfer {
    const std::vector<unsigned char>* request = nullptr;
    size_t requestOffset = 0;
    size_t headerBytes = 0;
    size_t headerLimit = kMaxHeaderBytes;
    size_t responseLimit = kMaxBodyBytes;
    std::vector<unsigned char> response;
    bool captureRedirectLocation = false;
    int responseStatus = 0;
    std::string redirectLocation;
    FailureKind failure = kNoFailure;
};

std::once_flag gCurlInitOnce;
CURLcode gCurlInitResult = CURLE_FAILED_INIT;

CURLcode initializeCurl() {
    std::call_once(gCurlInitOnce, [] { gCurlInitResult = curl_global_init(CURL_GLOBAL_DEFAULT); });
    return gCurlInitResult;
}

void throwIOException(JNIEnv* env, const char* message) {
    jclass type = env->FindClass("java/io/IOException");
    if (type != nullptr) {
        env->ThrowNew(type, message);
        env->DeleteLocalRef(type);
    }
}

bool copyString(JNIEnv* env, jstring value, std::string* output) {
    if (value == nullptr) return false;
    const char* characters = env->GetStringUTFChars(value, nullptr);
    if (characters == nullptr) return false;
    try {
        output->assign(characters);
    } catch (...) {
        env->ReleaseStringUTFChars(value, characters);
        throw;
    }
    env->ReleaseStringUTFChars(value, characters);
    return true;
}

bool copyBytes(JNIEnv* env, jbyteArray value, size_t maximum, std::vector<unsigned char>* output) {
    if (value == nullptr) return false;
    const jsize length = env->GetArrayLength(value);
    if (length < 0 || static_cast<size_t>(length) > maximum) return false;
    output->resize(static_cast<size_t>(length));
    if (length > 0) env->GetByteArrayRegion(value, 0, length, reinterpret_cast<jbyte*>(output->data()));
    return !env->ExceptionCheck();
}

size_t readRequest(char* destination, size_t size, size_t count, void* opaque) {
    auto* transfer = static_cast<Transfer*>(opaque);
    if (count != 0 && size > SIZE_MAX / count) return CURL_READFUNC_ABORT;
    const size_t capacity = size * count;
    const size_t remaining = transfer->request->size() - transfer->requestOffset;
    const size_t length = capacity < remaining ? capacity : remaining;
    if (length > 0) {
        std::memcpy(destination, transfer->request->data() + transfer->requestOffset, length);
        transfer->requestOffset += length;
    }
    return length;
}

size_t receiveBody(char* source, size_t size, size_t count, void* opaque) {
    auto* transfer = static_cast<Transfer*>(opaque);
    if (count != 0 && size > SIZE_MAX / count) {
        transfer->failure = kTooLarge;
        return 0;
    }
    const size_t length = size * count;
    if (length > transfer->responseLimit - transfer->response.size()) {
        transfer->failure = kTooLarge;
        return 0;
    }
    try {
        transfer->response.insert(transfer->response.end(), source, source + length);
    } catch (const std::bad_alloc&) {
        transfer->failure = kSetup;
        return 0;
    }
    return length;
}

size_t receiveHeader(char* source, size_t size, size_t count, void* opaque) {
    auto* transfer = static_cast<Transfer*>(opaque);
    if (count != 0 && size > SIZE_MAX / count) {
        transfer->failure = kHeaderTooLarge;
        return 0;
    }
    const size_t length = size * count;
    if (length > transfer->headerLimit - transfer->headerBytes) {
        transfer->failure = kHeaderTooLarge;
        return 0;
    }
    transfer->headerBytes += length;
    if (transfer->captureRedirectLocation && length >= 8 && std::memcmp(source, "HTTP/", 5) == 0) {
        transfer->redirectLocation.clear();
        transfer->responseStatus = 0;
        const char* status = static_cast<const char*>(std::memchr(source, ' ', length));
        if (status != nullptr && source + length - status >= 4 &&
            status[1] >= '0' && status[1] <= '9' && status[2] >= '0' && status[2] <= '9' &&
            status[3] >= '0' && status[3] <= '9') {
            transfer->responseStatus = (status[1] - '0') * 100 + (status[2] - '0') * 10 + status[3] - '0';
        }
    }
    // Location 只回传给无凭据的头像 GET；Java 按新主机重新查询系统代理后才发下一跳。
    constexpr size_t kHeaderNameLength = 9;
    if (transfer->captureRedirectLocation && transfer->responseStatus >= 300 &&
        transfer->responseStatus <= 399 && length >= kHeaderNameLength &&
        strncasecmp(source, "Location:", kHeaderNameLength) == 0) {
        size_t start = kHeaderNameLength;
        while (start < length && (source[start] == ' ' || source[start] == '\t')) ++start;
        size_t end = length;
        while (end > start && (source[end - 1] == '\r' || source[end - 1] == '\n' ||
                               source[end - 1] == ' ' || source[end - 1] == '\t')) --end;
        if (end - start > kMaxRedirectLocationBytes) {
            transfer->failure = kHeaderTooLarge;
            return 0;
        }
        for (size_t index = start; index < end; ++index) {
            const unsigned char character = static_cast<unsigned char>(source[index]);
            if (character <= 0x20 || character >= 0x7f) {
                transfer->failure = kSetup;
                return 0;
            }
        }
        try {
            transfer->redirectLocation.assign(source + start, source + end);
        } catch (const std::bad_alloc&) {
            transfer->failure = kSetup;
            return 0;
        }
    }
    return length;
}

jobject makeResult(JNIEnv* env, const char* resultClass, jint status,
                   const std::vector<unsigned char>& body, jint curlCode, jint failureKind) {
    jclass resultType = env->FindClass(resultClass);
    if (resultType == nullptr) return nullptr;
    jmethodID constructor = env->GetMethodID(resultType, "<init>", "(I[BII)V");
    if (constructor == nullptr) {
        env->DeleteLocalRef(resultType);
        return nullptr;
    }
    jbyteArray payload = env->NewByteArray(static_cast<jsize>(body.size()));
    if (payload == nullptr) {
        env->DeleteLocalRef(resultType);
        return nullptr;
    }
    if (!body.empty()) {
        env->SetByteArrayRegion(payload, 0, static_cast<jsize>(body.size()),
                                reinterpret_cast<const jbyte*>(body.data()));
    }
    jobject result = env->NewObject(resultType, constructor, status, payload, curlCode, failureKind);
    env->DeleteLocalRef(payload);
    env->DeleteLocalRef(resultType);
    return result;
}

jobject makeAuthResult(JNIEnv* env, jint status, const std::vector<unsigned char>& body,
                       jint curlCode, jint failureKind) {
    return makeResult(env, "com/kongshang/maichat/MaiChatAuthTransport$Result",
                      status, body, curlCode, failureKind);
}

jobject makeHttpResult(JNIEnv* env, jint status, const std::vector<unsigned char>& body,
                       jint curlCode, jint failureKind, const std::string& redirectLocation) {
    jclass resultType = env->FindClass("com/kongshang/maichat/MaiChatHttpTransport$Result");
    if (resultType == nullptr) return nullptr;
    jmethodID constructor = env->GetMethodID(resultType, "<init>", "(I[BIILjava/lang/String;)V");
    if (constructor == nullptr) {
        env->DeleteLocalRef(resultType);
        return nullptr;
    }
    jbyteArray payload = env->NewByteArray(static_cast<jsize>(body.size()));
    jstring location = redirectLocation.empty() ? nullptr : env->NewStringUTF(redirectLocation.c_str());
    if (payload == nullptr || (!redirectLocation.empty() && location == nullptr)) {
        if (payload != nullptr) env->DeleteLocalRef(payload);
        env->DeleteLocalRef(resultType);
        return nullptr;
    }
    if (!body.empty()) env->SetByteArrayRegion(payload, 0, static_cast<jsize>(body.size()),
                                                reinterpret_cast<const jbyte*>(body.data()));
    jobject result = env->NewObject(resultType, constructor, status, payload, curlCode, failureKind, location);
    if (location != nullptr) env->DeleteLocalRef(location);
    env->DeleteLocalRef(payload);
    env->DeleteLocalRef(resultType);
    return result;
}

FailureKind classify(CURLcode code, FailureKind transferFailure) {
    if (transferFailure != kNoFailure) return transferFailure;
    if (code == CURLE_OPERATION_TIMEDOUT) return kTimeout;
    if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CACERT_BADFILE || code == CURLE_SSL_CERTPROBLEM)
        return kCertificate;
    if (code == CURLE_FILESIZE_EXCEEDED) return kTooLarge;
    if (code == CURLE_FAILED_INIT || code == CURLE_BAD_FUNCTION_ARGUMENT || code == CURLE_OUT_OF_MEMORY) return kSetup;
    return kNoFailure;
}

std::string proxyUrl(const std::string& host, jint port, jint type) {
    std::string url = type == 1 ? "http://" : "socks5h://";
    const bool ipv6 = host.find(':') != std::string::npos;
    if (ipv6) url += '[';
    url += host;
    if (ipv6) url += ']';
    url += ':' + std::to_string(port);
    return url;
}

struct RequestOptions {
    size_t responseLimit = kMaxBodyBytes;
    size_t headerLimit = kMaxHeaderBytes;
    long connectTimeoutMs = 10000;
    long totalTimeoutMs = 30000;
    long lowSpeedSeconds = 20;
    bool captureRedirectLocation = false;
    bool accountHeaders = true;
};

struct RequestResult {
    long status = 0;
    CURLcode code = CURLE_OK;
    FailureKind failure = kNoFailure;
    std::vector<unsigned char> body;
    std::string redirectLocation;
};

// 作用：账号、头像和临时凭据共用同一条 curl 传输；差异只来自明确的请求选项。
// 收到超过上限的响应时由回调立即中止，不继续下载或累计到无界内存。
RequestResult performCurl(const std::string& url, const std::string& method,
                          const std::vector<unsigned char>& request, const std::string& bearer,
                          const std::vector<unsigned char>& ca, jint proxyType,
                          const std::string& proxyHost, jint proxyPort, const RequestOptions& options) {
    RequestResult output;
    const CURLcode initial = initializeCurl();
    if (initial != CURLE_OK) return {0, initial, kSetup, {}};
    CurlHandle handle;
    if (handle.value == nullptr) return {0, CURLE_FAILED_INIT, kSetup, {}};
    // 构造固定的请求头：头像不带令牌，JSON 请求按调用方传入的令牌发送。
    // POST 不自动重定向，因此令牌与请求正文不会被转发到另一主机。
    CurlHeaders headers;
    if (options.accountHeaders || method == "POST") {
        if (!headers.append("Accept: application/json") || !headers.append("Cache-Control: no-store"))
            return {0, CURLE_OUT_OF_MEMORY, kSetup, {}};
    }
    if (!headers.append("Expect:")) return {0, CURLE_OUT_OF_MEMORY, kSetup, {}};
    if (method == "POST" && !headers.append("Content-Type: application/json"))
        return {0, CURLE_OUT_OF_MEMORY, kSetup, {}};
    if (!bearer.empty() && !headers.append("Authorization: Bearer " + bearer))
        return {0, CURLE_OUT_OF_MEMORY, kSetup, {}};

    Transfer transfer;
    transfer.request = &request;
    transfer.responseLimit = options.responseLimit;
    transfer.headerLimit = options.headerLimit;
    transfer.captureRedirectLocation = options.captureRedirectLocation;
    // 证书、代理、超时和响应限额必须先全部配置成功，之后才能真正建立连接。
    // CA 字节由 Java 从 Android 当前信任证书读取，curl 在 TLS 握手时校验域名。
    curl_blob caBlob = {const_cast<unsigned char*>(ca.data()), ca.size(), CURL_BLOB_COPY};
    CURLcode setup = CURLE_OK;
    auto set = [&](CURLoption option, auto value) {
        if (setup == CURLE_OK) setup = curl_easy_setopt(handle.value, option, value);
    };
    set(CURLOPT_URL, url.c_str());
    set(CURLOPT_HTTPHEADER, headers.value);
    set(CURLOPT_WRITEFUNCTION, receiveBody);
    set(CURLOPT_WRITEDATA, &transfer);
    set(CURLOPT_HEADERFUNCTION, receiveHeader);
    set(CURLOPT_HEADERDATA, &transfer);
    set(CURLOPT_FOLLOWLOCATION, 0L);
    set(CURLOPT_MAXREDIRS, 0L);
    set(CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    set(CURLOPT_PROTOCOLS_STR, "http,https");
    set(CURLOPT_SSL_VERIFYPEER, 1L);
    set(CURLOPT_SSL_VERIFYHOST, 2L);
    set(CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    set(CURLOPT_SSL_SESSIONID_CACHE, 0L);
    set(CURLOPT_FRESH_CONNECT, 1L);
    set(CURLOPT_FORBID_REUSE, 1L);
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_CONNECTTIMEOUT_MS, options.connectTimeoutMs);
    if (options.totalTimeoutMs > 0) set(CURLOPT_TIMEOUT_MS, options.totalTimeoutMs);
    set(CURLOPT_LOW_SPEED_LIMIT, 1L);
    set(CURLOPT_LOW_SPEED_TIME, options.lowSpeedSeconds);
    set(CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(options.responseLimit));
    set(CURLOPT_ACCEPT_ENCODING, "identity");
    if (url.rfind("https://", 0) == 0) set(CURLOPT_CAINFO_BLOB, &caBlob);
    if (proxyType == 0) {
        set(CURLOPT_PROXY, "");
        set(CURLOPT_NOPROXY, "*");
    } else {
        const std::string address = proxyUrl(proxyHost, proxyPort, proxyType);
        set(CURLOPT_PROXY, address.c_str());
        set(CURLOPT_NOPROXY, "");
        if (proxyType == 1 && url.rfind("https://", 0) == 0)
            set(CURLOPT_HTTPPROXYTUNNEL, 1L);
    }
    if (method == "POST") {
        set(CURLOPT_POST, 1L);
        set(CURLOPT_READFUNCTION, readRequest);
        set(CURLOPT_READDATA, &transfer);
        set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.size()));
    } else {
        set(CURLOPT_HTTPGET, 1L);
    }
    if (setup != CURLE_OK) return {0, setup, kSetup, {}};
    // 传输回调一旦达到大小上限就停止接收；随后仍读取已得到的 HTTP 状态，
    // 让业务层能区分令牌被拒绝与成功响应过大。
    output.code = curl_easy_perform(handle.value);
    long status = 0;
    if (curl_easy_getinfo(handle.value, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK &&
        status >= 100 && status <= 599)
        output.status = status;
    if (output.code != CURLE_OK) {
        output.failure = classify(output.code, transfer.failure);
        return output;
    }
    if (output.status == 0) {
        output.code = CURLE_GOT_NOTHING;
        return output;
    }
    output.body = std::move(transfer.response);
    output.redirectLocation = std::move(transfer.redirectLocation);
    return output;
}

}  // namespace

extern "C" JNIEXPORT jobject JNICALL Java_com_kongshang_maichat_MaiChatAuthTransport_nativeExecute(
    JNIEnv* env, jclass, jstring jurl, jstring jmethod, jbyteArray jrequest, jstring jbearer, jbyteArray jca,
    jint proxyType, jstring jproxyHost, jint proxyPort) {
    try {
        std::string url, method, bearer, proxyHost;
        std::vector<unsigned char> request, ca;
        if (!copyString(env, jurl, &url) || !copyString(env, jmethod, &method) || !copyString(env, jbearer, &bearer) ||
            !copyString(env, jproxyHost, &proxyHost) || !copyBytes(env, jca, kMaxCaBytes, &ca) ||
            (jrequest != nullptr && !copyBytes(env, jrequest, kMaxRequestBytes, &request))) {
            if (!env->ExceptionCheck()) throwIOException(env, "账号网络请求数据无效");
            return nullptr;
        }
        if ((method != "GET" && method != "POST") || (method == "GET" && jrequest != nullptr) ||
            (method == "POST" && jrequest == nullptr) || url.size() > 2048 || bearer.size() > 8192 ||
            (proxyType != 0 && proxyType != 1 && proxyType != 2) ||
            (proxyType != 0 && (proxyHost.empty() || proxyPort <= 0 || proxyPort > 65535)) ||
            (url.rfind("https://", 0) == 0 && ca.empty())) {
            throwIOException(env, "账号网络请求数据无效");
            return nullptr;
        }
        RequestOptions options;
        if (url.find("/v1/session-sync/") != std::string::npos)
            options.totalTimeoutMs = 35000;
        const RequestResult result = performCurl(url, method, request, bearer, ca,
                                                 proxyType, proxyHost, proxyPort, options);
        return makeAuthResult(env, static_cast<jint>(result.status), result.body,
                              result.code, result.failure);
    } catch (const std::bad_alloc&) {
        if (!env->ExceptionCheck()) throwIOException(env, "账号网络内存不足");
        return nullptr;
    } catch (const std::exception&) {
        if (!env->ExceptionCheck()) throwIOException(env, "账号网络组件执行失败");
        return nullptr;
    }
}

extern "C" JNIEXPORT jobject JNICALL Java_com_kongshang_maichat_MaiChatHttpTransport_nativeExecute(
    JNIEnv* env, jclass, jstring jurl, jstring jmethod, jbyteArray jrequest, jstring jbearer, jbyteArray jca,
    jint proxyType, jstring jproxyHost, jint proxyPort, jint responseLimit, jint connectTimeoutMs,
    jint totalTimeoutMs, jint lowSpeedSeconds) {
    // 通用入口只接受 Java 已声明的 GET/POST 策略；原生层从不自行跟随重定向。
    // JNI 层再次限制最大值，避免调用方失误导致无限制下载或发送明文 Bearer。
    constexpr size_t kMaximumResponseBytes = 4 * 1024 * 1024;
    try {
        std::string url, method, bearer, proxyHost;
        std::vector<unsigned char> request, ca;
        if (!copyString(env, jurl, &url) || !copyString(env, jmethod, &method) ||
            !copyString(env, jbearer, &bearer) || !copyString(env, jproxyHost, &proxyHost) ||
            !copyBytes(env, jca, kMaxCaBytes, &ca) ||
            (jrequest != nullptr && !copyBytes(env, jrequest, kMaxRequestBytes, &request))) {
            if (!env->ExceptionCheck()) throwIOException(env, "网络请求数据无效");
            return nullptr;
        }
        const bool secure = url.rfind("https://", 0) == 0;
        if ((method != "GET" && method != "POST") || (method == "GET" && jrequest != nullptr) ||
            (method == "POST" && jrequest == nullptr) || url.size() > 2048 || bearer.size() > 8192 ||
            (!secure && url.rfind("http://", 0) != 0) || (secure && ca.empty()) ||
            (!bearer.empty() && (!secure || method != "POST")) ||
            (proxyType != 0 && proxyType != 1 && proxyType != 2) ||
            (proxyType != 0 && (proxyHost.empty() || proxyPort <= 0 || proxyPort > 65535)) ||
            responseLimit <= 0 || static_cast<size_t>(responseLimit) > kMaximumResponseBytes ||
            connectTimeoutMs <= 0 || connectTimeoutMs > 30000 || totalTimeoutMs < 0 ||
            totalTimeoutMs > 60000 || lowSpeedSeconds <= 0 || lowSpeedSeconds > 30) {
            throwIOException(env, "网络请求数据无效");
            return nullptr;
        }
        RequestOptions options;
        options.responseLimit = static_cast<size_t>(responseLimit);
        options.headerLimit = method == "GET" ? 64 * 1024 : kMaxHeaderBytes;
        options.connectTimeoutMs = connectTimeoutMs;
        options.totalTimeoutMs = totalTimeoutMs;
        options.lowSpeedSeconds = lowSpeedSeconds;
        options.captureRedirectLocation = method == "GET";
        options.accountHeaders = false;
        const RequestResult result = performCurl(url, method, request, bearer, ca,
                                                 proxyType, proxyHost, proxyPort, options);
        return makeHttpResult(env, static_cast<jint>(result.status), result.body,
                              result.code, result.failure, result.redirectLocation);
    } catch (const std::bad_alloc&) {
        if (!env->ExceptionCheck()) throwIOException(env, "网络组件内存不足");
        return nullptr;
    } catch (const std::exception&) {
        if (!env->ExceptionCheck()) throwIOException(env, "网络组件执行失败");
        return nullptr;
    }
}
