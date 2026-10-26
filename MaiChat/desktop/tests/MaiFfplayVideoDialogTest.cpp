// 文件作用：验证桌面端视频弹窗控制、分享保存、倍速音频及关闭释放。
// 运行边界：由测试进程驱动实际组件或替身，不作为正式 App 的用户入口。
#include <QElapsedTimer>
#include <QComboBox>
#include <QCameraInfo>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include <QMouseEvent>
#include <QScreen>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "MaiChatTools/DesktopMediaTools.h"
#include "MaiTool.h"
#include "ui/MaiFfplayVideoDialog.h"
#include "ui/DesktopCameraDialog.h"
#include "ui/MainWindow.h"
#include "app/RemoteIMApplication.h"
#include "im/FakeRemoteIMClient.h"

#if defined(__APPLE__)
extern "C" uint64_t maiFfplayAppleNonSilentBufferCount(void);
#endif

namespace {

std::atomic<bool> sSawFiniteSeek{false};

void captureDiagnostic(void*, int, const char* line) {
    if (!line) return;
    const char* seek = std::strstr(line, "Seek to ");
    if (!seek) return;
    seek += std::strlen("Seek to ");
    char* end = nullptr;
    const double percent = std::strtod(seek, &end);
    if (end != seek && *end == '%' && std::isfinite(percent) &&
        std::abs(percent - 50.0) < 0.01) {
        sSawFiniteSeek = true;
    }
}

}  // namespace

class MaiFfplayVideoDialogTest : public QObject {
    Q_OBJECT

private slots:
    void exposesSaveAndForwardActions();
    void opensControlsAndClosesVideoPopup_data();
    void opensControlsAndClosesVideoPopup();
    void opensAtRequestedSourceTime();
    void cameraDoesNotOpenAfterAgentCancellation();
    void cameraActivePreviewClosesOnAgentCancellation();
    void agentCameraOpensAndClosesWithoutWaitingForShutter();
    void slowedPlaybackExtendsAudibleOutput();
    void localVideoShowsPixels();
    void mainWindowDocksSamePlayerAcrossNavigationAndFullscreen();
};

void MaiFfplayVideoDialogTest::opensAtRequestedSourceTime() {
    QWidget host;
    MaiToolRegistry registry;
    registerDesktopMediaTools(registry, &host);
    MaiTool* openTool = registry.find("maichat_play_video");
    QVERIFY(openTool != nullptr);
    MaiTool* cameraTool = registry.find("maichat_camera_command");
    QVERIFY(cameraTool != nullptr);
    QVERIFY(!cameraTool->requiresApproval("{}"));
    QVERIFY(!cameraTool->requiresPerCallApproval("{}"));
    MaiToolContext emptyContext;
    QVERIFY(cameraTool->execute(R"({"action":"open"})", emptyContext).hasError());
    MaiTool* musicTool = registry.find("maichat_play_local_music");
    QVERIFY(musicTool != nullptr);
    QVERIFY(!musicTool->requiresApproval("{}"));
    QVERIFY(!musicTool->requiresPerCallApproval("{}"));
    QVERIFY(musicTool->execute(R"({"path":"song.mp3"})", emptyContext).hasError());
    QVERIFY(QString::fromStdString(openTool->parametersSchema()).contains(
        QStringLiteral("start_seconds")));
    MaiToolContext context;
    const QFileInfo fixture(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE));
    context.root = fixture.dir().absolutePath().toStdString();
    QVERIFY(openTool->execute(R"({"path":"unused.mp4","start_seconds":-1})", context)
                .hasError());
    QVERIFY(openTool->execute(R"({"path":"unused.mp4","start_seconds":"1"})", context)
                .hasError());
    const MaiToolResult opened = openTool->execute(
        QStringLiteral("{\"path\":\"%1\",\"start_seconds\":1.0}")
            .arg(fixture.fileName()).toStdString(), context);
    QVERIFY2(!opened.hasError(), opened.error().message().c_str());
    const auto result = QJsonDocument::fromJson(QByteArray::fromStdString(opened.output())).object();
    QCOMPARE(result.value(QStringLiteral("start_seconds")).toDouble(), 1.0);
    QPointer<MaiFfplayVideoDialog> dialog = MaiFfplayVideoDialog::activeDialog();
    QVERIFY(dialog);
    MaiFfplayPlaybackStatus status{};
    int64_t firstPosition = -1;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 4000 && firstPosition < 0) {
        QCoreApplication::processEvents();
        if (maiFfplayGetPlaybackStatus(&status) && status.position_us > 0)
            firstPosition = status.position_us;
        else QTest::qWait(10);
    }
    QVERIFY2(firstPosition >= 500'000, "playback started before the requested source time");
    dialog->close();
    QTRY_VERIFY(dialog.isNull());
}

void MaiFfplayVideoDialogTest::cameraDoesNotOpenAfterAgentCancellation() {
    QWidget host;
    MaiToolRegistry registry;
    registerDesktopMediaTools(registry, &host);
    MaiTool* camera = registry.find("maichat_camera_command");
    QVERIFY(camera != nullptr);
    std::atomic<bool> canceled{true};
    MaiToolContext context;
    context.sessionId = "ses_camera_test";
    context.cancel = &canceled;
    const MaiToolResult result = camera->execute(R"({"action":"open"})", context);
    QCOMPARE(result.error().code(), MaiErrorCode::Canceled);
    QString error;
    QCOMPARE(DesktopCameraDialog::capturePhoto(nullptr, QDir::tempPath(), &error, &canceled),
             QString());
    QVERIFY(error.isEmpty());
}

void MaiFfplayVideoDialogTest::agentCameraOpensAndClosesWithoutWaitingForShutter() {
    if (qEnvironmentVariable("MAICHAT_TEST_REAL_CAMERA_CANCEL") != QStringLiteral("1"))
        QSKIP("Real camera test must be explicitly enabled");
    if (qEnvironmentVariable("QT_QPA_PLATFORM") == QStringLiteral("offscreen"))
        QSKIP("Qt camera preview requires the normal desktop platform plugin");
    if (QCameraInfo::availableCameras().isEmpty()) QSKIP("No camera is connected");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QWidget host;
    host.show();
    MaiToolRegistry registry;
    registerDesktopMediaTools(registry, &host);
    MaiTool* camera = registry.find("maichat_camera_command");
    QVERIFY(camera != nullptr);
    MaiToolContext context;
    context.sessionId = "ses_camera_test";
    context.root = directory.path().toStdString();
    const MaiToolResult closed = camera->execute(R"({"action":"status"})", context);
    QVERIFY(!closed.hasError());
    QVERIFY(QString::fromStdString(closed.output()).contains(QStringLiteral("closed")));
    QElapsedTimer elapsed;
    elapsed.start();
    const MaiToolResult opened = camera->execute(R"({"action":"open"})", context);
    QVERIFY(!opened.hasError());
    QVERIFY(elapsed.elapsed() < 2000);
    QVERIFY(DesktopCameraDialog::isPreviewOpen(&host, QStringLiteral("ses_camera_test")));
    auto* preview = host.findChild<QDialog*>(QStringLiteral("desktopCameraDialog"));
    QVERIFY(preview != nullptr);
    auto* shutter = preview->findChild<QPushButton*>(QStringLiteral("desktopCameraShutter"));
    QVERIFY(shutter != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(shutter->isEnabled(), 7000);
    if (qEnvironmentVariableIsSet("MAICHAT_CAMERA_SCREENSHOT")) {
        const QPixmap picture = preview->grab();
        QVERIFY(!picture.isNull());
        QVERIFY(picture.save(qEnvironmentVariable("MAICHAT_CAMERA_SCREENSHOT")));
    }
    MaiToolContext other = context;
    other.sessionId = "ses_other";
    QVERIFY(camera->execute(R"({"action":"open"})", other).hasError());
    QVERIFY(!camera->execute(R"({"action":"close"})", other).hasError());
    QVERIFY(DesktopCameraDialog::isPreviewOpen(&host, QStringLiteral("ses_camera_test")));
    const MaiToolResult result = camera->execute(R"({"action":"close"})", context);
    QVERIFY(!result.hasError());
    const MaiToolResult reopened = camera->execute(R"({"action":"open"})", context);
    QVERIFY(!reopened.hasError());
    QCoreApplication::processEvents();
    QVERIFY(DesktopCameraDialog::isPreviewOpen(&host, QStringLiteral("ses_camera_test")));
    QVERIFY(!camera->execute(R"({"action":"close"})", context).hasError());
    QTRY_VERIFY_WITH_TIMEOUT(!DesktopCameraDialog::isPreviewOpen(
        &host, QStringLiteral("ses_camera_test")), 3000);
    QVERIFY(QDir(directory.path()).entryList(QStringList{QStringLiteral("*.jpg")},
        QDir::Files).isEmpty());
}

void MaiFfplayVideoDialogTest::cameraActivePreviewClosesOnAgentCancellation() {
    if (qEnvironmentVariable("MAICHAT_TEST_REAL_CAMERA_CANCEL") != QStringLiteral("1"))
        QSKIP("Real camera test must be explicitly enabled");
    if (qEnvironmentVariable("QT_QPA_PLATFORM") == QStringLiteral("offscreen"))
        QSKIP("Qt camera preview requires the normal desktop platform plugin");
    if (QCameraInfo::availableCameras().isEmpty()) QSKIP("No camera is connected");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    std::atomic<bool> canceled{false};
    QTimer::singleShot(1200, [&] { canceled.store(true, std::memory_order_relaxed); });
    QElapsedTimer elapsed;
    elapsed.start();
    QString error;
    const QString photo = DesktopCameraDialog::capturePhoto(
        nullptr, directory.path(), &error, &canceled);
    QVERIFY(photo.isEmpty());
    QVERIFY(error.isEmpty());
    QVERIFY(elapsed.elapsed() < 5000);
    QVERIFY(QDir(directory.path()).entryList(QStringList{QStringLiteral("*.jpg")},
        QDir::Files).isEmpty());
}

void MaiFfplayVideoDialogTest::mainWindowDocksSamePlayerAcrossNavigationAndFullscreen() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("video-dock-test"), std::move(client));
    MainWindow window(app);
    window.resize(1500, 900);
    window.show();
    QVERIFY(window.openDockedVideo(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE)));
    QPointer<MaiFfplayVideoDialog> player = MaiFfplayVideoDialog::activeDialog();
    QVERIFY(player);
    auto* slot = window.findChild<QWidget*>(QStringLiteral("videoDockSlot"));
    auto* stack = window.findChild<QStackedWidget*>(QStringLiteral("contentStack"));
    QVERIFY(slot);
    QVERIFY(stack);
    QTRY_VERIFY(slot->isVisible());
    QTRY_VERIFY(player->presentedFrames() > 0);
    QVERIFY(player->isVisible());
    player->raise();
    QCoreApplication::processEvents();
    QWidget* surface = player->findChild<QWidget*>(QStringLiteral("ffplayVideoSurface"));
    QVERIFY(surface);
    const auto visiblePixels = [surface] {
        const QPoint position = surface->mapToGlobal(QPoint(0, 0));
        const QImage pixels = surface->screen()->grabWindow(
            0, position.x(), position.y(), surface->width(), surface->height()).toImage();
        int low = 255;
        int high = 0;
        for (int y = pixels.height() / 4; y < pixels.height() * 3 / 4; y += 8)
            for (int x = pixels.width() / 4; x < pixels.width() * 3 / 4; x += 8) {
                const QColor color = pixels.pixelColor(x, y);
                const int value = std::max({color.red(), color.green(), color.blue()});
                low = std::min(low, value);
                high = std::max(high, value);
            }
        return high - low > 32;
    };
    QTRY_VERIFY_WITH_TIMEOUT(visiblePixels(), 2000);
    QTRY_COMPARE(player->pos(), slot->mapToGlobal(QPoint(0, 0)));
    QCOMPARE(player->size(), slot->size());
    QVERIFY2(slot->width() * 4 >= slot->height() * 5,
             "Desktop video dock must open as a landscape viewport");
    auto* dockSplitter = window.findChild<QSplitter*>(QStringLiteral("videoContentSplitter"));
    QVERIFY(dockSplitter && dockSplitter->handle(1)->isVisible());
    auto* resizeHandle = dockSplitter->handle(1);
    const int originalWidth = slot->width();
    const QPoint handleCenter = resizeHandle->rect().center();
    const QPoint finish = handleCenter - QPoint(40, 0);
    QTest::mousePress(resizeHandle, Qt::LeftButton, Qt::NoModifier, handleCenter);
    QMouseEvent move(QEvent::MouseMove, QPointF(finish),
                     QPointF(resizeHandle->mapToGlobal(finish)),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(resizeHandle, &move);
    QTest::mouseRelease(resizeHandle, Qt::LeftButton, Qt::NoModifier, finish);
    QTRY_VERIFY2(slot->width() < originalWidth,
                 qPrintable(QStringLiteral("before=%1 after=%2")
                     .arg(originalWidth).arg(slot->width())));
    QTRY_COMPARE(player->size(), slot->size());
    const QByteArray capture = qgetenv("MAICHAT_TEST_CAPTURE_PATH");
    if (!capture.isEmpty()) {
        QTest::qWait(200);
        QVERIFY(player->screen()->grabWindow(0, window.x(), window.y(),
                                              window.width(), window.height())
                    .save(QString::fromUtf8(capture)));
    }
    auto* fullscreen = player->findChild<QPushButton*>(QStringLiteral("ffplayFullscreenButton"));
    auto* close = player->findChild<QPushButton*>(QStringLiteral("ffplayCloseButton"));
#if defined(Q_OS_MAC)
    QVERIFY(fullscreen);
#else
    QVERIFY(!fullscreen);
#endif
    QVERIFY(close);
#if defined(Q_OS_MAC)
    fullscreen->click();
    QTRY_VERIFY(player->isFullScreen());
    QTRY_VERIFY(!slot->isVisible());
    fullscreen->click();
    QTRY_VERIFY(!player->isFullScreen());
    QTRY_VERIFY(slot->isVisible());
#endif
    auto* contacts = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    QVERIFY(contacts);
    contacts->click();
    QCOMPARE(MaiFfplayVideoDialog::activeDialog(), player);
    QVERIFY(stack->isVisible());
    window.resize(900, 800);
    QTRY_VERIFY(slot->parentWidget()->layout()->geometry().width() < 1100);
    QTRY_VERIFY(slot->height() < slot->parentWidget()->height());
    QTRY_COMPARE(player->pos(), slot->mapToGlobal(QPoint(0, 0)));
    QCOMPARE(MaiFfplayVideoDialog::activeDialog(), player);
    window.move(window.pos() + QPoint(20, 15));
    QTRY_COMPARE(player->pos(), slot->mapToGlobal(QPoint(0, 0)));
    close->click();
    QTRY_VERIFY(player.isNull());
    QVERIFY(!slot->isVisible());
    MaiToolRegistry registry;
    registerDesktopMediaTools(registry, &window);
    MaiToolContext context;
    const QFileInfo fixture(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE));
    context.root = fixture.dir().absolutePath().toStdString();
    QVERIFY(!registry.find("maichat_play_video")
                 ->execute(QStringLiteral("{\"path\":\"%1\"}")
                               .arg(fixture.fileName()).toStdString(), context)
                 .hasError());
    QTRY_VERIFY(slot->isVisible());
    QVERIFY(!registry.find("maichat_video_command")
                 ->execute(R"({"action":"pause"})", context).hasError());
    QVERIFY(!registry.find("maichat_video_command")
                 ->execute(R"({"action":"close"})", context).hasError());
    QTRY_VERIFY(!slot->isVisible());
}

void MaiFfplayVideoDialogTest::exposesSaveAndForwardActions() {
    MaiFfplayVideoDialog dialog(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE));
    auto* forward = dialog.findChild<QPushButton*>(QStringLiteral("videoForwardButton"));
    auto* save = dialog.findChild<QPushButton*>(QStringLiteral("videoSaveButton"));
    QVERIFY(forward != nullptr);
    QVERIFY(save != nullptr);
    bool forwarded = false;
    dialog.setForwardAction([&forwarded](const QString&) { forwarded = true; });
    forward->click();
    QVERIFY(forwarded);
}

void MaiFfplayVideoDialogTest::opensControlsAndClosesVideoPopup_data() {
    QTest::addColumn<QString>("fixturePath");
    QTest::newRow("video-audio-subtitles") << QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE);
    QTest::newRow("h264") << QStringLiteral(MAICHAT_FFPLAY_H264_FIXTURE);
    QTest::newRow("av1") << QStringLiteral(MAICHAT_FFPLAY_AV1_FIXTURE);
}

// 不同编码素材共用一套打开、控制、关闭路径；关闭后播放器资源必须退出，不能留后台声音。
void MaiFfplayVideoDialogTest::opensControlsAndClosesVideoPopup() {
    QFETCH(QString, fixturePath);
    sSawFiniteSeek = false;
    maiFfplaySetDiagnosticSink(captureDiagnostic, nullptr);
    QWidget host;
    MaiToolRegistry registry;
    registerDesktopMediaTools(registry, &host);
    MaiTool* openTool = registry.find("maichat_play_video");
    MaiTool* commandTool = registry.find("maichat_video_command");
    QVERIFY(openTool != nullptr);
    QVERIFY(commandTool != nullptr);
    QVERIFY(QString::fromStdString(commandTool->parametersSchema()).contains(
        QStringLiteral("set_speed")));
    QVERIFY(QString::fromStdString(commandTool->parametersSchema()).contains(
        QStringLiteral("replay")));
    MaiToolContext context;
    const QFileInfo fixture(fixturePath);
    context.root = fixture.dir().absolutePath().toStdString();
    const MaiToolResult opened = openTool->execute(
        QStringLiteral("{\"path\":\"%1\"}").arg(fixture.fileName()).toStdString(),
        context);
    QVERIFY2(!opened.hasError(), opened.error().message().c_str());
    QPointer<MaiFfplayVideoDialog> dialog = MaiFfplayVideoDialog::activeDialog();
    QVERIFY(!dialog.isNull());
    QVERIFY(dialog->isStarted());
    QCOMPARE(MaiFfplayVideoDialog::activeDialog(), dialog);
    auto* slider = dialog->findChild<QSlider*>(QStringLiteral("ffplaySeekSlider"));
    auto* timeLabel = dialog->findChild<QLabel*>(QStringLiteral("ffplayTimeLabel"));
    QVERIFY(slider != nullptr);
    QVERIFY(timeLabel != nullptr);
    auto* speedSelector = dialog->findChild<QComboBox*>(QStringLiteral("ffplaySpeedSelector"));
    QVERIFY(speedSelector != nullptr);
    QCOMPARE(speedSelector->count(), 5);
    QVERIFY(!commandTool->execute(R"({"action":"set_speed","speed":0.5})", context)
                 .hasError());
    MaiFfplayPlaybackStatus speedStatus{};
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&speedStatus) &&
                                 speedStatus.speed_percent == 50,
                             3000);
    QTRY_VERIFY_WITH_TIMEOUT(slider->value() > 0 &&
                             timeLabel->text().contains(QStringLiteral(" / 0:02")), 6000);
    QTRY_VERIFY_WITH_TIMEOUT(dialog->presentedFrames() >= 3, 4000);
    QWidget* surface = dialog->findChild<QWidget*>(QStringLiteral("ffplayVideoSurface"));
    QVERIFY(surface != nullptr);
    const auto hasVisibleColor = [surface] {
        const QPoint topLeft = surface->mapToGlobal(QPoint(0, 0));
        const QImage pixels = surface->screen()->grabWindow(
            0, topLeft.x(), topLeft.y(), surface->width(), surface->height()).toImage();
        int minimumValue = 255;
        int maximumValue = 0;
        for (int y = 0; y < pixels.height(); y += 8) {
            for (int x = 0; x < pixels.width(); x += 8) {
                const QColor color = pixels.pixelColor(x, y);
                const int value = std::max({color.red(), color.green(), color.blue()});
                minimumValue = std::min(minimumValue, value);
                maximumValue = std::max(maximumValue, value);
            }
        }
        return maximumValue - minimumValue > 32;
    };
    QTRY_VERIFY_WITH_TIMEOUT(hasVisibleColor(), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(!commandTool->execute(R"({"action":"pause"})", context).hasError(),
                             3000);
    QTest::qWait(200);
    const uint64_t pausedFrames = dialog->presentedFrames();
    QTest::qWait(250);
    QCOMPARE(dialog->presentedFrames(), pausedFrames);
    QVERIFY(!commandTool->execute(R"({"action":"play"})", context).hasError());
    QTRY_VERIFY_WITH_TIMEOUT(dialog->presentedFrames() > pausedFrames, 3000);
    QVERIFY(!commandTool->execute(R"({"action":"seek_percent","percent":50})", context)
                 .hasError());
    QTRY_VERIFY_WITH_TIMEOUT(sSawFiniteSeek.load(), 1500);
    QVERIFY(!commandTool->execute(R"({"action":"replay"})", context).hasError());
    QVERIFY(dialog->isStarted());
    QElapsedTimer timer;
    timer.start();
    QVERIFY(!commandTool->execute(R"({"action":"close"})", context).hasError());
    QVERIFY(timer.elapsed() < 5000);
    QTRY_VERIFY(dialog.isNull());
    QVERIFY(MaiFfplayVideoDialog::activeDialog().isNull());
    maiFfplaySetDiagnosticSink(nullptr, nullptr);
}

void MaiFfplayVideoDialogTest::slowedPlaybackExtendsAudibleOutput() {
#if defined(__APPLE__)
    QWidget host;
    MaiToolRegistry registry;
    registerDesktopMediaTools(registry, &host);
    MaiTool* openTool = registry.find("maichat_play_video");
    QVERIFY(openTool != nullptr);
    const QFileInfo fixture(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE));
    MaiToolContext context;
    context.root = fixture.dir().absolutePath().toStdString();
    const MaiToolResult opened = openTool->execute(
        QStringLiteral("{\"path\":\"%1\"}").arg(fixture.fileName()).toStdString(),
        context);
    QVERIFY2(!opened.hasError(), opened.error().message().c_str());
    QPointer<MaiFfplayVideoDialog> dialog = MaiFfplayVideoDialog::activeDialog();
    QVERIFY(!dialog.isNull());
    QVERIFY(dialog->setSpeed(0.5));
    const uint64_t before = maiFfplayAppleNonSilentBufferCount();
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayAppleNonSilentBufferCount() > before, 3000);
    // 两秒源音轨按 0.5 倍速应持续约四秒；若只改画面，音频此时已结束。
    QTest::qWait(2200);
    const uint64_t midway = maiFfplayAppleNonSilentBufferCount();
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayAppleNonSilentBufferCount() > midway, 700);
    dialog->close();
    QTRY_VERIFY(dialog.isNull());

    const MaiToolResult reopened = openTool->execute(
        QStringLiteral("{\"path\":\"%1\"}").arg(fixture.fileName()).toStdString(),
        context);
    QVERIFY2(!reopened.hasError(), reopened.error().message().c_str());
    dialog = MaiFfplayVideoDialog::activeDialog();
    QVERIFY(!dialog.isNull());
    QVERIFY(dialog->setSpeed(2.0));
    const uint64_t fastStart = maiFfplayAppleNonSilentBufferCount();
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayAppleNonSilentBufferCount() > fastStart, 3000);
    // 两秒源音轨按 2 倍速应在约一秒后结束，不应继续按原速输出声音。
    QTest::qWait(1500);
    const uint64_t fastEnd = maiFfplayAppleNonSilentBufferCount();
    QTest::qWait(300);
    QCOMPARE(maiFfplayAppleNonSilentBufferCount(), fastEnd);
    dialog->close();
    QTRY_VERIFY(dialog.isNull());
#endif
}

void MaiFfplayVideoDialogTest::localVideoShowsPixels() {
    const QString path = qEnvironmentVariable("MAICHAT_FFPLAY_LOCAL_VIDEO");
    if (!QFileInfo(path).isFile()) QSKIP("No local video fixture");
    const QString nextPath = qEnvironmentVariable("MAICHAT_FFPLAY_NEXT_VIDEO");
    MaiFfplayVideoDialog dialog(path);
    if (QFileInfo(nextPath).isFile()) dialog.setPlaylist({path, nextPath});
    dialog.resize(1280, 900);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    QVERIFY(dialog.isStarted());
    QTRY_VERIFY_WITH_TIMEOUT(dialog.presentedFrames() > 0, 5000);
    QWidget* surface = dialog.findChild<QWidget*>(QStringLiteral("ffplayVideoSurface"));
    QVERIFY(surface != nullptr);
    QTest::qWait(500);
    const QPoint topLeft = surface->mapToGlobal(QPoint(0, 0));
    const QImage pixels = surface->screen()->grabWindow(
        0, topLeft.x(), topLeft.y(), surface->width(), surface->height()).toImage();
    const QString capturePath = qEnvironmentVariable("MAICHAT_FFPLAY_CAPTURE");
    if (!capturePath.isEmpty()) QVERIFY(pixels.save(capturePath));
    const QString windowCapture = qEnvironmentVariable("MAICHAT_FFPLAY_WINDOW_CAPTURE");
    if (!windowCapture.isEmpty()) {
        const QRect frame = dialog.frameGeometry();
        QVERIFY(dialog.screen()->grabWindow(0, frame.x(), frame.y(),
                                            frame.width(), frame.height()).save(windowCapture));
    }
    int minimumValue = 255;
    int maximumValue = 0;
    for (int y = 0; y < pixels.height(); y += 8)
        for (int x = 0; x < pixels.width(); x += 8) {
            const QColor color = pixels.pixelColor(x, y);
            const int value = std::max({color.red(), color.green(), color.blue()});
            minimumValue = std::min(minimumValue, value);
            maximumValue = std::max(maximumValue, value);
        }
    qInfo() << "local video pixels" << pixels.size() << minimumValue << maximumValue;
    QVERIFY(maximumValue - minimumValue > 32);
    auto* pauseButton = dialog.findChild<QPushButton*>(QStringLiteral("ffplayPauseButton"));
    QVERIFY(pauseButton != nullptr && pauseButton->isVisible());
    QTest::mouseClick(pauseButton, Qt::LeftButton);
    MaiFfplayPlaybackStatus playback{};
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&playback) && playback.paused != 0, 2000);
    QTest::mouseClick(pauseButton, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&playback) && playback.paused == 0, 2000);
    auto* speedSelector = dialog.findChild<QComboBox*>(QStringLiteral("ffplaySpeedSelector"));
    QVERIFY(speedSelector != nullptr && speedSelector->isVisible());
    speedSelector->setCurrentIndex(3);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&playback) &&
                             playback.speed_percent == 150, 3000);
    if (QFileInfo(nextPath).isFile()) {
        QString forwardedPath;
        dialog.setForwardAction([&](const QString& currentPath) { forwardedPath = currentPath; });
        auto* nextButton = dialog.findChild<QPushButton*>(QStringLiteral("ffplayNextItemButton"));
        QVERIFY(nextButton != nullptr);
        QVERIFY(nextButton->isVisible() && nextButton->isEnabled());
        nextButton->click();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.windowTitle(), QFileInfo(nextPath).fileName(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(dialog.presentedFrames() > 0, 5000);
        auto* forwardButton = dialog.findChild<QPushButton*>(QStringLiteral("videoForwardButton"));
        QVERIFY(forwardButton != nullptr);
        forwardButton->click();
        QCOMPARE(QFileInfo(forwardedPath).canonicalFilePath(), QFileInfo(nextPath).canonicalFilePath());
        QCOMPARE(dialog.speed(), 1.5);
        QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&playback) &&
                                 playback.speed_percent == 150, 3000);
        QVERIFY(!dialog.navigateVideo(1));
        QVERIFY(dialog.navigateVideo(-1));
        QTRY_COMPARE_WITH_TIMEOUT(dialog.windowTitle(), QFileInfo(path).fileName(), 3000);
    }
    dialog.close();
}

QTEST_MAIN(MaiFfplayVideoDialogTest)
#include "MaiFfplayVideoDialogTest.moc"
