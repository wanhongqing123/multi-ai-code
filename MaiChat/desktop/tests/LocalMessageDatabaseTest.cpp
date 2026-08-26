#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
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
    void correctsExistingMessageTimeFromSdkDuplicate();
    void updatesMessageStatus();
    void cascadesContactDeletion();
    void adoptsMessageIdAndResolvesConflict();
    void loadsRecentPagePerPeerAndPagesBackward();
    void roundTripsVideoAttachment();
    void roundTripsApprovalRequest();
    void upgradesPreVideoDatabaseWithoutLosingOldMessages();
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
        db.upsertContact(RemoteIMContact{"peer", "Peer", "https://example.com/peer.png"});
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
    QCOMPARE(state.contacts().first().avatarUrl, QStringLiteral("https://example.com/peer.png"));
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

void LocalMessageDatabaseTest::correctsExistingMessageTimeFromSdkDuplicate() {
    QTemporaryDir dir;
    LocalMessageDatabase db(dir.filePath("messages.db"));

    const RemoteIMMessage legacyAck = makeTextMessage(
        "a-ack", "me", "peer", RemoteIMMessageDirection::Outgoing, 1001, "ack");
    const RemoteIMMessage legacyRequest = makeTextMessage(
        "z-request", "peer", "me", RemoteIMMessageDirection::Incoming, 1482, "request");
    QVERIFY(db.insertMessageIfAbsent(legacyAck, "peer"));
    QVERIFY(db.insertMessageIfAbsent(legacyRequest, "peer"));

    // 漫游命中旧记录时不重复插入，但会用 SDK 会话顺序生成的规范化时间
    // 替换本机乐观发送时间。
    RemoteIMMessage correctedRequest = legacyRequest;
    correctedRequest.createdAtMillis = 1000;
    QVERIFY(!db.insertMessageIfAbsent(correctedRequest, "peer"));

    ChatState state("me");
    db.loadInto(state);
    const QList<RemoteIMMessage> messages = state.messagesWith("peer");
    QCOMPARE(messages.size(), 2);
    QCOMPARE(messages.at(0).id, QStringLiteral("z-request"));
    QCOMPARE(messages.at(1).id, QStringLiteral("a-ack"));
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

void LocalMessageDatabaseTest::roundTripsVideoAttachment() {
    QTemporaryDir dir;
    LocalMessageDatabase db(dir.filePath("messages.db"));
    QVERIFY(db.isOpen());

    RemoteIMMessage message;
    message.id = QStringLiteral("video-1");
    message.fromUserId = QStringLiteral("me");
    message.toUserId = QStringLiteral("peer");
    message.direction = RemoteIMMessageDirection::Outgoing;
    message.status = RemoteIMMessageStatus::Sent;
    message.text = QStringLiteral("看下这段录屏");
    message.createdAtMillis = 1700000000000LL;
    message.hasVideo = true;
    message.video = RemoteIMVideoAttachment{
        QStringLiteral("E:/clips/screen-record.mp4"),
        QStringLiteral("screen-record.mp4"),
        QStringLiteral("E:/covers/cover-1.jpg"),
        42,
        8388608LL
    };
    QVERIFY(db.insertMessageIfAbsent(message, QStringLiteral("peer")));

    ChatState restored(QStringLiteral("me"));
    db.loadInto(restored);
    const QList<RemoteIMMessage> messages = restored.messagesWith(QStringLiteral("peer"));
    QCOMPARE(messages.size(), 1);
    const RemoteIMMessage& loaded = messages.first();
    // 插入是位置绑定（29 个 ?），列名单/占位符/绑定顺序错位不会报错，只会把值写进
    // 相邻的列。逐字段比对是唯一能发现错位的手段。
    QVERIFY(loaded.hasVideo);
    QVERIFY(!loaded.hasFile);
    QVERIFY(!loaded.hasImage);
    QCOMPARE(loaded.video.localPath, QStringLiteral("E:/clips/screen-record.mp4"));
    QCOMPARE(loaded.video.fileName, QStringLiteral("screen-record.mp4"));
    QCOMPARE(loaded.video.coverPath, QStringLiteral("E:/covers/cover-1.jpg"));
    QCOMPARE(loaded.video.durationSeconds, 42);
    QCOMPARE(loaded.video.sizeBytes, 8388608LL);
    QCOMPARE(loaded.text, QStringLiteral("看下这段录屏"));
}

void LocalMessageDatabaseTest::roundTripsApprovalRequest() {
    QTemporaryDir dir;
    LocalMessageDatabase db(dir.filePath("messages.db"));
    QVERIFY(db.isOpen());

    RemoteIMMessage message = makeTextMessage(
        QStringLiteral("approval-1"),
        QStringLiteral("multi-ai-code"),
        QStringLiteral("me"),
        RemoteIMMessageDirection::Incoming,
        1700000000100LL,
        QStringLiteral("Codex 请求执行一条高风险命令"));
    message.hasApprovalRequest = true;
    message.approvalRequest = RemoteIMApprovalRequest{
        QStringLiteral("approval-desktop-persisted"),
        {RemoteIMApprovalAction::ApproveOnce,
         RemoteIMApprovalAction::ApprovePrefix,
         RemoteIMApprovalAction::Reject}
    };
    QVERIFY(db.insertMessageIfAbsent(message, QStringLiteral("multi-ai-code")));

    ChatState restored(QStringLiteral("me"));
    db.loadInto(restored);
    const RemoteIMMessage loaded = restored.messagesWith(QStringLiteral("multi-ai-code")).first();
    QVERIFY(loaded.hasApprovalRequest);
    QCOMPARE(loaded.approvalRequest.token, QStringLiteral("approval-desktop-persisted"));
    QVERIFY(loaded.approvalRequest.actions == message.approvalRequest.actions);
}

void LocalMessageDatabaseTest::upgradesPreVideoDatabaseWithoutLosingOldMessages() {
    QTemporaryDir dir;
    const QString path = dir.filePath("messages.db");

    // 1) 先造一个「视频功能之前」的库：手工建不含 video_* 列的表并塞一条文件消息。
    {
        QSqlDatabase legacy = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                        QStringLiteral("legacy_pre_video"));
        legacy.setDatabaseName(path);
        QVERIFY(legacy.open());
        QSqlQuery q(legacy);
        QVERIFY(q.exec(QStringLiteral(
            "CREATE TABLE messages ("
            "  id TEXT PRIMARY KEY, from_user TEXT NOT NULL, to_user TEXT NOT NULL,"
            "  peer TEXT NOT NULL, direction INTEGER NOT NULL, status INTEGER NOT NULL,"
            "  text TEXT NOT NULL DEFAULT '', created_at INTEGER NOT NULL,"
            "  has_image INTEGER NOT NULL DEFAULT 0,"
            "  image_path TEXT, image_w INTEGER, image_h INTEGER, image_bytes INTEGER,"
            "  has_voice INTEGER NOT NULL DEFAULT 0, voice_path TEXT, voice_seconds INTEGER,"
            "  has_file INTEGER NOT NULL DEFAULT 0,"
            "  file_path TEXT, file_name TEXT, file_mime TEXT, file_bytes INTEGER)")));
        QVERIFY(q.exec(QStringLiteral(
            "INSERT INTO messages(id, from_user, to_user, peer, direction, status, text,"
            " created_at, has_file, file_path, file_name, file_mime, file_bytes)"
            " VALUES('old-1','peer','me','peer',0,2,'旧的文件消息',1600000000000,1,"
            "'E:/old/report.md','report.md','text/markdown',2048)")));
        legacy.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("legacy_pre_video"));

    // 2) 用新版打开：迁移必须补列，且旧消息一条不丢。
    {
        LocalMessageDatabase db(path);
        QVERIFY(db.isOpen());
        ChatState restored(QStringLiteral("me"));
        db.loadInto(restored);
        const QList<RemoteIMMessage> messages = restored.messagesWith(QStringLiteral("peer"));
        QCOMPARE(messages.size(), 1);
        QVERIFY(messages.first().hasFile);
        QCOMPARE(messages.first().file.fileName, QStringLiteral("report.md"));
        QVERIFY(!messages.first().hasVideo);

        // 读取按列名取值，列不存在只会得到无效 QVariant（=0/空），所以上面那条
        // hasVideo==false 两种情况都成立、证明不了迁移跑了。直接问 schema。
        QSet<QString> columns;
        {
            // QSqlQuery 必须先出作用域，否则 removeDatabase 会警告「连接仍在使用」。
            QSqlDatabase probe = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                           QStringLiteral("probe_schema"));
            probe.setDatabaseName(path);
            QVERIFY(probe.open());
            QSqlQuery probeQuery(probe);
            QVERIFY(probeQuery.exec(QStringLiteral("PRAGMA table_info(messages)")));
            while (probeQuery.next()) columns.insert(probeQuery.value(1).toString());
        }
        QSqlDatabase::removeDatabase(QStringLiteral("probe_schema"));
        for (const QString& expected : {QStringLiteral("has_video"), QStringLiteral("video_path"),
                                        QStringLiteral("video_name"), QStringLiteral("video_cover"),
                                        QStringLiteral("video_seconds"), QStringLiteral("video_bytes"),
                                        QStringLiteral("approval_token"), QStringLiteral("approval_actions")}) {
            QVERIFY2(columns.contains(expected), qPrintable(QStringLiteral("缺列 %1").arg(expected)));
        }

        RemoteIMMessage video;
        video.id = QStringLiteral("video-new");
        video.fromUserId = QStringLiteral("me");
        video.toUserId = QStringLiteral("peer");
        video.direction = RemoteIMMessageDirection::Outgoing;
        video.status = RemoteIMMessageStatus::Sent;
        video.createdAtMillis = 1700000000000LL;
        // text 必须显式给：默认构造的 QString 是 null，会绑成 SQL NULL 撞上
        // text NOT NULL，而 INSERT OR IGNORE 会把约束冲突静默吞掉。
        video.text = QStringLiteral("[视频消息] a.mp4");
        video.hasVideo = true;
        video.video = RemoteIMVideoAttachment{QStringLiteral("E:/clips/a.mp4"),
                                              QStringLiteral("a.mp4"),
                                              QStringLiteral("E:/covers/a.jpg"), 7, 1024LL};
        QVERIFY(db.insertMessageIfAbsent(video, QStringLiteral("peer")));
    }

    // 3) 再开一次：迁移必须幂等（重复 ALTER 会报 duplicate column），两条都在。
    {
        LocalMessageDatabase db(path);
        QVERIFY(db.isOpen());
        ChatState restored(QStringLiteral("me"));
        db.loadInto(restored);
        const QList<RemoteIMMessage> messages = restored.messagesWith(QStringLiteral("peer"));
        QCOMPARE(messages.size(), 2);
        const RemoteIMMessage& video = messages.last();
        QVERIFY(video.hasVideo);
        QCOMPARE(video.video.coverPath, QStringLiteral("E:/covers/a.jpg"));
        QCOMPARE(video.video.durationSeconds, 7);
    }
}

QTEST_MAIN(LocalMessageDatabaseTest)
#include "LocalMessageDatabaseTest.moc"
