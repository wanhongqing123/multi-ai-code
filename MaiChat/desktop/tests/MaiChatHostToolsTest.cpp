// 文件作用：验证桌面端Agent 读取 IM 数据和需要审批的发送工具。
// 运行边界：由测试进程驱动实际组件或替身，不作为正式 App 的用户入口。
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QTimer>
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
  void nicknameToolUsesTheAccountUpdater();
  void readsContactsMessagesSearchAndUnreadState();
  void sendsAndRepliesOnlyThroughApprovalGatedTools();
  void sendsAgentImageAsPersistentMediaMessage();
  void groupToolsUseTheSameApplicationFlowAndApprovalRules();
  void groupMessageToolWaitsForInitialHistoryInsteadOfReportingEmpty();
  void createGroupToolReturnsExistingPendingGroupWithoutDuplicating();
};

void MaiChatHostToolsTest::registersMaiChatReadAndWriteTools() {
  RemoteIMApplication app(QStringLiteral("owner"),
                          std::make_unique<FakeRemoteIMClient>());
  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);

  for (const char *name :
       {"maichat_list_contacts", "maichat_request_friend", "maichat_set_nickname",
        "maichat_list_conversations",
        "maichat_get_messages", "maichat_search_messages",
        "maichat_get_unread_summary", "maichat_send_text", "maichat_send_media",
        "maichat_reply_message", "maichat_broadcast_text", "maichat_list_groups",
        "maichat_create_group", "maichat_get_group_messages", "maichat_send_group_text"}) {
    QVERIFY2(registry.find(name) != nullptr, name);
  }
  QVERIFY(!registry.find("maichat_list_contacts")->requiresApproval("{}"));
  QVERIFY(registry.find("maichat_request_friend")->requiresApproval("{}"));
  QVERIFY(registry.find("maichat_set_nickname")->requiresApproval("{}"));
  QVERIFY(registry.find("maichat_set_nickname")->requiresPerCallApproval("{}"));
  QVERIFY(registry.find("maichat_send_text")->requiresApproval("{}"));
  QVERIFY(registry.find("maichat_send_media")->requiresApproval("{}"));
  QVERIFY(registry.find("maichat_reply_message")->requiresApproval("{}"));
  QVERIFY(registry.find("maichat_broadcast_text")->requiresApproval("{}"));
  QVERIFY(!registry.find("maichat_list_contacts")
               ->requiresPerCallApproval("{}"));
  QVERIFY(registry.find("maichat_send_text")
              ->requiresPerCallApproval("{}"));
  QVERIFY(registry.find("maichat_send_media")
              ->requiresPerCallApproval("{}"));
  QVERIFY(registry.find("maichat_reply_message")
              ->requiresPerCallApproval("{}"));
  QVERIFY(registry.find("maichat_broadcast_text")
              ->requiresPerCallApproval("{}"));
  QVERIFY(!registry.find("maichat_list_groups")->requiresApproval("{}"));
  QVERIFY(!registry.find("maichat_get_group_messages")->requiresApproval("{}"));
  QVERIFY(registry.find("maichat_create_group")->requiresPerCallApproval("{}"));
  QVERIFY(registry.find("maichat_send_group_text")->requiresPerCallApproval("{}"));
}

void MaiChatHostToolsTest::groupToolsUseTheSameApplicationFlowAndApprovalRules() {
  auto client = std::make_unique<FakeRemoteIMClient>();
  auto *fake = client.get();
  RemoteIMApplication app(QStringLiteral("owner"), std::move(client));
  app.connectToService(123456, QStringLiteral("sig"));
  app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);
  MaiToolContext context;

  const auto invalid = registry.find("maichat_create_group")->execute(
      R"({"name":"Team","member_ids":["owner"]})", context);
  QVERIFY(invalid.hasError());
  const auto created = registry.find("maichat_create_group")->execute(
      R"({"name":"Team","member_ids":["alice"]})", context);
  QVERIFY(!created.hasError());
  const QJsonObject createdJson = QJsonDocument::fromJson(
      QByteArray::fromStdString(created.output())).object();
  const QString groupId = createdJson.value(QStringLiteral("group_id")).toString();
  QVERIFY(!groupId.isEmpty());
  QCOMPARE(createdJson.value(QStringLiteral("member_count")).toInt(), 2);
  QVERIFY(createdJson.value(QStringLiteral("created")).toBool());
  QCOMPARE(app.groups().first().groupId, groupId);

  const auto listed = registry.find("maichat_list_groups")->execute(
      R"({"query":"Tea","limit":2})", context);
  QVERIFY(!listed.hasError());
  QCOMPARE(QJsonDocument::fromJson(QByteArray::fromStdString(listed.output()))
               .object().value(QStringLiteral("count")).toInt(), 1);
  const QByteArray sendArgs = QJsonDocument(QJsonObject{
      {QStringLiteral("group_id"), groupId}, {QStringLiteral("text"), QStringLiteral("hello")}
  }).toJson(QJsonDocument::Compact);
  const auto sent = registry.find("maichat_send_group_text")->execute(sendArgs.toStdString(), context);
  QVERIFY(!sent.hasError());
  const QJsonObject sentJson = QJsonDocument::fromJson(
      QByteArray::fromStdString(sent.output())).object();
  QVERIFY(sentJson.value(QStringLiteral("queued")).toBool());
  QVERIFY(sentJson.value(QStringLiteral("sent")).toBool());
  QVERIFY(!sentJson.value(QStringLiteral("message_id")).toString().isEmpty());
  QVERIFY(!sentJson.value(QStringLiteral("remote_id")).toString().isEmpty());
  QCOMPARE(fake->lastGroupId(), groupId);
  QCOMPARE(fake->lastGroupText(), QStringLiteral("hello"));
  fake->emitIncomingGroupText(groupId, QStringLiteral("alice"),
                              QStringLiteral("received"), QStringLiteral("remote-1#0"));
  const QByteArray getArgs = QJsonDocument(QJsonObject{
      {QStringLiteral("group_id"), groupId}, {QStringLiteral("limit"), 20}
  }).toJson(QJsonDocument::Compact);
  const auto messages = registry.find("maichat_get_group_messages")
                            ->execute(getArgs.toStdString(), context);
  QVERIFY(!messages.hasError());
  const auto rows = QJsonDocument::fromJson(QByteArray::fromStdString(messages.output()))
                        .object().value(QStringLiteral("messages")).toArray();
  QCOMPARE(rows.size(), 2);
  QCOMPARE(rows.last().toObject().value(QStringLiteral("sender_id")).toString(),
           QStringLiteral("alice"));
  QCOMPARE(rows.last().toObject().value(QStringLiteral("remote_id")).toString(),
           QStringLiteral("remote-1#0"));
  QCOMPARE(rows.last().toObject().value(QStringLiteral("direction")).toString(),
           QStringLiteral("incoming"));
}

void MaiChatHostToolsTest::groupMessageToolWaitsForInitialHistoryInsteadOfReportingEmpty() {
  auto client = std::make_unique<FakeRemoteIMClient>();
  auto* fake = client.get();
  RemoteIMApplication app(QStringLiteral("owner"), std::move(client));
  app.connectToService(123456, QStringLiteral("sig"));
  app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
  fake->deferNextGroupHistory();
  RemoteIMGroup created;
  app.createGroup(QStringLiteral("Team"), {QStringLiteral("alice")},
                  [&](bool ok, const QString&, const RemoteIMGroup& group) {
    QVERIFY(ok);
    created = group;
  });
  QTRY_VERIFY(!created.groupId.isEmpty());
  QVERIFY(!app.hasLoadedGroupHistory(created.groupId));
  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);
  const QByteArray args = QJsonDocument(QJsonObject{
      {QStringLiteral("group_id"), created.groupId}, {QStringLiteral("limit"), 20}
  }).toJson(QJsonDocument::Compact);
  QTimer::singleShot(50, [fake, groupId = created.groupId] {
    fake->finishDeferredGroupHistory({RemoteIMGroupMessage{
        QStringLiteral("remote-history#0"), groupId, QStringLiteral("alice"),
        QStringLiteral("already there"), 1700000000000LL, 1}});
  });
  MaiToolContext context;
  const auto result = registry.find("maichat_get_group_messages")
                          ->execute(args.toStdString(), context);
  QVERIFY(!result.hasError());
  const auto rows = QJsonDocument::fromJson(QByteArray::fromStdString(result.output()))
                        .object().value(QStringLiteral("messages")).toArray();
  QCOMPARE(rows.size(), 1);
  QCOMPARE(rows.first().toObject().value(QStringLiteral("text")).toString(),
           QStringLiteral("already there"));
}

void MaiChatHostToolsTest::createGroupToolReturnsExistingPendingGroupWithoutDuplicating() {
  auto client = std::make_unique<FakeRemoteIMClient>();
  auto* fake = client.get();
  RemoteIMApplication app(QStringLiteral("owner"), std::move(client));
  app.connectToService(123456, QStringLiteral("sig"));
  emit fake->groupsReceived({RemoteIMGroup{QStringLiteral("existing-group"),
                                            QStringLiteral("Existing"), 2, true}});
  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);
  MaiToolContext context;
  const auto result = registry.find("maichat_create_group")->execute(
      R"({"name":"Another","member_ids":["friend"]})", context);
  QVERIFY(!result.hasError());
  const auto output = QJsonDocument::fromJson(QByteArray::fromStdString(result.output())).object();
  QCOMPARE(output.value(QStringLiteral("created")).toBool(), false);
  QCOMPARE(output.value(QStringLiteral("pending_activation")).toBool(), true);
  QCOMPARE(output.value(QStringLiteral("group_id")).toString(), QStringLiteral("existing-group"));
  QCOMPARE(app.groups().size(), 1);
}

void MaiChatHostToolsTest::nicknameToolUsesTheAccountUpdater() {
  RemoteIMApplication app(QStringLiteral("owner"),
                          std::make_unique<FakeRemoteIMClient>());
  QString updated;
  app.setNicknameUpdater([&updated](const QString& nickname, RemoteIMCompletion completion) {
    updated = nickname;
    if (completion) completion(true, {});
  });
  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);
  MaiToolContext context;
  const auto result = registry.find("maichat_set_nickname")
                          ->execute(R"({"nickname":" 小王 "})", context);
  QVERIFY(!result.hasError());
  QCOMPARE(updated, QStringLiteral("小王"));
  QVERIFY(QString::fromStdString(result.output()).contains(QStringLiteral("queued")));
  QVERIFY(registry.find("maichat_set_nickname")->execute("{}", context).hasError());
}

void MaiChatHostToolsTest::sendsAgentImageAsPersistentMediaMessage() {
  QTemporaryDir workspace;
  QVERIFY(workspace.isValid());
  const QString source = workspace.filePath(QStringLiteral("picture.png"));
  QImage image(2, 2, QImage::Format_ARGB32);
  image.fill(Qt::red);
  QVERIFY(image.save(source));

  auto client = std::make_unique<FakeRemoteIMClient>();
  RemoteIMApplication app(QStringLiteral("owner"), std::move(client));
  app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);
  MaiToolContext context;
  context.root = workspace.path().toUtf8().toStdString();
  const auto sent = registry.find("maichat_send_media")->execute(
      R"({"peer_id":"alice","file_path":"picture.png","type":"image","caption":"Processed image"})",
      context);
  QVERIFY2(!sent.hasError(), sent.error().message().c_str());
  const auto sentBody = QJsonDocument::fromJson(QByteArray::fromStdString(sent.output())).object();
  QCOMPARE(sentBody.value(QStringLiteral("caption_attached")).toBool(), true);
  const auto messages = app.chatState().messagesWith(QStringLiteral("alice"));
  QCOMPARE(messages.size(), 1);
  QVERIFY(messages.last().hasImage);
  QCOMPARE(messages.last().text, QStringLiteral("Processed image"));
  QVERIFY(QFileInfo(messages.last().image.localPath).isFile());
  QVERIFY(messages.last().image.localPath != source);

  const auto denied = registry.find("maichat_send_media")->execute(
      R"({"peer_id":"alice","file_path":"../picture.png","type":"image"})",
      context);
  QVERIFY(denied.hasError());
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

// 发送和回复工具都必须经过审批门槛；测试同时确认只读工具不会意外获得写入能力。
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
