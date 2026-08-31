#include <QtTest>

#include "model/MessageQuote.h"

class MessageQuoteTest : public QObject {
    Q_OBJECT

private slots:
    void extractsSdkMessageIdOnlyFromTrailingNumericSuffix();
    void omitsMsgIdForLocallyGeneratedIds();
    void usesAuthoredCaptionBeforeTypePlaceholder();
    void showsFileNameButNeverLocalPath();
    void collapsesWhitespaceAndClampsLongText();
    void ignoresInternalPlaceholderTextAsCaption();
};

// 协议里的 msgId 必须是**原始 SDK 消息 ID**：desktop 把 SDK id 加 `#<elem下标>`
// 当本地主键，而 iOS/Android 存的是不带下标的原始 msgID。两边格式不一致时，
// 跨端「跳到原文」会静默失效——不报错、不空白，只是点了没反应。
void MessageQuoteTest::extractsSdkMessageIdOnlyFromTrailingNumericSuffix() {
    QCOMPARE(MessageQuote::sdkMessageIdOf(QStringLiteral("abc123#0")), QStringLiteral("abc123"));
    QCOMPARE(MessageQuote::sdkMessageIdOf(QStringLiteral("abc123#12")), QStringLiteral("abc123"));

    // 只剥末尾一段，不按第一个 # 截断：协议层不该假设 SDK id 自身不含 #。
    QCOMPARE(MessageQuote::sdkMessageIdOf(QStringLiteral("a#b#3")), QStringLiteral("a#b"));
}

void MessageQuoteTest::omitsMsgIdForLocallyGeneratedIds() {
    // 随机 UUID：拿不到 SDK id 时主键会退化成这个，跨端无法解析，必须留空。
    QVERIFY(MessageQuote::sdkMessageIdOf(
                QStringLiteral("6a5f1f0e-3c1a-4d67-9f2b-1e0c9d7a4b55")).isEmpty());
    // 末尾不是数字。
    QVERIFY(MessageQuote::sdkMessageIdOf(QStringLiteral("abc123#x")).isEmpty());
    // # 结尾、# 开头、以及根本没有 #。
    QVERIFY(MessageQuote::sdkMessageIdOf(QStringLiteral("abc123#")).isEmpty());
    QVERIFY(MessageQuote::sdkMessageIdOf(QStringLiteral("#3")).isEmpty());
    QVERIFY(MessageQuote::sdkMessageIdOf(QStringLiteral("abc123")).isEmpty());

    RemoteIMMessage message;
    message.id = QStringLiteral("6a5f1f0e-3c1a-4d67-9f2b-1e0c9d7a4b55");
    message.text = QStringLiteral("普通文本");
    QVERIFY(MessageQuote::quoteFor(message).msgId.isEmpty());
}

void MessageQuoteTest::usesAuthoredCaptionBeforeTypePlaceholder() {
    RemoteIMMessage withCaption;
    withCaption.hasImage = true;
    withCaption.text = QStringLiteral("这张图是重点");
    QCOMPARE(MessageQuote::kindOf(withCaption), QStringLiteral("image"));
    QCOMPARE(MessageQuote::digestOf(withCaption), QStringLiteral("这张图是重点"));

    RemoteIMMessage bare;
    bare.hasImage = true;
    QCOMPARE(MessageQuote::digestOf(bare), QStringLiteral("[图片]"));

    RemoteIMMessage video;
    video.hasVideo = true;
    QCOMPARE(MessageQuote::kindOf(video), QStringLiteral("video"));
    QCOMPARE(MessageQuote::digestOf(video), QStringLiteral("[视频]"));

    RemoteIMMessage voice;
    voice.hasVoice = true;
    QCOMPARE(MessageQuote::kindOf(voice), QStringLiteral("voice"));
    QCOMPARE(MessageQuote::digestOf(voice), QStringLiteral("[语音]"));
}

// 文件名保留（引用块在会话内部，文件名对辨认「回复的是哪一份」有价值），
// 但本地路径绝不能进入要发给对端的消息。
void MessageQuoteTest::showsFileNameButNeverLocalPath() {
    RemoteIMMessage message;
    message.hasFile = true;
    message.file.fileName = QStringLiteral("季度报表.xlsx");
    message.file.localPath = QStringLiteral("C:/Users/someone/Downloads/季度报表.xlsx");
    QCOMPARE(MessageQuote::kindOf(message), QStringLiteral("file"));
    QCOMPARE(MessageQuote::digestOf(message), QStringLiteral("[文件] 季度报表.xlsx"));

    // fileName 被写成了整条路径时也只取最后一段。
    RemoteIMMessage pathAsName;
    pathAsName.hasFile = true;
    pathAsName.file.fileName = QStringLiteral("D:/private/内部资料/名单.csv");
    const QString digest = MessageQuote::digestOf(pathAsName);
    QCOMPARE(digest, QStringLiteral("[文件] 名单.csv"));
    QVERIFY(!digest.contains(QStringLiteral("private")));
}

void MessageQuoteTest::collapsesWhitespaceAndClampsLongText() {
    RemoteIMMessage multiline;
    multiline.text = QStringLiteral("  第一行\n\n第二行\t第三行  ");
    QCOMPARE(MessageQuote::digestOf(multiline), QStringLiteral("第一行 第二行 第三行"));

    RemoteIMMessage overlong;
    overlong.text = QString(MessageQuote::kDigestLimit + 40, QLatin1Char('a'));
    const QString digest = MessageQuote::digestOf(overlong);
    QCOMPARE(digest.size(), MessageQuote::kDigestLimit + 1);
    QVERIFY(digest.endsWith(QStringLiteral("…")));

    // 正好卡在上限上不该被截断。
    RemoteIMMessage exact;
    exact.text = QString(MessageQuote::kDigestLimit, QLatin1Char('b'));
    QCOMPARE(MessageQuote::digestOf(exact).size(), MessageQuote::kDigestLimit);
}

// 入站附件消息的 text 常是「[图片消息] 文件名」这种内部占位，
// 那不是用户写的配文，拿去当摘要会把内部约定连同文件名一起漏进引用块。
void MessageQuoteTest::ignoresInternalPlaceholderTextAsCaption() {
    RemoteIMMessage message;
    message.hasImage = true;
    message.text = QStringLiteral("[图片消息] IMG_20260831_0001.jpg");
    const QString digest = MessageQuote::digestOf(message);
    QCOMPARE(digest, QStringLiteral("[图片]"));
    QVERIFY(!digest.contains(QStringLiteral("IMG_")));
}

QTEST_MAIN(MessageQuoteTest)
#include "MessageQuoteTest.moc"
