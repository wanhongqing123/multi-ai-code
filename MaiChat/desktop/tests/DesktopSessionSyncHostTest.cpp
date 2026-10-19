// 文件作用：验证远端操作只落到所属桌面 Agent，会话快照不泄漏本机媒体路径。
#include <QJsonArray>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QPushButton>
#include <QQueue>
#include <QSignalSpy>
#include <QTextEdit>
#include <QTest>
#include <QTimer>

#include "agent/AgentController.h"
#include "agent/AgentSessionList.h"
#include "agent/DesktopSessionSyncCoordinator.h"
#include "agent/DesktopSessionSyncHost.h"
#include "agent/RemoteAgentPanel.h"
#include "markdown/MarkdownView.h"

class DesktopSessionSyncHostTest : public QObject {
    Q_OBJECT

private slots:
    void appliesRemotePromptToTheExistingLocalSession();
    void omitsLocalMediaPathsFromRemoteHistory();
    void coordinatorRoutesPhoneCommandToTheExistingDesktopSession();
    void coordinatorReadsRemoteHistoryWithVerifiedChunks();
    void remotePanelShowsHistoryAndRoutesTextToOwner();
    void sessionListKeepsLocalAndRemoteSelectionsSeparate();
};

void DesktopSessionSyncHostTest::appliesRemotePromptToTheExistingLocalSession() {
    AgentController::ModelConfig model;
    AgentController controller(model, {});
    const QString sessionId = controller.createSession({}, QStringLiteral("编译项目"));
    QVERIFY(!sessionId.isEmpty());
    DesktopSessionSyncHost host(controller);
    const QJsonArray sessions = host.rootSessions();
    QCOMPARE(sessions.size(), 1);
    QCOMPARE(sessions.first().toObject().value(QStringLiteral("id")).toString(), sessionId);
    QCOMPARE(sessions.first().toObject().value(QStringLiteral("title")).toString(), QStringLiteral("编译项目"));
    QVERIFY(!host.applyCommand(sessionId, {{QStringLiteral("action"), QStringLiteral("unknown")}})
                 .value(QStringLiteral("ok"))
                 .toBool());
    QCOMPARE(host.applyCommand(sessionId, {{QStringLiteral("action"), QStringLiteral("permission")},
                                           {QStringLiteral("id"), QStringLiteral("per_test")},
                                           {QStringLiteral("decision"), QStringLiteral("grant_everything")}})
                 .value(QStringLiteral("error"))
                 .toString(),
             QStringLiteral("权限决定无效"));
    QVERIFY(host.applyCommand(sessionId, {{QStringLiteral("action"), QStringLiteral("prompt")},
                                          {QStringLiteral("text"), QStringLiteral("继续检查编译错误")}})
                .value(QStringLiteral("ok"))
                .toBool());
    const QJsonObject page = host.historyPage(sessionId, {}, 20);
    QVERIFY(page.value(QStringLiteral("ok")).toBool());
    QVERIFY(page.value(QStringLiteral("permissions")).isArray());
    QVERIFY(page.value(QStringLiteral("questions")).isArray());
    const QJsonArray messages = page.value(QStringLiteral("messages")).toArray();
    QVERIFY(!messages.isEmpty());
    bool sawPrompt = false;
    for (const QJsonValue& value : messages) {
        const QJsonObject message = value.toObject();
        if (message.value(QStringLiteral("role")).toString() != QStringLiteral("user")) continue;
        for (const QJsonValue& part : message.value(QStringLiteral("parts")).toArray())
            sawPrompt |= part.toObject().value(QStringLiteral("text")).toString() == QStringLiteral("继续检查编译错误");
    }
    QVERIFY(sawPrompt);
    QCOMPARE(host.historyPage(QStringLiteral("ses_missing"), {}, 20).value(QStringLiteral("ok")).toBool(), false);
}

void DesktopSessionSyncHostTest::omitsLocalMediaPathsFromRemoteHistory() {
    AgentController::ModelConfig model;
    AgentController controller(model, {});
    const QString sessionId = controller.createSession({}, QStringLiteral("照片"));
    QVERIFY(!sessionId.isEmpty());
    const std::vector<MaiModelImage> images{{"/private/workspace/secret-photo.png", "image/png"}};
    const auto sent = controller.agent().submit(MaiSendPrompt{sessionId.toStdString(), "Describe this", images});
    QVERIFY(sent);
    DesktopSessionSyncHost host(controller);
    const QJsonArray messages = host.historyPage(sessionId, {}, 20).value(QStringLiteral("messages")).toArray();
    bool sawImage = false;
    for (const QJsonValue& message : messages) {
        for (const QJsonValue& part : message.toObject().value(QStringLiteral("parts")).toArray()) {
            const QJsonObject image = part.toObject();
            if (image.value(QStringLiteral("kind")).toString() != QStringLiteral("image")) continue;
            sawImage = true;
            QCOMPARE(image.value(QStringLiteral("filename")).toString(), QStringLiteral("secret-photo.png"));
            QVERIFY(!image.contains(QStringLiteral("path")));
            QVERIFY(!image.value(QStringLiteral("remotePartId")).toString().isEmpty());
        }
    }
    QVERIFY(sawImage);
}

void DesktopSessionSyncHostTest::coordinatorRoutesPhoneCommandToTheExistingDesktopSession() {
    AgentController::ModelConfig model;
    AgentController controller(model, {});
    const QString sessionId = controller.createSession({}, QStringLiteral("桌面任务"));
    QVERIFY(!sessionId.isEmpty());
    int polls = 0;
    int acknowledgements = 0;
    int presenceUpdates = 0;
    QList<QJsonObject> sent;
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject& body,
            DesktopSessionSyncCoordinator::Completion completion) {
            QJsonObject response;
            if (path == QStringLiteral("session-sync/presence")) {
                ++presenceUpdates;
                QCOMPARE(body.value(QStringLiteral("sessions")).toArray().size(), 1);
                response = {{QStringLiteral("online"), true}};
            } else if (path.startsWith(QStringLiteral("session-sync/poll"))) {
                QJsonArray messages;
                if (polls == 0) {
                    messages.append(
                        QJsonObject{{QStringLiteral("id"), 1},
                                    {QStringLiteral("kind"), QStringLiteral("command")},
                                    {QStringLiteral("session_id"), sessionId},
                                    {QStringLiteral("from_slot"), QStringLiteral("ios_phone")},
                                    {QStringLiteral("from_login_session_id"), QStringLiteral("phone-login")},
                                    {QStringLiteral("client_message_id"), QStringLiteral("command-1")},
                                    {QStringLiteral("payload"),
                                     QJsonObject{{QStringLiteral("action"), QStringLiteral("prompt")},
                                                 {QStringLiteral("text"), QStringLiteral("检查编译错误")}}}});
                } else if (polls == 1) {
                    messages.append(
                        QJsonObject{{QStringLiteral("id"), 2},
                                    {QStringLiteral("kind"), QStringLiteral("history_request")},
                                    {QStringLiteral("session_id"), sessionId},
                                    {QStringLiteral("from_slot"), QStringLiteral("ios_phone")},
                                    {QStringLiteral("from_login_session_id"), QStringLiteral("phone-login")},
                                    {QStringLiteral("payload"), QJsonObject{{QStringLiteral("limit"), 10}}}});
                }
                ++polls;
                response = {{QStringLiteral("messages"), messages}};
            } else if (path == QStringLiteral("session-sync/send")) {
                sent.append(body);
                response = {{QStringLiteral("id"), sent.size()}};
            } else if (path == QStringLiteral("session-sync/ack")) {
                ++acknowledgements;
                response = {{QStringLiteral("acknowledged"), true}};
            }
            QTimer::singleShot(0, [completion = std::move(completion), response] { completion(response, {}); });
        },
        [&] { return &controller; }, nullptr, QStringLiteral("desktop-test-install-id"), QStringLiteral("Test Mac"));
    coordinator.start();
    QTRY_VERIFY_WITH_TIMEOUT(presenceUpdates > 0 && acknowledgements >= 2 && sent.size() >= 2, 5000);
    bool sawResult = false;
    bool sawHistory = false;
    for (const QJsonObject& message : sent) {
        const QString kind = message.value(QStringLiteral("kind")).toString();
        const QJsonObject chunk = message.value(QStringLiteral("payload")).toObject();
        const QJsonDocument document = QJsonDocument::fromJson(
            QByteArray::fromBase64(chunk.value(QStringLiteral("data_base64")).toString().toLatin1()));
        if (kind == QStringLiteral("event") &&
            document.object().value(QStringLiteral("type")).toString() == QStringLiteral("command_result"))
            sawResult = true;
        if (kind == QStringLiteral("history_page")) {
            const QJsonArray messages = document.object().value(QStringLiteral("messages")).toArray();
            for (const QJsonValue& value : messages)
                for (const QJsonValue& part : value.toObject().value(QStringLiteral("parts")).toArray())
                    sawHistory |=
                        part.toObject().value(QStringLiteral("text")).toString() == QStringLiteral("检查编译错误");
        }
    }
    QVERIFY(sawResult);
    QVERIFY(sawHistory);
}

void DesktopSessionSyncHostTest::coordinatorReadsRemoteHistoryWithVerifiedChunks() {
    AgentController::ModelConfig model;
    AgentController controller(model, {});
    QVERIFY(!controller.createSession({}, QStringLiteral("本地任务")).isEmpty());
    const QJsonObject remote{{QStringLiteral("id"), QStringLiteral("ses_phone")},
                             {QStringLiteral("title"), QStringLiteral("手机任务")},
                             {QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                             {QStringLiteral("device_name"), QStringLiteral("iPhone")},
                             {QStringLiteral("device_id"), QStringLiteral("phone-install")},
                             {QStringLiteral("login_session_id"), QStringLiteral("phone-login")},
                             {QStringLiteral("online"), true}};
    QQueue<QJsonObject> queued;
    QJsonArray catalog;
    QJsonObject received;
    QJsonObject sentCommand;
    int acknowledgements = 0;
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject& body,
            DesktopSessionSyncCoordinator::Completion completion) {
            QJsonObject response;
            if (path == QStringLiteral("session-sync/sessions")) {
                response = {{QStringLiteral("sessions"), QJsonArray{remote}}};
            } else if (path.startsWith(QStringLiteral("session-sync/poll"))) {
                QJsonArray messages;
                if (!queued.isEmpty()) messages.append(queued.dequeue());
                response = {{QStringLiteral("messages"), messages}};
            } else if (path == QStringLiteral("session-sync/send")) {
                if (body.value(QStringLiteral("kind")).toString() == QStringLiteral("command")) {
                    sentCommand = body;
                } else {
                    QCOMPARE(body.value(QStringLiteral("kind")).toString(), QStringLiteral("history_request"));
                    const QJsonObject page{{QStringLiteral("ok"), true},
                                           {QStringLiteral("messages"), QJsonArray{QJsonObject{
                                                {QStringLiteral("id"), QStringLiteral("msg_remote")}}}}};
                    const QByteArray bytes = QJsonDocument(page).toJson(QJsonDocument::Compact);
                    const QString digest = QString::fromLatin1(QCryptographicHash::hash(
                        bytes, QCryptographicHash::Sha256).toHex());
                    queued.enqueue({{QStringLiteral("id"), 2},
                                    {QStringLiteral("kind"), QStringLiteral("history_page")},
                                    {QStringLiteral("session_id"), QStringLiteral("ses_phone")},
                                    {QStringLiteral("from_slot"), QStringLiteral("ios_phone")},
                                    {QStringLiteral("from_login_session_id"), QStringLiteral("phone-login")},
                                    {QStringLiteral("payload"), QJsonObject{
                                        {QStringLiteral("transfer_id"), QStringLiteral("transfer-1")},
                                        {QStringLiteral("chunk_index"), 0},
                                        {QStringLiteral("chunk_count"), 1},
                                        {QStringLiteral("sha256"), digest},
                                        {QStringLiteral("data_base64"), QString::fromLatin1(bytes.toBase64())}}}});
                }
                response = {{QStringLiteral("id"), 1}};
            } else if (path == QStringLiteral("session-sync/ack")) {
                ++acknowledgements;
                response = {{QStringLiteral("acknowledged"), true}};
            }
            QTimer::singleShot(0, [completion = std::move(completion), response] { completion(response, {}); });
        },
        [&] { return &controller; }, nullptr, QStringLiteral("mac-install"), QStringLiteral("Mac"));
    coordinator.setCatalogHandler([&](const QJsonArray& value) { catalog = value; });
    coordinator.setPayloadHandler([&](const QJsonObject&, const QString& kind, const QJsonObject& payload) {
        QCOMPARE(kind, QStringLiteral("history_page"));
        received = payload;
    });
    coordinator.start();
    QTRY_COMPARE_WITH_TIMEOUT(catalog.size(), 1, 5000);
    QString error;
    coordinator.requestRemoteHistory(catalog.first().toObject(), {}, 10,
                                     [&](const QJsonObject&, const QString& value) { error = value; });
    QTRY_VERIFY_WITH_TIMEOUT(received.value(QStringLiteral("ok")).toBool() && acknowledgements > 0, 5000);
    QVERIFY(error.isEmpty());
    QCOMPARE(received.value(QStringLiteral("messages")).toArray().first().toObject()
                 .value(QStringLiteral("id")).toString(), QStringLiteral("msg_remote"));
    coordinator.sendRemoteCommand(catalog.first().toObject(),
                                  {{QStringLiteral("action"), QStringLiteral("prompt")},
                                   {QStringLiteral("text"), QStringLiteral("继续")}},
                                  [&](const QJsonObject&, const QString& value) { error = value; });
    QTRY_VERIFY_WITH_TIMEOUT(!sentCommand.isEmpty(), 5000);
    QVERIFY(error.isEmpty());
    QCOMPARE(sentCommand.value(QStringLiteral("to_slot")).toString(), QStringLiteral("ios_phone"));
    QCOMPARE(sentCommand.value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("text")).toString(), QStringLiteral("继续"));
    const QString firstCommandId = sentCommand.value(QStringLiteral("client_message_id")).toString();
    coordinator.sendRemoteCommand(catalog.first().toObject(),
                                  {{QStringLiteral("action"), QStringLiteral("prompt")},
                                   {QStringLiteral("text"), QStringLiteral("继续")}},
                                  [&](const QJsonObject&, const QString& value) { error = value; });
    QCOMPARE(sentCommand.value(QStringLiteral("client_message_id")).toString(), firstCommandId);
}

void DesktopSessionSyncHostTest::remotePanelShowsHistoryAndRoutesTextToOwner() {
    RemoteAgentPanel panel;
    panel.resize(900, 680);
    const QJsonObject remote{{QStringLiteral("id"), QStringLiteral("ses_remote")},
                             {QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                             {QStringLiteral("device_name"), QStringLiteral("测试 iPhone")},
                             {QStringLiteral("login_session_id"), QStringLiteral("login_phone")},
                             {QStringLiteral("online"), true},
                             {QStringLiteral("busy"), false}};
    QJsonObject sent;
    panel.setHistoryHandler([&](const QJsonObject& session, const QString&, int,
                                RemoteAgentPanel::Completion completion) {
        QCOMPARE(session.value(QStringLiteral("id")).toString(), QStringLiteral("ses_remote"));
        const QJsonObject part{{QStringLiteral("kind"), QStringLiteral("text")},
                               {QStringLiteral("text"), QStringLiteral("所属手机的历史")}};
        const QJsonObject message{{QStringLiteral("id"), QStringLiteral("msg_1")},
                                  {QStringLiteral("role"), QStringLiteral("assistant")},
                                  {QStringLiteral("parts"), QJsonArray{part}}};
        panel.receivePayload(remote, QStringLiteral("history_page"),
                             {{QStringLiteral("ok"), true},
                              {QStringLiteral("messages"), QJsonArray{message}}});
        completion({});
    });
    panel.setCommandHandler([&](const QJsonObject& session, const QJsonObject& action,
                                RemoteAgentPanel::Completion completion) {
        QCOMPARE(session.value(QStringLiteral("id")).toString(), QStringLiteral("ses_remote"));
        sent = action;
        completion({});
    });
    panel.show();
    panel.openSession(remote);
    auto* transcript = panel.findChild<MarkdownView*>();
    QVERIFY(transcript != nullptr);
    QVERIFY(transcript->contains(QStringLiteral("msg_1")));
    auto* composer = panel.findChild<QTextEdit*>(QStringLiteral("remoteAgentComposer"));
    QVERIFY(composer != nullptr);
    composer->setPlainText(QStringLiteral("继续本机任务"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("remoteAgentSendButton"));
    QVERIFY(send != nullptr && send->isEnabled());
    QTest::mouseClick(send, Qt::LeftButton);
    QCOMPARE(sent.value(QStringLiteral("action")).toString(), QStringLiteral("prompt"));
    QCOMPARE(sent.value(QStringLiteral("text")).toString(), QStringLiteral("继续本机任务"));
    QCOMPARE(composer->toPlainText(), QString());
    panel.receivePayload(remote, QStringLiteral("event"),
                         {{QStringLiteral("type"), QStringLiteral("command_result")},
                          {QStringLiteral("result"), QJsonObject{{QStringLiteral("ok"), true}}}});
    composer->setPlainText(QStringLiteral("请保留草稿"));
    QTest::mouseClick(send, Qt::LeftButton);
    QCOMPARE(composer->toPlainText(), QString());
    panel.receivePayload(remote, QStringLiteral("event"),
                         {{QStringLiteral("type"), QStringLiteral("command_result")},
                          {QStringLiteral("result"), QJsonObject{{QStringLiteral("ok"), false},
                                                                   {QStringLiteral("error"),
                                                                    QStringLiteral("所属设备拒绝")}}}});
    QCOMPARE(composer->toPlainText(), QStringLiteral("请保留草稿"));
    panel.setCommandHandler([&](const QJsonObject&, const QJsonObject&,
                                RemoteAgentPanel::Completion completion) {
        panel.receivePayload(remote, QStringLiteral("event"),
                             {{QStringLiteral("type"), QStringLiteral("command_result")},
                              {QStringLiteral("result"), QJsonObject{{QStringLiteral("ok"), false},
                                                                       {QStringLiteral("error"),
                                                                        QStringLiteral("立即拒绝")}}}});
        completion({});
    });
    composer->setPlainText(QStringLiteral("快速拒绝也保留"));
    QTest::mouseClick(send, Qt::LeftButton);
    QCOMPARE(composer->toPlainText(), QStringLiteral("快速拒绝也保留"));
    if (qEnvironmentVariableIsSet("MAICHAT_REMOTE_PANEL_SCREENSHOT"))
        QVERIFY(panel.grab().save(qEnvironmentVariable("MAICHAT_REMOTE_PANEL_SCREENSHOT")));
}

void DesktopSessionSyncHostTest::sessionListKeepsLocalAndRemoteSelectionsSeparate() {
    AgentController::ModelConfig model;
    AgentController controller(model, {});
    const QString local = controller.createSession({}, QStringLiteral("本地会话"));
    QVERIFY(!local.isEmpty());
    AgentSessionList sessions(controller);
    sessions.resize(300, 500);
    sessions.setRemoteSessions(QJsonArray{QJsonObject{
        {QStringLiteral("id"), QStringLiteral("ses_remote")},
        {QStringLiteral("title"), QStringLiteral("远端会话")},
        {QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
        {QStringLiteral("device_name"), QStringLiteral("iPhone")},
        {QStringLiteral("device_id"), QStringLiteral("phone-install")},
        {QStringLiteral("login_session_id"), QStringLiteral("phone-login")},
        {QStringLiteral("online"), true}}});
    sessions.show();
    auto* list = sessions.findChild<QListWidget*>();
    QVERIFY(list != nullptr);
    QCOMPARE(list->count(), 2);
    QSignalSpy localSelection(&sessions, &AgentSessionList::selected);
    QSignalSpy remoteSelection(&sessions, &AgentSessionList::remoteSelected);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(1)).center());
    QCOMPARE(remoteSelection.size(), 1);
    QCOMPARE(localSelection.size(), 0);
    QCOMPARE(remoteSelection.takeFirst().first().toJsonObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("ses_remote"));
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(0)).center());
    QCOMPARE(localSelection.size(), 1);
    QCOMPARE(localSelection.takeFirst().first().toString(), local);
}

QTEST_MAIN(DesktopSessionSyncHostTest)
#include "DesktopSessionSyncHostTest.moc"
