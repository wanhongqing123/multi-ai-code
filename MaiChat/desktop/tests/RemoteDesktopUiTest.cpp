#include <QtTest/QtTest>

#include <QSignalSpy>

#include "ui/RemoteDesktopConsentDialog.h"
#include "ui/RemoteDesktopSessionCard.h"
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
    void viewerKeepsOneCardPerPeer();
    void viewerRoutesStreamStateToMatchingCard();
    void viewerFallsBackToSoleCardOnUserIdMismatch();
    void viewerGivesOneCardTheWholePageAndTilesTheRest();
    void viewerFullScreenHidesOtherCardsAndReportsState();
    void viewerFullScreenTogglesFromCardDoubleClickAndButton();
    void viewerFullScreenExitsOnEscape();
    void viewerFullScreenExitsWhenThatSessionEnds();
    void noticeBannerHiddenUntilPeerReportsSomething();
    void noticeRoutesToMatchingCardOnly();
};

void RemoteDesktopUiTest::noticeBannerHiddenUntilPeerReportsSomething() {
    RemoteDesktopViewPanel panel;
    panel.beginSession(QStringLiteral("host-pc"));
    auto* card = panel.cardFor(QStringLiteral("host-pc"));
    QVERIFY(card != nullptr);

    // 平时不该占地方，更不该显示空白条。
    auto* banner = card->findChild<QLabel*>(QStringLiteral("remoteCardNotice"));
    QVERIFY(banner != nullptr);
    QVERIFY(banner->isHidden());
    QVERIFY(card->noticeText().isEmpty());

    card->setNoticeText(QStringLiteral("对方电脑弹出了系统授权框"));
    QVERIFY(!banner->isHidden());
    QVERIFY(card->noticeText().contains(QStringLiteral("系统授权框")));

    // 对端恢复正常后必须撤下，不能一直挂着"正在等待授权"。
    card->setNoticeText(QString());
    QVERIFY(banner->isHidden());
    QVERIFY(card->noticeText().isEmpty());
}

void RemoteDesktopUiTest::noticeRoutesToMatchingCardOnly() {
    RemoteDesktopViewPanel panel;
    panel.beginSession(QStringLiteral("host-a"));
    panel.beginSession(QStringLiteral("host-b"));

    // 一台机器弹 UAC 不该在另一台的卡片上也挂提示。
    panel.setNoticeText(QStringLiteral("host-b"), QStringLiteral("对方电脑弹出了系统授权框"));
    QVERIFY(panel.cardFor(QStringLiteral("host-a"))->noticeText().isEmpty());
    QVERIFY(!panel.cardFor(QStringLiteral("host-b"))->noticeText().isEmpty());
}

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
    panel.beginSession(QStringLiteral("host-pc"));

    auto* placeholder = panel.findChild<QLabel*>(QStringLiteral("remoteCardPlaceholder"));
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
    panel.beginSession(QStringLiteral("host-pc"));
    QVERIFY(panel.isSessionVisible());
    panel.showIdle();
    QVERIFY(!panel.isSessionVisible());
    QVERIFY(!panel.isStreamActive());
}

void RemoteDesktopUiTest::viewerExposesRenderHandle() {
    RemoteDesktopViewPanel panel;
    panel.beginSession(QStringLiteral("host-pc"));
    // TRTC 需要一个真实的原生窗口句柄来渲染；拿不到句柄画面就出不来。
    QVERIFY(panel.renderWindowHandle() != nullptr);
    QVERIFY(panel.renderWindowHandle(QStringLiteral("host-pc")) != nullptr);
}

void RemoteDesktopUiTest::viewerKeepsOneCardPerPeer() {
    RemoteDesktopViewPanel panel;

    // 重复发起同一个人不应堆出第二张卡片。
    panel.beginSession(QStringLiteral("host-a"));
    panel.beginSession(QStringLiteral("host-a"));
    QCOMPARE(panel.sessionCount(), 1);

    panel.beginSession(QStringLiteral("host-b"));
    QCOMPARE(panel.sessionCount(), 2);
    QCOMPARE(panel.sessionPeerIds(),
             QVector<QString>({QStringLiteral("host-a"), QStringLiteral("host-b")}));

    // 结束其中一路，另一路必须留在页面上，且不能掉回空态。
    panel.endSession(QStringLiteral("host-a"));
    QCOMPARE(panel.sessionCount(), 1);
    QVERIFY(panel.isSessionVisible());
    QVERIFY(panel.cardFor(QStringLiteral("host-a")) == nullptr);
    QVERIFY(panel.cardFor(QStringLiteral("host-b")) != nullptr);

    panel.endSession(QStringLiteral("host-b"));
    QCOMPARE(panel.sessionCount(), 0);
    QVERIFY(!panel.isSessionVisible());
}

void RemoteDesktopUiTest::viewerRoutesStreamStateToMatchingCard() {
    RemoteDesktopViewPanel panel;
    panel.beginSession(QStringLiteral("host-a"));
    panel.beginSession(QStringLiteral("host-b"));

    // 一路来画面不能把另一路也点亮，否则多路时状态会串。
    panel.setStreamActive(QStringLiteral("host-b"), true);
    QVERIFY(!panel.cardFor(QStringLiteral("host-a"))->isStreamActive());
    QVERIFY(panel.cardFor(QStringLiteral("host-b"))->isStreamActive());

    QVERIFY(panel.renderWindowHandle(QStringLiteral("host-a")) !=
            panel.renderWindowHandle(QStringLiteral("host-b")));
}

void RemoteDesktopUiTest::viewerFallsBackToSoleCardOnUserIdMismatch() {
    RemoteDesktopViewPanel panel;
    panel.beginSession(QStringLiteral("host-pc"));

    // TRTC 侧 userId 理论上等于 IM userId；万一对不上而此刻只有一路，
    // 应落到那一路上——宁可画面照常出来，也不要退回黑屏（之前踩过）。
    panel.setStreamActive(QStringLiteral("someone-else"), true);
    QVERIFY(panel.cardFor(QStringLiteral("host-pc"))->isStreamActive());
    QVERIFY(panel.renderWindowHandle(QStringLiteral("someone-else")) != nullptr);
}

void RemoteDesktopUiTest::viewerGivesOneCardTheWholePageAndTilesTheRest() {
    RemoteDesktopViewPanel panel;
    panel.resize(960, 600);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));

    // 单路必须铺满整页，观感等同"全屏看对方屏幕"——否则用户会觉得画面莫名缩水。
    panel.beginSession(QStringLiteral("host-a"));
    auto* first = panel.cardFor(QStringLiteral("host-a"));
    QVERIFY(first != nullptr);
    QTRY_VERIFY(first->width() > panel.width() * 0.9);
    QTRY_VERIFY(first->height() > panel.height() * 0.9);

    // 两路并排且等分：网格重排要真的生效，不能把第二张摞在第一张上。
    panel.beginSession(QStringLiteral("host-b"));
    auto* second = panel.cardFor(QStringLiteral("host-b"));
    QVERIFY(second != nullptr);
    QTRY_COMPARE(first->y(), second->y());
    QTRY_COMPARE(first->width(), second->width());
    QVERIFY(first->x() < second->x());
    QVERIFY(second->width() < panel.width() * 0.6);

    // 关掉一路，剩下那路要重新铺满，而不是继续占半边。
    panel.endSession(QStringLiteral("host-a"));
    QTRY_VERIFY(second->width() > panel.width() * 0.9);
}

void RemoteDesktopUiTest::viewerFullScreenHidesOtherCardsAndReportsState() {
    RemoteDesktopViewPanel panel;
    panel.resize(960, 600);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    panel.beginSession(QStringLiteral("host-a"));
    panel.beginSession(QStringLiteral("host-b"));

    QSignalSpy fullScreenSpy(&panel, &RemoteDesktopViewPanel::fullScreenChanged);
    panel.enterFullScreen(QStringLiteral("host-b"));

    QCOMPARE(panel.isFullScreen(), true);
    QCOMPARE(panel.fullScreenPeerId(), QStringLiteral("host-b"));
    QCOMPARE(fullScreenSpy.count(), 1);
    QCOMPARE(fullScreenSpy.takeFirst().at(0).toBool(), true);

    auto* focused = panel.cardFor(QStringLiteral("host-b"));
    auto* other = panel.cardFor(QStringLiteral("host-a"));
    QVERIFY(other->isHidden());
    QVERIFY(!focused->isHidden());
    QVERIFY(focused->isFullScreenActive());
    // 铺满且贴边：全屏时网格边距归零，画面不该还留着一圈白。
    QTRY_COMPARE(focused->width(), panel.width());
    QTRY_COMPARE(focused->height(), panel.height());

    // 另一路只是藏起来，卡片和它的渲染句柄都还在——退出全屏画面要立刻回来，
    // 不能因为销毁重建把 TRTC 手里的句柄弄失效。
    QVERIFY(panel.cardFor(QStringLiteral("host-a")) != nullptr);
    QVERIFY(panel.renderWindowHandle(QStringLiteral("host-a")) != nullptr);

    panel.exitFullScreen();
    QCOMPARE(panel.isFullScreen(), false);
    QCOMPARE(fullScreenSpy.count(), 1);
    QCOMPARE(fullScreenSpy.takeFirst().at(0).toBool(), false);
    QTRY_VERIFY(!other->isHidden());
    QVERIFY(!focused->isFullScreenActive());
    QTRY_COMPARE(other->y(), focused->y());
}

void RemoteDesktopUiTest::viewerFullScreenTogglesFromCardDoubleClickAndButton() {
    RemoteDesktopViewPanel panel;
    panel.beginSession(QStringLiteral("host-a"));
    auto* card = panel.cardFor(QStringLiteral("host-a"));
    QVERIFY(card != nullptr);

    // 双击画面进全屏，再双击退出。
    QTest::mouseDClick(card, Qt::LeftButton);
    QCOMPARE(panel.fullScreenPeerId(), QStringLiteral("host-a"));
    QTest::mouseDClick(card, Qt::LeftButton);
    QVERIFY(!panel.isFullScreen());

    // 顶栏按钮是兜底入口：SDK 若在原生句柄上盖了自己的子窗口把鼠标吃掉，
    // 双击会失灵，按钮必须照样能进出全屏。
    auto* button = card->findChild<QPushButton*>(QStringLiteral("remoteCardFullScreen"));
    QVERIFY(button != nullptr);
    button->click();
    QCOMPARE(panel.fullScreenPeerId(), QStringLiteral("host-a"));

    // 图标是自绘的：断言真画出了东西、且进出全屏两态不同。
    // 之前用 ⤢/⤡ 字符，Windows 上落到 fallback 字体细到几乎看不见，
    // 而只查"按钮在不在"的用例照样全绿。
    const QImage expanded = button->icon().pixmap(button->iconSize()).toImage();
    QVERIFY(!expanded.isNull());
    int inkedPixels = 0;
    for (int y = 0; y < expanded.height(); ++y) {
        for (int x = 0; x < expanded.width(); ++x) {
            if (qAlpha(expanded.pixel(x, y)) > 32) ++inkedPixels;
        }
    }
    QVERIFY(inkedPixels > 8);

    button->click();
    QVERIFY(!panel.isFullScreen());
    QVERIFY(button->icon().pixmap(button->iconSize()).toImage() != expanded);
}

void RemoteDesktopUiTest::viewerFullScreenExitsOnEscape() {
    RemoteDesktopViewPanel panel;
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    panel.beginSession(QStringLiteral("host-a"));
    panel.enterFullScreen(QStringLiteral("host-a"));
    QVERIFY(panel.isFullScreen());

    // Esc 必须能退出：全屏后侧栏和窗口边框都没了，这是最直觉的退路。
    QTest::keyClick(&panel, Qt::Key_Escape);
    QVERIFY(!panel.isFullScreen());
}

void RemoteDesktopUiTest::viewerFullScreenExitsWhenThatSessionEnds() {
    RemoteDesktopViewPanel panel;
    panel.beginSession(QStringLiteral("host-a"));
    panel.beginSession(QStringLiteral("host-b"));
    panel.enterFullScreen(QStringLiteral("host-a"));

    QSignalSpy fullScreenSpy(&panel, &RemoteDesktopViewPanel::fullScreenChanged);
    // 正全屏看的那一路断了要自动退出，否则会剩一个没有内容的全屏窗口。
    panel.endSession(QStringLiteral("host-a"));
    QVERIFY(!panel.isFullScreen());
    QCOMPARE(fullScreenSpy.count(), 1);
    QCOMPARE(fullScreenSpy.takeFirst().at(0).toBool(), false);
    // 剩下那一路要显示出来，不能因为之前被全屏藏了就一直不见。
    QVERIFY(!panel.cardFor(QStringLiteral("host-b"))->isHidden());

    // 另一路断开时正全屏的那路不受影响。
    panel.enterFullScreen(QStringLiteral("host-b"));
    panel.endSession(QStringLiteral("host-b"));
    QVERIFY(!panel.isFullScreen());
    QVERIFY(!panel.isSessionVisible());
}

QTEST_MAIN(RemoteDesktopUiTest)
#include "RemoteDesktopUiTest.moc"
