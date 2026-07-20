#include <QtTest/QtTest>

#include "im/FakeRemoteIMClient.h"

class FakeRemoteIMClientTest : public QObject {
    Q_OBJECT

private slots:
    void sendsTextAndEmitsIncomingText();
    void canFailNextOperation();
};

void FakeRemoteIMClientTest::sendsTextAndEmitsIncomingText() {
    FakeRemoteIMClient client;
    QString incomingPeer;
    QString incomingText;

    QObject::connect(&client, &RemoteIMClient::incomingText, [&](const QString& peerId, const QString& text) {
        incomingPeer = peerId;
        incomingText = text;
    });

    bool sent = false;
    client.sendText("ios-user", "ping", [&](bool ok, const QString&, const RemoteIMSendReceipt&) {
        sent = ok;
    });
    client.emitIncomingText("ios-user", "done");

    QVERIFY(sent);
    QCOMPARE(client.lastTextPeerId(), QString("ios-user"));
    QCOMPARE(client.lastText(), QString("ping"));
    QCOMPARE(incomingPeer, QString("ios-user"));
    QCOMPARE(incomingText, QString("done"));
}

void FakeRemoteIMClientTest::canFailNextOperation() {
    FakeRemoteIMClient client;
    client.failNext("network failed");

    bool sent = true;
    QString error;
    client.sendText("ios-user", "ping", [&](bool ok, const QString& message, const RemoteIMSendReceipt&) {
        sent = ok;
        error = message;
    });

    QVERIFY(!sent);
    QCOMPARE(error, QString("network failed"));
}

QTEST_MAIN(FakeRemoteIMClientTest)
#include "FakeRemoteIMClientTest.moc"
