#include <QtTest/QtTest>

#include "model/ChatState.h"

class ChatStateTest : public QObject {
    Q_OBJECT

private slots:
    void queuesOutgoingText();
    void removesAicliProtocolPrefixFromIncomingText();
    void removesAicliProtocolPrefixFromRestoredIncomingText();
    void receivesIncomingImage();
    void receivesIncomingFile();
    void updatesMessageStatus();
    void returnsPeerMessagesChronologically();
    void removesContactAndMessages();
};

void ChatStateTest::queuesOutgoingText() {
    ChatState state("desktop-user");
    state.upsertContact(RemoteIMContact{"ios-user", "ios-user"});
    state.selectPeer("ios-user");

    const RemoteIMMessage message = state.queueOutgoingText("ping");

    QCOMPARE(message.fromUserId, QString("desktop-user"));
    QCOMPARE(message.toUserId, QString("ios-user"));
    QCOMPARE(message.text, QString("ping"));
    QCOMPARE(message.direction, RemoteIMMessageDirection::Outgoing);
    QCOMPARE(message.status, RemoteIMMessageStatus::Pending);
    QCOMPARE(state.messagesWith("ios-user").size(), 1);
}

void ChatStateTest::removesAicliProtocolPrefixFromIncomingText() {
    ChatState state("desktop-user");
    const QString hiddenPrefix = QStringLiteral("\u2063\u200B\u200C\u200D\u2063");

    const RemoteIMMessage hiddenMarked = state.receiveText(
        "electron-user",
        hiddenPrefix + QStringLiteral("## 结论\n\nMarkdown 正文")
    );
    const RemoteIMMessage legacyMarked = state.receiveText(
        "electron-user",
        QStringLiteral("【AICLI 输出】\n## 结果")
    );

    QCOMPARE(hiddenMarked.text, QStringLiteral("## 结论\n\nMarkdown 正文"));
    QCOMPARE(legacyMarked.text, QStringLiteral("## 结果"));
}

void ChatStateTest::removesAicliProtocolPrefixFromRestoredIncomingText() {
    ChatState state("desktop-user");
    const QString hiddenPrefix = QStringLiteral("\u2063\u200B\u200C\u200D\u2063");
    RemoteIMMessage message;
    message.fromUserId = QStringLiteral("electron-user");
    message.toUserId = QStringLiteral("desktop-user");
    message.text = hiddenPrefix + QStringLiteral("# Win/Mac 每周 Crash 详细报表");
    message.direction = RemoteIMMessageDirection::Incoming;

    state.appendMessageForRestore(message);

    QCOMPARE(state.messagesWith(QStringLiteral("electron-user")).first().text,
             QStringLiteral("# Win/Mac 每周 Crash 详细报表"));
}

void ChatStateTest::receivesIncomingImage() {
    ChatState state("desktop-user");

    const RemoteIMMessage message = state.receiveImage("ios-user", "/tmp/a.jpg", 640, 480, 1024);

    QCOMPARE(message.fromUserId, QString("ios-user"));
    QCOMPARE(message.toUserId, QString("desktop-user"));
    QCOMPARE(message.text, QString("[图片消息] a.jpg"));
    QCOMPARE(message.direction, RemoteIMMessageDirection::Incoming);
    QCOMPARE(message.status, RemoteIMMessageStatus::Received);
    QVERIFY(message.hasImage);
    QCOMPARE(state.contacts().size(), 1);
    QCOMPARE(state.selectedPeerId(), QString("ios-user"));
}

void ChatStateTest::receivesIncomingFile() {
    ChatState state("desktop-user");

    const RemoteIMMessage message = state.receiveFile(
        "ios-user",
        "/tmp/report.md",
        "report.md",
        "text/markdown",
        4096
    );

    QCOMPARE(message.fromUserId, QString("ios-user"));
    QCOMPARE(message.toUserId, QString("desktop-user"));
    QCOMPARE(message.text, QString("[文件消息] report.md"));
    QCOMPARE(message.direction, RemoteIMMessageDirection::Incoming);
    QCOMPARE(message.status, RemoteIMMessageStatus::Received);
    QVERIFY(message.hasFile);
    QCOMPARE(message.file.localPath, QString("/tmp/report.md"));
    QCOMPARE(message.file.fileName, QString("report.md"));
    QCOMPARE(message.file.mimeType, QString("text/markdown"));
    QCOMPARE(message.file.sizeBytes, qint64(4096));
}

void ChatStateTest::updatesMessageStatus() {
    ChatState state("desktop-user");
    state.upsertContact(RemoteIMContact{"ios-user", "ios-user"});
    state.selectPeer("ios-user");
    const RemoteIMMessage message = state.queueOutgoingText("ping");

    QVERIFY(state.updateMessageStatus(message.id, RemoteIMMessageStatus::Sent));

    QCOMPARE(state.messagesWith("ios-user").first().status, RemoteIMMessageStatus::Sent);
}

void ChatStateTest::returnsPeerMessagesChronologically() {
    ChatState state("desktop-user");

    RemoteIMMessage newest;
    newest.id = QStringLiteral("newest");
    newest.fromUserId = QStringLiteral("ios-user");
    newest.toUserId = QStringLiteral("desktop-user");
    newest.text = QStringLiteral("30s");
    newest.createdAtMillis = 30'000;

    RemoteIMMessage oldest = newest;
    oldest.id = QStringLiteral("oldest");
    oldest.text = QStringLiteral("10s");
    oldest.createdAtMillis = 10'000;

    RemoteIMMessage middle = newest;
    middle.id = QStringLiteral("middle");
    middle.text = QStringLiteral("20s");
    middle.createdAtMillis = 20'000;

    state.appendMessageForRestore(newest);
    state.appendMessageForRestore(oldest);
    state.appendMessageForRestore(middle);

    const QList<RemoteIMMessage> messages = state.messagesWith(QStringLiteral("ios-user"));

    QCOMPARE(messages.size(), 3);
    QCOMPARE(messages.at(0).text, QStringLiteral("10s"));
    QCOMPARE(messages.at(1).text, QStringLiteral("20s"));
    QCOMPARE(messages.at(2).text, QStringLiteral("30s"));
}

void ChatStateTest::removesContactAndMessages() {
    ChatState state("desktop-user");
    state.upsertContact(RemoteIMContact{"ios-user", "iPhone"});
    state.upsertContact(RemoteIMContact{"other-user", "Other"});
    state.selectPeer("ios-user");
    state.receiveText("ios-user", "hello");
    state.receiveText("other-user", "keep");

    state.removeContactAndMessages(" ios-user ");

    QCOMPARE(state.contacts().size(), 1);
    QCOMPARE(state.contacts().first().userId, QString("other-user"));
    QCOMPARE(state.messagesWith("ios-user").size(), 0);
    QCOMPARE(state.messagesWith("other-user").size(), 1);
    QCOMPARE(state.selectedPeerId(), QString("other-user"));
}

QTEST_MAIN(ChatStateTest)
#include "ChatStateTest.moc"
