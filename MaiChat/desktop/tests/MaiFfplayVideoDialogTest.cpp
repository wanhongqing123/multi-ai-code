#include <QElapsedTimer>
#include <QFileInfo>
#include <QPointer>
#include <QSlider>
#include <QLabel>
#include <QScreen>
#include <QWidget>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <cstring>

#include "MaiChatTools/DesktopMediaTools.h"
#include "MaiTool.h"
#include "ui/MaiFfplayVideoDialog.h"

namespace {

std::atomic<bool> sSawFiniteSeek{false};

void captureDiagnostic(void*, int, const char* line) {
    if (line && std::strstr(line, "Seek to 50%")) sSawFiniteSeek = true;
}

}  // namespace

class MaiFfplayVideoDialogTest : public QObject {
    Q_OBJECT

private slots:
    void opensControlsAndClosesVideoPopup_data();
    void opensControlsAndClosesVideoPopup();
};

void MaiFfplayVideoDialogTest::opensControlsAndClosesVideoPopup_data() {
    QTest::addColumn<QString>("fixturePath");
    QTest::newRow("video-audio-subtitles") << QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE);
    QTest::newRow("h264") << QStringLiteral(MAICHAT_FFPLAY_H264_FIXTURE);
    QTest::newRow("av1") << QStringLiteral(MAICHAT_FFPLAY_AV1_FIXTURE);
}

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
    QTRY_VERIFY_WITH_TIMEOUT(slider->value() > 0 &&
                             timeLabel->text().contains(QStringLiteral(" / 0:02")), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(dialog->presentedFrames() >= 3, 4000);
    QWidget* surface = dialog->findChild<QWidget*>(QStringLiteral("ffplayVideoSurface"));
    QVERIFY(surface != nullptr);
    const auto hasVisibleColor = [surface] {
        const QImage pixels = surface->screen()->grabWindow(surface->winId()).toImage();
        int colorfulPixels = 0;
        for (int y = 0; y < pixels.height(); y += 8) {
            for (int x = 0; x < pixels.width(); x += 8) {
                const QColor color = pixels.pixelColor(x, y);
                if (std::max({color.red(), color.green(), color.blue()}) > 32) ++colorfulPixels;
            }
        }
        return colorfulPixels > 20;
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
    QElapsedTimer timer;
    timer.start();
    QVERIFY(!commandTool->execute(R"({"action":"close"})", context).hasError());
    QVERIFY(timer.elapsed() < 5000);
    QTRY_VERIFY(dialog.isNull());
    QVERIFY(MaiFfplayVideoDialog::activeDialog().isNull());
    maiFfplaySetDiagnosticSink(nullptr, nullptr);
}

QTEST_MAIN(MaiFfplayVideoDialogTest)
#include "MaiFfplayVideoDialogTest.moc"
