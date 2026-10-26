#include "im/DesktopCurlAttachmentDownload.h"

#include <QFile>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QResource>

namespace {

QString proxyUrl(const QNetworkProxy& proxy) {
    if (proxy.type() != QNetworkProxy::HttpProxy &&
        proxy.type() != QNetworkProxy::Socks5Proxy) return {};
    if (proxy.hostName().isEmpty() || proxy.port() <= 0) return {};
    QUrl location;
    location.setScheme(proxy.type() == QNetworkProxy::HttpProxy ?
                           QStringLiteral("http") : QStringLiteral("socks5h"));
    location.setHost(proxy.hostName());
    location.setPort(proxy.port());
    return location.toString(QUrl::FullyEncoded);
}

struct ProxyResolverContext {
    QByteArray initialUrl;
    QNetworkProxy initialProxy;
    QByteArray url;
    QByteArray username;
    QByteArray password;
};

int resolveProxy(const char* currentUrl, void* context, MaiDownloadProxy* output) {
    auto* resolver = static_cast<ProxyResolverContext*>(context);
    if (!currentUrl || !resolver || !output) return 0;
    const QUrl location = QUrl::fromEncoded(QByteArray(currentUrl));
    QNetworkProxy proxy = resolver->initialProxy;
    if (QByteArray(currentUrl) != resolver->initialUrl) {
        proxy = QNetworkProxy(QNetworkProxy::DefaultProxy);
        const auto candidates = QNetworkProxyFactory::systemProxyForQuery(
            QNetworkProxyQuery(location));
        for (const QNetworkProxy& candidate : candidates) {
            if (candidate.type() == QNetworkProxy::NoProxy ||
                candidate.type() == QNetworkProxy::HttpProxy ||
                candidate.type() == QNetworkProxy::Socks5Proxy) {
                proxy = candidate;
                break;
            }
        }
    }
    if (location.host() == QStringLiteral("localhost") ||
        location.host() == QStringLiteral("127.0.0.1")) {
        proxy = QNetworkProxy(QNetworkProxy::NoProxy);
    }
    resolver->url = proxyUrl(proxy).toUtf8();
    resolver->username = proxy.user().toUtf8();
    resolver->password = proxy.password().toUtf8();
    output->url_utf8 = proxy.type() == QNetworkProxy::NoProxy ? "" :
        resolver->url.isEmpty() ? nullptr : resolver->url.constData();
    output->username_utf8 = resolver->username.isEmpty() ? nullptr :
        resolver->username.constData();
    output->password_utf8 = resolver->password.isEmpty() ? nullptr :
        resolver->password.constData();
    return 1;
}

DesktopCurlAttachmentResult::Status statusFrom(MaiDownloadStatus status) {
    switch (status) {
    case MAI_DOWNLOAD_OK: return DesktopCurlAttachmentResult::Status::Downloaded;
    case MAI_DOWNLOAD_INVALID_ARGUMENT: return DesktopCurlAttachmentResult::Status::InvalidUrl;
    case MAI_DOWNLOAD_WRITE_ERROR: return DesktopCurlAttachmentResult::Status::WriteError;
    case MAI_DOWNLOAD_CANCELED: return DesktopCurlAttachmentResult::Status::Canceled;
    default: return DesktopCurlAttachmentResult::Status::NetworkError;
    }
}

}  // namespace

DesktopCurlAttachmentResult DesktopCurlAttachmentDownload::perform(
    const QUrl& url, const QString& targetPath, qint64 maximumBytes,
    const QByteArray& certificatePem, const QNetworkProxy& proxy,
    const std::shared_ptr<MaiDownloadCancel>& canceled) {
    DesktopCurlAttachmentResult output;
    if (maximumBytes <= 0 || targetPath.isEmpty()) {
        output.status = DesktopCurlAttachmentResult::Status::InvalidUrl;
        return output;
    }

    QByteArray trustedRoots = certificatePem;
#ifdef Q_OS_MACOS
    Q_INIT_RESOURCE(AuthCertificates);
    if (trustedRoots.isEmpty()) {
        QFile certificates(QStringLiteral(":/maichat/auth/cacert.pem"));
        if (certificates.open(QIODevice::ReadOnly)) trustedRoots = certificates.readAll();
    }
    if (trustedRoots.isEmpty()) return output;
#endif

    const QByteArray encodedUrl = url.toString(QUrl::FullyEncoded).toUtf8();
    const QByteArray encodedPath = targetPath.toUtf8();
    const QByteArray encodedProxy = proxyUrl(proxy).toUtf8();
    const QByteArray proxyUser = proxy.user().toUtf8();
    const QByteArray proxyPassword = proxy.password().toUtf8();
    const char* selectedProxy = nullptr;
    if (proxy.type() == QNetworkProxy::NoProxy) selectedProxy = "";
    else if (!encodedProxy.isEmpty()) selectedProxy = encodedProxy.constData();

    MaiDownloadOptions options{};
    options.url_utf8 = encodedUrl.constData();
    options.target_path_utf8 = encodedPath.constData();
    options.max_bytes = static_cast<uint64_t>(maximumBytes);
    options.ca_pem = trustedRoots.isEmpty() ? nullptr : trustedRoots.constData();
    options.ca_pem_length = static_cast<size_t>(trustedRoots.size());
    options.proxy_url_utf8 = selectedProxy;
    options.proxy_username_utf8 = proxyUser.isEmpty() ? nullptr : proxyUser.constData();
    options.proxy_password_utf8 = proxyPassword.isEmpty() ? nullptr : proxyPassword.constData();
    ProxyResolverContext resolver{encodedUrl, proxy, {}, {}, {}};
    options.proxy_resolver = resolveProxy;
    options.proxy_context = &resolver;

    MaiDownloadResult result{};
    mai_download_file(&options, canceled.get(), &result);
    output.status = statusFrom(result.status);
    output.transferCode = result.curl_code;
    output.bytes = static_cast<qint64>(result.bytes);
    return output;
}
