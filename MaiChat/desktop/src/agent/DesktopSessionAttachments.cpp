#include "agent/DesktopSessionAttachments.h"

#include "MaiCloudDriveClient.h"
#include "MaiCurlDownload.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QResource>
#include <QStandardPaths>
#include <QUuid>

static QByteArray trustedRoots(const DesktopVideoCircleAccess& access) {
    if (!access.caPem.isEmpty()) return access.caPem;
    Q_INIT_RESOURCE(AuthCertificates);
    QFile resource(QStringLiteral(":/maichat/auth/cacert.pem"));
    return resource.open(QIODevice::ReadOnly) ? resource.readAll() : QByteArray();
}

namespace {

struct Call {
    QByteArray base, token, roots, proxy, host, slot, login, session;
    MaiCloudDriveOptions options{};
    explicit Call(const DesktopVideoCircleAccess& access, const DesktopSessionRoom& room)
        : base(access.authBaseUrl.toUtf8()), token(access.accountToken.toUtf8()),
          roots(trustedRoots(access)), proxy(access.proxyUrl.toUtf8()),
          host(access.signedUploadHostSuffix.toUtf8()), slot(room.slot.toUtf8()),
          login(room.login.toUtf8()), session(room.session.toUtf8()) {
        options.auth_base_url_utf8 = base.constData();
        options.account_token_utf8 = token.constData();
        options.ca_pem = roots.isEmpty() ? nullptr : roots.constData();
        options.ca_pem_length = size_t(roots.size());
        options.proxy_url_utf8 = access.proxyUrl.isNull() ? nullptr : proxy.constData();
        options.signed_upload_host_suffix_utf8 = host.isEmpty() ? nullptr : host.constData();
        options.owner_device_slot_utf8 = slot.constData();
        options.owner_login_session_id_utf8 = login.constData();
        options.session_id_utf8 = session.constData();
        options.retain_completed_checkpoint = 1;
    }
};

DesktopSessionAttachmentResult parse(MaiCloudDriveResult* raw) {
    DesktopSessionAttachmentResult reply;
    if (raw->response_json_utf8)
        reply.metadata = QJsonDocument::fromJson(QByteArray(raw->response_json_utf8,
            int(raw->response_bytes))).object();
    if (raw->status != MAI_CLOUD_DRIVE_OK)
        reply.error = raw->http_status == 401 ? QStringLiteral("请重新登录 MaiChat") :
            raw->http_status == 413 ? QStringLiteral("附件超过大小限制或会话空间已满") :
            raw->http_status == 404 ? QStringLiteral("会话附件已不存在") :
            QStringLiteral("会话附件传输失败，请重试（%1）").arg(int(raw->status));
    mai_cloud_drive_result_free(raw);
    return reply;
}

QString directory(const QString& name, const QString& account, bool cache = false) {
    const QString root = QStandardPaths::writableLocation(
        cache ? QStandardPaths::CacheLocation : QStandardPaths::AppDataLocation);
    const QString digest = QString::fromLatin1(QCryptographicHash::hash(
        account.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
    const QString path = QDir(root).filePath(QStringLiteral("session-attachments/") + digest +
                                             QLatin1Char('/') + name);
    return QDir().mkpath(path) ? path : QString();
}

}  // namespace

DesktopSessionAttachmentResult DesktopSessionAttachments::upload(
    const DesktopVideoCircleAccess& access, const DesktopSessionRoom& room,
    const QString& source) {
    if (!access.isValid() || !QFileInfo(source).isFile())
        return {{}, {}, QStringLiteral("会话附件文件或账号不可用")};
    const QFileInfo file(source);
    const QByteArray name = file.fileName().toUtf8();
    MaiCloudDriveMediaInfo media{};
    if (!mai_session_attachment_media_info(name.constData(), &media) ||
        file.size() < 1 || quint64(file.size()) > media.max_size_bytes)
        return {{}, {}, QStringLiteral("会话附件格式或大小无效")};
    const QString folder = directory(QStringLiteral("uploads"), access.accountId);
    if (folder.isEmpty()) return {{}, {}, QStringLiteral("无法保存会话附件上传记录")};
    const QByteArray digest = QCryptographicHash::hash(
        (room.key() + QLatin1Char(':') + file.absoluteFilePath()).toUtf8(),
        QCryptographicHash::Sha256).toHex();
    const QString checkpoint = QDir(folder).filePath(
        QString::fromLatin1(digest) + QStringLiteral(".maicloud.json"));
    Call call(access, room);
    const QByteArray path = file.absoluteFilePath().toUtf8();
    const QByteArray task = checkpoint.toUtf8();
    MaiCloudDriveResult raw{};
    mai_cloud_drive_upload_resumable(&call.options, path.constData(), task.constData(),
        name.constData(), media.content_type_utf8, nullptr, nullptr, nullptr, &raw);
    auto reply = parse(&raw);
    if (reply.ok() &&
        (reply.metadata.value(QStringLiteral("room_key")).toString() != room.key() ||
         reply.metadata.value(QStringLiteral("name")).toString() != file.fileName() ||
         reply.metadata.value(QStringLiteral("content_type")).toString() !=
             QString::fromLatin1(media.content_type_utf8) ||
         reply.metadata.value(QStringLiteral("media_type")).toString() !=
             QString::fromLatin1(media.media_type_utf8) ||
         reply.metadata.value(QStringLiteral("size_bytes")).toVariant().toLongLong() != file.size()))
        reply.error = QStringLiteral("会话附件上传回执不匹配");
    return reply;
}

DesktopSessionAttachmentResult DesktopSessionAttachments::download(
    const DesktopVideoCircleAccess& access, const DesktopSessionRoom& room,
    const QString& attachmentId) {
    if (!access.isValid()) return {{}, {}, QStringLiteral("请先登录 MaiChat")};
    Call call(access, room);
    const QByteArray request = QJsonDocument(QJsonObject{
        {QStringLiteral("video_id"), attachmentId}}).toJson(QJsonDocument::Compact);
    MaiCloudDriveResult raw{};
    mai_cloud_drive_request(&call.options, MAI_CLOUD_DRIVE_PLAY_LINK,
        request.constData(), nullptr, &raw);
    auto reply = parse(&raw);
    if (!reply.ok()) return reply;
    const QString name = reply.metadata.value(QStringLiteral("name")).toString();
    const QByteArray display = name.toUtf8();
    MaiCloudDriveMediaInfo media{};
    const qint64 bytes = reply.metadata.value(QStringLiteral("size_bytes")).toVariant().toLongLong();
    if (!mai_session_attachment_media_info(display.constData(), &media) ||
        bytes < 1 || quint64(bytes) > media.max_size_bytes ||
        reply.metadata.value(QStringLiteral("room_key")).toString() != room.key() ||
        reply.metadata.value(QStringLiteral("content_type")).toString() !=
            QString::fromLatin1(media.content_type_utf8) ||
        reply.metadata.value(QStringLiteral("media_type")).toString() !=
            QString::fromLatin1(media.media_type_utf8)) {
        reply.error = QStringLiteral("会话附件归属或文件信息无效");
        return reply;
    }
    const QString folder = directory(QStringLiteral("downloads"), access.accountId, true);
    if (folder.isEmpty()) return {{}, {}, QStringLiteral("无法准备会话附件目录")};
    const QString target = QDir(folder).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) +
                                               QLatin1Char('.') + QFileInfo(name).suffix());
    const QByteArray signedUrl = reply.metadata.value(QStringLiteral("read_url")).toString().toUtf8();
    const QByteArray path = target.toUtf8();
    MaiDownloadOptions options{};
    options.url_utf8 = signedUrl.constData();
    options.target_path_utf8 = path.constData();
    options.max_bytes = uint64_t(bytes);
    options.ca_pem = call.roots.isEmpty() ? nullptr : call.roots.constData();
    options.ca_pem_length = size_t(call.roots.size());
    options.proxy_url_utf8 = access.proxyUrl.isNull() ? nullptr : call.proxy.constData();
    options.transfer_timeout_ms = 2 * 60 * 60 * 1000;
    MaiDownloadResult downloaded{};
    mai_download_file(&options, nullptr, &downloaded);
    if (downloaded.status != MAI_DOWNLOAD_OK || downloaded.bytes != uint64_t(bytes)) {
        QFile::remove(target);
        reply.error = QStringLiteral("会话附件下载失败，请重试");
        return reply;
    }
    reply.path = target;
    return reply;
}

DesktopSessionAttachmentResult DesktopSessionAttachments::cleanupRoom(
    const DesktopVideoCircleAccess& access, const DesktopSessionRoom& room) {
    if (!access.isValid()) return {{}, {}, QStringLiteral("请先登录 MaiChat")};
    Call call(access, room);
    MaiCloudDriveResult raw{};
    mai_cloud_drive_request(&call.options, MAI_SESSION_ATTACHMENT_CLEANUP_ROOM,
                            "{}", nullptr, &raw);
    return parse(&raw);
}
