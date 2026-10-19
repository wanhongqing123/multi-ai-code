// 文件作用：验证远端操作只落到所属桌面 Agent，会话快照不泄漏本机媒体路径。
#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

#include "agent/AgentController.h"
#include "agent/DesktopSessionSyncHost.h"

class DesktopSessionSyncHostTest : public QObject {
    Q_OBJECT

private slots:
    void appliesRemotePromptToTheExistingLocalSession();
    void omitsLocalMediaPathsFromRemoteHistory();
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

QTEST_MAIN(DesktopSessionSyncHostTest)
#include "DesktopSessionSyncHostTest.moc"
