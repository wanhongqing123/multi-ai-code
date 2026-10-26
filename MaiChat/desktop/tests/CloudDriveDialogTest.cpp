#include "ui/CloudDriveDialog.h"
#include "MaiCloudVod.h"
#include "ui/ImagePreviewDialog.h"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QFutureWatcher>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include <cstdlib>
#include <iostream>

namespace {

void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

QPushButton* visibleButton(QWidget& parent, const QString& name) {
    for (QPushButton* button : parent.findChildren<QPushButton*>(name))
        if (button->isVisible() && button->isEnabled()) return button;
    return nullptr;
}

bool waitForButton(QWidget& parent, const QString& name, int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QApplication::processEvents();
        if (visibleButton(parent, name)) return true;
        QThread::msleep(20);
    }
    return false;
}

bool waitForLabel(QWidget& parent, const QString& name, const QString& needle,
                  int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QApplication::processEvents();
        QLabel* label = parent.findChild<QLabel*>(name);
        if (label && label->text().contains(needle)) return true;
        QThread::msleep(20);
    }
    return false;
}

bool waitForVisibleText(QWidget& parent, const QString& needle, int timeoutMs = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QApplication::processEvents();
        for (QLabel* label : parent.findChildren<QLabel*>())
            if (label->isVisible() && label->text().contains(needle)) return true;
        QThread::msleep(20);
    }
    return false;
}

bool waitForThumbnail(QWidget& parent, int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QApplication::processEvents();
        for (QLabel* label : parent.findChildren<QLabel*>(QStringLiteral("cloudDriveThumbnail")))
            if (label->property("thumbnailLoaded").toBool()) return true;
        QThread::msleep(20);
    }
    return false;
}

ImagePreviewDialog* waitForImagePreview(QWidget& parent, int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QApplication::processEvents();
        auto* preview = parent.findChild<ImagePreviewDialog*>(
            QStringLiteral("cloudDriveImagePreview"));
        if (preview && preview->isVisible()) return preview;
        QThread::msleep(20);
    }
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    require(argc == 5, "expected TLS base, certificate, fixture video, screenshots directory");
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("MaiChatTests"));
    QCoreApplication::setApplicationName(QStringLiteral("CloudDriveDialogTest-") +
        QString::number(QCoreApplication::applicationPid()));
    QFile ca(QString::fromUtf8(argv[2]));
    require(ca.open(QIODevice::ReadOnly), "could not load test certificate");
    const QString base = QString::fromUtf8(argv[1]);
    const QString source = QString::fromUtf8(argv[3]);
    const QString screenshots = QString::fromUtf8(argv[4]);
    require(QFileInfo(source).isFile(), "missing fixture video");
    const QString imageSource = screenshots + QStringLiteral("/cloud-test-image.jpg");
    QImage imageFixture(320, 180, QImage::Format_RGB32);
    imageFixture.fill(QColor(28, 138, 211));
    require(imageFixture.save(imageSource, "JPEG"), "could not create image fixture");

    DesktopVideoCircleAccess access;
    access.authBaseUrl = base + QStringLiteral("/auth");
    access.mediaBaseUrl = base + QStringLiteral("/oss");
    access.accountToken = QStringLiteral("author-token");
    access.accountId = QStringLiteral("author");
    access.caPem = ca.readAll();
    access.signedUploadHostSuffix = QStringLiteral("localhost");
    access.proxyUrl = QStringLiteral("");
    require(CloudDriveDialog::availability(access) == 1,
            "cloud feature gate did not accept available account API");
    DesktopVideoCircleAccess disabled = access;
    disabled.authBaseUrl = base + QStringLiteral("/disabled");
    require(CloudDriveDialog::availability(disabled) == 0,
            "cloud feature gate exposed a disabled API");
    QString playedName;
    std::shared_ptr<MaiCloudVod> playedVod;
    CloudDriveDialog dialog([access] { return access; },
        [] { return QStringLiteral("author"); },
        [&playedName, &playedVod](std::shared_ptr<MaiCloudVod> source,
                                  const QString& name) {
            playedVod = std::move(source);
            playedName = name;
        });
    dialog.openDrive();
    require(waitForLabel(dialog, QStringLiteral("cloudDriveQuota"),
        QStringLiteral("10.00 GB")), "10 GB cloud quota did not load");
    require(!visibleButton(dialog, QStringLiteral("cloudDrivePlay")),
            "empty cloud has play button");
    require(waitForVisibleText(dialog, QStringLiteral("云盘里还没有图片或视频")),
            "empty cloud text is not visible");
    require(dialog.grab().save(screenshots + QStringLiteral("/cloud-empty.png")),
            "failed to save empty state screenshot");

    QTimer confirmation;
    confirmation.setInterval(20);
    QObject::connect(&confirmation, &QTimer::timeout, [&] {
        auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!modal) return;
        auto* confirm = modal->findChild<QPushButton*>(
            QStringLiteral("appMessageConfirm"));
        if (!confirm) confirm = modal->findChild<QPushButton*>(
            QStringLiteral("appMessageDanger"));
        if (confirm) {
            confirmation.stop();
            confirm->click();
        }
    });
    confirmation.start();
    dialog.openDrive(source);
    require(waitForButton(dialog, QStringLiteral("cloudDrivePlay")),
            "video did not upload and appear in the list");
    require(visibleButton(dialog, QStringLiteral("cloudDriveSaveVideo")),
            "explicit save action disappeared after switching playback to VOD");
    require(waitForThumbnail(dialog), "signed thumbnail did not load for video");
    require(dialog.grab().save(screenshots + QStringLiteral("/cloud-uploaded.png")),
            "failed to save uploaded screenshot");
    visibleButton(dialog, QStringLiteral("cloudDrivePlay"))->click();
    QElapsedTimer playback;
    playback.start();
    while (!playedVod && playback.elapsed() < 15000) {
        QApplication::processEvents();
        QThread::msleep(20);
    }
    require(playedVod && mai_cloud_vod_size(playedVod.get()) ==
                quint64(QFileInfo(source).size()) && !QFileInfo(playedName).exists(),
            "cloud playback did not open a seekable stream without a local video file");
    playedVod.reset();

    confirmation.start();
    require(waitForButton(dialog, QStringLiteral("cloudDriveDelete")),
            "delete button not available");
    visibleButton(dialog, QStringLiteral("cloudDriveDelete"))->click();
    require(waitForLabel(dialog, QStringLiteral("cloudDriveQuota"),
        QStringLiteral("已使用 0.0 KB")), "quota did not return to zero after delete");
    require(dialog.grab().save(screenshots + QStringLiteral("/cloud-deleted.png")),
            "failed to save deleted screenshot");
    if (visibleButton(dialog, QStringLiteral("cloudDrivePlay"))) {
        QLabel* state = dialog.findChild<QLabel*>(QStringLiteral("cloudDriveStatus"));
        std::cerr << "after delete status: " << (state ? state->text().toStdString() : "missing") << '\n';
    }
    require(!visibleButton(dialog, QStringLiteral("cloudDrivePlay")),
            "deleted video still in list");
    require(waitForVisibleText(dialog, QStringLiteral("云盘里还没有图片或视频")),
            "deleted cloud empty state is not visible");

    confirmation.start();
    dialog.openDrive(imageSource);
    require(waitForButton(dialog, QStringLiteral("cloudDriveViewImage")),
            "image did not upload and appear in the list");
    require(waitForThumbnail(dialog), "signed thumbnail did not load for image");
    require(dialog.grab().save(screenshots + QStringLiteral("/cloud-image-uploaded.png")),
            "failed to save image row screenshot");
    visibleButton(dialog, QStringLiteral("cloudDriveViewImage"))->click();
    ImagePreviewDialog* preview = waitForImagePreview(dialog);
    require(preview && !preview->sourceImage().isNull(),
            "cloud image preview did not decode original bytes");
    require(preview->grab().save(screenshots + QStringLiteral("/cloud-image-preview.png")),
            "failed to save image preview screenshot");
    preview->close();
    confirmation.start();
    require(waitForButton(dialog, QStringLiteral("cloudDriveDelete")),
            "image delete button not available");
    visibleButton(dialog, QStringLiteral("cloudDriveDelete"))->click();
    require(waitForLabel(dialog, QStringLiteral("cloudDriveQuota"),
        QStringLiteral("已使用 0.0 KB")), "image delete did not release quota");

    const QString largeFile = screenshots + QStringLiteral("/resume-fixture.mov");
    QFile large(largeFile);
    require(large.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "could not create resumable fixture");
    const QByteArray block(512 * 1024, 'v');
    for (int i = 0; i < 48; ++i)
        require(large.write(block) == block.size(), "could not write resumable fixture");
    large.close();
    const QString authorId = QStringLiteral("cloud-ui-author-") +
                             QString::number(QCoreApplication::applicationPid());
    const QString viewerId = QStringLiteral("cloud-ui-viewer-") +
                             QString::number(QCoreApplication::applicationPid());
    QString currentAccount = authorId;
    DesktopVideoCircleAccess mutableAccess = access;
    mutableAccess.accountId = authorId;
    CloudDriveDialog switching([&] { return mutableAccess; },
        [&] { return currentAccount; },
        [](std::shared_ptr<MaiCloudVod>, const QString&) {});
    confirmation.start();
    switching.openDrive(largeFile);
    const QString key = QString::fromLatin1(QCryptographicHash::hash(
        authorId.toUtf8(), QCryptographicHash::Sha256).toHex());
    const QString checkpointDir = QDir(QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation)).filePath(QStringLiteral("CloudDrive/") + key);
    QElapsedTimer checkpointWait;
    checkpointWait.start();
    while (checkpointWait.elapsed() < 12000 &&
           QDir(checkpointDir).entryList({QStringLiteral("*.maicloud.json")},
                                         QDir::Files).isEmpty()) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    require(!QDir(checkpointDir).entryList({QStringLiteral("*.maicloud.json")},
                                           QDir::Files).isEmpty(),
            "upload did not make a durable checkpoint");
    // Switching accounts while a part is in flight cancels the old account's
    // worker and must never show its rows under the new account.
    currentAccount = viewerId;
    mutableAccess.accountId = viewerId;
    mutableAccess.accountToken = QStringLiteral("viewer-token");
    switching.openDrive();
    require(waitForLabel(switching, QStringLiteral("cloudDriveQuota"),
        QStringLiteral("已使用 0.0 KB")), "new account did not load isolated cloud quota");
    QElapsedTimer finishOldWorker;
    finishOldWorker.start();
    while (finishOldWorker.elapsed() < 5000) {
        QApplication::processEvents();
        bool pending = false;
        for (QFutureWatcherBase* worker : switching.findChildren<QFutureWatcherBase*>())
            pending |= !worker->isFinished();
        if (!pending) break;
        QThread::msleep(20);
    }
    require(!QDir(checkpointDir).entryList({QStringLiteral("*.maicloud.json")},
                                           QDir::Files).isEmpty(),
            "account switch lost the resumable checkpoint");
    switching.close();
    const QString movedFile = screenshots + QStringLiteral("/relocated-video.mov");
    QFile::remove(movedFile);
    require(QFile::rename(largeFile, movedFile), "could not relocate upload source");
    DesktopVideoCircleAccess restored = access;
    restored.accountId = authorId;
    CloudDriveDialog resumed([restored] { return restored; },
        [authorId] { return authorId; },
        [](std::shared_ptr<MaiCloudVod>, const QString&) {}, nullptr,
        [&movedFile](const QString&) { return movedFile; });
    resumed.openDrive();
    require(waitForButton(resumed, QStringLiteral("cloudDriveResume")),
            "recreated page did not enumerate the pending upload");
    require(resumed.grab().save(screenshots + QStringLiteral("/cloud-resume.png")),
            "failed to save resume screenshot");
    visibleButton(resumed, QStringLiteral("cloudDriveResume"))->click();
    require(waitForButton(resumed, QStringLiteral("cloudDrivePlay"), 30000),
            "resumed upload did not complete");
    confirmation.start();
    visibleButton(resumed, QStringLiteral("cloudDriveDelete"))->click();
    require(waitForLabel(resumed, QStringLiteral("cloudDriveQuota"),
        QStringLiteral("已使用 0.0 KB")), "resumed video was not deleted");
    QFile::remove(movedFile);
    const QString testData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    require(testData.contains(QStringLiteral("CloudDriveDialogTest-")),
            "refusing to remove a non-test data directory");
    require(QDir(testData).removeRecursively(), "could not remove isolated test checkpoints");
    std::cout << "desktop cloud video/image upload/thumbnail/preview/delete/account-switch/resume passed\n";
    return 0;
}
