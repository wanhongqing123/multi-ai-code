// 文件作用：验证远端操作只落到所属桌面 Agent，会话快照不泄漏本机媒体路径。
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QTimer>

#include "agent/AgentController.h"
#include "agent/DesktopSessionSyncCoordinator.h"
#include "agent/DesktopSessionSyncHost.h"

class DesktopSessionSyncHostTest : public QObject {
    Q_OBJECT

private slots:
    void appliesRemotePromptToTheExistingLocalSession();
    void omitsLocalMediaPathsFromRemoteHistory();
    void coordinatorRoutesPhoneCommandToTheExistingDesktopSession();
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

QTEST_MAIN(DesktopSessionSyncHostTest)
#include "DesktopSessionSyncHostTest.moc"
