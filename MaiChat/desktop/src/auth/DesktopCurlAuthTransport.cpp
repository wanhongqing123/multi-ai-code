// 文件作用：用 MaiAgent 同一份 libcurl + OpenSSL 发送桌面账号 GET/POST 请求。
// 安全边界：校验服务端证书与域名，不跟随重定向、不自动重试或重放 POST。
// 资源边界：响应正文上限 1 MiB、响应头上限 16 KiB；请求完成即释放连接。
#include "auth/DesktopCurlAuthTransport.h"

#include <QFile>
#include <QResource>

#include <curl/curl.h>

#include <cstring>
#include <mutex>

namespace {

constexpr int kMaximumResponseBytes = 1024 * 1024;
constexpr int kMaximumHeaderBytes = 16 * 1024;
constexpr int kMaximumRequestBytes = 32 * 1024;

struct ResponseCollector {
    QByteArray body;
    int headerBytes = 0;
    bool exceededLimit = false;
};

size_t receiveBody(char* bytes, size_t size, size_t count, void* context) {
    auto* response = static_cast<ResponseCollector*>(context);
    if (size != 0 && count > SIZE_MAX / size) {
        response->exceededLimit = true;
        return 0;
    }
    const size_t length = size * count;
    if (length > size_t(kMaximumResponseBytes - response->body.size())) {
        response->exceededLimit = true;
        return 0;
    }
    response->body.append(bytes, int(length));
    return length;
}

size_t receiveHeader(char*, size_t size, size_t count, void* context) {
    auto* response = static_cast<ResponseCollector*>(context);
    if (size != 0 && count > SIZE_MAX / size) {
        response->exceededLimit = true;
        return 0;
    }
    const size_t length = size * count;
    if (length > size_t(kMaximumHeaderBytes - response->headerBytes)) {
        response->exceededLimit = true;
        return 0;
    }
    response->headerBytes += int(length);
    return length;
}

int stopCancelledTransfer(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<std::atomic_bool*>(context)->load() ? 1 : 0;
}

bool validRequest(const QUrl& url, const QByteArray& method, const QByteArray& body, const QByteArray& bearer) {
    if (!url.isValid() || !url.userInfo().isEmpty() || !url.fragment().isEmpty() ||
        url.toString(QUrl::FullyEncoded).size() > 2048)
        return false;
    const bool production = url.scheme() == QStringLiteral("https") && url.host() == QStringLiteral("ichat.life") &&
                            url.port(-1) == -1 && url.path().startsWith(QStringLiteral("/api/v1/"));
    const bool localTest = url.scheme() == QStringLiteral("http") && url.host() == QStringLiteral("127.0.0.1") &&
                           url.port() > 0 && url.path().startsWith(QStringLiteral("/v1/"));
    if (!production && !localTest) return false;
    const bool sessionSync = url.path().contains(QStringLiteral("/v1/session-sync/"));
    if (!url.query().isEmpty()) {
        if (!sessionSync || !url.path().endsWith(QStringLiteral("/poll")) ||
            !url.query().startsWith(QStringLiteral("wait_ms="))) return false;
        bool validWait = false;
        const int wait = url.query().mid(8).toInt(&validWait);
        if (!validWait || wait < 0 || wait > 20000) return false;
    }
    const int maximumRequest = sessionSync ? 96 * 1024 : kMaximumRequestBytes;
    if (url.path().contains(QStringLiteral("..")) || body.size() > maximumRequest || bearer.size() > 8192)
        return false;
    for (char character : bearer) {
        if (character < 33 || character > 126) return false;
    }
    return method == "POST" || (method == "GET" && body.isEmpty());
}

bool addHeader(curl_slist** headers, const QByteArray& value) {
    curl_slist* updated = curl_slist_append(*headers, value.constData());
    if (updated == nullptr) return false;
    *headers = updated;
    return true;
}

QString transferError(CURLcode code) {
    // libcurl 错误只报告类别，不拼接包含账号令牌的 URL 或请求正文。
    return QString::fromLatin1(curl_easy_strerror(code));
}

}  // namespace

DesktopCurlAuthResult DesktopCurlAuthTransport::perform(const QUrl& url, const QByteArray& method,
                                                        const QByteArray& body, const QByteArray& bearer,
                                                        const std::shared_ptr<std::atomic_bool>& cancelled) {
    DesktopCurlAuthResult result;
    if (!cancelled || !validRequest(url, method, body, bearer)) {
        result.error = QStringLiteral("账号请求参数无效");
        return result;
    }
    if (cancelled->load()) {
        result.error = QStringLiteral("账号请求已取消");
        return result;
    }

    static std::once_flag initialization;
    static CURLcode initializationCode = CURLE_FAILED_INIT;
    std::call_once(initialization, [] { initializationCode = curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (initializationCode != CURLE_OK) {
        result.error = QStringLiteral("账号网络组件初始化失败");
        return result;
    }
    const curl_version_info_data* version = curl_version_info(CURLVERSION_NOW);
    if (version == nullptr || version->ssl_version == nullptr ||
        std::strncmp(version->ssl_version, "OpenSSL/", 8) != 0) {
        result.error = QStringLiteral("账号网络加密组件配置错误");
        return result;
    }
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        result.error = QStringLiteral("账号网络组件初始化失败");
        return result;
    }

    const QByteArray encodedUrl = url.toString(QUrl::FullyEncoded).toUtf8();
    ResponseCollector response;
    CURLcode setup = CURLE_OK;
    auto set = [&](CURLoption option, auto value) {
        if (setup == CURLE_OK) setup = curl_easy_setopt(curl, option, value);
    };
    set(CURLOPT_URL, encodedUrl.constData());
    set(CURLOPT_PROTOCOLS_STR, url.scheme() == QStringLiteral("https") ? "https" : "http");
    set(CURLOPT_FOLLOWLOCATION, 0L);
    set(CURLOPT_MAXREDIRS, 0L);
    set(CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    set(CURLOPT_FRESH_CONNECT, 1L);
    set(CURLOPT_FORBID_REUSE, 1L);
    set(CURLOPT_SSL_SESSIONID_CACHE, 0L);
    set(CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    const bool sessionSync = url.path().contains(QStringLiteral("/v1/session-sync/"));
    set(CURLOPT_TIMEOUT_MS, sessionSync ? 35000L : 15000L);
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_SSL_VERIFYPEER, 1L);
    set(CURLOPT_SSL_VERIFYHOST, 2L);
    set(CURLOPT_NETRC, CURL_NETRC_IGNORED);
    set(CURLOPT_WRITEFUNCTION, receiveBody);
    set(CURLOPT_WRITEDATA, &response);
    set(CURLOPT_HEADERFUNCTION, receiveHeader);
    set(CURLOPT_HEADERDATA, &response);
    set(CURLOPT_MAXFILESIZE_LARGE, curl_off_t(kMaximumResponseBytes));
    set(CURLOPT_ACCEPT_ENCODING, "identity");
    set(CURLOPT_NOPROGRESS, 0L);
    set(CURLOPT_XFERINFOFUNCTION, stopCancelledTransfer);
    set(CURLOPT_XFERINFODATA, cancelled.get());
    if (url.scheme() == QStringLiteral("http")) {
        // 本机测试服务不得经过环境代理，避免把测试中的令牌发到外部代理。
        set(CURLOPT_PROXY, "");
        set(CURLOPT_NOPROXY, "*");
    }

#ifdef Q_OS_WIN
    // OpenSSL 版 curl 使用 Windows 受信任根证书库，不接受无来源的开发机 PEM。
    set(CURLOPT_SSL_OPTIONS, long(CURLSSLOPT_NATIVE_CA));
#elif defined(Q_OS_MACOS)
    // macOS OpenSSL 后端显式使用随程序内嵌的 Mozilla CA，避免依赖开发机路径。
    Q_INIT_RESOURCE(AuthCertificates);
    QByteArray caBundle;
    if (url.scheme() == QStringLiteral("https")) {
        QFile certificates(QStringLiteral(":/maichat/auth/cacert.pem"));
        if (!certificates.open(QIODevice::ReadOnly)) {
            result.error = QStringLiteral("账号证书资源不可用");
            curl_easy_cleanup(curl);
            return result;
        }
        caBundle = certificates.readAll();
    }
    curl_blob caBlob = {const_cast<char*>(caBundle.constData()), size_t(caBundle.size()), CURL_BLOB_COPY};
    if (url.scheme() == QStringLiteral("https") && caBundle.isEmpty()) {
        result.error = QStringLiteral("账号证书资源不可用");
        curl_easy_cleanup(curl);
        return result;
    }
    if (url.scheme() == QStringLiteral("https")) set(CURLOPT_CAINFO_BLOB, &caBlob);
#endif

    if (method == "POST") {
        set(CURLOPT_POST, 1L);
        set(CURLOPT_POSTFIELDS, body.isEmpty() ? static_cast<const void*>("") : body.constData());
        set(CURLOPT_POSTFIELDSIZE_LARGE, curl_off_t(body.size()));
    } else {
        set(CURLOPT_HTTPGET, 1L);
    }

    curl_slist* headers = nullptr;
    bool ready = addHeader(&headers, "Accept: application/json") && addHeader(&headers, "Cache-Control: no-store") &&
                 addHeader(&headers, "Expect:");
    if (method == "POST") ready = ready && addHeader(&headers, "Content-Type: application/json");
    if (!bearer.isEmpty()) ready = ready && addHeader(&headers, "Authorization: Bearer " + bearer);
    if (ready) set(CURLOPT_HTTPHEADER, headers);
    if (!ready || setup != CURLE_OK) {
        result.error = QStringLiteral("账号请求配置失败");
    } else if (cancelled->load()) {
        result.error = QStringLiteral("账号请求已取消");
    } else {
        const CURLcode transferred = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (response.exceededLimit || transferred == CURLE_FILESIZE_EXCEEDED) {
            result.responseTooLarge = true;
            result.error = QStringLiteral("登录服务返回的数据过大");
        } else if (transferred != CURLE_OK) {
            result.error = cancelled->load() ? QStringLiteral("账号请求已取消") : transferError(transferred);
        } else {
            result.status = int(status);
            result.body = std::move(response.body);
        }
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}
