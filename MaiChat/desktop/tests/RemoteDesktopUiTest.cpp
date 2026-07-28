#include <QtTest/QtTest>

#include <QSignalSpy>

#include "ui/RemoteDesktopConsentDialog.h"
#include "ui/RemoteDesktopViewerDialog.h"
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
    void viewerEmitsDisconnectRequest();
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
    RemoteDesktopViewerDialog viewer(QStringLiteral("host-pc"));
    auto* placeholder = viewer.findChild<QLabel*>(QStringLiteral("viewerPlaceholder"));
    QVERIFY(placeholder != nullptr);

    QVERIFY(!viewer.isStreamActive());
    QCOMPARE(viewer.statusText(), QStringLiteral("等待画面"));

    viewer.setStreamActive(true);
    QVERIFY(viewer.isStreamActive());
    QCOMPARE(viewer.statusText(), QStringLiteral("画面已连接"));
    // 占位文字必须撤下：它盖在渲染面上会挡住 SDK 画的画面。
    QVERIFY(placeholder->isHidden());

    viewer.setStreamActive(false);
    QVERIFY(!placeholder->isHidden());
}

void RemoteDesktopUiTest::viewerEmitsDisconnectRequest() {
    RemoteDesktopViewerDialog viewer(QStringLiteral("host-pc"));
    auto* button = viewer.findChild<QPushButton*>(QStringLiteral("viewerDisconnect"));
    QVERIFY(button != nullptr);

    QSignalSpy spy(&viewer, &RemoteDesktopViewerDialog::disconnectRequested);
    button->click();
    QCOMPARE(spy.count(), 1);
}

void RemoteDesktopUiTest::viewerExposesRenderHandle() {
    RemoteDesktopViewerDialog viewer(QStringLiteral("host-pc"));
    // TRTC 需要一个真实的原生窗口句柄来渲染；拿不到句柄画面就出不来。
    QVERIFY(viewer.renderWindowHandle() != nullptr);
}

QTEST_MAIN(RemoteDesktopUiTest)
#include "RemoteDesktopUiTest.moc"
