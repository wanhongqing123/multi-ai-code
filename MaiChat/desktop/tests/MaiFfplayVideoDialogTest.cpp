#include <QElapsedTimer>
#include <QFileInfo>
#include <QPointer>
#include <QWidget>
#include <QtTest>

#include "MaiChatTools/DesktopMediaTools.h"
#include "MaiTool.h"
#include "ui/MaiFfplayVideoDialog.h"

class MaiFfplayVideoDialogTest : public QObject {
    Q_OBJECT

private slots:
    void opensControlsAndClosesVideoPopup();
};

void MaiFfplayVideoDialogTest::opensControlsAndClosesVideoPopup() {
    QWidget host;
    MaiToolRegistry registry;
    registerDesktopMediaTools(registry, &host);
    MaiTool* openTool = registry.find("maichat_play_video");
    MaiTool* commandTool = registry.find("maichat_video_command");
    QVERIFY(openTool != nullptr);
    QVERIFY(commandTool != nullptr);
    MaiToolContext context;
    const QFileInfo fixture(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE));
    context.root = fixture.dir().absolutePath().toStdString();
    const MaiToolResult opened = openTool->execute(
        QStringLiteral("{\"path\":\"%1\"}").arg(fixture.fileName()).toStdString(),
        context);
    QVERIFY2(!opened.hasError(), opened.error().message().c_str());
    QPointer<MaiFfplayVideoDialog> dialog = MaiFfplayVideoDialog::activeDialog();
    QVERIFY(!dialog.isNull());
    QVERIFY(dialog->isStarted());
    QCOMPARE(MaiFfplayVideoDialog::activeDialog(), dialog);
    QTRY_VERIFY_WITH_TIMEOUT(!commandTool->execute(R"({"action":"pause"})", context).hasError(),
                             3000);
    QVERIFY(!commandTool->execute(R"({"action":"play"})", context).hasError());
    QVERIFY(!commandTool->execute(R"({"action":"seek_percent","percent":50})", context)
                 .hasError());
    QElapsedTimer timer;
    timer.start();
    QVERIFY(!commandTool->execute(R"({"action":"close"})", context).hasError());
    QVERIFY(timer.elapsed() < 5000);
    QTRY_VERIFY(dialog.isNull());
    QVERIFY(MaiFfplayVideoDialog::activeDialog().isNull());
}

QTEST_MAIN(MaiFfplayVideoDialogTest)
#include "MaiFfplayVideoDialogTest.moc"
