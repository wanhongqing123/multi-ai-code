#include "im/TencentUserSigGenerator.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
#include <zlib.h>

namespace {

QString hmacSha256Base64(const QString& secretKey, const QString& content) {
    const QByteArray signature = QMessageAuthenticationCode::hash(
        content.toUtf8(),
        secretKey.toUtf8(),
        QCryptographicHash::Sha256
    );
    return QString::fromLatin1(signature.toBase64());
}

QByteArray zlibCompress(const QByteArray& input) {
    uLongf outputSize = compressBound(static_cast<uLong>(input.size()));
    QByteArray output;
    output.resize(static_cast<int>(outputSize));
    const int result = compress2(reinterpret_cast<Bytef*>(output.data()),
                                 &outputSize,
                                 reinterpret_cast<const Bytef*>(input.constData()),
                                 static_cast<uLong>(input.size()),
                                 Z_BEST_SPEED);
    if (result != Z_OK) return {};
    output.resize(static_cast<int>(outputSize));
    return output;
}

QString toTencentBase64Url(QByteArray input) {
    QString value = QString::fromLatin1(input.toBase64());
    value.replace(QStringLiteral("+"), QStringLiteral("*"));
    value.replace(QStringLiteral("/"), QStringLiteral("-"));
    value.replace(QStringLiteral("="), QStringLiteral("_"));
    return value;
}

}  // namespace

QString TencentUserSigGenerator::generate(int sdkAppId,
                                          const QString& userId,
                                          const QString& secretKey,
                                          int expireSeconds,
                                          int currentTimeSeconds) {
    const QString cleanUserId = userId.trimmed();
    const QString cleanSecretKey = secretKey.trimmed();
    if (sdkAppId <= 0 || cleanUserId.isEmpty() || cleanSecretKey.isEmpty() || expireSeconds <= 0) {
        return {};
    }

    const int now = currentTimeSeconds >= 0
                        ? currentTimeSeconds
                        : static_cast<int>(QDateTime::currentSecsSinceEpoch());
    const QString contentToSign =
        QStringLiteral("TLS.identifier:%1\nTLS.sdkappid:%2\nTLS.time:%3\nTLS.expire:%4\n")
            .arg(cleanUserId)
            .arg(sdkAppId)
            .arg(now)
            .arg(expireSeconds);
    const QString signature = hmacSha256Base64(cleanSecretKey, contentToSign);

    QJsonObject payload;
    payload[QStringLiteral("TLS.ver")] = QStringLiteral("2.0");
    payload[QStringLiteral("TLS.identifier")] = cleanUserId;
    payload[QStringLiteral("TLS.sdkappid")] = sdkAppId;
    payload[QStringLiteral("TLS.expire")] = expireSeconds;
    payload[QStringLiteral("TLS.time")] = now;
    payload[QStringLiteral("TLS.sig")] = signature;

    const QByteArray json = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    const QByteArray compressed = zlibCompress(json);
    if (compressed.isEmpty()) return {};
    return toTencentBase64Url(compressed);
}
