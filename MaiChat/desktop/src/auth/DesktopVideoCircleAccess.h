#pragma once

#include <QByteArray>
#include <QPair>
#include <QString>
#include <QUrl>

// Borrowed only by the caller. The account token must never be persisted or logged.
struct DesktopVideoCircleAccess {
    QString authBaseUrl;
    QString mediaBaseUrl;
    QString accountToken;
    // Empty defaults are for production trust/proxy behavior. Local TLS tests
    // may provide a fixture CA and a localhost upload host restriction.
    QByteArray caPem;
    QString signedUploadHostSuffix;
    QString proxyUrl;
    QString accountId;

    bool isValid() const {
        return !authBaseUrl.isEmpty() && !mediaBaseUrl.isEmpty() &&
               !accountToken.isEmpty() && !accountId.isEmpty();
    }
};

// The auth client is configured with a /v1/ login base; the video API's
// shared C++ client appends /v1/video-circle itself. Preserve proxy prefixes.
inline QPair<QString, QString> desktopVideoCircleEndpointBases(const QUrl& loginBase) {
    if (!loginBase.isValid() || loginBase.scheme() != QStringLiteral("https")) return {};
    QUrl authUrl = loginBase;
    QString authPath = authUrl.path();
    while (authPath.endsWith(QLatin1Char('/'))) authPath.chop(1);
    if (!authPath.endsWith(QStringLiteral("/v1"))) return {};
    authPath.chop(3);
    authUrl.setPath(authPath);
    authUrl.setQuery(QString());
    authUrl.setFragment(QString());
    QUrl mediaUrl = loginBase;
    const QString prefix = authPath.endsWith(QStringLiteral("/api"))
        ? authPath.left(authPath.size() - 4) : QString();
    mediaUrl.setPath(prefix + QStringLiteral("/maichat"));
    mediaUrl.setQuery(QString());
    mediaUrl.setFragment(QString());
    return {authUrl.toString(QUrl::FullyEncoded), mediaUrl.toString(QUrl::FullyEncoded)};
}
