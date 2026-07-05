#include <QtTest/QtTest>

#include "storage/LocalChatHistoryStore.h"

class LocalChatHistoryStoreTest : public QObject {
    Q_OBJECT

private slots:
    void savesAndLoadsMessagesAndContacts();
};

void LocalChatHistoryStoreTest::savesAndLoadsMessagesAndContacts() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LocalChatHistoryStore store(dir.path());

    ChatState state("desktop-user");
    state.upsertContact(RemoteIMContact{"ios-user", "iPhone"});
    state.selectPeer("ios-user");
    state.queueOutgoingText("ping");
    state.receiveImage("ios-user", "/tmp/a.jpg", 640, 480, 1024);

    QVERIFY(store.save(state));
    ChatState restored("desktop-user");
    QVERIFY(store.load("desktop-user", restored));

    QCOMPARE(restored.contacts().size(), 1);
    QCOMPARE(restored.contacts().first().displayName, QString("iPhone"));
    QCOMPARE(restored.messagesWith("ios-user").size(), 2);
    QCOMPARE(restored.messagesWith("ios-user").at(0).text, QString("ping"));
    QVERIFY(restored.messagesWith("ios-user").at(1).hasImage);
    QCOMPARE(restored.selectedPeerId(), QString("ios-user"));
}

QTEST_MAIN(LocalChatHistoryStoreTest)
#include "LocalChatHistoryStoreTest.moc"
