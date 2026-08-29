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

QTEST_MAIN(MessageNotificationTest)
#include "MessageNotificationTest.moc"
