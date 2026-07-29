#include <QtTest/QtTest>

#include <QSignalSpy>

#include "ui/RemoteDesktopConsentDialog.h"
#include "ui/RemoteDesktopViewPanel.h"
#include "ui/SharingIndicatorBar.h"

class RemoteDesktopUiTest : public QObject {
    Q_OBJECT

private slots:
    void consentDialogShowsRequesterAndConsequence();
    void consentDialogDefaultsToReject();
    void consentDialogAutoRejectsWhenCountdownExpires();
    void consentDialogAcceptsOnAllow();
    void indicatorBarHiddenUntilSharing();
    void indicatorBarNamesPeerAndCountsTime();
    void indicatorBarEmitsStopRequest();
    void indicatorBarActuallyPaintsRedBackground();
    void viewerShowsPlaceholderUntilStreamArrives();
    void viewerHasNoDisconnectButtonAndClosesCleanly();
    void viewerExposesRenderHandle();
};

void RemoteDesktopUiTest::consentDialogShowsRequesterAndConsequence() {
    RemoteDesktopConsentDialog dialog(QStringLiteral("whq-iphone"), 60000);

    const auto* body = dialog.findChild<QLabel*>(QStringLiteral("remoteDesktopConsentBody"));
    QVERIFY(body != nullptr);
    QVERIFY(body->text().contains(QStringLiteral("whq-iphone")));
    // 必须写清后果，不能只问"是否允许"。
    QVERIFY(body->text().contains(QStringLiteral("实时看到")));
    QVERIFY(body->text().contains(QStringLiteral("随时停止")));
}

void RemoteDesktopUiTest::consentDialogDefaultsToReject() {
    RemoteDesktopConsentDialog dialog(QStringLiteral("whq-iphone"), 60000);

    const auto* reject = dialog.findChild<QPushButton*>(QStringLiteral("remoteDesktopConsentReject"));
    const auto* allow = dialog.findChild<QPushButton*>(QStringLiteral("remoteDesktopConsentAllow"));
    QVERIFY(reject != nullptr && allow != nullptr);
    // 误按回车应当拒绝而不是把屏幕共享出去。
    QVERIFY(reject->isDefault());
    QVERIFY(!allow->isDefault());
}

void RemoteDesktopUiTest::consentDialogAutoRejectsWhenCountdownExpires() {
    // 用 3 秒超时以免测试真的等 60 秒。
    RemoteDesktopConsentDialog dialog(QStringLiteral("whq-iphone"), 3000);
    QCOMPARE(dialog.remainingSeconds(), 3);

    QSignalSpy rejectedSpy(&dialog, &QDialog::rejected);
    dialog.tickForTest();
    QCOMPARE(dialog.remainingSeconds(), 2);
    QCOMPARE(rejectedSpy.count(), 0);

    dialog.tickForTest();
    dialog.tickForTest();
    // 倒计时归零 → 自动拒绝，无人应答时保持收紧。
    QCOMPARE(dialog.remainingSeconds(), 0);
    QCOMPARE(rejectedSpy.count(), 1);
}

void RemoteDesktopUiTest::consentDialogAcceptsOnAllow() {
    RemoteDesktopConsentDialog dialog(QStringLiteral("whq-iphone"), 60000);
    auto* allow = dialog.findChild<QPushButton*>(QStringLiteral("remoteDesktopConsentAllow"));
    QVERIFY(allow != nullptr);

    QSignalSpy acceptedSpy(&dialog, &QDialog::accepted);
    allow->click();
    QCOMPARE(acceptedSpy.count(), 1);
}

void RemoteDesktopUiTest::indicatorBarHiddenUntilSharing() {
    SharingIndicatorBar bar;
    QVERIFY(!bar.isVisible());
}

void RemoteDesktopUiTest::indicatorBarNamesPeerAndCountsTime() {
    SharingIndicatorBar bar;
    bar.startSharing(QStringLiteral("whq-iphone"));

    // 回到电脑前必须一眼看出是谁在看、看了多久。
    QVERIFY(bar.currentText().contains(QStringLiteral("whq-iphone")));
    QVERIFY(bar.currentText().contains(QStringLiteral("正在共享屏幕")));
    QVERIFY(bar.currentText().contains(QStringLiteral("00:00")));

    bar.stopSharing();
    QVERIFY(!bar.isVisible());
}

void RemoteDesktopUiTest::indicatorBarEmitsStopRequest() {
    SharingIndicatorBar bar;
    bar.startSharing(QStringLiteral("whq-iphone"));

    auto* stopButton = bar.findChild<QPushButton*>(QStringLiteral("sharingIndicatorStop"));
    QVERIFY(stopButton != nullptr);

    QSignalSpy stopSpy(&bar, &SharingIndicatorBar::stopRequested);
    stopButton->click();
    QCOMPARE(stopSpy.count(), 1);
}

void RemoteDesktopUiTest::indicatorBarActuallyPaintsRedBackground() {
    // 这条断言的是"画出来了什么"，不是"控件在不在"。
    // QWidget 子类默认忽略样式表背景，曾导致指示条占了 44px 却整条透明——
    // 白字落白底肉眼全无，而只查文案/可见性的用例照样全绿。
    SharingIndicatorBar bar;
    bar.resize(600, 44);
    bar.startSharing(QStringLiteral("whq-iphone"));

    const QImage painted = bar.grab().toImage();
    QVERIFY(!painted.isNull());

    const QColor corner = painted.pixelColor(4, 4);
    QCOMPARE(corner, QColor(QStringLiteral("#b42318")));
    // 中部同样是红底（而不是只有边缘被画到）。
    QCOMPARE(painted.pixelColor(painted.width() / 2, painted.height() / 2),
             QColor(QStringLiteral("#b42318")));
}

void RemoteDesktopUiTest::viewerShowsPlaceholderUntilStreamArrives() {
    RemoteDesktopViewPanel panel;
    panel.showConnecting(QStringLiteral("host-pc"));

    auto* placeholder = panel.findChild<QLabel*>(QStringLiteral("remoteViewPlaceholder"));
    QVERIFY(placeholder != nullptr);

    QVERIFY(!panel.isStreamActive());
    QCOMPARE(panel.statusText(), QStringLiteral("等待画面"));

    panel.setStreamActive(true);
    QVERIFY(panel.isStreamActive());
    QCOMPARE(panel.statusText(), QStringLiteral("画面已连接"));
    // 占位文字必须撤下：它盖在渲染面上会挡住 SDK 画的画面。
    QVERIFY(placeholder->isHidden());

    panel.setStreamActive(false);
    QVERIFY(!placeholder->isHidden());
}

void RemoteDesktopUiTest::viewerHasNoDisconnectButtonAndClosesCleanly() {
    RemoteDesktopViewPanel panel;

    // 页面内不放断开按钮：出口收敛到聊天顶栏的三态按钮。
    QVERIFY(panel.findChild<QPushButton*>(QStringLiteral("viewerDisconnect")) == nullptr);

    // 无会话时是空态；发起后切到画面区；结束后必须切回空态，
    // 否则用户会对着一块黑屏以为还连着。
    QVERIFY(!panel.isSessionVisible());
    panel.showConnecting(QStringLiteral("host-pc"));
    QVERIFY(panel.isSessionVisible());
    panel.showIdle();
    QVERIFY(!panel.isSessionVisible());
    QVERIFY(!panel.isStreamActive());
}

void RemoteDesktopUiTest::viewerExposesRenderHandle() {
    RemoteDesktopViewPanel panel;
    // TRTC 需要一个真实的原生窗口句柄来渲染；拿不到句柄画面就出不来。
    QVERIFY(panel.renderWindowHandle() != nullptr);
}

QTEST_MAIN(RemoteDesktopUiTest)
#include "RemoteDesktopUiTest.moc"
