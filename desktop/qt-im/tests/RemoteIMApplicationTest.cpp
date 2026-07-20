#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <memory>

#include "app/RemoteIMApplication.h"
#include "im/FakeRemoteIMClient.h"
#include "storage/LocalMessageDatabase.h"

class RemoteIMApplicationTest : public QObject {
    Q_OBJECT

private slots:
    void sendsTextThroughClientAndMarksSent();
    void receivesIncomingTextIntoSelectedConversation();
    void deletesContactThroughClientAfterRemoteSuccess();
    void keepsContactWhenRemoteDeletionFails();
    void persistsMessagesAcrossRestart();
    void mergesRoamingMessagesWithoutDuplicates();
    void cascadesLocalHistoryOnContactDeletion();
    void adoptsSentRemoteIdSoRoamingDoesNotDuplicate();
    void loadsRecentPageOnStartAndPagesEarlier();
};

void RemoteIMApplicationTest::sendsTextThroughClientAndMarksSent() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    QSignalSpy stateSpy(&app, &RemoteIMApplication::stateChanged);

    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.sendText(QStringLiteral("hello"));

    QCOMPARE(fakeClient->lastTextPeerId(), QStringLiteral("phone-user"));
    QCOMPARE(fakeClient->lastText(), QStringLiteral("hello"));
    const QList<RemoteIMMessage> messages = app.chatState().messagesWith(QStringLiteral("phone-user"));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().status, RemoteIMMessageStatus::Sent);
    QVERIFY(stateSpy.count() >= 2);
}

void RemoteIMApplicationTest::receivesIncomingTextIntoSelectedConversation() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));

    fakeClient->emitIncomingText(QStringLiteral("phone-user"), QStringLiteral("from phone"));

    QCOMPARE(app.chatState().selectedPeerId(), QStringLiteral("phone-user"));
    const QList<RemoteIMMessage> messages = app.chatState().messagesWith(QStringLiteral("phone-user"));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().text, QStringLiteral("from phone"));
    QCOMPARE(messages.first().direction, RemoteIMMessageDirection::Incoming);
}

void RemoteIMApplicationTest::deletesContactThroughClientAfterRemoteSuccess() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveText(QStringLiteral("phone-user"), QStringLiteral("remove me"));

    app.deleteContact(QStringLiteral(" phone-user "));

    QCOMPARE(fakeClient->lastDeletedContactId(), QStringLiteral("phone-user"));
    QVERIFY(app.chatState().contacts().isEmpty());
    QVERIFY(app.chatState().messagesWith(QStringLiteral("phone-user")).isEmpty());
}

void RemoteIMApplicationTest::keepsContactWhenRemoteDeletionFails() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    fakeClient->failNext(QStringLiteral("network failed"));
    QSignalSpy errorSpy(&app, &RemoteIMApplication::errorMessage);

    app.deleteContact(QStringLiteral("phone-user"));

    QCOMPARE(app.chatState().contacts().size(), 1);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.takeFirst().at(0).toString(), QStringLiteral("network failed"));
}

void RemoteIMApplicationTest::persistsMessagesAcrossRestart() {
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("messages.db");
    {
        auto client = std::make_unique<FakeRemoteIMClient>();
        auto* fakeClient = client.get();
        RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client),
                                std::make_unique<LocalMessageDatabase>(dbPath));
        app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
        app.sendText(QStringLiteral("hello"));
        fakeClient->emitIncomingText(QStringLiteral("phone-user"), QStringLiteral("from phone"));
    }

    // 重启：新 app + 同一库文件，历史（含消息状态）应完整恢复。
    RemoteIMApplication restarted(QStringLiteral("desktop-user"), std::make_unique<FakeRemoteIMClient>(),
                                  std::make_unique<LocalMessageDatabase>(dbPath));
    const QList<RemoteIMMessage> messages = restarted.chatState().messagesWith(QStringLiteral("phone-user"));
    QCOMPARE(messages.size(), 2);
    const auto sent = std::find_if(messages.cbegin(), messages.cend(), [](const RemoteIMMessage& message) {
        return message.text == QStringLiteral("hello");
    });
    const auto received = std::find_if(messages.cbegin(), messages.cend(), [](const RemoteIMMessage& message) {
        return message.text == QStringLiteral("from phone");
    });
    QVERIFY(sent != messages.cend());
    QVERIFY(received != messages.cend());
    QCOMPARE(sent->status, RemoteIMMessageStatus::Sent);
    QCOMPARE(restarted.chatState().contacts().size(), 1);
}

void RemoteIMApplicationTest::mergesRoamingMessagesWithoutDuplicates() {
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("messages.db");

    RemoteIMMessage roamed;
    roamed.id = QStringLiteral("sdk-100#0");
    roamed.fromUserId = QStringLiteral("phone-user");
    roamed.toUserId = QStringLiteral("desktop-user");
    roamed.direction = RemoteIMMessageDirection::Incoming;
    roamed.status = RemoteIMMessageStatus::Received;
    roamed.createdAtMillis = 1000;
    roamed.text = QStringLiteral("roamed once");

    {
        auto client = std::make_unique<FakeRemoteIMClient>();
        auto* fakeClient = client.get();
        RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client),
                                std::make_unique<LocalMessageDatabase>(dbPath));
        // 同一条 SDK 消息重复投递（实时 + 漫游），按 id 只展示一次。
        emit fakeClient->messagesReceived({roamed});
        emit fakeClient->messagesReceived({roamed});
        QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).size(), 1);
    }

    // 重启后 SDK 再次漫游同一条消息：本地库已有，不重复。
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication restarted(QStringLiteral("desktop-user"), std::move(client),
                                  std::make_unique<LocalMessageDatabase>(dbPath));
    emit fakeClient->messagesReceived({roamed});
    QCOMPARE(restarted.chatState().messagesWith(QStringLiteral("phone-user")).size(), 1);
}

void RemoteIMApplicationTest::cascadesLocalHistoryOnContactDeletion() {
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("messages.db");
    {
        auto client = std::make_unique<FakeRemoteIMClient>();
        auto* fakeClient = client.get();
        RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client),
                                std::make_unique<LocalMessageDatabase>(dbPath));
        fakeClient->emitIncomingText(QStringLiteral("phone-user"), QStringLiteral("remove me"));
        app.deleteContact(QStringLiteral("phone-user"));
    }

    RemoteIMApplication restarted(QStringLiteral("desktop-user"), std::make_unique<FakeRemoteIMClient>(),
                                  std::make_unique<LocalMessageDatabase>(dbPath));
    QVERIFY(restarted.chatState().messagesWith(QStringLiteral("phone-user")).isEmpty());
    QVERIFY(restarted.chatState().contacts().isEmpty());
}

void RemoteIMApplicationTest::adoptsSentRemoteIdSoRoamingDoesNotDuplicate() {
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("messages.db");
    QString adoptedId;
    {
        auto client = std::make_unique<FakeRemoteIMClient>();
        RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client),
                                std::make_unique<LocalMessageDatabase>(dbPath));
        app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
        app.sendText(QStringLiteral("hello"));
        const QList<RemoteIMMessage> messages = app.chatState().messagesWith(QStringLiteral("phone-user"));
        QCOMPARE(messages.size(), 1);
        adoptedId = messages.first().id;
        // Fake 客户端回执 fake-remote-<n>#0：临时 UUID 已被替换成稳定 id。
        QVERIFY(adoptedId.startsWith(QStringLiteral("fake-remote-")));
        QCOMPARE(messages.first().status, RemoteIMMessageStatus::Sent);
    }

    // 重启后 SDK 漫游重投同一条已发消息（相同稳定 id）：不重复显示。
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication restarted(QStringLiteral("desktop-user"), std::move(client),
                                  std::make_unique<LocalMessageDatabase>(dbPath));
    RemoteIMMessage roamed;
    roamed.id = adoptedId;
    roamed.fromUserId = QStringLiteral("desktop-user");
    roamed.toUserId = QStringLiteral("phone-user");
    roamed.direction = RemoteIMMessageDirection::Outgoing;
    roamed.status = RemoteIMMessageStatus::Sent;
    roamed.createdAtMillis = 1;
    roamed.text = QStringLiteral("hello");
    emit fakeClient->messagesReceived({roamed});
    QCOMPARE(restarted.chatState().messagesWith(QStringLiteral("phone-user")).size(), 1);
}

void RemoteIMApplicationTest::loadsRecentPageOnStartAndPagesEarlier() {
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("messages.db");
    {
        // 预置 450 条历史（超过一页 200）。
        LocalMessageDatabase db(dbPath);
        db.upsertContact(RemoteIMContact{"phone-user", "iPhone"});
        for (int i = 1; i <= 450; ++i) {
            RemoteIMMessage message;
            message.id = QStringLiteral("m%1").arg(i);
            message.fromUserId = "phone-user";
            message.toUserId = "desktop-user";
            message.direction = RemoteIMMessageDirection::Incoming;
            message.status = RemoteIMMessageStatus::Received;
            message.createdAtMillis = i;
            message.text = QStringLiteral("msg-%1").arg(i);
            db.insertMessageIfAbsent(message, "phone-user");
        }
    }

    RemoteIMApplication app(QStringLiteral("desktop-user"), std::make_unique<FakeRemoteIMClient>(),
                            std::make_unique<LocalMessageDatabase>(dbPath));
    // 启动只载最近一页 200 条（m251..m450）。
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).size(), 200);
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).first().id, QStringLiteral("m251"));
    QVERIFY(app.hasEarlierMessages(QStringLiteral("phone-user")));

    // 翻一页：+200（m51..m250）。
    QCOMPARE(app.loadEarlierMessages(QStringLiteral("phone-user")), 200);
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).size(), 400);
    QVERIFY(app.hasEarlierMessages(QStringLiteral("phone-user")));

    // 再翻：只剩 50，翻完后没有更早了。
    QCOMPARE(app.loadEarlierMessages(QStringLiteral("phone-user")), 50);
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).size(), 450);
    QVERIFY(!app.hasEarlierMessages(QStringLiteral("phone-user")));
    QCOMPARE(app.loadEarlierMessages(QStringLiteral("phone-user")), 0);
}

QTEST_MAIN(RemoteIMApplicationTest)
#include "RemoteIMApplicationTest.moc"
