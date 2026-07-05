#include <QSignalSpy>
#include <QTest>
#include <memory>

#include "app/RemoteIMApplication.h"
#include "im/FakeRemoteIMClient.h"

class RemoteIMApplicationTest : public QObject {
    Q_OBJECT

private slots:
    void sendsTextThroughClientAndMarksSent();
    void receivesIncomingTextIntoSelectedConversation();
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

QTEST_MAIN(RemoteIMApplicationTest)
#include "RemoteIMApplicationTest.moc"

