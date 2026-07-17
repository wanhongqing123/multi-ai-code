#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
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
    QCOMPARE(messages.first().text, QStringLiteral("hello"));
    QCOMPARE(messages.first().status, RemoteIMMessageStatus::Sent);
    QCOMPARE(messages.last().text, QStringLiteral("from phone"));
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

QTEST_MAIN(RemoteIMApplicationTest)
#include "RemoteIMApplicationTest.moc"
