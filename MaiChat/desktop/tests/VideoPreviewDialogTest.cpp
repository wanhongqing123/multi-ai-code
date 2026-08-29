#include <QFileInfo>
#include <QMediaPlayer>
#include <QPointer>
#include <QTest>

#include "ui/VideoPreviewDialog.h"

class VideoPreviewDialogTest final : public QObject {
    Q_OBJECT

private slots:
    void closeDestroysPlayerBeforeDeferredDialogDestruction();
    void directDestructionUsesSafeFallback();
};

void VideoPreviewDialogTest::closeDestroysPlayerBeforeDeferredDialogDestruction() {
    const QString videoPath = QString::fromUtf8(MAICHAT_TEST_VIDEO_PATH);
    QVERIFY2(QFileInfo::exists(videoPath), qPrintable(videoPath));

    for (int iteration = 0; iteration < 3; ++iteration) {
        auto* dialog = new VideoPreviewDialog(videoPath, QStringLiteral("生命周期测试"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        auto* player = dialog->findChild<QMediaPlayer*>();
        QVERIFY(player != nullptr);
        QPointer<VideoPreviewDialog> dialogGuard(dialog);
        QPointer<QMediaPlayer> playerGuard(player);

        dialog->show();
        QTest::qWait(200);
        dialog->close();
        // close() 返回时 DeferredDelete 尚未处理，窗口对象仍在；播放器必须已经先释放。
        // 旧实现此刻 playerGuard 仍非空，要等对话框析构时才 stop/delete，正是 crash 窗口。
        QVERIFY(playerGuard.isNull());
        QVERIFY(!dialogGuard.isNull());
        QTRY_VERIFY_WITH_TIMEOUT(dialogGuard.isNull(), 3000);
    }
}

void VideoPreviewDialogTest::directDestructionUsesSafeFallback() {
    const QString videoPath = QString::fromUtf8(MAICHAT_TEST_VIDEO_PATH);
    auto* dialog = new VideoPreviewDialog(videoPath, QStringLiteral("直接析构测试"));
    auto* player = dialog->findChild<QMediaPlayer*>();
    QVERIFY(player != nullptr);
    QPointer<QMediaPlayer> playerGuard(player);
    dialog->show();
    QTest::qWait(200);

    // 应用退出/父窗口销毁不一定经过 closeEvent；析构兜底不能再调用危险的 stop()。
    delete dialog;
    QVERIFY(playerGuard.isNull());
}

QTEST_MAIN(VideoPreviewDialogTest)
#include "VideoPreviewDialogTest.moc"
