// Opt-in real-account cloud-drive acceptance. This executable never starts the
// normal Desktop IM process or reads its settings, message database or Keychain.
#include "auth/DesktopAuthClient.h"
#include "ui/CloudDriveDialog.h"
#include "ui/ImagePreviewDialog.h"

#include "MaiCloudDriveClient.h"
#include "MaiCurlDownload.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QFrame>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QResource>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <sys/stat.h>
#include <unistd.h>

#include <functional>
#include <iostream>

namespace {

struct Credentials {
    QString accountId;
    QString password;
};

struct MediaFixture {
    QString kind;
    QString path;
    QString name;
    QByteArray sha256;
    qint64 bytes = 0;
    bool image = false;
};

bool readCredentials(const QString& path, const QString& alias, Credentials* output) {
    struct stat metadata {};
    const QByteArray encoded = QFile::encodeName(path);
    if (lstat(encoded.constData(), &metadata) != 0 ||
        !S_ISREG(metadata.st_mode) || (metadata.st_mode & 0777) != 0600 ||
        (metadata.st_uid != geteuid() && metadata.st_uid != 0) ||
        metadata.st_size < 1 || metadata.st_size > 8192)
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QJsonDocument parsed = QJsonDocument::fromJson(file.readAll());
    const QJsonObject root = parsed.object();
    const QJsonObject account = root.value(QStringLiteral("accounts")).toObject()
                                .value(alias).toObject();
    if (root.value(QStringLiteral("version")).toInt() != 1 ||
        account.value(QStringLiteral("state")).toString() != QStringLiteral("verified"))
        return false;
    output->accountId = account.value(QStringLiteral("id")).toString();
    output->password = account.value(QStringLiteral("password")).toString();
    return !output->accountId.isEmpty() && output->accountId.size() <= 128 &&
           !output->password.isEmpty() && output->password.size() <= 4096;
}

bool readMedia(const QString& path, const QString& kind, bool expectedImage,
               MediaFixture* output) {
    const QFileInfo info(path);
    if (!info.isFile() || !info.isReadable() || info.size() < 1) return false;
    const QByteArray display = info.fileName().toUtf8();
    MaiCloudDriveMediaInfo media{};
    if (!mai_cloud_drive_media_info(display.constData(), &media) ||
        (QByteArray(media.media_type_utf8) == QByteArray("image")) != expectedImage ||
        quint64(info.size()) > media.max_size_bytes ||
        (!expectedImage && info.size() > 8 * 1024 * 1024)) return false;
    QFile source(info.canonicalFilePath());
    if (!source.open(QIODevice::ReadOnly)) return false;
    output->kind = kind;
    output->path = info.canonicalFilePath();
    output->name = info.fileName();
    output->bytes = info.size();
    output->image = expectedImage;
    output->sha256 = QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256);
    return !output->path.isEmpty() && !output->sha256.isEmpty();
}

bool waitFor(const std::function<bool()>& predicate, int timeoutMs) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeoutMs) {
        QApplication::processEvents();
        if (predicate()) return true;
        QThread::msleep(20);
    }
    return predicate();
}

QPushButton* visibleButton(QWidget& parent, const QString& objectName) {
    for (auto* button : parent.findChildren<QPushButton*>(objectName))
        if (button->isVisible() && button->isEnabled()) return button;
    return nullptr;
}

bool visibleText(QWidget& parent, const QString& text) {
    for (auto* label : parent.findChildren<QLabel*>())
        if (label->isVisible() && label->text().contains(text)) return true;
    return false;
}

QFrame* rowForName(QWidget& parent, const QString& name) {
    for (auto* label : parent.findChildren<QLabel*>()) {
        if (label->text() != name || !label->isVisible()) continue;
        auto* frame = qobject_cast<QFrame*>(label->parentWidget());
        if (frame && frame->findChild<QLabel*>(QStringLiteral("cloudDriveThumbnail")))
            return frame;
    }
    return nullptr;
}

bool thumbnailLoaded(QWidget& parent, const QString& name) {
    QFrame* row = rowForName(parent, name);
    QLabel* thumbnail = row ? row->findChild<QLabel*>(
        QStringLiteral("cloudDriveThumbnail")) : nullptr;
    return thumbnail && thumbnail->property("thumbnailLoaded").toBool();
}

ImagePreviewDialog* imagePreview(QWidget& parent) {
    auto* preview = parent.findChild<ImagePreviewDialog*>(
        QStringLiteral("cloudDriveImagePreview"));
    return preview && preview->isVisible() ? preview : nullptr;
}

struct ApiResponse {
    MaiCloudDriveStatus status = MAI_CLOUD_DRIVE_INVALID_RESPONSE;
    long http = 0;
    QJsonObject body;
    bool ok() const { return status == MAI_CLOUD_DRIVE_OK && http >= 200 && http < 300; }
};

ApiResponse request(const DesktopVideoCircleAccess& access,
                    MaiCloudDriveAction action, const QJsonObject& payload) {
    const QByteArray base = access.authBaseUrl.toUtf8();
    const QByteArray token = access.accountToken.toUtf8();
    const QByteArray ca = access.caPem;
    const QByteArray json = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    MaiCloudDriveOptions options{};
    options.auth_base_url_utf8 = base.constData();
    options.account_token_utf8 = token.constData();
    options.ca_pem = ca.isEmpty() ? nullptr : ca.constData();
    options.ca_pem_length = size_t(ca.size());
    MaiCloudDriveResult native{};
    mai_cloud_drive_request(&options, action, json.constData(), nullptr, &native);
    ApiResponse reply;
    reply.status = native.status;
    reply.http = native.http_status;
    if (native.response_json_utf8)
        reply.body = QJsonDocument::fromJson(QByteArray(
            native.response_json_utf8, int(native.response_bytes))).object();
    mai_cloud_drive_result_free(&native);
    return reply;
}

bool downloadSignedThumbnail(const DesktopVideoCircleAccess& access,
                             const QString& mediaId, const QString& target,
                             QByteArray* sha256) {
    const ApiResponse link = request(access, MAI_CLOUD_DRIVE_THUMBNAIL_LINK,
        {{QStringLiteral("video_id"), mediaId}});
    if (!link.ok()) return false;
    const QByteArray url = link.body.value(QStringLiteral("thumbnail_url"))
                               .toString().toUtf8();
    const QByteArray path = target.toUtf8();
    const QByteArray proxy = access.proxyUrl.toUtf8();
    MaiDownloadOptions options{};
    options.url_utf8 = url.constData();
    options.target_path_utf8 = path.constData();
    options.max_bytes = 5'000'000;
    options.transfer_timeout_ms = 30'000;
    options.ca_pem = access.caPem.isEmpty() ? nullptr : access.caPem.constData();
    options.ca_pem_length = size_t(access.caPem.size());
    options.proxy_url_utf8 = access.proxyUrl.isNull() ? nullptr : proxy.constData();
    MaiDownloadResult result{};
    mai_download_file(&options, nullptr, &result);
    if (result.status != MAI_DOWNLOAD_OK || QImage(target).isNull()) return false;
    QFile file(target);
    if (!file.open(QIODevice::ReadOnly)) return false;
    *sha256 = QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
    return !sha256->isEmpty();
}

QJsonObject list(const DesktopVideoCircleAccess& access, bool* ok) {
    const ApiResponse reply = request(access, MAI_CLOUD_DRIVE_LIST_VIDEOS, {});
    *ok = reply.ok();
    return reply.body;
}

QStringList checkpoints(const QString& accountId) {
    const QByteArray key = QCryptographicHash::hash(accountId.toUtf8(),
        QCryptographicHash::Sha256).toHex();
    const QString directory = QDir(QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation)).filePath(
            QStringLiteral("CloudDrive/") + QString::fromLatin1(key));
    const QByteArray encoded = directory.toUtf8();
    MaiCloudDriveResult native{};
    mai_cloud_drive_list_local_checkpoints(encoded.constData(), &native);
    QStringList paths;
    if (native.status == MAI_CLOUD_DRIVE_OK && native.response_json_utf8) {
        const QJsonArray uploads = QJsonDocument::fromJson(QByteArray(
            native.response_json_utf8, int(native.response_bytes))).object()
                                      .value(QStringLiteral("uploads")).toArray();
        for (const QJsonValue& upload : uploads)
            paths << upload.toObject().value(QStringLiteral("checkpoint_path")).toString();
    }
    mai_cloud_drive_result_free(&native);
    return paths;
}

bool abortCheckpoint(const DesktopVideoCircleAccess& access, const QString& path) {
    const QByteArray base = access.authBaseUrl.toUtf8();
    const QByteArray token = access.accountToken.toUtf8();
    const QByteArray ca = access.caPem;
    const QByteArray encodedPath = path.toUtf8();
    MaiCloudDriveOptions options{};
    options.auth_base_url_utf8 = base.constData();
    options.account_token_utf8 = token.constData();
    options.ca_pem = ca.isEmpty() ? nullptr : ca.constData();
    options.ca_pem_length = size_t(ca.size());
    MaiCloudDriveResult native{};
    mai_cloud_drive_abort_resumable(&options, encodedPath.constData(), nullptr, &native);
    const bool ok = native.status == MAI_CLOUD_DRIVE_OK;
    mai_cloud_drive_result_free(&native);
    return ok;
}

bool writeEvidence(const QString& directory, const QJsonObject& evidence) {
    QSaveFile file(QDir(directory).filePath(QStringLiteral("cloud-drive-live-result.json")));
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(QJsonDocument(evidence).toJson(QJsonDocument::Indented)) < 0)
        return false;
    return file.commit();
}

void usage() {
    std::cout << "Usage: cloud_drive_dialog_live_test --execute-live=cloud-drive-canary "
                 "--credentials-file=PATH --account-alias=B --video=PATH "
                 "--jpeg=PATH --heic=PATH --evidence-dir=PATH\n"
                 "       cloud_drive_dialog_live_test --check-isolation\n"
                 "Credential file: mode 0600 JSON with version=1 and accounts.B "
                 "{id,password,state:verified}. Only disposable reserved account B.\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && QString::fromUtf8(argv[1]) == QStringLiteral("--check-isolation")) {
        QStandardPaths::setTestModeEnabled(true);
        QApplication app(argc, argv);
        QCoreApplication::setOrganizationName(QStringLiteral("MaiChatCloudLiveAcceptance"));
        QCoreApplication::setApplicationName(QStringLiteral("CloudDriveLive-") +
            QUuid::createUuid().toString(QUuid::WithoutBraces));
        const QString data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (data.isEmpty() || !data.endsWith(QCoreApplication::applicationName()))
            return 3;
        std::cout << "Qt data path is isolated\n";
        return 0;
    }
    QString credentialPath;
    QString alias;
    QString videoPath;
    QString jpegPath;
    QString heicPath;
    QString evidenceDirectory;
    bool execute = false;
    for (int i = 1; i < argc; ++i) {
        const QString argument = QString::fromUtf8(argv[i]);
        if (argument == QStringLiteral("--help")) { usage(); return 0; }
        if (argument == QStringLiteral("--execute-live=cloud-drive-canary")) execute = true;
        else if (argument.startsWith(QStringLiteral("--credentials-file=")))
            credentialPath = argument.mid(19);
        else if (argument.startsWith(QStringLiteral("--account-alias=")))
            alias = argument.mid(16);
        else if (argument.startsWith(QStringLiteral("--video=")))
            videoPath = argument.mid(8);
        else if (argument.startsWith(QStringLiteral("--jpeg=")))
            jpegPath = argument.mid(7);
        else if (argument.startsWith(QStringLiteral("--heic=")))
            heicPath = argument.mid(7);
        else if (argument.startsWith(QStringLiteral("--evidence-dir=")))
            evidenceDirectory = argument.mid(15);
        else { usage(); return 2; }
    }
    if (!execute || credentialPath.isEmpty() || alias != QStringLiteral("B") ||
        videoPath.isEmpty() || jpegPath.isEmpty() || heicPath.isEmpty() ||
        evidenceDirectory.isEmpty()) { usage(); return 2; }
    Credentials credentials;
    if (!readCredentials(credentialPath, alias, &credentials)) {
        std::cerr << "credential file missing, unsafe, or invalid\n";
        return 3;
    }
    MediaFixture video;
    MediaFixture jpeg;
    MediaFixture heic;
    if (!readMedia(videoPath, QStringLiteral("video"), false, &video) ||
        !readMedia(jpegPath, QStringLiteral("jpeg"), true, &jpeg) ||
        !readMedia(heicPath, QStringLiteral("heic"), true, &heic) ||
        video.bytes > 8 * 1024 * 1024 ||
        QFileInfo(video.path).suffix().compare(QStringLiteral("mp4"), Qt::CaseInsensitive) != 0 ||
        QFileInfo(jpeg.path).suffix().compare(QStringLiteral("jpg"), Qt::CaseInsensitive) != 0 ||
        QFileInfo(heic.path).suffix().compare(QStringLiteral("heic"), Qt::CaseInsensitive) != 0) {
        std::cerr << "fixtures must be readable small H.264 MP4, JPEG and HEIC files\n";
        return 3;
    }
    if (!QDir().mkpath(evidenceDirectory)) {
        std::cerr << "could not create evidence directory\n";
        return 3;
    }

    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("MaiChatCloudLiveAcceptance"));
    QCoreApplication::setApplicationName(QStringLiteral("CloudDriveLive-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString appData = QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation);
    if (appData.isEmpty() ||
        !appData.endsWith(QCoreApplication::applicationName())) {
        std::cerr << "Qt test data directory is not isolated\n";
        return 3;
    }
    Q_INIT_RESOURCE(AuthCertificates);

    QJsonObject evidence{{QStringLiteral("account_alias"), alias},
        {QStringLiteral("client"), QStringLiteral("mac-desktop-dialog")},
        {QStringLiteral("time_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {QStringLiteral("status"), QStringLiteral("started")}};
    const QList<MediaFixture> fixtures{video, jpeg, heic};
    DesktopAuthClient auth(nullptr, QUrl(QStringLiteral("https://ichat.life/api/v1/")), false);
    bool loginFinished = false;
    bool loginOk = false;
    auth.loginWithPassword(credentials.accountId, credentials.password,
        [&](bool ok, const QString&) { loginOk = ok; loginFinished = true; });
    credentials.password.fill(QChar(0));
    QString failure;
    if (!waitFor([&] { return loginFinished; }, 30000) || !loginOk)
        failure = QStringLiteral("disposable account login failed");

    DesktopVideoCircleAccess access;
    if (failure.isEmpty()) {
        access = auth.videoCircleAccessProvider()();
        if (!access.isValid() || access.accountId != credentials.accountId ||
            access.authBaseUrl != QStringLiteral("https://ichat.life/api"))
            failure = QStringLiteral("unexpected account or auth endpoint");
    }
    QFile certificate(QStringLiteral(":/maichat/auth/cacert.pem"));
    if (failure.isEmpty() && certificate.open(QIODevice::ReadOnly))
        access.caPem = certificate.readAll();
    bool listed = false;
    QJsonObject initial;
    bool accountInitiallyEmpty = false;
    if (failure.isEmpty()) {
        initial = list(access, &listed);
        const QJsonObject quota = initial.value(QStringLiteral("quota")).toObject();
        accountInitiallyEmpty = listed &&
            initial.value(QStringLiteral("videos")).toArray().isEmpty() &&
            initial.value(QStringLiteral("next_cursor")).isNull() &&
            quota.value(QStringLiteral("used_bytes")).toDouble() == 0 &&
            quota.value(QStringLiteral("reserved_bytes")).toDouble() == 0;
        if (!accountInitiallyEmpty)
            failure = QStringLiteral("cloud gate unavailable or test account is not empty");
    }

    QString playedPath;
    QJsonArray mediaEvidence;
    bool workersStopped = true;
    if (failure.isEmpty()) {
        CloudDriveDialog dialog([&] { return access; },
            [&] { return credentials.accountId; },
            [&](const QString& path) { playedPath = path; });
        dialog.openDrive();
        if (!waitFor([&] { return visibleText(dialog,
                QStringLiteral("云盘里还没有图片或视频")); }, 20000))
            failure = QStringLiteral("empty cloud UI did not render");
        if (failure.isEmpty() && !dialog.grab().save(QDir(evidenceDirectory)
                .filePath(QStringLiteral("cloud-live-empty.png"))))
            failure = QStringLiteral("could not capture empty UI");
        QTimer confirmations;
        int confirmationCount = 0;
        int visibleConfirmDialogs = 0;
        QString confirmationShot;
        confirmations.setInterval(20);
        QObject::connect(&confirmations, &QTimer::timeout, [&] {
            auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!modal) {
                for (QWidget* top : QApplication::topLevelWidgets()) {
                    if (top->isVisible() &&
                        top->objectName() == QStringLiteral("appMessageDialog")) {
                        modal = qobject_cast<QDialog*>(top);
                        break;
                    }
                }
            }
            if (!modal) return;
            ++visibleConfirmDialogs;
            auto* button = modal->findChild<QPushButton*>(
                QStringLiteral("appMessageConfirm"));
            if (!button) button = modal->findChild<QPushButton*>(
                QStringLiteral("appMessageDanger"));
            if (button) {
                modal->grab().save(QDir(evidenceDirectory).filePath(confirmationShot));
                confirmations.stop();
                ++confirmationCount;
                button->click();
            }
        });
        for (const MediaFixture& fixture : fixtures) {
            if (!failure.isEmpty()) break;
            confirmationShot = QStringLiteral("cloud-live-%1-upload-confirm.png")
                .arg(fixture.kind);
            confirmations.start();
            dialog.openDrive(fixture.path);
            const QString action = fixture.image ? QStringLiteral("cloudDriveViewImage") :
                                                   QStringLiteral("cloudDrivePlay");
            if (!waitFor([&] {
                    QFrame* row = rowForName(dialog, fixture.name);
                    return row && visibleButton(*row, action);
                }, 60000))
                failure = fixture.kind + QStringLiteral(" upload or UI list timed out");
            confirmations.stop();
            if (failure.isEmpty() && !waitFor([&] {
                    return thumbnailLoaded(dialog, fixture.name);
                }, 30000))
                failure = fixture.kind + QStringLiteral(" signed thumbnail did not render");
            if (!failure.isEmpty()) {
                dialog.grab().save(QDir(evidenceDirectory)
                    .filePath(QStringLiteral("cloud-live-%1-failed.png").arg(fixture.kind)));
                auto* status = dialog.findChild<QLabel*>(QStringLiteral("cloudDriveStatus"));
                evidence.insert(QStringLiteral("status_text"),
                    status ? status->text() : QStringLiteral("missing"));
                evidence.insert(QStringLiteral("confirmation_count"), confirmationCount);
                evidence.insert(QStringLiteral("visible_confirm_dialog_ticks"), visibleConfirmDialogs);
                break;
            }
            bool ok = false;
            const QJsonArray ready = list(access, &ok)
                .value(QStringLiteral("videos")).toArray();
            const QJsonObject entry = ready.size() == 1 ? ready.first().toObject() :
                                                       QJsonObject{};
            const QString mediaId = entry.value(QStringLiteral("id")).toString();
            if (!ok || ready.size() != 1 || mediaId.isEmpty() ||
                entry.value(QStringLiteral("name")).toString() != fixture.name ||
                entry.value(QStringLiteral("size_bytes")).toDouble() != fixture.bytes ||
                entry.value(QStringLiteral("media_type")).toString() !=
                    (fixture.image ? QStringLiteral("image") : QStringLiteral("video"))) {
                failure = fixture.kind + QStringLiteral(" uploaded metadata mismatch");
                break;
            }
            const QString stem = QStringLiteral("cloud-live-%1").arg(fixture.kind);
            if (!dialog.grab().save(QDir(evidenceDirectory)
                    .filePath(stem + QStringLiteral("-uploaded.png")))) {
                failure = fixture.kind + QStringLiteral(" UI capture failed");
                break;
            }
            QByteArray thumbnailSha;
            if (!downloadSignedThumbnail(access, mediaId,
                    QDir(evidenceDirectory).filePath(stem + QStringLiteral("-thumbnail.jpg")),
                    &thumbnailSha)) {
                failure = fixture.kind + QStringLiteral(" signed thumbnail GET failed");
                break;
            }
            QFrame* row = rowForName(dialog, fixture.name);
            auto* open = row ? visibleButton(*row, action) : nullptr;
            if (!open) {
                failure = fixture.kind + QStringLiteral(" open action disappeared");
                break;
            }
            open->click();
            if (fixture.image) {
                if (!waitFor([&] { return imagePreview(dialog) != nullptr; }, 60000)) {
                    failure = fixture.kind + QStringLiteral(" original preview timed out");
                    break;
                }
                ImagePreviewDialog* preview = imagePreview(dialog);
                if (!preview || preview->sourceImage().isNull() ||
                    !preview->grab().save(QDir(evidenceDirectory)
                        .filePath(stem + QStringLiteral("-preview.png"))))
                    failure = fixture.kind + QStringLiteral(" original preview failed");
                if (preview) preview->close();
            } else {
                if (!waitFor([&] { return !playedPath.isEmpty(); }, 60000))
                    failure = QStringLiteral("video playback download timed out");
                else {
                    QFile downloaded(playedPath);
                    if (!downloaded.open(QIODevice::ReadOnly) ||
                        QCryptographicHash::hash(downloaded.readAll(),
                                                 QCryptographicHash::Sha256) != fixture.sha256)
                        failure = QStringLiteral("video playback bytes differ from source");
                    QFile::remove(playedPath);
                    playedPath.clear();
                }
            }
            if (!failure.isEmpty()) break;
            confirmationShot = stem + QStringLiteral("-delete-confirm.png");
            confirmations.start();
            row = rowForName(dialog, fixture.name);
            auto* remove = row ? visibleButton(*row, QStringLiteral("cloudDriveDelete")) : nullptr;
            if (!remove) failure = fixture.kind + QStringLiteral(" delete action not visible");
            else {
                remove->click();
                if (!waitFor([&] { return visibleText(dialog,
                        QStringLiteral("云盘里还没有图片或视频")); }, 60000))
                    failure = fixture.kind + QStringLiteral(" delete did not restore empty UI");
            }
            confirmations.stop();
            if (!failure.isEmpty()) break;
            if (!dialog.grab().save(QDir(evidenceDirectory)
                    .filePath(stem + QStringLiteral("-deleted.png")))) {
                failure = fixture.kind + QStringLiteral(" deleted UI capture failed");
                break;
            }
            mediaEvidence.append(QJsonObject{
                {QStringLiteral("kind"), fixture.kind},
                {QStringLiteral("media_id"), mediaId},
                {QStringLiteral("source_name"), fixture.name},
                {QStringLiteral("source_size_bytes"), double(fixture.bytes)},
                {QStringLiteral("source_sha256"), QString::fromLatin1(fixture.sha256.toHex())},
                {QStringLiteral("thumbnail_sha256"), QString::fromLatin1(thumbnailSha.toHex())},
                {QStringLiteral("ui_thumbnail"), true},
                {QStringLiteral("ui_opened"), true},
                {QStringLiteral("ui_deleted"), true}});
        }
        dialog.close();
        workersStopped = waitFor([&] {
            for (auto* watcher : dialog.findChildren<QFutureWatcherBase*>())
                if (!watcher->isFinished()) return false;
            return true;
        }, 15000);
    }

    // This account was empty at entry. Clean only its fixture media and this
    // executable's isolated checkpoints; never touch another account.
    bool cleanupComplete = workersStopped && access.isValid() && accountInitiallyEmpty;
    if (cleanupComplete) {
        for (const QString& checkpoint : checkpoints(credentials.accountId))
            cleanupComplete &= abortCheckpoint(access, checkpoint);
        bool ok = false;
        const QJsonObject current = list(access, &ok);
        cleanupComplete &= ok;
        if (ok) for (const QJsonValue& mediaItem : current.value(QStringLiteral("videos")).toArray()) {
            const QJsonObject body = mediaItem.toObject();
            const QString id = body.value(QStringLiteral("id")).toString();
            bool owned = false;
            for (const MediaFixture& fixture : fixtures)
                owned |= body.value(QStringLiteral("name")).toString() == fixture.name &&
                         body.value(QStringLiteral("size_bytes")).toDouble() == fixture.bytes;
            if (id.isEmpty() || !owned) {
                cleanupComplete = false;
                continue;
            }
            cleanupComplete &= request(access, MAI_CLOUD_DRIVE_DELETE_VIDEO,
                {{QStringLiteral("video_id"), id}}).ok();
        }
        cleanupComplete &= waitFor([&] {
            bool finalOk = false;
            const QJsonObject final = list(access, &finalOk);
            const QJsonObject quota = final.value(QStringLiteral("quota")).toObject();
            return finalOk && final.value(QStringLiteral("videos")).toArray().isEmpty() &&
                quota.value(QStringLiteral("used_bytes")).toDouble() == 0 &&
                quota.value(QStringLiteral("reserved_bytes")).toDouble() == 0;
        }, 30000);
    }
    if (!playedPath.isEmpty()) QFile::remove(playedPath);
    evidence.insert(QStringLiteral("account_id"), credentials.accountId);
    evidence.insert(QStringLiteral("test_data_directory"), appData);
    if (access.isValid()) {
        bool finalOk = false;
        const QJsonObject final = list(access, &finalOk);
        if (finalOk) {
            QJsonArray remainingMediaIds;
            for (const QJsonValue& mediaItem : final.value(QStringLiteral("videos")).toArray())
                remainingMediaIds.append(mediaItem.toObject().value(QStringLiteral("id")));
            evidence.insert(QStringLiteral("remaining_media_ids"), remainingMediaIds);
            evidence.insert(QStringLiteral("remaining_quota"),
                final.value(QStringLiteral("quota")));
        }
        evidence.insert(QStringLiteral("remaining_checkpoints"),
            QJsonArray::fromStringList(checkpoints(credentials.accountId)));
    }
    if (!auth.maiChatAccountId().isEmpty()) {
        bool loggedOut = false;
        auth.logout([&](bool, const QString&) { loggedOut = true; });
        waitFor([&] { return loggedOut; }, 20000);
    }
    evidence.insert(QStringLiteral("media"), mediaEvidence);
    evidence.insert(QStringLiteral("cleanup_complete"), cleanupComplete);
    evidence.insert(QStringLiteral("status"), failure.isEmpty() && cleanupComplete ?
        QStringLiteral("passed") : QStringLiteral("failed"));
    if (!failure.isEmpty()) evidence.insert(QStringLiteral("failure"), failure);
    if (!writeEvidence(evidenceDirectory, evidence)) {
        std::cerr << "could not write recovery evidence\n";
        return 8;
    }
    if (!failure.isEmpty() || !cleanupComplete) {
        std::cerr << "cloud UI acceptance failed; inspect recovery evidence\n";
        return 7;
    }
    QDir(appData).removeRecursively();
    std::cout << "desktop real-account cloud video/JPEG/HEIC UI and thumbnail GET passed; cleanup complete\n";
    return 0;
}
