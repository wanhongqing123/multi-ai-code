#include <QTest>

#include "model/MessageNotification.h"

namespace {

RemoteIMMessage textMessage(const QString& text) {
    RemoteIMMessage message;
    message.text = text;
    return message;
}

RemoteIMMessage imageMessage(const QString& caption) {
    RemoteIMMessage message;
    message.hasImage = true;
    message.image.localPath = QStringLiteral("C:/Users/someone/AppData/Roaming/MaiChat/img/a.png");
    message.text = caption;
    return message;
}

}  // namespace

class MessageNotificationTest : public QObject {
    Q_OBJECT

private slots:
    void titleFallsBackToUserIdWhenThereIsNoDisplayName();
    void previewNeverLeaksLocalFilePaths();
    void previewUsesCaptionWhenTheAttachmentHasOne();
    void previewDropsInternalPlaceholderText();
    void longTextIsTruncated();
    void aggregatedPreviewReportsHowManyArePending();
    void deliveryTrackerSuppressesStartupBacklog();
    void deliveryTrackerShowsOnlyOnceUntilConversationIsViewed();
    void foregroundWindowSuppressesSystemNotification();
};

void MessageNotificationTest::titleFallsBackToUserIdWhenThereIsNoDisplayName() {
    QCOMPARE(MessageNotification::title(QStringLiteral("Alice"), QStringLiteral("alice")),
             QStringLiteral("Alice"));
    // 没有备注名时总得让人知道是谁发的，退回 userId 好过空标题。
    QCOMPARE(MessageNotification::title(QString(), QStringLiteral("alice")),
             QStringLiteral("alice"));
    QCOMPARE(MessageNotification::title(QStringLiteral("   "), QStringLiteral("alice")),
             QStringLiteral("alice"));
}

void MessageNotificationTest::previewNeverLeaksLocalFilePaths() {
    const QString body = MessageNotification::preview(imageMessage(QString()));

    // 系统通知会出现在锁屏、通知中心、录屏里。本地路径既没用又泄露目录结构，
    // 而且里面常常带着用户名。
    QCOMPARE(body, QStringLiteral("[图片]"));
    QVERIFY(!body.contains(QStringLiteral("AppData")));
    QVERIFY(!body.contains(QStringLiteral("someone")));
    QVERIFY(!body.contains(QStringLiteral(".png")));
}

void MessageNotificationTest::previewUsesCaptionWhenTheAttachmentHasOne() {
    // 有配文时两样都要：只显示配文，用户不知道还带了张图。
    QCOMPARE(MessageNotification::preview(imageMessage(QStringLiteral("看这个报错"))),
             QStringLiteral("[图片] 看这个报错"));
}

void MessageNotificationTest::previewDropsInternalPlaceholderText() {
    // 「[图片消息] …」是内部约定的占位串，不是用户写的配文。
    // 拿它当预览等于把内部实现摆到通知栏里。
    QCOMPARE(MessageNotification::preview(imageMessage(QStringLiteral("[图片消息] a.png"))),
             QStringLiteral("[图片]"));
}

void MessageNotificationTest::longTextIsTruncated() {
    const QString longText = QString(200, QLatin1Char('x'));
    const QString body = MessageNotification::preview(textMessage(longText));
    QCOMPARE(body.size(), MessageNotification::kPreviewLimit + 1);  // +1 是省略号
    QVERIFY(body.endsWith(QStringLiteral("…")));

    // 刚好等于上限的不截断，也不加省略号。
    const QString exact = QString(MessageNotification::kPreviewLimit, QLatin1Char('y'));
    QCOMPARE(MessageNotification::preview(textMessage(exact)), exact);
}

void MessageNotificationTest::aggregatedPreviewReportsHowManyArePending() {
    const RemoteIMMessage latest = textMessage(QStringLiteral("在吗"));

    // 一条就正常显示，不要写成「1 条新消息：在吗」那种啰嗦话。
    QCOMPARE(MessageNotification::aggregatedPreview(latest, 1), QStringLiteral("在吗"));
    QCOMPARE(MessageNotification::aggregatedPreview(latest, 0), QStringLiteral("在吗"));

    // 堆了多条时报条数 + 最新一条：逐条弹窗会把桌面刷满，
    // 而用户真正想知道的是「谁找我、有几条」。
    QCOMPARE(MessageNotification::aggregatedPreview(latest, 5),
             QStringLiteral("5 条新消息：在吗"));
}

void MessageNotificationTest::deliveryTrackerSuppressesStartupBacklog() {
    MessageNotification::DeliveryTracker tracker(/*sessionStartedAtMillis=*/10'500);

    // 启动前一秒及更早的 SDK 补投只进入聊天记录/未读，不进入系统通知队列。
    const auto backlog = tracker.record(QStringLiteral("peer-a"), 9'999);
    QCOMPARE(backlog.disposition, MessageNotification::DeliveryDisposition::StartupBacklog);
    QVERIFY(!backlog.shouldShow());
    QCOMPARE(backlog.pendingCount, 0);

    // SDK 时间是秒级：启动所在的同一秒必须放行，否则刚启动后立刻来的真消息会丢提醒。
    const auto sameSecond = tracker.record(QStringLiteral("peer-a"), 10'000);
    QCOMPARE(sameSecond.disposition, MessageNotification::DeliveryDisposition::Show);
    QVERIFY(sameSecond.shouldShow());
}

void MessageNotificationTest::deliveryTrackerShowsOnlyOnceUntilConversationIsViewed() {
    MessageNotification::DeliveryTracker tracker(/*sessionStartedAtMillis=*/10'500);

    const auto first = tracker.record(QStringLiteral("peer-a"), 11'000);
    QCOMPARE(first.disposition, MessageNotification::DeliveryDisposition::Show);
    QCOMPARE(first.pendingCount, 1);

    // 以前这里仍会调用 showMessage 68 次，只是正文从 2 一直改到 69；
    // Windows 不会覆盖旧气泡，结果就是用户关都关不掉的通知风暴。
    for (int pending = 2; pending <= 69; ++pending) {
        const auto repeated = tracker.record(QStringLiteral("peer-a"), 11'000 + pending);
        QCOMPARE(repeated.disposition, MessageNotification::DeliveryDisposition::AlreadyPending);
        QVERIFY(!repeated.shouldShow());
        QCOMPARE(repeated.pendingCount, pending);
    }

    // 打开会话等同于确认已看到；清闸后下一条真正的新消息可以再次提醒。
    tracker.clear(QStringLiteral("peer-a"));
    const auto afterViewed = tracker.record(QStringLiteral("peer-a"), 12'000);
    QCOMPARE(afterViewed.disposition, MessageNotification::DeliveryDisposition::Show);
    QCOMPARE(afterViewed.pendingCount, 1);

    // 一个联系人的待处理状态不能把另一个人的第一条通知一起吞掉。
    const auto otherPeer = tracker.record(QStringLiteral("peer-b"), 12'000);
    QCOMPARE(otherPeer.disposition, MessageNotification::DeliveryDisposition::Show);

    tracker.clearAll();
    const auto afterForeground = tracker.record(QStringLiteral("peer-a"), 13'000);
    QCOMPARE(afterForeground.disposition, MessageNotification::DeliveryDisposition::Show);
    QCOMPARE(afterForeground.pendingCount, 1);
}

void MessageNotificationTest::foregroundWindowSuppressesSystemNotification() {
    // 用户正在操作 MaiChat 时，无论看的是联系人、设置还是别人的会话，都不弹系统通知。
    QVERIFY(MessageNotification::shouldSuppressForForegroundWindow(true, false));

    // 最小化或切到别的应用后才需要系统级提醒。
    QVERIFY(!MessageNotification::shouldSuppressForForegroundWindow(true, true));
    QVERIFY(!MessageNotification::shouldSuppressForForegroundWindow(false, false));
}

QTEST_MAIN(MessageNotificationTest)
#include "MessageNotificationTest.moc"
