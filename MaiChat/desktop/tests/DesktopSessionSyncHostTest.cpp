// 文件作用：验证远端操作只落到所属桌面 Agent，会话快照不泄漏本机媒体路径。
#include <QJsonArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QQueue>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTest>
#include <QTimer>
#include <QtConcurrent>

#include "MaiTool.h"
#include "MaiChatTools/DesktopRemoteSessionTools.h"
#include "agent/AgentController.h"
#include "agent/AgentSessionList.h"
#include "agent/DesktopSessionSyncCoordinator.h"
#include "agent/DesktopSessionSyncHost.h"
#include "agent/RemoteAgentPanel.h"
#include "markdown/MarkdownView.h"
#include "ui/ComposerTextEdit.h"
#include "ui/UiZoom.h"

class DesktopSessionSyncHostTest : public QObject {
    Q_OBJECT

private slots:
    void appliesRemotePromptToTheExistingLocalSession();
    void omitsLocalMediaPathsFromRemoteHistory();
    void coordinatorRoutesPhoneCommandToTheExistingDesktopSession();
    void coordinatorPublishesLocalMessagesToSharedRoom();
    void coordinatorBackfillsOlderMessagesIntoSharedRoom();
    void coordinatorDeletesRoomWhenLocalSessionIsRemoved();
    void coordinatorReadsRemoteHistoryWithVerifiedChunks();
    void remotePanelShowsHistoryAndRoutesTextToOwner();
    void remotePanelDoesNotRepeatPendingHistory();
    void remotePanelReconcilesAfterReconnect();
    void remotePanelKeepsOwnPromptWhenRoomBroadcastsCanonicalMessage();
    void remotePanelPushCanArriveBeforeInitialPage();
    void sessionListKeepsLocalAndRemoteSelectionsSeparate();
    void sessionListSeparatesDevicesWithTheSameModel();
    void remoteLifecycleCreatesAndDeletesInRequestedWorkspace();
    void coordinatorCreatesFirstRemoteSessionAndMatchesLifecycleResult();
    void coordinatorAppliesPhoneLifecycleToDesktop();
    void sessionListOffersLifecycleActionsForRemoteDevice();
    void agentToolsReuseRemoteLifecycleCoordinator();
    void remotePermissionButtonsUseSharedWireDecisions();
};

void DesktopSessionSyncHostTest::remoteLifecycleCreatesAndDeletesInRequestedWorkspace() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    AgentController controller(AgentController::ModelConfig{}, {});
    DesktopSessionSyncHost host(controller);
    const QJsonObject created = host.applyCommand(
        QStringLiteral("session_lifecycle"),
        {{QStringLiteral("action"), QStringLiteral("session_create")},
         {QStringLiteral("workspace_path"), workspace.path()}});
    QVERIFY2(created.value(QStringLiteral("ok")).toBool(),
             qPrintable(created.value(QStringLiteral("error")).toString()));
    const QString id = created.value(QStringLiteral("session_id")).toString();
    QVERIFY(!id.isEmpty());
    MaiSession session;
    QVERIFY(controller.agent().getSession(id.toStdString(), session));
    QCOMPARE(QString::fromStdString(session.directory), QDir(workspace.path()).canonicalPath());
    const QJsonObject removed = host.applyCommand(
        QStringLiteral("session_lifecycle"),
        {{QStringLiteral("action"), QStringLiteral("session_delete")},
         {QStringLiteral("target_session_id"), id}});
    QVERIFY2(removed.value(QStringLiteral("ok")).toBool(),
             qPrintable(removed.value(QStringLiteral("error")).toString()));
    QVERIFY(!controller.agent().getSession(id.toStdString(), session));
    const QJsonObject invalid = host.applyCommand(
        QStringLiteral("session_lifecycle"),
        {{QStringLiteral("action"), QStringLiteral("session_create")},
         {QStringLiteral("workspace_path"), QStringLiteral("relative/path")}});
    QVERIFY(!invalid.value(QStringLiteral("ok")).toBool());
    QTemporaryDir defaultWorkspace;
    QVERIFY(defaultWorkspace.isValid());
    host.setDefaultWorkspaceDirectory(defaultWorkspace.path());
    const QJsonObject defaultCreated = host.applyCommand(
        QStringLiteral("session_lifecycle"),
        {{QStringLiteral("action"), QStringLiteral("session_create")}});
    QVERIFY(defaultCreated.value(QStringLiteral("ok")).toBool());
    const QString defaultId = defaultCreated.value(QStringLiteral("session_id")).toString();
    QVERIFY(controller.agent().getSession(defaultId.toStdString(), session));
    QCOMPARE(QString::fromStdString(session.directory), QDir(defaultWorkspace.path()).canonicalPath());
    QTemporaryDir otherWorkspace;
    QVERIFY(otherWorkspace.isValid());
    const QJsonObject switched = host.applyCommand(
        QStringLiteral("session_lifecycle"),
        {{QStringLiteral("action"), QStringLiteral("session_set_workspace")},
         {QStringLiteral("target_session_id"), defaultId},
         {QStringLiteral("workspace_path"), otherWorkspace.path()}});
    QVERIFY2(switched.value(QStringLiteral("ok")).toBool(),
             qPrintable(switched.value(QStringLiteral("error")).toString()));
    QCOMPARE(switched.value(QStringLiteral("session_id")).toString(), defaultId);
    QVERIFY(controller.agent().getSession(defaultId.toStdString(), session));
    QCOMPARE(QString::fromStdString(session.directory), QDir(otherWorkspace.path()).canonicalPath());
    QCOMPARE(controller.agent().listSessions().size(), size_t(1));
    const QJsonObject badWorkspace = host.applyCommand(
        QStringLiteral("session_lifecycle"),
        {{QStringLiteral("action"), QStringLiteral("session_set_workspace")},
         {QStringLiteral("target_session_id"), defaultId},
         {QStringLiteral("workspace_path"), QStringLiteral("relative/path")}});
    QVERIFY(!badWorkspace.value(QStringLiteral("ok")).toBool());
}

void DesktopSessionSyncHostTest::coordinatorCreatesFirstRemoteSessionAndMatchesLifecycleResult() {
    AgentController controller(AgentController::ModelConfig{}, {});
    const QJsonObject device{{QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                             {QStringLiteral("device_id"), QStringLiteral("phone-install")},
                             {QStringLiteral("login_session_id"), QStringLiteral("phone-login")},
                             {QStringLiteral("online"), true}};
    QJsonObject sent;
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject& body,
            DesktopSessionSyncCoordinator::Completion completion) {
            if (path == QStringLiteral("session-sync/send") &&
                body.value(QStringLiteral("kind")) == QStringLiteral("command"))
                sent = body;
            QTimer::singleShot(0, [completion = std::move(completion)] { completion({}, {}); });
        },
        [&] { return &controller; }, nullptr, QStringLiteral("mac-install"), QStringLiteral("Mac"));
    coordinator.start();
    coordinator.acceptDevices(QJsonArray{device});
    QJsonObject completed;
    QString error;
    coordinator.sendRemoteLifecycle(
        device, {{QStringLiteral("action"), QStringLiteral("session_create")}},
        [&](const QJsonObject& value, const QString& failure) { completed = value; error = failure; });
    QCOMPARE(sent.value(QStringLiteral("session_id")).toString(), QStringLiteral("session_lifecycle"));
    QCOMPARE(sent.value(QStringLiteral("to_slot")).toString(), QStringLiteral("ios_phone"));
    QVERIFY(!sent.value(QStringLiteral("payload")).toObject().contains(QStringLiteral("workspace_path")));
    const QString commandId = sent.value(QStringLiteral("client_message_id")).toString();
    QVERIFY(!commandId.isEmpty());
    const QJsonObject result{{QStringLiteral("type"), QStringLiteral("command_result")},
                             {QStringLiteral("command_id"), commandId},
                             {QStringLiteral("result"), QJsonObject{
                                 {QStringLiteral("ok"), true},
                                 {QStringLiteral("session_id"), QStringLiteral("ses_new")}}}};
    const QJsonObject envelope{{QStringLiteral("id"), 7},
                               {QStringLiteral("kind"), QStringLiteral("event")},
                               {QStringLiteral("session_id"), QStringLiteral("session_lifecycle")},
                               {QStringLiteral("from_slot"), QStringLiteral("ios_phone")},
                               {QStringLiteral("from_login_session_id"), QStringLiteral("phone-login")},
                               {QStringLiteral("payload"), result}};
    coordinator.acceptInbox(QJsonArray{envelope});
    QVERIFY(error.isEmpty());
    QCOMPARE(completed.value(QStringLiteral("session_id")).toString(), QStringLiteral("ses_new"));
}

void DesktopSessionSyncHostTest::coordinatorAppliesPhoneLifecycleToDesktop() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    AgentController controller(AgentController::ModelConfig{}, {});
    QList<QJsonObject> sent;
    int latestPresenceCount = -1;
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject& body,
            DesktopSessionSyncCoordinator::Completion completion) {
            if (path == QStringLiteral("session-sync/send")) sent.append(body);
            if (path == QStringLiteral("session-sync/presence"))
                latestPresenceCount = body.value(QStringLiteral("sessions")).toArray().size();
            QTimer::singleShot(0, [completion = std::move(completion)] { completion({}, {}); });
        },
        [&] { return &controller; }, nullptr, QStringLiteral("mac-install"), QStringLiteral("Mac"));
    coordinator.setDefaultWorkspaceDirectory(workspace.path());
    coordinator.start();
    coordinator.connectionReady();
    coordinator.acceptInbox(QJsonArray{QJsonObject{
        {QStringLiteral("id"), 1},
        {QStringLiteral("kind"), QStringLiteral("command")},
        {QStringLiteral("session_id"), QStringLiteral("session_lifecycle")},
        {QStringLiteral("from_slot"), QStringLiteral("ios_phone")},
        {QStringLiteral("from_login_session_id"), QStringLiteral("phone-login")},
        {QStringLiteral("client_message_id"), QStringLiteral("create-1")},
        {QStringLiteral("payload"), QJsonObject{{QStringLiteral("action"),
                                                  QStringLiteral("session_create")}}}}});
    QTRY_VERIFY_WITH_TIMEOUT(latestPresenceCount == 1 && !sent.isEmpty(), 5000);
    QCOMPARE(sent.first().value(QStringLiteral("session_id")).toString(),
             QStringLiteral("session_lifecycle"));
    const QJsonObject created = sent.first().value(QStringLiteral("payload")).toObject();
    QCOMPARE(created.value(QStringLiteral("command_id")).toString(), QStringLiteral("create-1"));
    const QString id = created.value(QStringLiteral("result")).toObject()
        .value(QStringLiteral("session_id")).toString();
    QVERIFY(!id.isEmpty());
    MaiSession session;
    QVERIFY(controller.agent().getSession(id.toStdString(), session));
    QCOMPARE(QString::fromStdString(session.directory), QDir(workspace.path()).canonicalPath());
    QTemporaryDir otherWorkspace;
    QVERIFY(otherWorkspace.isValid());
    coordinator.acceptInbox(QJsonArray{QJsonObject{
        {QStringLiteral("id"), 3},
        {QStringLiteral("kind"), QStringLiteral("command")},
        {QStringLiteral("session_id"), QStringLiteral("session_lifecycle")},
        {QStringLiteral("from_slot"), QStringLiteral("ios_phone")},
        {QStringLiteral("from_login_session_id"), QStringLiteral("phone-login")},
        {QStringLiteral("client_message_id"), QStringLiteral("workspace-1")},
        {QStringLiteral("payload"), QJsonObject{{QStringLiteral("action"),
                                                  QStringLiteral("session_set_workspace")},
                                                 {QStringLiteral("target_session_id"), id},
                                                 {QStringLiteral("workspace_path"),
                                                  otherWorkspace.path()}}}}});
    QTRY_VERIFY_WITH_TIMEOUT(sent.size() >= 2, 5000);
    QVERIFY(controller.agent().getSession(id.toStdString(), session));
    QCOMPARE(QString::fromStdString(session.directory), QDir(otherWorkspace.path()).canonicalPath());
    QCOMPARE(sent.last().value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("command_id")).toString(), QStringLiteral("workspace-1"));
    coordinator.acceptInbox(QJsonArray{QJsonObject{
        {QStringLiteral("id"), 2},
        {QStringLiteral("kind"), QStringLiteral("command")},
        {QStringLiteral("session_id"), QStringLiteral("session_lifecycle")},
        {QStringLiteral("from_slot"), QStringLiteral("ios_phone")},
        {QStringLiteral("from_login_session_id"), QStringLiteral("phone-login")},
        {QStringLiteral("client_message_id"), QStringLiteral("delete-1")},
        {QStringLiteral("payload"), QJsonObject{{QStringLiteral("action"),
                                                  QStringLiteral("session_delete")},
                                                 {QStringLiteral("target_session_id"), id}}}}});
    QTRY_VERIFY_WITH_TIMEOUT(latestPresenceCount == 0 && sent.size() >= 3, 5000);
    QVERIFY(!controller.agent().getSession(id.toStdString(), session));
    const QJsonObject deleted = sent.last().value(QStringLiteral("payload")).toObject();
    QCOMPARE(deleted.value(QStringLiteral("command_id")).toString(), QStringLiteral("delete-1"));
    QCOMPARE(deleted.value(QStringLiteral("result")).toObject()
                 .value(QStringLiteral("session_id")).toString(), id);
}

void DesktopSessionSyncHostTest::sessionListOffersLifecycleActionsForRemoteDevice() {
    AgentController controller(AgentController::ModelConfig{}, {});
    AgentSessionList sessions(controller);
    sessions.resize(320, 460);
    const QJsonObject device{{QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                             {QStringLiteral("device_name"), QStringLiteral("iPhone")},
                             {QStringLiteral("device_id"), QStringLiteral("phone-install")},
                             {QStringLiteral("login_session_id"), QStringLiteral("phone-login")},
                             {QStringLiteral("online"), true}};
    sessions.setRemoteDevices(QJsonArray{device});
    sessions.show();
    auto* list = sessions.findChild<QListWidget*>();
    QVERIFY(list);
    QCOMPARE(list->count(), 1);
    QVERIFY(list->item(0)->text().contains(QStringLiteral("0")));
    if (qEnvironmentVariableIsSet("MAICHAT_REMOTE_SESSION_SCREENSHOT"))
        QVERIFY(sessions.grab().save(qEnvironmentVariable("MAICHAT_REMOTE_SESSION_SCREENSHOT")));
    QSignalSpy create(&sessions, &AgentSessionList::remoteCreateRequested);
    const QRect header = list->visualItemRect(list->item(0));
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {},
                      QPoint(header.right() - UiZoom::s(100), header.center().y()));
    QCOMPARE(create.size(), 1);
    QCOMPARE(create.first().first().toJsonObject().value(QStringLiteral("device_id")).toString(),
             QStringLiteral("phone-install"));
    QJsonObject remote = device;
    remote.insert(QStringLiteral("id"), QStringLiteral("ses_phone"));
    remote.insert(QStringLiteral("title"), QStringLiteral("手机对话"));
    sessions.setRemoteSessions(QJsonArray{remote});
    QCOMPARE(list->count(), 2);
    QSignalSpy remove(&sessions, &AgentSessionList::remoteDeleteRequested);
    const QRect row = list->visualItemRect(list->item(1));
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {},
                      QPoint(row.right() - UiZoom::s(25), row.center().y()));
    auto* menu = sessions.findChild<QMenu*>(QStringLiteral("agentRemoteSessionMenu"));
    QVERIFY(menu);
    QVERIFY(menu->actions().size() == 1);
    menu->actions().first()->trigger();
    QCOMPARE(remove.size(), 1);
    QCOMPARE(remove.first().first().toJsonObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("ses_phone"));
}

void DesktopSessionSyncHostTest::agentToolsReuseRemoteLifecycleCoordinator() {
    AgentController controller(AgentController::ModelConfig{}, {});
    QObject dispatcher;
    QJsonObject lastCommand;
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject& body,
            DesktopSessionSyncCoordinator::Completion completion) {
            if (path == QStringLiteral("session-sync/send") &&
                body.value(QStringLiteral("kind")) == QStringLiteral("command")) {
                lastCommand = body;
                const QString id = body.value(QStringLiteral("client_message_id")).toString();
                const QString action =
                    body.value(QStringLiteral("payload")).toObject().value(QStringLiteral("action")).toString();
                const QString targetSlot = body.value(QStringLiteral("to_slot")).toString();
                const QString targetLogin = body.value(QStringLiteral("to_login_session_id")).toString();
                const QString resultId = action == QStringLiteral("session_create") ? QStringLiteral("ses_created")
                                         : targetSlot == QStringLiteral("windows")  ? QStringLiteral("ses_windows")
                                                                                    : QStringLiteral("ses_phone");
                const QJsonObject result =
                    action == QStringLiteral("session_set_workspace") && targetSlot == QStringLiteral("ios_phone")
                        ? QJsonObject{{QStringLiteral("ok"), false},
                                      {QStringLiteral("error"),
                                       QStringLiteral("iPhone does not support Workspace switching")}}
                        : QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("session_id"), resultId}};
                QTimer::singleShot(0, &dispatcher, [&, id, result, targetSlot, targetLogin] {
                    coordinator.acceptInbox(
                        QJsonArray{QJsonObject{{QStringLiteral("id"), 7},
                                               {QStringLiteral("kind"), QStringLiteral("event")},
                                               {QStringLiteral("session_id"), QStringLiteral("session_lifecycle")},
                                               {QStringLiteral("from_slot"), targetSlot},
                                               {QStringLiteral("from_login_session_id"), targetLogin},
                                               {QStringLiteral("payload"),
                                                QJsonObject{{QStringLiteral("type"), QStringLiteral("command_result")},
                                                            {QStringLiteral("command_id"), id},
                                                            {QStringLiteral("result"), result}}}}});
                });
            }
            QTimer::singleShot(0, &dispatcher, [completion = std::move(completion)] { completion({}, {}); });
        },
        [&] { return &controller; }, nullptr, QStringLiteral("mac-install"), QStringLiteral("Mac"));
    const QJsonObject device{{QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                             {QStringLiteral("device_name"), QStringLiteral("iPhone")},
                             {QStringLiteral("device_id"), QStringLiteral("phone-install")},
                             {QStringLiteral("login_session_id"), QStringLiteral("phone-login")},
                             {QStringLiteral("online"), true}};
    QJsonObject remote = device;
    remote.insert(QStringLiteral("id"), QStringLiteral("ses_phone"));
    remote.insert(QStringLiteral("title"), QStringLiteral("手机对话"));
    coordinator.acceptDevices(QJsonArray{device});
    coordinator.acceptCatalog(QJsonArray{remote});
    MaiToolRegistry registry;
    registerDesktopRemoteSessionTools(registry, &dispatcher, [&] { return &coordinator; });
    MaiTool* list = registry.findForModel("maichat_remote_session_list");
    MaiTool* create = registry.findForModel("maichat_remote_session_create");
    MaiTool* remove = registry.findForModel("maichat_remote_session_delete");
    MaiTool* workspace = registry.findForModel("maichat_remote_session_set_workspace");
    QVERIFY(list && create && remove && workspace);
    QVERIFY(!list->requiresApproval("{}"));
    QVERIFY(!create->requiresApproval("{}"));
    QVERIFY(!create->requiresPerCallApproval("{}"));
    QVERIFY(remove->requiresApproval("{}"));
    QVERIFY(remove->requiresPerCallApproval("{}"));
    QVERIFY(workspace->requiresApproval("{}"));
    QVERIFY(workspace->requiresPerCallApproval("{}"));
    MaiToolContext context;
    const MaiToolResult listed = list->execute("{}", context);
    QVERIFY(!listed.hasError());
    const QJsonObject listing = QJsonDocument::fromJson(QByteArray::fromStdString(listed.output())).object();
    QCOMPARE(listing.value(QStringLiteral("devices")).toArray().size(), 1);
    QCOMPARE(listing.value(QStringLiteral("sessions"))
                 .toArray()
                 .first()
                 .toObject()
                 .value(QStringLiteral("session_id"))
                 .toString(),
             QStringLiteral("ses_phone"));
    auto created = QtConcurrent::run([&] { return create->execute(R"({"device_slot":"ios_phone"})", context); });
    QTRY_VERIFY_WITH_TIMEOUT(created.isFinished(), 5000);
    const MaiToolResult createdResult = created.result();
    QVERIFY2(!createdResult.hasError(), createdResult.error().message().c_str());
    QCOMPARE(QJsonDocument::fromJson(QByteArray::fromStdString(createdResult.output()))
                 .object()
                 .value(QStringLiteral("session_id"))
                 .toString(),
             QStringLiteral("ses_created"));
    QVERIFY(!lastCommand.value(QStringLiteral("payload")).toObject().contains(QStringLiteral("workspace_path")));
    auto removed = QtConcurrent::run(
        [&] { return remove->execute(R"({"device_slot":"ios_phone","session_id":"ses_phone"})", context); });
    QTRY_VERIFY_WITH_TIMEOUT(removed.isFinished(), 5000);
    const MaiToolResult removedResult = removed.result();
    QVERIFY2(!removedResult.hasError(), removedResult.error().message().c_str());
    QCOMPARE(lastCommand.value(QStringLiteral("payload")).toObject().value(QStringLiteral("action")).toString(),
             QStringLiteral("session_delete"));
    QCOMPARE(
        lastCommand.value(QStringLiteral("payload")).toObject().value(QStringLiteral("target_session_id")).toString(),
        QStringLiteral("ses_phone"));
    auto unsupported = QtConcurrent::run([&] {
        return workspace->execute(
            R"({"device_slot":"ios_phone","session_id":"ses_phone","workspace_path":"/tmp/other"})", context);
    });
    QTRY_VERIFY_WITH_TIMEOUT(unsupported.isFinished(), 5000);
    const MaiToolResult unsupportedResult = unsupported.result();
    QVERIFY(unsupportedResult.hasError());
    QCOMPARE(QString::fromStdString(unsupportedResult.error().message()),
             QStringLiteral("iPhone does not support Workspace switching"));
    QCOMPARE(lastCommand.value(QStringLiteral("payload")).toObject().value(QStringLiteral("action")).toString(),
             QStringLiteral("session_set_workspace"));

    QJsonObject windowsDevice{{QStringLiteral("device_slot"), QStringLiteral("windows")},
                              {QStringLiteral("device_name"), QStringLiteral("Windows PC")},
                              {QStringLiteral("device_id"), QStringLiteral("windows-install")},
                              {QStringLiteral("login_session_id"), QStringLiteral("windows-login")},
                              {QStringLiteral("online"), true}};
    QJsonObject windowsSession = windowsDevice;
    windowsSession.insert(QStringLiteral("id"), QStringLiteral("ses_windows"));
    windowsSession.insert(QStringLiteral("title"), QStringLiteral("Desktop conversation"));
    coordinator.acceptDevices(QJsonArray{device, windowsDevice});
    coordinator.acceptCatalog(QJsonArray{remote, windowsSession});
    auto changed = QtConcurrent::run([&] {
        return workspace->execute(
            R"({"device_slot":"windows","session_id":"ses_windows","workspace_path":"C:\\Projects\\Existing"})",
            context);
    });
    QTRY_VERIFY_WITH_TIMEOUT(changed.isFinished(), 5000);
    const MaiToolResult changedResult = changed.result();
    QVERIFY2(!changedResult.hasError(), changedResult.error().message().c_str());
    QCOMPARE(lastCommand.value(QStringLiteral("to_slot")).toString(), QStringLiteral("windows"));
    QCOMPARE(lastCommand.value(QStringLiteral("payload")).toObject().value(QStringLiteral("action")).toString(),
             QStringLiteral("session_set_workspace"));
    QCOMPARE(QJsonDocument::fromJson(QByteArray::fromStdString(changedResult.output()))
                 .object()
                 .value(QStringLiteral("workspace_path"))
                 .toString(),
             QStringLiteral("C:\\Projects\\Existing"));
}

void DesktopSessionSyncHostTest::remotePermissionButtonsUseSharedWireDecisions() {
    const QJsonObject remote{{QStringLiteral("id"), QStringLiteral("ses_remote")},
                             {QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                             {QStringLiteral("login_session_id"), QStringLiteral("phone-login")},
                             {QStringLiteral("online"), true}};
    const QJsonArray permissions{QJsonObject{
        {QStringLiteral("id"), QStringLiteral("per_one")},
        {QStringLiteral("tool"), QStringLiteral("file_write")},
        {QStringLiteral("allowForSession"), true}}};
    const QList<QPair<QString, QString>> cases{
        {QStringLiteral("允许一次"), QStringLiteral("approved")},
        {QStringLiteral("本次会话允许"), QStringLiteral("approved_for_session")},
        {QStringLiteral("拒绝"), QStringLiteral("denied")}};
    for (const auto& test : cases) {
        RemoteAgentPanel panel;
        QJsonObject sent;
        panel.setHistoryHandler([&](const QJsonObject&, const QString&, int,
                                    RemoteAgentPanel::Completion completion) {
            panel.receivePayload(remote, QStringLiteral("history_page"),
                                 {{QStringLiteral("ok"), true},
                                  {QStringLiteral("messages"), QJsonArray{}},
                                  {QStringLiteral("permissions"), permissions}});
            completion({});
        });
        panel.setCommandHandler([&](const QJsonObject&, const QJsonObject& payload,
                                    RemoteAgentPanel::Completion completion) {
            sent = payload;
            completion({});
        });
        panel.show();
        panel.openSession(remote);
        QPushButton* decision = nullptr;
        for (QPushButton* button : panel.findChildren<QPushButton*>())
            if (button->text() == test.first) decision = button;
        QVERIFY(decision != nullptr);
        QTest::mouseClick(decision, Qt::LeftButton);
        QCOMPARE(sent.value(QStringLiteral("action")).toString(), QStringLiteral("permission"));
        QCOMPARE(sent.value(QStringLiteral("decision")).toString(), test.second);
    }
    AgentController controller(AgentController::ModelConfig{}, {});
    const QString sessionId = controller.createSession({}, QStringLiteral("权限测试"));
    DesktopSessionSyncHost host(controller);
    for (const auto& test : cases) {
        const QJsonObject result = host.applyCommand(
            sessionId, {{QStringLiteral("action"), QStringLiteral("permission")},
                        {QStringLiteral("id"), QStringLiteral("per_missing")},
                        {QStringLiteral("decision"), test.second}});
        QVERIFY(!result.value(QStringLiteral("ok")).toBool());
        QVERIFY(result.value(QStringLiteral("error")).toString() != QStringLiteral("权限决定无效"));
    }
}

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
    coordinator.connectionReady();
    coordinator.acceptInbox(QJsonArray{
        QJsonObject{{QStringLiteral("id"), 1},
                    {QStringLiteral("kind"), QStringLiteral("command")},
                    {QStringLiteral("session_id"), sessionId},
                    {QStringLiteral("from_slot"), QStringLiteral("ios_phone")},
                    {QStringLiteral("from_login_session_id"), QStringLiteral("phone-login")},
                    {QStringLiteral("client_message_id"), QStringLiteral("command-1")},
                    {QStringLiteral("payload"),
                     QJsonObject{{QStringLiteral("action"), QStringLiteral("prompt")},
                                 {QStringLiteral("text"), QStringLiteral("检查编译错误")}}}}});
    QTRY_VERIFY_WITH_TIMEOUT(presenceUpdates > 0 && acknowledgements >= 1 && sent.size() >= 1, 5000);
    bool sawResult = false;
    for (const QJsonObject& message : sent) {
        const QString kind = message.value(QStringLiteral("kind")).toString();
        const QJsonObject chunk = message.value(QStringLiteral("payload")).toObject();
        const QJsonDocument document = QJsonDocument::fromJson(
            QByteArray::fromBase64(chunk.value(QStringLiteral("data_base64")).toString().toLatin1()));
        if (kind == QStringLiteral("event") &&
            document.object().value(QStringLiteral("type")).toString() == QStringLiteral("command_result"))
            sawResult = true;
    }
    QVERIFY(sawResult);
}

void DesktopSessionSyncHostTest::coordinatorPublishesLocalMessagesToSharedRoom() {
    AgentController::ModelConfig model;
    AgentController controller(model, {});
    const QString sessionId = controller.createSession({}, QStringLiteral("共享任务"));
    QVERIFY(!sessionId.isEmpty());
    QList<QJsonObject> published;
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject& body,
            DesktopSessionSyncCoordinator::Completion completion) {
            if (path == QStringLiteral("session-sync/publish")) published.append(body);
            QTimer::singleShot(0, [completion = std::move(completion)] {
                completion({}, {});
            });
        },
        [&] { return &controller; }, nullptr, QStringLiteral("test-install"), QStringLiteral("Mac"));
    coordinator.start();
    coordinator.connectionReady();
    coordinator.acceptInbox(QJsonArray{QJsonObject{
        {QStringLiteral("id"), 1},
        {QStringLiteral("kind"), QStringLiteral("command")},
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("from_slot"), QStringLiteral("ios_phone")},
        {QStringLiteral("from_login_session_id"), QStringLiteral("ios-login")},
        {QStringLiteral("client_message_id"), QStringLiteral("test-command")},
        {QStringLiteral("payload"), QJsonObject{{QStringLiteral("action"), QStringLiteral("prompt")},
                                                   {QStringLiteral("text"), QStringLiteral("你好")}}}}});
    QTRY_VERIFY_WITH_TIMEOUT(published.size() >= 2, 5000);
    bool sawUser = false;
    bool sawAssistant = false;
    for (const QJsonObject& item : published) {
        QCOMPARE(item.value(QStringLiteral("session_id")).toString(), sessionId);
        const QJsonArray messages = item.value(QStringLiteral("messages")).toArray();
        if (messages.isEmpty()) continue;
        const QString role = messages.first().toObject().value(QStringLiteral("role")).toString();
        sawUser |= role == QStringLiteral("user");
        sawAssistant |= role == QStringLiteral("assistant");
    }
    QVERIFY(sawUser && sawAssistant);
}

void DesktopSessionSyncHostTest::coordinatorBackfillsOlderMessagesIntoSharedRoom() {
    AgentController::ModelConfig model;
    AgentController controller(model, {});
    const QString sessionId = controller.createSession({}, QStringLiteral("长会话"));
    QVERIFY(!sessionId.isEmpty());
    for (int index = 0; index < 11; ++index) {
        QTRY_VERIFY_WITH_TIMEOUT(!controller.agent().isBusy(sessionId.toStdString()), 3000);
        const auto result = controller.agent().submit(MaiSendPrompt{
            sessionId.toStdString(), "prompt " + std::to_string(index)});
        QVERIFY(result);
    }
    QTRY_VERIFY_WITH_TIMEOUT(!controller.agent().isBusy(sessionId.toStdString()), 3000);
    const auto history = controller.agent().listMessagesPage(sessionId.toStdString(), "", 100);
    QVERIFY(history.size() > 20);
    const QString oldest = QString::fromStdString(history.back().id);
    QSet<QString> published;
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject& body,
            DesktopSessionSyncCoordinator::Completion completion) {
            if (path == QStringLiteral("session-sync/publish"))
                for (const QJsonValue& value : body.value(QStringLiteral("messages")).toArray())
                    published.insert(value.toObject().value(QStringLiteral("id")).toString());
            QTimer::singleShot(0, [completion = std::move(completion)] { completion({}, {}); });
        },
        [&] { return &controller; }, nullptr, QStringLiteral("test-install"), QStringLiteral("Mac"));
    coordinator.start();
    coordinator.connectionReady();
    QTRY_VERIFY_WITH_TIMEOUT(published.contains(oldest), 15'000);
}

void DesktopSessionSyncHostTest::coordinatorDeletesRoomWhenLocalSessionIsRemoved() {
    AgentController::ModelConfig model;
    AgentController controller(model, {});
    const QString sessionId = controller.createSession({}, QStringLiteral("待删除"));
    QVERIFY(!sessionId.isEmpty());
    QString removedRoom;
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject& body,
            DesktopSessionSyncCoordinator::Completion completion) {
            if (path == QStringLiteral("session-sync/room-delete"))
                removedRoom = body.value(QStringLiteral("session_id")).toString();
            QTimer::singleShot(0, [completion = std::move(completion)] { completion({}, {}); });
        },
        [&] { return &controller; }, nullptr, QStringLiteral("test-install"), QStringLiteral("Mac"));
    coordinator.start();
    coordinator.connectionReady();
    QCoreApplication::processEvents();
    QVERIFY(controller.agent().submit(MaiDeleteSession{sessionId.toStdString()}));
    QTRY_COMPARE_WITH_TIMEOUT(removedRoom, sessionId, 5000);
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
    QJsonArray catalog;
    QJsonObject received;
    QJsonObject sentCommand;
    QJsonObject historyRequest;
    int historyCalls = 0;
    int acknowledgements = 0;
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject& body,
            DesktopSessionSyncCoordinator::Completion completion) {
            QJsonObject response;
            if (path == QStringLiteral("session-sync/send")) {
                sentCommand = body;
                response = {{QStringLiteral("id"), 1}};
            } else if (path == QStringLiteral("session-sync/history")) {
                ++historyCalls;
                historyRequest = body;
                response = {{QStringLiteral("ok"), true},
                            {QStringLiteral("messages"), QJsonArray{QJsonObject{
                                 {QStringLiteral("id"), QStringLiteral("msg_remote")}}}}};
            } else if (path == QStringLiteral("session-sync/ack")) {
                ++acknowledgements;
                response = {{QStringLiteral("acknowledged"), true}};
            }
            QTimer::singleShot(0, [completion = std::move(completion), response] { completion(response, {}); });
        },
        [&] { return &controller; }, nullptr, QStringLiteral("mac-install"), QStringLiteral("Mac"));
    coordinator.setCatalogHandler([&](const QJsonArray& value) { catalog = value; });
    coordinator.setPayloadHandler([&](const QJsonObject&, const QString& kind, const QJsonObject& payload) {
        if (kind == QStringLiteral("history_page")) received = payload;
    });
    coordinator.start();
    coordinator.acceptCatalog(QJsonArray{remote});
    QCOMPARE(catalog.size(), 1);
    QString error;
    coordinator.requestRemoteHistory(catalog.first().toObject(), {}, 10,
                                     [&](const QJsonObject&, const QString& value) { error = value; });
    QTRY_VERIFY_WITH_TIMEOUT(received.value(QStringLiteral("ok")).toBool(), 5000);
    QVERIFY(error.isEmpty());
    QCOMPARE(historyRequest.value(QStringLiteral("owner_slot")).toString(), QStringLiteral("ios_phone"));
    QCOMPARE(received.value(QStringLiteral("messages")).toArray().first().toObject()
                 .value(QStringLiteral("id")).toString(), QStringLiteral("msg_remote"));
    QJsonObject offline = remote;
    offline.insert(QStringLiteral("online"), false);
    coordinator.acceptCatalog(QJsonArray{offline});
    coordinator.requestRemoteHistory(offline, {}, 10,
                                     [&](const QJsonObject&, const QString& value) { error = value; });
    QTRY_VERIFY_WITH_TIMEOUT(historyCalls == 2 && error.isEmpty(), 5000);
    coordinator.acceptCatalog(QJsonArray{remote});
    const QJsonObject update{{QStringLiteral("ok"), true},
                             {QStringLiteral("messages"), QJsonArray{QJsonObject{
                                  {QStringLiteral("id"), QStringLiteral("msg_pushed")}}}}};
    const QByteArray bytes = QJsonDocument(update).toJson(QJsonDocument::Compact);
    const QString digest = QString::fromLatin1(QCryptographicHash::hash(
        bytes, QCryptographicHash::Sha256).toHex());
    coordinator.acceptInbox(QJsonArray{QJsonObject{
        {QStringLiteral("id"), 2}, {QStringLiteral("kind"), QStringLiteral("event")},
        {QStringLiteral("session_id"), QStringLiteral("ses_phone")},
        {QStringLiteral("from_slot"), QStringLiteral("ios_phone")},
        {QStringLiteral("from_login_session_id"), QStringLiteral("phone-login")},
        {QStringLiteral("payload"), QJsonObject{
            {QStringLiteral("transfer_id"), QStringLiteral("transfer-1")},
            {QStringLiteral("chunk_index"), 0}, {QStringLiteral("chunk_count"), 1},
            {QStringLiteral("sha256"), digest},
            {QStringLiteral("data_base64"), QString::fromLatin1(bytes.toBase64())}}}}});
    QTRY_VERIFY_WITH_TIMEOUT(acknowledgements > 0, 5000);
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
    bool retryFinished = false;
    coordinator.sendRemoteCommand(catalog.first().toObject(),
                                  {{QStringLiteral("action"), QStringLiteral("prompt")},
                                   {QStringLiteral("text"), QStringLiteral("继续")}},
                                  [&](const QJsonObject&, const QString& value) {
                                      error = value;
                                      retryFinished = true;
                                  });
    QCOMPARE(sentCommand.value(QStringLiteral("client_message_id")).toString(), firstCommandId);
    QTRY_VERIFY_WITH_TIMEOUT(retryFinished, 5000);
    QVERIFY(error.isEmpty());
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
    QVERIFY(dynamic_cast<ComposerTextEdit*>(composer) != nullptr);
    composer->setPlainText(QStringLiteral("继续本机任务"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("remoteAgentSendButton"));
    QVERIFY(send != nullptr && send->isEnabled());
    QCOMPARE(send->parentWidget(), composer);
    QVERIFY(!send->icon().isNull());
    auto* attachment = panel.findChild<QPushButton*>(QStringLiteral("remoteAgentAttachButton"));
    QVERIFY(attachment && !attachment->isEnabled());
    QTest::mouseClick(send, Qt::LeftButton);
    QCOMPARE(sent.value(QStringLiteral("action")).toString(), QStringLiteral("prompt"));
    QCOMPARE(sent.value(QStringLiteral("text")).toString(), QStringLiteral("继续本机任务"));
    QCOMPARE(composer->toPlainText(), QString());
    panel.receivePayload(remote, QStringLiteral("event"),
                         {{QStringLiteral("type"), QStringLiteral("command_result")},
                          {QStringLiteral("result"), QJsonObject{{QStringLiteral("ok"), true}}}});
    QVERIFY(transcript->contains(QStringLiteral("remote-thinking")));
    auto* status = panel.findChild<QLabel*>(QStringLiteral("remoteAgentStatus"));
    QVERIFY(status && !status->isVisible());
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QJsonObject active{{QStringLiteral("id"), QStringLiteral("msg_new")},
                             {QStringLiteral("role"), QStringLiteral("assistant")},
                             {QStringLiteral("created"), double(now)},
                             {QStringLiteral("active"), true},
                             {QStringLiteral("parts"), QJsonArray{}}};
    panel.receivePayload(remote, QStringLiteral("history_page"),
                         {{QStringLiteral("ok"), true},
                          {QStringLiteral("messages"), QJsonArray{active}}});
    QVERIFY(transcript->contains(QStringLiteral("remote-thinking")));
    QJsonObject completed = active;
    completed.insert(QStringLiteral("active"), false);
    completed.insert(QStringLiteral("parts"), QJsonArray{QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("text")},
        {QStringLiteral("text"), QStringLiteral("已完成")}}});
    panel.receivePayload(remote, QStringLiteral("history_page"),
                         {{QStringLiteral("ok"), true},
                          {QStringLiteral("messages"), QJsonArray{completed}}});
    QVERIFY(!transcript->contains(QStringLiteral("remote-thinking")));
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
    panel.setCommandHandler([&](const QJsonObject&, const QJsonObject&,
                                RemoteAgentPanel::Completion completion) {
        panel.receivePayload(remote, QStringLiteral("event"),
                             {{QStringLiteral("type"), QStringLiteral("command_result")},
                              {QStringLiteral("result"), QJsonObject{{QStringLiteral("ok"), true}}}});
        completion({});
    });
    composer->setPlainText(QStringLiteral("快速确认应清空"));
    QTest::mouseClick(send, Qt::LeftButton);
    QCOMPARE(composer->toPlainText(), QString());
    if (qEnvironmentVariableIsSet("MAICHAT_REMOTE_PANEL_SCREENSHOT"))
        QVERIFY(panel.grab().save(qEnvironmentVariable("MAICHAT_REMOTE_PANEL_SCREENSHOT")));
}

void DesktopSessionSyncHostTest::remotePanelDoesNotRepeatPendingHistory() {
    RemoteAgentPanel panel;
    const QJsonObject remote{{QStringLiteral("id"), QStringLiteral("ses_remote")},
                             {QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                             {QStringLiteral("login_session_id"), QStringLiteral("phone_login")},
                             {QStringLiteral("online"), true}};
    int requests = 0;
    panel.setHistoryHandler([&](const QJsonObject&, const QString&, int limit,
                                RemoteAgentPanel::Completion completion) {
        ++requests;
        QCOMPARE(limit, 10);
        completion({});
    });
    panel.show();
    panel.openSession(remote);
    QCOMPARE(requests, 1);
    QTest::qWait(5500);
    QCOMPARE(requests, 1);
}

void DesktopSessionSyncHostTest::remotePanelReconcilesAfterReconnect() {
    RemoteAgentPanel panel;
    const QJsonObject remote{{QStringLiteral("id"), QStringLiteral("ses_remote")},
                             {QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                             {QStringLiteral("login_session_id"), QStringLiteral("phone_login")},
                             {QStringLiteral("online"), true}};
    QList<int> limits;
    panel.setHistoryHandler([&](const QJsonObject&, const QString&, int limit,
                                RemoteAgentPanel::Completion completion) {
        limits.append(limit);
        QJsonArray messages;
        if (limits.size() > 1) {
            messages.append(QJsonObject{
                {QStringLiteral("id"), QStringLiteral("msg_assistant")},
                {QStringLiteral("role"), QStringLiteral("assistant")},
                {QStringLiteral("parts"), QJsonArray{QJsonObject{
                    {QStringLiteral("kind"), QStringLiteral("text")},
                    {QStringLiteral("text"), QStringLiteral("收到")}}}}});
            messages.append(QJsonObject{
                {QStringLiteral("id"), QStringLiteral("msg_user")},
                {QStringLiteral("role"), QStringLiteral("user")},
                {QStringLiteral("parts"), QJsonArray{QJsonObject{
                    {QStringLiteral("kind"), QStringLiteral("text")},
                    {QStringLiteral("text"), QStringLiteral("你好")}}}}});
        }
        panel.receivePayload(remote, QStringLiteral("history_page"),
                             {{QStringLiteral("ok"), true},
                              {QStringLiteral("messages"), messages}});
        completion({});
    });
    panel.show();
    panel.openSession(remote);
    QCOMPARE(limits, QList<int>{10});
    QJsonObject offline = remote;
    offline.insert(QStringLiteral("online"), false);
    panel.updateSession(offline);
    panel.updateSession(remote);
    QCOMPARE(limits, (QList<int>{10, 10}));
    auto* transcript = panel.findChild<MarkdownView*>();
    QVERIFY(transcript != nullptr);
    QVERIFY(transcript->contains(QStringLiteral("msg_user")));
    if (qEnvironmentVariableIsSet("MAICHAT_REMOTE_PAIR_CAPTURE"))
        QVERIFY(panel.grab().save(qEnvironmentVariable("MAICHAT_REMOTE_PAIR_CAPTURE")));
}

void DesktopSessionSyncHostTest::remotePanelKeepsOwnPromptWhenRoomBroadcastsCanonicalMessage() {
    RemoteAgentPanel panel;
    const QJsonObject remote{{QStringLiteral("id"), QStringLiteral("ses_remote")},
                             {QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                             {QStringLiteral("login_session_id"), QStringLiteral("phone_login")},
                             {QStringLiteral("online"), true}};
    QList<int> limits;
    panel.setHistoryHandler([&](const QJsonObject&, const QString&, int limit,
                                RemoteAgentPanel::Completion completion) {
        limits.append(limit);
        panel.receivePayload(remote, QStringLiteral("history_page"),
                             {{QStringLiteral("ok"), true},
                              {QStringLiteral("messages"), QJsonArray{}}});
        completion({});
    });
    panel.setCommandHandler([&](const QJsonObject&, const QJsonObject&,
                                RemoteAgentPanel::Completion completion) { completion({}); });
    panel.show();
    panel.openSession(remote);
    QCOMPARE(limits, QList<int>{10});
    auto* composer = panel.findChild<QTextEdit*>(QStringLiteral("remoteAgentComposer"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("remoteAgentSendButton"));
    QVERIFY(composer && send);
    auto* transcript = panel.findChild<MarkdownView*>();
    QVERIFY(transcript);
    composer->setPlainText(QStringLiteral("你好"));
    QTest::mouseClick(send, Qt::LeftButton);
    QCOMPARE(transcript->itemCount(), 1);
    QCOMPARE(composer->toPlainText(), QString());
    panel.receivePayload(remote, QStringLiteral("event"),
                         {{QStringLiteral("type"), QStringLiteral("command_result")},
                          {QStringLiteral("result"), QJsonObject{{QStringLiteral("ok"), true},
                                                                   {QStringLiteral("user_message_id"),
                                                                    QStringLiteral("msg_user")}}}});
    QVERIFY(transcript->contains(QStringLiteral("msg_user")));
    QCOMPARE(transcript->itemCount(), 2); // The second item is the pending assistant indicator.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QJsonObject user{{QStringLiteral("id"), QStringLiteral("msg_user")},
                           {QStringLiteral("role"), QStringLiteral("user")},
                           {QStringLiteral("created"), double(now)},
                           {QStringLiteral("parts"), QJsonArray{QJsonObject{
                               {QStringLiteral("kind"), QStringLiteral("text")},
                               {QStringLiteral("text"), QStringLiteral("你好")}}}}};
    const QJsonObject assistant{{QStringLiteral("id"), QStringLiteral("msg_assistant")},
                                {QStringLiteral("role"), QStringLiteral("assistant")},
                                {QStringLiteral("created"), double(now + 1)},
                                {QStringLiteral("parts"), QJsonArray{QJsonObject{
                                    {QStringLiteral("kind"), QStringLiteral("text")},
                                    {QStringLiteral("text"), QStringLiteral("收到")}}}}};
    panel.receivePayload(remote, QStringLiteral("event"),
                         {{QStringLiteral("ok"), true},
                          {QStringLiteral("messages"), QJsonArray{assistant, user}}});
    QCOMPARE(limits, QList<int>{10});
    QCOMPARE(transcript->itemCount(), 2);
    QVERIFY(transcript->contains(QStringLiteral("msg_user")));
    QVERIFY(transcript->contains(QStringLiteral("msg_assistant")));
}

void DesktopSessionSyncHostTest::remotePanelPushCanArriveBeforeInitialPage() {
    RemoteAgentPanel panel;
    const QJsonObject remote{{QStringLiteral("id"), QStringLiteral("ses_remote")},
                             {QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                             {QStringLiteral("login_session_id"), QStringLiteral("phone_login")},
                             {QStringLiteral("online"), true}};
    QList<int> limits;
    panel.setHistoryHandler([&](const QJsonObject&, const QString&, int limit,
                                RemoteAgentPanel::Completion completion) {
        Q_UNUSED(completion);
        limits.append(limit);
    });
    panel.show();
    panel.openSession(remote);
    QCOMPARE(limits, QList<int>{10});
    const QJsonObject user{{QStringLiteral("id"), QStringLiteral("msg_user")},
                           {QStringLiteral("role"), QStringLiteral("user")},
                           {QStringLiteral("parts"), QJsonArray{QJsonObject{
                               {QStringLiteral("kind"), QStringLiteral("text")},
                               {QStringLiteral("text"), QStringLiteral("你好")}}}}};
    panel.receivePayload(remote, QStringLiteral("event"),
                         {{QStringLiteral("ok"), true},
                          {QStringLiteral("messages"), QJsonArray{user}}});
    auto* transcript = panel.findChild<MarkdownView*>();
    QVERIFY(transcript && transcript->contains(QStringLiteral("msg_user")));
    panel.receivePayload(remote, QStringLiteral("event"),
                         {{QStringLiteral("ok"), true},
                          {QStringLiteral("messages"), QJsonArray{user}}});
    QCOMPARE(transcript->itemCount(), 1);
    const QJsonObject older{{QStringLiteral("id"), QStringLiteral("msg_older")},
                            {QStringLiteral("role"), QStringLiteral("assistant")},
                            {QStringLiteral("parts"), QJsonArray{QJsonObject{
                                {QStringLiteral("kind"), QStringLiteral("text")},
                                {QStringLiteral("text"), QStringLiteral("旧消息")}}}}};
    panel.receivePayload(remote, QStringLiteral("history_page"),
                         {{QStringLiteral("ok"), true},
                          {QStringLiteral("messages"), QJsonArray{older}}});
    QCOMPARE(limits, QList<int>{10});
    QCOMPARE(transcript->itemCount(), 2);
    QVERIFY(transcript->contains(QStringLiteral("msg_older")));
    QVERIFY(transcript->contains(QStringLiteral("msg_user")));
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
        {QStringLiteral("device_name"), QStringLiteral("iPhone 14 Pro Max")},
        {QStringLiteral("device_id"), QStringLiteral("phone-install")},
        {QStringLiteral("login_session_id"), QStringLiteral("phone-login")},
        {QStringLiteral("online"), true}}});
    sessions.show();
    auto* list = sessions.findChild<QListWidget*>();
    QVERIFY(list != nullptr);
    QCOMPARE(list->count(), 4);
    QVERIFY(list->item(0)->text().contains(QStringLiteral("这台")));
    QVERIFY(list->item(2)->text().contains(QStringLiteral("iPhone 14 Pro Max")));
    QSignalSpy localSelection(&sessions, &AgentSessionList::selected);
    QSignalSpy remoteSelection(&sessions, &AgentSessionList::remoteSelected);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(2)).center());
    QCOMPARE(remoteSelection.size(), 0);
    QCOMPARE(list->count(), 3);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(2)).center());
    QCOMPARE(list->count(), 4);
    if (qEnvironmentVariableIsSet("MAICHAT_SESSION_GROUP_SCREENSHOT"))
        QVERIFY(sessions.grab().save(qEnvironmentVariable("MAICHAT_SESSION_GROUP_SCREENSHOT")));
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(3)).center());
    QCOMPARE(remoteSelection.size(), 1);
    QCOMPARE(localSelection.size(), 0);
    QCOMPARE(remoteSelection.takeFirst().first().toJsonObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("ses_remote"));
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(1)).center());
    QCOMPARE(localSelection.size(), 1);
    QCOMPARE(localSelection.takeFirst().first().toString(), local);
}

void DesktopSessionSyncHostTest::sessionListSeparatesDevicesWithTheSameModel() {
    AgentController controller(AgentController::ModelConfig{}, {});
    AgentSessionList sessions(controller);
    QJsonArray remote;
    for (const QString& id : {QStringLiteral("phone_one"), QStringLiteral("phone_two")}) {
        remote.append(QJsonObject{{QStringLiteral("id"), id},
                                  {QStringLiteral("title"), QStringLiteral("同型号会话")},
                                  {QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                                  {QStringLiteral("device_name"), QStringLiteral("iPhone 14 Pro Max")},
                                  {QStringLiteral("device_id"), id},
                                  {QStringLiteral("login_session_id"), QStringLiteral("login_phone")},
                                  {QStringLiteral("online"), true}});
    }
    sessions.setRemoteSessions(remote);
    sessions.show();
    auto* list = sessions.findChild<QListWidget*>();
    QVERIFY(list);
    QCOMPARE(list->count(), 4);
    QVERIFY(list->item(0)->text().contains(QStringLiteral("设备 1")));
    QVERIFY(list->item(2)->text().contains(QStringLiteral("设备 2")));
}

QTEST_MAIN(DesktopSessionSyncHostTest)
#include "DesktopSessionSyncHostTest.moc"
