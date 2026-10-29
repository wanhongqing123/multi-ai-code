// 文件作用：使用 App 已链接的 libcurl/OpenSSL 建立实时语音 WebSocket。
// 单个原生 I/O 线程独占 curl 句柄；Java 工作线程只操作有界发送队列和取消信号。
#include <curl/curl.h>
#include <jni.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <new>
#include <poll.h>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr size_t kMaxQueuedBytes = 128 * 1024;
constexpr size_t kMaxQueuedFrames = 256;
constexpr size_t kMaxResponseBytes = 64 * 1024;
constexpr size_t kMaxCaBytes = 2 * 1024 * 1024;
constexpr int kClosed = 0;
constexpr int kCancelled = -1;
constexpr int kSetupFailed = -2;
constexpr int kBadMessage = -3;
constexpr int kNoActiveSocket = -4;
constexpr int kNoJavaCallback = -5;

struct Frame {
    std::vector<unsigned char> payload;
    bool text = false;
    size_t offset = 0;
};

struct SocketState {
    std::mutex mutex;
    std::deque<Frame> queue;
    size_t queuedBytes = 0;
    size_t queuedFrames = 0;
    std::atomic<bool> cancelled{false};
    int wakeRead = -1;
    int wakeWrite = -1;

    ~SocketState() {
        if (wakeRead >= 0) close(wakeRead);
        if (wakeWrite >= 0) close(wakeWrite);
    }

    bool initialize() {
        int descriptors[2];
        if (pipe(descriptors) != 0) return false;
        wakeRead = descriptors[0];
        wakeWrite = descriptors[1];
        if (fcntl(wakeRead, F_SETFL, O_NONBLOCK) != 0 ||
            fcntl(wakeWrite, F_SETFL, O_NONBLOCK) != 0) return false;
        return true;
    }

    void wake() const {
        const unsigned char signal = 1;
        if (wakeWrite >= 0) (void)write(wakeWrite, &signal, 1);
    }

    void drainWake() const {
        unsigned char buffer[32];
        while (read(wakeRead, buffer, sizeof(buffer)) > 0) { }
    }
};

struct CurlHandle {
    CURL* value = curl_easy_init();
    ~CurlHandle() { if (value != nullptr) curl_easy_cleanup(value); }
};

struct CurlHeaders {
    curl_slist* value = nullptr;
    ~CurlHeaders() { if (value != nullptr) curl_slist_free_all(value); }
};

std::once_flag gCurlInitOnce;
CURLcode gCurlInitResult = CURLE_FAILED_INIT;

CURLcode initializeCurl() {
    std::call_once(gCurlInitOnce, [] { gCurlInitResult = curl_global_init(CURL_GLOBAL_DEFAULT); });
    return gCurlInitResult;
}

bool copyString(JNIEnv* env, jstring source, size_t maximum, std::string* result) {
    if (source == nullptr) return false;
    const char* value = env->GetStringUTFChars(source, nullptr);
    if (value == nullptr) return false;
    result->assign(value);
    env->ReleaseStringUTFChars(source, value);
    return result->size() <= maximum;
}

bool copyBytes(JNIEnv* env, jbyteArray source, size_t maximum, std::vector<unsigned char>* result) {
    if (source == nullptr) return false;
    jsize count = env->GetArrayLength(source);
    if (count < 0 || static_cast<size_t>(count) > maximum) return false;
    result->resize(static_cast<size_t>(count));
    if (count > 0) env->GetByteArrayRegion(source, 0, count, reinterpret_cast<jbyte*>(result->data()));
    return !env->ExceptionCheck();
}

bool validUrl(const std::string& url, bool* secure) {
    *secure = url.rfind("wss://", 0) == 0;
    if (!*secure && url.rfind("ws://", 0) != 0) return false;
    for (unsigned char value : url) {
        if (value <= 32 || value >= 127) return false;
    }
    return true;
}

bool validProxy(const std::string& host, jint port, jint type) {
    if (type == 0) return true;
    if ((type != 1 && type != 2) || host.empty() || port <= 0 || port > 65535) return false;
    for (unsigned char value : host) {
        if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '.' || value == '-' || value == ':') continue;
        return false;
    }
    return true;
}

std::string proxyAddress(const std::string& host, jint port, jint type) {
    std::string address = type == 1 ? "http://" : "socks5h://";
    bool ipv6 = host.find(':') != std::string::npos;
    if (ipv6) address += '[';
    address += host;
    if (ipv6) address += ']';
    address += ':' + std::to_string(port);
    return address;
}

int cancelledProgress(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<SocketState*>(opaque)->cancelled.load() ? 1 : 0;
}

size_t discardBody(char*, size_t size, size_t count, void*) {
    return count != 0 && size > SIZE_MAX / count ? 0 : size * count;
}

// curl 8.11.1 的帧元数据不暴露 FIN 位；腾讯响应是单个 JSON 对象，
// 因此按括号和字符串转义状态拼接分片，严格限制长度并拒绝对象后的非空白字节。
class JsonResponse {
public:
    int accept(JNIEnv* env, jobject owner, jmethodID callback, const unsigned char* bytes,
               size_t count, const curl_ws_frame* meta) {
        // curl 8.11.1 把同一帧的分块保留原 opcode，也不在 meta 中暴露 FIN；offset=0 才是新帧。
        if (meta->offset < 0) return kBadMessage;
        if (meta->offset == 0 && (meta->flags & CURLWS_TEXT)) {
            if (started_ && !delivered_) return kBadMessage;
            reset();
            started_ = true;
        } else if (!(meta->flags & (CURLWS_TEXT | CURLWS_CONT)) || !started_) {
            return kBadMessage;
        }
        if (meta->bytesleft < 0 || count > kMaxResponseBytes - totalBytes_ ||
            static_cast<uint64_t>(meta->bytesleft) > kMaxResponseBytes - totalBytes_ - count)
            return kBadMessage;
        // 腾讯响应是单个 JSON 对象；跨帧追踪括号，忽略字符串内括号。
        // TEXT 是首帧，CONT 是后续帧；同一帧分块还要用 offset/bytesleft 合并。
        // 完整对象形成之前绝不通知 Java，避免业务层看到半个 JSON。
        totalBytes_ += count;
        for (size_t index = 0; index < count; ++index) {
            unsigned char value = bytes[index];
            if (delivered_ || complete_) {
                if (!space(value)) return kBadMessage;
                continue;
            }
            data_.push_back(value);
            if (!opened_) {
                if (space(value)) continue;
                if (value != '{') return kBadMessage;
                opened_ = true;
                stack_.push_back('}');
            } else if (quoted_) {
                if (escaped_) escaped_ = false;
                else if (value == '\\') escaped_ = true;
                else if (value == '"') quoted_ = false;
            } else if (value == '"') {
                quoted_ = true;
            } else if (value == '{') {
                stack_.push_back('}');
            } else if (value == '[') {
                stack_.push_back(']');
            } else if (value == '}' || value == ']') {
                if (stack_.empty() || stack_.back() != value) return kBadMessage;
                stack_.pop_back();
                if (stack_.empty()) complete_ = true;
            }
        }
        // 等当前帧字节收齐再回调；后续 CONT 帧只能携带该 JSON 对象的空白尾部。
        // 对象闭合且当前传输块收齐后只交付一次；JNI 回调异常转为连接失败。
        if (complete_ && !delivered_ && meta->bytesleft == 0) {
            jbyteArray payload = env->NewByteArray(static_cast<jsize>(data_.size()));
            if (payload == nullptr) return kSetupFailed;
            env->SetByteArrayRegion(payload, 0, static_cast<jsize>(data_.size()),
                                    reinterpret_cast<const jbyte*>(data_.data()));
            if (!env->ExceptionCheck()) env->CallVoidMethod(owner, callback, payload);
            env->DeleteLocalRef(payload);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                return kSetupFailed;
            }
            delivered_ = true;
            data_.clear();
        }
        return 1;
    }

private:
    static bool space(unsigned char value) {
        return value == ' ' || value == '\r' || value == '\n' || value == '\t';
    }

    void reset() {
        data_.clear();
        stack_.clear();
        totalBytes_ = 0;
        started_ = false;
        opened_ = false;
        quoted_ = false;
        escaped_ = false;
        complete_ = false;
        delivered_ = false;
    }

    std::vector<unsigned char> data_;
    std::vector<unsigned char> stack_;
    size_t totalBytes_ = 0;
    bool started_ = false;
    bool opened_ = false;
    bool quoted_ = false;
    bool escaped_ = false;
    bool complete_ = false;
    bool delivered_ = false;
};

int runSocket(JNIEnv* env, jobject owner, SocketState* state, const std::string& url,
              const std::string& token, std::vector<unsigned char>& ca, jint proxyType,
              const std::string& proxyHost, jint proxyPort) {
    if (state->cancelled.load()) return kCancelled;
    bool secure = false;
    if (!validUrl(url, &secure) || !validProxy(proxyHost, proxyPort, proxyType) ||
        (secure && ca.empty())) return kSetupFailed;
    // STS 只透传为官方固定头；Java/JNI 两层拒绝 CRLF、控制字符及过长值。
    if (token.size() > 8192) return kSetupFailed;
    for (unsigned char value : token) {
        if (value <= 32 || value >= 127) return kSetupFailed;
    }
    CURLcode initialized = initializeCurl();
    if (initialized != CURLE_OK) return static_cast<int>(initialized);
    CurlHeaders headers;
    CurlHandle curl;
    if (curl.value == nullptr) return kSetupFailed;
    curl_blob caBlob = {ca.data(), ca.size(), CURL_BLOB_COPY};
    std::string proxy = proxyType == 0 ? "" : proxyAddress(proxyHost, proxyPort, proxyType);
    CURLcode setup = CURLE_OK;
    auto set = [&](CURLoption option, auto value) {
        if (setup == CURLE_OK) setup = curl_easy_setopt(curl.value, option, value);
    };
    // 握手只允许 WS/WSS，不跟随重定向；WSS 同时验证 Android CA 与目标主机名。
    set(CURLOPT_URL, url.c_str());
    if (!token.empty()) {
        std::string header = "X-TC-Token: " + token;
        headers.value = curl_slist_append(nullptr, header.c_str());
        if (headers.value == nullptr) return kSetupFailed;
        set(CURLOPT_HTTPHEADER, headers.value);
    }
    set(CURLOPT_PROTOCOLS_STR, "ws,wss");
    set(CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    set(CURLOPT_CONNECT_ONLY, 2L);
    set(CURLOPT_FOLLOWLOCATION, 0L);
    set(CURLOPT_WRITEFUNCTION, discardBody);
    set(CURLOPT_SSL_VERIFYPEER, 1L);
    set(CURLOPT_SSL_VERIFYHOST, 2L);
    set(CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    set(CURLOPT_SSL_SESSIONID_CACHE, 0L);
    set(CURLOPT_FRESH_CONNECT, 1L);
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    set(CURLOPT_TIMEOUT_MS, 6000L);
    set(CURLOPT_NOPROGRESS, 0L);
    set(CURLOPT_XFERINFOFUNCTION, cancelledProgress);
    set(CURLOPT_XFERINFODATA, state);
    if (secure) set(CURLOPT_CAINFO_BLOB, &caBlob);
    if (proxyType == 0) {
        set(CURLOPT_PROXY, "");
        set(CURLOPT_NOPROXY, "*");
    } else {
        set(CURLOPT_PROXY, proxy.c_str());
        set(CURLOPT_NOPROXY, "");
        if (proxyType == 1 && secure) set(CURLOPT_HTTPPROXYTUNNEL, 1L);
    }
    if (setup != CURLE_OK) return static_cast<int>(setup);
    if (state->cancelled.load()) return kCancelled;
    // CONNECT_ONLY=2 在收到 101 后交还控制权；禁止复用会关闭升级连接。
    CURLcode result = curl_easy_perform(curl.value);
    if (state->cancelled.load()) return kCancelled;
    if (result != CURLE_OK) return static_cast<int>(result);
    // 六秒上限只约束握手；连接建立后可能持续整个录音过程。
    result = curl_easy_setopt(curl.value, CURLOPT_TIMEOUT_MS, 0L);
    if (result != CURLE_OK) return static_cast<int>(result);
    long status = 0;
    if (curl_easy_getinfo(curl.value, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK || status != 101)
        return kBadMessage;
    curl_socket_t socket = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(curl.value, CURLINFO_ACTIVESOCKET, &socket) != CURLE_OK ||
        socket == CURL_SOCKET_BAD) return kNoActiveSocket;
    jclass ownerClass = env->GetObjectClass(owner);
    if (ownerClass == nullptr) return kNoJavaCallback;
    jmethodID callback = env->GetMethodID(ownerClass, "onMessage", "([B)V");
    env->DeleteLocalRef(ownerClass);
    if (callback == nullptr) return kNoJavaCallback;

    JsonResponse response;
    Frame sending;
    bool hasSending = false;
    unsigned char buffer[8192];
    // 只有本线程调用 curl_ws_send/recv；队列锁只保护 Java 线程提交的帧和在途字节计数。
    while (!state->cancelled.load()) {
        if (!hasSending) {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->queue.empty()) {
                sending = std::move(state->queue.front());
                state->queue.pop_front();
                hasSending = true;
            }
        }
        bool waitForWrite = false;
        if (hasSending) {
            // OFFSET 保留同一 WebSocket 帧在短写后的边界；直到全部发送才释放队列额度。
            size_t sent = 0;
            unsigned int flags = (sending.text ? CURLWS_TEXT : CURLWS_BINARY) | CURLWS_OFFSET;
            result = curl_ws_send(curl.value, sending.payload.data() + sending.offset,
                                  sending.payload.size() - sending.offset, &sent,
                                  sending.offset == 0 ? static_cast<curl_off_t>(sending.payload.size()) : 0,
                                  flags);
            if (result != CURLE_OK && result != CURLE_AGAIN) return static_cast<int>(result);
            sending.offset += sent;
            if (sending.offset == sending.payload.size() && result == CURLE_OK) {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->queuedFrames == 0 || state->queuedBytes < sending.payload.size())
                    return kSetupFailed;
                state->queuedBytes -= sending.payload.size();
                --state->queuedFrames;
                hasSending = false;
                sending = Frame{};
            } else {
                waitForWrite = true;
            }
        }
        // 优先排空已收到的数据；libcurl 自动回 PONG，CLOSE 显式结束，文本帧有界重组。
        int received = 0;
        for (; received < 16 && !state->cancelled.load(); ++received) {
            size_t count = 0;
            const curl_ws_frame* meta = nullptr;
            result = curl_ws_recv(curl.value, buffer, sizeof(buffer), &count, &meta);
            if (result == CURLE_AGAIN) break;
            if (result == CURLE_GOT_NOTHING) return kClosed;
            if (result != CURLE_OK) return static_cast<int>(result);
            if (meta == nullptr) return kBadMessage;
            if (meta->flags & CURLWS_CLOSE) return kClosed;
            if (meta->flags & (CURLWS_TEXT | CURLWS_CONT)) {
                int accepted = response.accept(env, owner, callback, buffer, count, meta);
                if (accepted != 1) return accepted;
            }
        }
        if (state->cancelled.load()) break;
        bool queued = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            queued = !state->queue.empty();
        }
        if (received == 16 || (!waitForWrite && !hasSending && queued)) continue;
        // 同时等待网络事件和取消/入队唤醒；读写空闲时不忙轮询。
        pollfd descriptors[2] = {{static_cast<int>(socket), static_cast<short>(POLLIN |
                                  (waitForWrite ? POLLOUT : 0)), 0},
                                 {state->wakeRead, POLLIN, 0}};
        int ready = poll(descriptors, 2, 1000);
        if (ready < 0 && errno != EINTR) return static_cast<int>(CURLE_RECV_ERROR);
        if (descriptors[1].revents & POLLIN) state->drainWake();
    }
    return kCancelled;
}

}  // namespace

extern "C" JNIEXPORT jlong JNICALL Java_com_kongshang_maichat_CurlRealtimeSpeechWebSocket_nativeCreate(
    JNIEnv*, jclass) {
    auto* state = new (std::nothrow) SocketState();
    if (state == nullptr) return 0;
    if (!state->initialize()) {
        delete state;
        return 0;
    }
    return reinterpret_cast<jlong>(state);
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_kongshang_maichat_CurlRealtimeSpeechWebSocket_nativeQueue(
    JNIEnv* env, jclass, jlong handle, jbyteArray payload, jboolean text) {
    auto* state = reinterpret_cast<SocketState*>(handle);
    if (state == nullptr || payload == nullptr) return JNI_FALSE;
    try {
        Frame frame;
        if (!copyBytes(env, payload, kMaxQueuedBytes, &frame.payload)) return JNI_FALSE;
        if (frame.payload.empty()) return JNI_TRUE;
        frame.text = text == JNI_TRUE;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->cancelled.load() || state->queuedFrames >= kMaxQueuedFrames ||
                frame.payload.size() > kMaxQueuedBytes - state->queuedBytes) return JNI_FALSE;
            size_t size = frame.payload.size();
            state->queue.push_back(std::move(frame));
            state->queuedBytes += size;
            ++state->queuedFrames;
        }
        state->wake();
        return JNI_TRUE;
    } catch (...) {
        return JNI_FALSE;
    }
}

extern "C" JNIEXPORT void JNICALL Java_com_kongshang_maichat_CurlRealtimeSpeechWebSocket_nativeCancel(
    JNIEnv*, jclass, jlong handle) {
    auto* state = reinterpret_cast<SocketState*>(handle);
    if (state != nullptr) {
        state->cancelled.store(true);
        state->wake();
    }
}

extern "C" JNIEXPORT void JNICALL Java_com_kongshang_maichat_CurlRealtimeSpeechWebSocket_nativeDestroy(
    JNIEnv*, jclass, jlong handle) {
    delete reinterpret_cast<SocketState*>(handle);
}

extern "C" JNIEXPORT jint JNICALL Java_com_kongshang_maichat_CurlRealtimeSpeechWebSocket_nativeRun(
    JNIEnv* env, jobject owner, jlong handle, jstring jurl, jstring jtoken, jbyteArray jca, jint proxyType,
    jstring jproxyHost, jint proxyPort) {
    auto* state = reinterpret_cast<SocketState*>(handle);
    if (state == nullptr) return kSetupFailed;
    try {
        std::string url, token, proxyHost;
        std::vector<unsigned char> ca;
        if (!copyString(env, jurl, 4096, &url) || !copyString(env, jtoken, 8192, &token) ||
            !copyString(env, jproxyHost, 255, &proxyHost) ||
            !copyBytes(env, jca, kMaxCaBytes, &ca)) return kSetupFailed;
        return runSocket(env, owner, state, url, token, ca, proxyType, proxyHost, proxyPort);
    } catch (...) {
        return kSetupFailed;
    }
}
