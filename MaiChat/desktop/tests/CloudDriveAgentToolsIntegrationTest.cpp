#include "MaiChatTools/DesktopCloudDriveTools.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QThread>
#include <QWidget>

#include <cstdlib>
#include <iostream>
#include <string>

#include "MaiTool.h"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

QJsonObject output(const MaiToolResult& result) {
    require(!result.hasError(), result.hasError() ? result.error().message().c_str() : "tool failed");
    const QJsonDocument body = QJsonDocument::fromJson(QByteArray::fromStdString(result.output()));
    require(body.isObject(), "cloud tool returned invalid JSON");
    return body.object();
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 5, "expected TLS base, certificate, fixture video, screenshots directory");
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QFile certificate(QString::fromUtf8(argv[2]));
    require(certificate.open(QIODevice::ReadOnly), "could not load TLS test certificate");
    const QString source = QString::fromUtf8(argv[3]);
    require(QFileInfo(source).isFile(), "missing fixture MP4");
    DesktopVideoCircleAccess access;
    access.authBaseUrl = QString::fromUtf8(argv[1]) + QStringLiteral("/auth");
    access.mediaBaseUrl = QString::fromUtf8(argv[1]) + QStringLiteral("/oss");
    access.accountToken = QStringLiteral("author-token");
    access.accountId = QStringLiteral("author-") +
        QString::number(QCoreApplication::applicationPid());
    access.caPem = certificate.readAll();
    access.signedUploadHostSuffix = QStringLiteral("localhost");
    access.proxyUrl = QStringLiteral("");

    QWidget presenter;
    MaiToolRegistry registry;
    QString playedPath;
    bool openedAsImage = false;
    registerDesktopCloudDriveTools(
        registry, [access] { return access; }, &presenter,
        [&playedPath, &openedAsImage](const QString& path, bool image) {
            playedPath = path;
            openedAsImage = image;
            return QFileInfo(path).isFile();
        });
    MaiToolContext context;
    context.root = QFileInfo(source).absolutePath().toUtf8().toStdString();
    const QJsonObject first = output(registry.findForModel("maichat_cloud_drive_list_media")->execute("{}", context));
    require(first.value(QStringLiteral("media")).toArray().isEmpty(), "private cloud fixture was not initially empty");

    MaiTool* upload = registry.findForModel("maichat_cloud_drive_upload_media");
    require(upload && upload->requiresPerCallApproval("{}"), "upload lost per-call approval");
    const QByteArray uploadArgs =
        QJsonDocument(QJsonObject{{QStringLiteral("file_path"), source}}).toJson(QJsonDocument::Compact);
    DesktopVideoCircleAccess disabled = access;
    disabled.authBaseUrl = QString::fromUtf8(argv[1]) + QStringLiteral("/disabled");
    MaiToolRegistry disabledRegistry;
    registerDesktopCloudDriveTools(
        disabledRegistry, [disabled] { return disabled; }, &presenter, [](const QString&, bool) { return false; });
    require(disabledRegistry.findForModel("maichat_cloud_drive_list_media")->execute("{}", context).error().code() ==
                MaiErrorCode::NotConfigured,
            "disabled account gate was advertised as usable");
    require(disabledRegistry.findForModel("maichat_cloud_drive_upload_media")
                    ->execute(uploadArgs.toStdString(), context)
                    .error()
                    .code() == MaiErrorCode::NotConfigured,
            "disabled account gate started an upload");
    const QJsonObject accepted = output(upload->execute(uploadArgs.toStdString(), context));
    require(accepted.value(QStringLiteral("accepted")) == true &&
            !accepted.value(QStringLiteral("task_id")).toString().isEmpty(),
            "upload was not queued with a task ID");
    QJsonArray listedVideos;
    QElapsedTimer completedWait;
    completedWait.start();
    while (completedWait.elapsed() < 15000) {
        listedVideos = output(registry.findForModel("maichat_cloud_drive_list_media")
                                  ->execute("{}", context)).value(QStringLiteral("media")).toArray();
        if (listedVideos.size() == 1) break;
        QThread::msleep(20);
    }
    require(listedVideos.size() == 1, "queued upload did not complete");
    QElapsedTimer checkpointWait;
    checkpointWait.start();
    bool checkpointCleared = false;
    while (checkpointWait.elapsed() < 5000) {
        checkpointCleared = output(registry.findForModel("maichat_cloud_drive_list_pending_uploads")
                                       ->execute("{}", context))
                                .value(QStringLiteral("uploads"))
                                .toArray()
                                .isEmpty();
        if (checkpointCleared) break;
        QThread::msleep(20);
    }
    require(checkpointCleared, "completed upload left a local checkpoint");
    const QJsonObject listedVideo = listedVideos.at(0).toObject();
    const QString videoId = listedVideo.value(QStringLiteral("media_id")).toString();
    require(!videoId.isEmpty() && !listedVideo.contains(QStringLiteral("id")) &&
            listedVideo.value(QStringLiteral("media_type")) == QStringLiteral("video"),
            "Desktop cloud media list must expose one media_id and type");

    const QByteArray idArgs =
        QJsonDocument(QJsonObject{{QStringLiteral("media_id"), videoId}}).toJson(QJsonDocument::Compact);
    const QJsonObject opened =
        output(registry.findForModel("maichat_cloud_drive_open_media")->execute(idArgs.toStdString(), context));
    require(opened.value(QStringLiteral("opened")) == true && QFileInfo(playedPath).isFile(),
            "cloud video did not open from a private download");
    require(!openedAsImage, "video opened through the image viewer");
    QFile::remove(playedPath);

    MaiTool* remove = registry.findForModel("maichat_cloud_drive_delete_media");
    require(remove && remove->requiresPerCallApproval("{}"), "delete lost per-call approval");
    const QJsonObject deleted = output(remove->execute(idArgs.toStdString(), context));
    require(deleted.value(QStringLiteral("status")) == QStringLiteral("deleted"),
            "delete was not confirmed by account server");
    require(output(registry.findForModel("maichat_cloud_drive_list_media")->execute("{}", context))
                .value(QStringLiteral("media"))
                .toArray()
                .isEmpty(),
            "deleted video remained in owner list");

    // Repeated accepted tasks must stay usable as completed worker threads are reaped.
    for (int repeat = 0; repeat < 3; ++repeat) {
        const QJsonObject next = output(upload->execute(uploadArgs.toStdString(), context));
        require(next.value(QStringLiteral("accepted")) == true, "repeat upload was not queued");
        QElapsedTimer readyWait;
        readyWait.start();
        bool ready = false;
        while (readyWait.elapsed() < 15000) {
            const QJsonArray videos = output(registry.findForModel(
                "maichat_cloud_drive_list_media")->execute("{}", context))
                .value(QStringLiteral("media")).toArray();
            const QJsonArray pending = output(registry.findForModel(
                "maichat_cloud_drive_list_pending_uploads")->execute("{}", context))
                .value(QStringLiteral("uploads")).toArray();
            ready = videos.size() == 1 && pending.isEmpty();
            if (ready) break;
            QThread::msleep(20);
        }
        require(ready, "repeat upload did not finish and release its worker");
        require(output(remove->execute(idArgs.toStdString(), context))
                    .value(QStringLiteral("status")) == QStringLiteral("deleted"),
                "repeat upload could not be deleted");
    }

    const QString imagePath = QString::fromUtf8(argv[4]) + QStringLiteral("/cloud-agent-image.png");
    QImage image(64, 48, QImage::Format_RGB32);
    image.fill(QColor(45, 133, 206));
    require(image.save(imagePath, "PNG"), "could not create image fixture");
    MaiToolContext imageContext;
    imageContext.root = QFileInfo(imagePath).absolutePath().toUtf8().toStdString();
    const QByteArray imageArgs = QJsonDocument(QJsonObject{
        {QStringLiteral("file_path"), imagePath}}).toJson(QJsonDocument::Compact);
    require(output(upload->execute(imageArgs.toStdString(), imageContext))
                .value(QStringLiteral("accepted")) == true,
            "image upload was not queued");
    QJsonObject listedImage;
    QElapsedTimer imageWait;
    imageWait.start();
    while (imageWait.elapsed() < 15000) {
        const QJsonArray media = output(registry.findForModel(
            "maichat_cloud_drive_list_media")->execute("{}", imageContext))
            .value(QStringLiteral("media")).toArray();
        if (media.size() == 1 && media.at(0).toObject()
                .value(QStringLiteral("media_type")) == QStringLiteral("image")) {
            listedImage = media.at(0).toObject();
            break;
        }
        QThread::msleep(20);
    }
    require(!listedImage.isEmpty(), "uploaded image was not listed as media");
    const QByteArray imageIdArgs = QJsonDocument(QJsonObject{
        {QStringLiteral("media_id"), listedImage.value(QStringLiteral("media_id"))}})
        .toJson(QJsonDocument::Compact);
    require(output(registry.findForModel("maichat_cloud_drive_open_media")
                ->execute(imageIdArgs.toStdString(), imageContext))
                .value(QStringLiteral("media_type")) == QStringLiteral("image") &&
            openedAsImage && QFileInfo(playedPath).isFile(),
            "image did not open through its private download");
    QFile::remove(playedPath);
    require(output(remove->execute(imageIdArgs.toStdString(), imageContext))
                .value(QStringLiteral("status")) == QStringLiteral("deleted"),
            "image delete was not confirmed");

    const QString largePath = QString::fromUtf8(argv[4]) + QStringLiteral("/pause-fixture.mov");
    QFile large(largePath);
    require(large.open(QIODevice::WriteOnly | QIODevice::Truncate), "could not create upload pause fixture");
    const QByteArray block(512 * 1024, 'v');
    for (int i = 0; i < 48; ++i)
        require(large.write(block) == block.size(), "could not write upload pause fixture");
    large.close();
    MaiToolContext largeContext;
    largeContext.root = QFileInfo(largePath).absolutePath().toUtf8().toStdString();
    const QByteArray largeArgs = QJsonDocument(QJsonObject{
        {QStringLiteral("file_path"), largePath}}).toJson(QJsonDocument::Compact);
    const QJsonObject queued = output(upload->execute(largeArgs.toStdString(), largeContext));
    const QString queuedTaskId = queued.value(QStringLiteral("task_id")).toString();
    require(queued.value(QStringLiteral("accepted")) == true && !queuedTaskId.isEmpty(),
            "same-session upload did not return a task ID immediately");
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(
        access.accountId.toUtf8(), QCryptographicHash::Sha256).toHex());
    const QString checkpointDir = QDir(QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation)).filePath(QStringLiteral("CloudDrive/") + hash);
    QString taskId;
    QElapsedTimer wait;
    wait.start();
    while (wait.elapsed() < 15000) {
        const QJsonArray uploads = output(registry.findForModel(
            "maichat_cloud_drive_list_pending_uploads")->execute("{}", largeContext))
            .value(QStringLiteral("uploads")).toArray();
        for (const QJsonValue& entry : uploads) {
            const QJsonObject item = entry.toObject();
            if (item.value(QStringLiteral("name")) != QStringLiteral("pause-fixture.mov")) continue;
            const QString candidate = item.value(QStringLiteral("task_id")).toString();
            if (candidate != queuedTaskId ||
                !QFileInfo(QDir(checkpointDir).filePath(candidate + QStringLiteral(".maicloud.json"))).isFile())
                continue;
            taskId = candidate;
            break;
        }
        if (!taskId.isEmpty()) break;
        QThread::msleep(10);
    }
    require(!taskId.isEmpty(), "active upload did not expose a resumable task ID");
    const QByteArray taskArgs = QJsonDocument(QJsonObject{
        {QStringLiteral("task_id"), taskId}}).toJson(QJsonDocument::Compact);
    MaiTool* pause = registry.findForModel("maichat_cloud_drive_pause_upload");
    MaiTool* abort = registry.findForModel("maichat_cloud_drive_abort_upload");
    require(pause && abort && !pause->requiresPerCallApproval("{}") &&
        abort->requiresPerCallApproval("{}"), "pause/abort approval policy changed");
    require(abort->execute(taskArgs.toStdString(), largeContext).error().code() == MaiErrorCode::Busy,
            "active upload could be discarded without pausing");
    const QJsonObject pauseResult = output(pause->execute(taskArgs.toStdString(), largeContext));
    require(pauseResult.value(QStringLiteral("state")) == QStringLiteral("pausing"),
            "pause did not request upload cancellation");
    QElapsedTimer pausedWait;
    pausedWait.start();
    QString state;
    while (pausedWait.elapsed() < 15000) {
        const QJsonArray uploads = output(registry.findForModel(
            "maichat_cloud_drive_list_pending_uploads")->execute("{}", largeContext))
            .value(QStringLiteral("uploads")).toArray();
        for (const QJsonValue& entry : uploads) {
            const QJsonObject item = entry.toObject();
            if (item.value(QStringLiteral("task_id")) == taskId) {
                state = item.value(QStringLiteral("state")).toString();
                break;
            }
        }
        if (state == QStringLiteral("paused")) break;
        QThread::msleep(20);
    }
    require(state == QStringLiteral("paused"), "paused upload did not stop with a retained checkpoint");
    require(QFileInfo(QDir(checkpointDir).filePath(taskId + QStringLiteral(".maicloud.json"))).isFile(),
            "paused upload checkpoint was lost");
    require(output(pause->execute(taskArgs.toStdString(), largeContext))
                .value(QStringLiteral("state")) == QStringLiteral("paused"),
            "pausing an already paused task should be idempotent");
    require(output(abort->execute(taskArgs.toStdString(), largeContext))
                .value(QStringLiteral("aborted")) == true,
            "aborted upload did not release its server session");
    require(!QFileInfo(QDir(checkpointDir).filePath(taskId + QStringLiteral(".maicloud.json"))).exists(),
            "aborted upload retained its checkpoint");
    require(abort->execute(taskArgs.toStdString(), largeContext).error().code() == MaiErrorCode::NotFound,
            "aborted upload remained addressable");
    QFile::remove(largePath);
    std::cout << "Desktop cloud Agent local TLS lifecycle PASS\n";
}
