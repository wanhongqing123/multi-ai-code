#include <QTemporaryDir>
#include <QTest>

#include "model/ChatState.h"
#include "storage/LocalMessageDatabase.h"

namespace {

RemoteIMMessage makeTextMessage(const QString& id, const QString& from, const QString& to,
                                RemoteIMMessageDirection direction, qint64 createdAtMillis,
                                const QString& text) {
    RemoteIMMessage message;
    message.id = id;
    message.fromUserId = from;
    message.toUserId = to;
    message.direction = direction;
    message.status = direction == RemoteIMMessageDirection::Outgoing ? RemoteIMMessageStatus::Pending
                                                                     : RemoteIMMessageStatus::Received;
    message.createdAtMillis = createdAtMillis;
    message.text = text;
    return message;
}

}  // namespace

class LocalMessageDatabaseTest : public QObject {
    Q_OBJECT

private slots:
    void insertsAndDeduplicatesById();
    void persistsAcrossReopen();
    void loadsMessagesSortedByTime();
    void updatesMessageStatus();
    void cascadesContactDeletion();
    void adoptsMessageIdAndResolvesConflict();
    void loadsRecentPagePerPeerAndPagesBackward();
};

void LocalMessageDatabaseTest::insertsAndDeduplicatesById() {
    QTemporaryDir dir;
    LocalMessageDatabase db(dir.filePath("messages.db"));
    QVERIFY(db.isOpen());

    const RemoteIMMessage message = makeTextMessage(
        "sdk-1#0", "peer", "me", RemoteIMMessageDirection::Incoming, 1000, "hello");
    QVERIFY(db.insertMessageIfAbsent(message, "peer"));
    // 同 id 二次插入（如 SDK 漫游重复投递）不生效。
    QVERIFY(!db.insertMessageIfAbsent(message, "peer"));

    ChatState state("me");
    db.loadInto(state);
    QCOMPARE(state.messages().size(), 1);
    QCOMPARE(state.messages().first().id, QStringLiteral("sdk-1#0"));
}

void LocalMessageDatabaseTest::persistsAcrossReopen() {
    QTemporaryDir dir;
    const QString path = dir.filePath("messages.db");
    {
        LocalMessageDatabase db(path);
        db.upsertContact(RemoteIMContact{"peer", "Peer"});
        db.insertMessageIfAbsent(
            makeTextMessage("m1", "peer", "me", RemoteIMMessageDirection::Incoming, 1000, "first"),
            "peer");

        RemoteIMMessage fileMessage = makeTextMessage(
            "m2", "me", "peer", RemoteIMMessageDirection::Outgoing, 2000, "[文件消息] a.md");
        fileMessage.hasFile = true;
        fileMessage.file = RemoteIMFileAttachment{"C:/tmp/a.md", "a.md", "text/markdown", 42};
        db.insertMessageIfAbsent(fileMessage, "peer");
    }

    LocalMessageDatabase reopened(path);
    ChatState state("me");
    reopened.loadInto(state);
    QCOMPARE(state.contacts().size(), 1);
    QCOMPARE(state.contacts().first().displayName, QStringLiteral("Peer"));
    QCOMPARE(state.messages().size(), 2);
    const RemoteIMMessage restoredFile = state.messages().last();
    QVERIFY(restoredFile.hasFile);
    QCOMPARE(restoredFile.file.fileName, QStringLiteral("a.md"));
    QCOMPARE(restoredFile.file.sizeBytes, static_cast<qint64>(42));
}

void LocalMessageDatabaseTest::loadsMessagesSortedByTime() {
    QTemporaryDir dir;
    LocalMessageDatabase db(dir.filePath("messages.db"));
    db.insertMessageIfAbsent(
        makeTextMessage("late", "peer", "me", RemoteIMMessageDirection::Incoming, 3000, "late"), "peer");
    db.insertMessageIfAbsent(
        makeTextMessage("early", "peer", "me", RemoteIMMessageDirection::Incoming, 1000, "early"), "peer");

    ChatState state("me");
    db.loadInto(state);
    QCOMPARE(state.messages().size(), 2);
    QCOMPARE(state.messages().first().id, QStringLiteral("early"));
    QCOMPARE(state.messages().last().id, QStringLiteral("late"));
}

void LocalMessageDatabaseTest::updatesMessageStatus() {
    QTemporaryDir dir;
    LocalMessageDatabase db(dir.filePath("messages.db"));
    db.insertMessageIfAbsent(
        makeTextMessage("m1", "me", "peer", RemoteIMMessageDirection::Outgoing, 1000, "hi"), "peer");
    db.updateMessageStatus("m1", RemoteIMMessageStatus::Sent);

    ChatState state("me");
    db.loadInto(state);
    QCOMPARE(state.messages().first().status, RemoteIMMessageStatus::Sent);
}

void LocalMessageDatabaseTest::cascadesContactDeletion() {
    QTemporaryDir dir;
    LocalMessageDatabase db(dir.filePath("messages.db"));
    db.upsertContact(RemoteIMContact{"peer-a", "A"});
    db.upsertContact(RemoteIMContact{"peer-b", "B"});
    db.insertMessageIfAbsent(
        makeTextMessage("a1", "peer-a", "me", RemoteIMMessageDirection::Incoming, 1000, "from a"), "peer-a");
    db.insertMessageIfAbsent(
        makeTextMessage("b1", "peer-b", "me", RemoteIMMessageDirection::Incoming, 2000, "from b"), "peer-b");

    db.removeContactCascade("peer-a");

    ChatState state("me");
    db.loadInto(state);
    QCOMPARE(state.contacts().size(), 1);
    QCOMPARE(state.contacts().first().userId, QStringLiteral("peer-b"));
    QCOMPARE(state.messages().size(), 1);
    QCOMPARE(state.messages().first().id, QStringLiteral("b1"));
}

void LocalMessageDatabaseTest::adoptsMessageIdAndResolvesConflict() {
    QTemporaryDir dir;
    LocalMessageDatabase db(dir.filePath("messages.db"));
    db.insertMessageIfAbsent(
        makeTextMessage("temp-uuid", "me", "peer", RemoteIMMessageDirection::Outgoing, 1000, "hi"), "peer");

    // 常规采纳：主键换成 SDK 稳定 id。
    db.adoptMessageId("temp-uuid", "sdk-1#0");
    ChatState state("me");
    db.loadInto(state);
    QCOMPARE(state.messages().size(), 1);
    QCOMPARE(state.messages().first().id, QStringLiteral("sdk-1#0"));

    // 稳定 id 已被漫游占用：旧临时行按重复项清除。
    db.insertMessageIfAbsent(
        makeTextMessage("temp-2", "me", "peer", RemoteIMMessageDirection::Outgoing, 2000, "hi2"), "peer");
    db.insertMessageIfAbsent(
        makeTextMessage("sdk-2#0", "me", "peer", RemoteIMMessageDirection::Outgoing, 2000, "hi2"), "peer");
    db.adoptMessageId("temp-2", "sdk-2#0");
    ChatState reloaded("me");
    db.loadInto(reloaded);
    QCOMPARE(reloaded.messages().size(), 2);
}

void LocalMessageDatabaseTest::loadsRecentPagePerPeerAndPagesBackward() {
    QTemporaryDir dir;
    LocalMessageDatabase db(dir.filePath("messages.db"));
    // peer-a 5 条、peer-b 2 条；每会话页大小 3。
    for (int i = 1; i <= 5; ++i) {
        db.insertMessageIfAbsent(makeTextMessage(QStringLiteral("a%1").arg(i), "peer-a", "me",
                                                 RemoteIMMessageDirection::Incoming, i * 100,
                                                 QStringLiteral("a-msg-%1").arg(i)),
                                 "peer-a");
    }
    for (int i = 1; i <= 2; ++i) {
        db.insertMessageIfAbsent(makeTextMessage(QStringLiteral("b%1").arg(i), "peer-b", "me",
                                                 RemoteIMMessageDirection::Incoming, i * 100,
                                                 QStringLiteral("b-msg-%1").arg(i)),
                                 "peer-b");
    }

    ChatState state("me");
    const QHash<QString, bool> hasEarlier = db.loadRecentInto(state, 3);
    // peer-a 只载最近 3 条（a3..a5），peer-b 全量 2 条。
    QCOMPARE(state.messagesWith("peer-a").size(), 3);
    QCOMPARE(state.messagesWith("peer-a").first().id, QStringLiteral("a3"));
    QCOMPARE(state.messagesWith("peer-b").size(), 2);
    QCOMPARE(hasEarlier.value("peer-a"), true);
    QCOMPARE(hasEarlier.value("peer-b"), false);

    // 键集向上翻页：严格早于 (300, "a3") 的是 a1、a2，升序返回。
    const QList<RemoteIMMessage> earlier = db.loadMessagesBefore("peer-a", 300, "a3", 3);
    QCOMPARE(earlier.size(), 2);
    QCOMPARE(earlier.first().id, QStringLiteral("a1"));
    QCOMPARE(earlier.last().id, QStringLiteral("a2"));

    // 已到最早：不再返回。
    QCOMPARE(db.loadMessagesBefore("peer-a", 100, "a1", 3).size(), 0);
}

QTEST_MAIN(LocalMessageDatabaseTest)
#include "LocalMessageDatabaseTest.moc"
