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
    void reordersSameSecondMessagesAfterTimeCorrection();
    void removesContactAndMessages();
    void adoptsRemoteMessageIdAndDropsDuplicateTemp();
    void countsUnreadOnlyForBackgroundPeers();
    void selectPeerClearsUnread();
    void restoreDoesNotAffectUnread();
    void removingContactDropsItsUnread();
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
    QCOMPARE(message.createdAtMillis % 1000, Q_INT64_C(0));
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

void ChatStateTest::reordersSameSecondMessagesAfterTimeCorrection() {
    ChatState state("desktop-user");

    RemoteIMMessage request;
    request.id = QStringLiteral("z-request");
    request.fromUserId = QStringLiteral("ios-user");
    request.toUserId = QStringLiteral("desktop-user");
    request.text = QStringLiteral("检查编译流程");
    request.direction = RemoteIMMessageDirection::Incoming;
    request.createdAtMillis = 10'482;

    RemoteIMMessage ack = request;
    ack.id = QStringLiteral("a-ack");
    ack.fromUserId = QStringLiteral("desktop-user");
    ack.toUserId = QStringLiteral("ios-user");
    ack.text = QStringLiteral("已发送给当前 AICLI，开始处理。");
    ack.direction = RemoteIMMessageDirection::Outgoing;
    ack.createdAtMillis = 10'000;

    // 本机乐观消息带毫秒，随后收到的 SDK 消息只有秒级时间：确认信息回写前
    // 会错误地把后发回执排在用户消息前面。
    state.appendMessageForRestore(request);
    state.appendMessageForRestore(ack);
    QCOMPARE(state.messagesWith(QStringLiteral("ios-user")).first().id, QStringLiteral("a-ack"));

    QVERIFY(state.updateMessageTime(QStringLiteral("z-request"), 10'000));

    const QList<RemoteIMMessage> messages = state.messagesWith(QStringLiteral("ios-user"));
    QCOMPARE(messages.size(), 2);
    QCOMPARE(messages.at(0).id, QStringLiteral("z-request"));
    QCOMPARE(messages.at(1).id, QStringLiteral("a-ack"));
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

void ChatStateTest::adoptsRemoteMessageIdAndDropsDuplicateTemp() {
    ChatState state("me");
    state.upsertContact(RemoteIMContact{"peer", "peer"});
    state.selectPeer("peer");

    // 常规采纳：临时 UUID 换成 SDK 稳定 id，状态更新按新 id 生效。
    const RemoteIMMessage queued = state.queueOutgoingText("hello");
    QVERIFY(state.adoptMessageId(queued.id, "sdk-1#0"));
    QVERIFY(state.updateMessageStatus("sdk-1#0", RemoteIMMessageStatus::Sent));
    QCOMPARE(state.messagesWith("peer").first().id, QStringLiteral("sdk-1#0"));

    // 稳定 id 已存在（漫游先到）：采纳失败并移除临时重复项。
    RemoteIMMessage roamed;
    roamed.id = "sdk-2#0";
    roamed.fromUserId = "me";
    roamed.toUserId = "peer";
    roamed.direction = RemoteIMMessageDirection::Outgoing;
    roamed.status = RemoteIMMessageStatus::Sent;
    roamed.text = "dup";
    state.appendMessageForRestore(roamed);
    const RemoteIMMessage temp = state.queueOutgoingText("dup");
    QVERIFY(!state.adoptMessageId(temp.id, "sdk-2#0"));
    int dupCount = 0;
    for (const RemoteIMMessage& message : state.messagesWith("peer")) {
        if (message.text == QStringLiteral("dup")) ++dupCount;
    }
    QCOMPARE(dupCount, 1);
}

void ChatStateTest::countsUnreadOnlyForBackgroundPeers() {
    ChatState state("desktop-user");

    // 首条消息自动选中该会话，视为已读：不计红点。
    state.receiveText("peer-a", "hello");
    QCOMPARE(state.selectedPeerId(), QString("peer-a"));
    QCOMPARE(state.unreadCount("peer-a"), 0);

    // 非选中会话的实时消息累计未读（文本/图片/文件/语音同规则）。
    state.receiveText("peer-b", "one");
    state.receiveImage("peer-b", "C:/tmp/pic.png", 10, 10, 100);
    state.receiveFile("peer-b", "C:/tmp/report.pdf", "report.pdf", "application/pdf", 200);
    state.receiveVoice("peer-b", "C:/tmp/voice.mp3", 3);
    QCOMPARE(state.unreadCount("peer-b"), 4);
    QCOMPARE(state.unreadCount("peer-a"), 0);

    // 当前选中会话继续收消息不计未读（消息就在屏幕上）。
    state.receiveText("peer-a", "again");
    QCOMPARE(state.unreadCount("peer-a"), 0);
}

void ChatStateTest::selectPeerClearsUnread() {
    ChatState state("desktop-user");
    state.receiveText("peer-a", "hello");
    state.receiveText("peer-b", "hi");
    state.receiveText("peer-b", "there");
    QCOMPARE(state.unreadCount("peer-b"), 2);

    state.selectPeer("peer-b");

    QCOMPARE(state.unreadCount("peer-b"), 0);
    // 切走后新消息重新计数。
    state.selectPeer("peer-a");
    state.receiveText("peer-b", "back");
    QCOMPARE(state.unreadCount("peer-b"), 1);
}

void ChatStateTest::restoreDoesNotAffectUnread() {
    ChatState state("desktop-user");
    state.selectPeer("peer-a");

    // 本地库加载/SDK 漫游补充是历史消息，不产生红点。
    RemoteIMMessage history;
    history.fromUserId = QStringLiteral("peer-b");
    history.toUserId = QStringLiteral("desktop-user");
    history.text = QStringLiteral("yesterday");
    history.direction = RemoteIMMessageDirection::Incoming;
    state.appendMessageForRestore(history);

    QCOMPARE(state.unreadCount("peer-b"), 0);
}

void ChatStateTest::removingContactDropsItsUnread() {
    ChatState state("desktop-user");
    state.receiveText("peer-a", "hello");
    state.receiveText("peer-b", "hi");
    QCOMPARE(state.unreadCount("peer-b"), 1);

    state.removeContactAndMessages("peer-b");
    QCOMPARE(state.unreadCount("peer-b"), 0);

    // 再次加回联系人不携带历史红点。
    state.receiveText("peer-b", "fresh");
    QCOMPARE(state.unreadCount("peer-b"), 1);
    state.selectPeer("peer-b");
    QCOMPARE(state.unreadCount("peer-b"), 0);
}

QTEST_MAIN(ChatStateTest)
#include "ChatStateTest.moc"
