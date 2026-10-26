#pragma once

#include "auth/DesktopVideoCircleAccess.h"
#include "MaiCloudVod.h"

#include <QByteArray>
#include <QFile>
#include <QResource>
#include <QString>

#include <memory>

// Both the cloud-drive UI and Agent tool use the same seekable C++ source.
// The cancel tokens must outlive FFplay because a later range may renew the
// account-owned, sixty-second OSS read link.
using DesktopCloudVod = std::shared_ptr<MaiCloudVod>;

inline DesktopCloudVod desktopOpenCloudVod(
    const DesktopVideoCircleAccess& access, const QString& mediaId,
    std::shared_ptr<MaiCloudDriveCancel> accountCancel,
    std::shared_ptr<MaiCloudVodCancel> vodCancel, MaiCloudVodResult* result) {
    if (!access.isValid() || !accountCancel || !vodCancel || mediaId.isEmpty()) return {};
    Q_INIT_RESOURCE(AuthCertificates);
    QByteArray roots = access.caPem;
    if (roots.isEmpty()) {
        QFile certificates(QStringLiteral(":/maichat/auth/cacert.pem"));
        if (certificates.open(QIODevice::ReadOnly)) roots = certificates.readAll();
    }
    const QByteArray base = access.authBaseUrl.toUtf8();
    const QByteArray token = access.accountToken.toUtf8();
    const QByteArray proxy = access.proxyUrl.toUtf8();
    const QByteArray host = access.signedUploadHostSuffix.toUtf8();
    const QByteArray id = mediaId.toUtf8();
    MaiCloudDriveOptions options{};
    options.auth_base_url_utf8 = base.constData();
    options.account_token_utf8 = token.constData();
    options.ca_pem = roots.isEmpty() ? nullptr : roots.constData();
    options.ca_pem_length = size_t(roots.size());
    options.proxy_url_utf8 = access.proxyUrl.isNull() ? nullptr : proxy.constData();
    options.signed_upload_host_suffix_utf8 = host.isEmpty() ? nullptr : host.constData();
    MaiCloudVod* source = mai_cloud_vod_open_cloud_drive(
        &options, id.constData(), accountCancel.get(), vodCancel.get(), result);
    if (!source) return {};
    return DesktopCloudVod(source, [accountCancel = std::move(accountCancel),
                                    vodCancel = std::move(vodCancel)](MaiCloudVod* value) {
        mai_cloud_vod_cancel(value);
        mai_cloud_vod_close(value);
    });
}
