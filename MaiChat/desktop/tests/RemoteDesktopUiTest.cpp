#include <QtTest/QtTest>

#include <QSignalSpy>

#include "ui/RemoteDesktopConsentDialog.h"
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

QTEST_MAIN(RemoteDesktopUiTest)
#include "RemoteDesktopUiTest.moc"
