#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include <memory>

#include "MaiTool.h"
#include "MaiChatTools/MaiChatHostTools.h"
#include "app/RemoteIMApplication.h"
#include "im/FakeRemoteIMClient.h"

class MaiChatHostToolsTest : public QObject {
  Q_OBJECT

private slots:
  void registersMaiChatReadAndWriteTools();
  void readsContactsMessagesSearchAndUnreadState();
  void sendsAndRepliesOnlyThroughApprovalGatedTools();
};

void MaiChatHostToolsTest::registersMaiChatReadAndWriteTools() {
  RemoteIMApplication app(QStringLiteral("owner"),
                          std::make_unique<FakeRemoteIMClient>());
  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);

  for (const char *name :
       {"maichat_list_contacts", "maichat_list_conversations",
        "maichat_get_messages", "maichat_search_messages",
        "maichat_get_unread_summary", "maichat_send_text",
        "maichat_reply_message", "maichat_broadcast_text"}) {
    QVERIFY2(registry.find(name) != nullptr, name);
  }
  QVERIFY(!registry.find("maichat_list_contacts")->requiresApproval("{}"));
  QVERIFY(registry.find("maichat_send_text")->requiresApproval("{}"));
  QVERIFY(registry.find("maichat_reply_message")->requiresApproval("{}"));
  QVERIFY(registry.find("maichat_broadcast_text")->requiresApproval("{}"));
  QVERIFY(!registry.find("maichat_list_contacts")
               ->requiresPerCallApproval("{}"));
  QVERIFY(registry.find("maichat_send_text")
              ->requiresPerCallApproval("{}"));
  QVERIFY(registry.find("maichat_reply_message")
              ->requiresPerCallApproval("{}"));
  QVERIFY(registry.find("maichat_broadcast_text")
              ->requiresPerCallApproval("{}"));
}

void MaiChatHostToolsTest::readsContactsMessagesSearchAndUnreadState() {
  auto client = std::make_unique<FakeRemoteIMClient>();
  auto *fake = client.get();
  RemoteIMApplication app(QStringLiteral("owner"), std::move(client));
  app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
  app.addContact(QStringLiteral("bob"), QStringLiteral("Bob"));
  app.selectPeer(QStringLiteral("alice"));
  fake->emitIncomingText(QStringLiteral("alice"),
                         QStringLiteral("release is ready"));
  fake->emitIncomingText(QStringLiteral("bob"),
                         QStringLiteral("please review release"));

  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);
  MaiToolContext context;

  const auto contacts =
      registry.find("maichat_list_contacts")->execute("{}", context);
  QVERIFY(!contacts.hasError());
  QVERIFY(QString::fromStdString(contacts.output())
              .contains(QStringLiteral("alice")));
  QVERIFY(QString::fromStdString(contacts.output())
              .contains(QStringLiteral("Alice")));

  const auto messages =
      registry.find("maichat_get_messages")
          ->execute(R"({"peer_id":"alice","limit":10})", context);
  QVERIFY(!messages.hasError());
  QVERIFY(QString::fromStdString(messages.output())
              .contains(QStringLiteral("release is ready")));

  const auto search =
      registry.find("maichat_search_messages")
          ->execute(R"({"query":"release","limit":10})", context);
  QVERIFY(!search.hasError());
  const QString searchText = QString::fromStdString(search.output());
  QVERIFY(searchText.contains(QStringLiteral("alice")));
  QVERIFY(searchText.contains(QStringLiteral("bob")));

  const auto unread =
      registry.find("maichat_get_unread_summary")->execute("{}", context);
  QVERIFY(!unread.hasError());
  const QJsonObject unreadObject =
      QJsonDocument::fromJson(QByteArray::fromStdString(unread.output()))
          .object();
  QCOMPARE(unreadObject.value(QStringLiteral("total_unread")).toInt(), 1);
}

void MaiChatHostToolsTest::sendsAndRepliesOnlyThroughApprovalGatedTools() {
  auto client = std::make_unique<FakeRemoteIMClient>();
  auto *fake = client.get();
  RemoteIMApplication app(QStringLiteral("owner"), std::move(client));
  app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
  app.addContact(QStringLiteral("bob"), QStringLiteral("Bob"));
  fake->emitIncomingText(QStringLiteral("alice"),
                         QStringLiteral("Can you review this?"));
  const QString messageId =
      app.chatState().messagesWith(QStringLiteral("alice")).last().id;

  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);
  MaiToolContext context;

  MaiTool *send = registry.find("maichat_send_text");
  QVERIFY(send->requiresApproval(R"({"peer_id":"alice","text":"Sure"})"));
  const auto sent =
      send->execute(R"({"peer_id":"alice","text":"Sure"})", context);
  QVERIFY(!sent.hasError());
  QCOMPARE(fake->lastTextPeerId(), QStringLiteral("alice"));
  QCOMPARE(fake->lastText(), QStringLiteral("Sure"));

  const QByteArray replyArguments =
      QJsonDocument(
          QJsonObject{
              {QStringLiteral("peer_id"), QStringLiteral("alice")},
              {QStringLiteral("message_id"), messageId},
              {QStringLiteral("text"), QStringLiteral("I will review it")},
          })
          .toJson(QJsonDocument::Compact);
  MaiTool *reply = registry.find("maichat_reply_message");
  QVERIFY(reply->requiresApproval(replyArguments.toStdString()));
  const auto replied = reply->execute(replyArguments.toStdString(), context);
  QVERIFY(!replied.hasError());
  const QList<RemoteIMMessage> messages =
      app.chatState().messagesWith(QStringLiteral("alice"));
  QVERIFY(messages.last().hasQuote);
  QCOMPARE(messages.last().text, QStringLiteral("I will review it"));

  MaiTool *broadcast = registry.find("maichat_broadcast_text");
  QVERIFY(broadcast->requiresPerCallApproval(
      R"({"peer_ids":["alice","bob"],"text":"Release is ready"})"));
  const auto broadcasted = broadcast->execute(
      R"({"peer_ids":["alice","bob","alice"],"text":"Release is ready"})",
      context);
  QVERIFY(!broadcasted.hasError());
  const QJsonObject broadcastResult = QJsonDocument::fromJson(
      QByteArray::fromStdString(broadcasted.output())).object();
  QCOMPARE(broadcastResult.value(QStringLiteral("recipient_count")).toInt(), 2);
  QCOMPARE(app.chatState().messagesWith(QStringLiteral("alice")).last().text,
           QStringLiteral("Release is ready"));
  QCOMPARE(app.chatState().messagesWith(QStringLiteral("bob")).last().text,
           QStringLiteral("Release is ready"));
}

QTEST_MAIN(MaiChatHostToolsTest)
#include "MaiChatHostToolsTest.moc"
