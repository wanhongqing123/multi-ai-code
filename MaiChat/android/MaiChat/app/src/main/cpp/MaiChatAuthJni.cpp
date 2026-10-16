// 文件作用：把 Android 账号请求交给 App 已链接的 libcurl 执行。
// 只传输字节和 HTTP 状态；账号协议、令牌与错误文案由 Java 层处理。
#include <curl/curl.h>
#include <jni.h>

#include <cstring>
#include <exception>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace {

constexpr size_t kMaxBodyBytes = 64 * 1024;
constexpr size_t kMaxHeaderBytes = 16 * 1024;
constexpr size_t kMaxRequestBytes = 32 * 1024;
constexpr size_t kMaxCaBytes = 2 * 1024 * 1024;

enum FailureKind {
    kNoFailure = 0,
    kTimeout = 1,
    kCertificate = 2,
    kTooLarge = 3,
    kSetup = 4,
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
    std::vector<unsigned char> response;
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
    if (length > kMaxBodyBytes - transfer->response.size()) {
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

size_t receiveHeader(char*, size_t size, size_t count, void* opaque) {
    auto* transfer = static_cast<Transfer*>(opaque);
    if (count != 0 && size > SIZE_MAX / count) {
        transfer->failure = kTooLarge;
        return 0;
    }
    const size_t length = size * count;
    if (length > kMaxHeaderBytes - transfer->headerBytes) {
        transfer->failure = kTooLarge;
        return 0;
    }
    transfer->headerBytes += length;
    return length;
}

jobject makeResult(JNIEnv* env, jint status, const std::vector<unsigned char>& body, jint curlCode, jint failureKind) {
    jclass resultType = env->FindClass("com/kongshang/maichat/MaiChatAuthTransport$Result");
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
        const CURLcode initial = initializeCurl();
        if (initial != CURLE_OK) return makeResult(env, 0, {}, initial, kSetup);

        CurlHandle handle;
        if (handle.value == nullptr) return makeResult(env, 0, {}, CURLE_FAILED_INIT, kSetup);
        CurlHeaders headers;
        if (!headers.append("Accept: application/json") || !headers.append("Cache-Control: no-store") ||
            !headers.append("Expect:"))
            return makeResult(env, 0, {}, CURLE_OUT_OF_MEMORY, kSetup);
        if (method == "POST" && !headers.append("Content-Type: application/json"))
            return makeResult(env, 0, {}, CURLE_OUT_OF_MEMORY, kSetup);
        if (!bearer.empty() && !headers.append("Authorization: Bearer " + bearer))
            return makeResult(env, 0, {}, CURLE_OUT_OF_MEMORY, kSetup);

        Transfer transfer;
        transfer.request = &request;
        curl_blob caBlob = {ca.data(), ca.size(), CURL_BLOB_COPY};
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
        set(CURLOPT_CONNECTTIMEOUT_MS, 10000L);
        set(CURLOPT_TIMEOUT_MS, 30000L);
        set(CURLOPT_LOW_SPEED_LIMIT, 1L);
        set(CURLOPT_LOW_SPEED_TIME, 20L);
        set(CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(kMaxBodyBytes));
        set(CURLOPT_ACCEPT_ENCODING, "identity");
        if (url.rfind("https://", 0) == 0) set(CURLOPT_CAINFO_BLOB, &caBlob);
        if (proxyType == 0) {
            set(CURLOPT_PROXY, "");
            set(CURLOPT_NOPROXY, "*");
        } else {
            const std::string address = proxyUrl(proxyHost, proxyPort, proxyType);
            set(CURLOPT_PROXY, address.c_str());
            set(CURLOPT_NOPROXY, "");
            if (proxyType == 1 && url.rfind("https://", 0) == 0) set(CURLOPT_HTTPPROXYTUNNEL, 1L);
        }
        if (method == "POST") {
            set(CURLOPT_POST, 1L);
            set(CURLOPT_READFUNCTION, readRequest);
            set(CURLOPT_READDATA, &transfer);
            set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.size()));
        } else {
            set(CURLOPT_HTTPGET, 1L);
        }
        if (setup != CURLE_OK) return makeResult(env, 0, {}, setup, kSetup);
        const CURLcode result = curl_easy_perform(handle.value);
        if (result != CURLE_OK) return makeResult(env, 0, {}, result, classify(result, transfer.failure));
        long status = 0;
        if (curl_easy_getinfo(handle.value, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK || status < 100 ||
            status > 599)
            return makeResult(env, 0, {}, CURLE_GOT_NOTHING, kNoFailure);
        return makeResult(env, static_cast<jint>(status), transfer.response, CURLE_OK, kNoFailure);
    } catch (const std::bad_alloc&) {
        if (!env->ExceptionCheck()) throwIOException(env, "账号网络内存不足");
        return nullptr;
    } catch (const std::exception&) {
        if (!env->ExceptionCheck()) throwIOException(env, "账号网络组件执行失败");
        return nullptr;
    }
}
