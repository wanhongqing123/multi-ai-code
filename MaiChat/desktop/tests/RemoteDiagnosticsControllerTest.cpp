#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include "app/RemoteIMApplication.h"
#include "diagnostics/RemoteDiagnosticsController.h"
#include "diagnostics/RemoteDiagnosticsProtocol.h"

class DiagnosticsClient final : public RemoteIMClient {
public:
    RemoteDiagnostics::AccountTag account;
    QStringList textTargets, texts, reportTargets, reportPaths;
    QList<QByteArray> reports;
    RemoteIMSendCompletion pendingText, pendingFile;
    bool holdText = false, holdFile = false;
    void recordFailure(const QString& id, const QString& peer = "machine", RemoteDiagnostics::AccountTag owner = {1, "phone"}) {
        RemoteDiagnostics::AttachmentEvent event;
        event.account = owner; event.peerId = peer; event.messageId = "sdk-download-id";
        event.requestId = id; event.phase = RemoteDiagnostics::AttachmentPhase::DownloadFailed;
        event.atMs = QDateTime::currentMSecsSinceEpoch(); event.code = 7;
        evidence_.record(event);
    }
    RemoteDiagnostics::AccountTag currentAccount() const override { return account; }
    void connectToService(int sdk, const QString& owner, const QString&, RemoteIMCompletion done) override {
        account = {quint64(sdk), owner}; done(true, {});
    }
    void disconnectFromService(RemoteIMCompletion done) override { account = {}; done(true, {}); emit disconnected(); }
    void deleteContact(const QString&, RemoteIMCompletion done) override { done(true, {}); }
    void sendText(const QString& peer, const QString& text, RemoteIMSendCompletion done) override {
        textTargets.append(peer); texts.append(text);
        if (holdText) pendingText = std::move(done); else done(true, {}, {});
    }
    void sendImage(const QString&, const QString&, RemoteIMSendCompletion done) override { done(false, {}, {}); }
    void sendVoice(const QString&, const QString&, int, RemoteIMCompletion done) override { done(false, {}); }
    void sendFile(const QString& peer, const QString& path, const QString&, RemoteIMSendCompletion done) override {
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        reportTargets.append(peer); reportPaths.append(path); reports.append(file.readAll());
        if (holdFile) pendingFile = std::move(done); else done(true, {}, {});
    }
    void deliver(const RemoteIMMessage& message) { emit liveMessagesReceived({message}); }
    ~DiagnosticsClient() override { for (const auto& path : reportPaths) QFile::remove(path); }
};

class RemoteDiagnosticsControllerTest : public QObject {
    Q_OBJECT
    QTemporaryDir temp_;
    std::unique_ptr<RemoteIMApplication> app_;
    std::unique_ptr<RemoteDiagnosticsController> controller_;
    DiagnosticsClient* client_ = nullptr;

    void response(const QString& from = "machine", const QString& payloadId = {}) {
        const auto id = controller_->requestId();
        const QJsonObject payload{{"schemaVersion", 1}, {"requestId", payloadId.isEmpty() ? id : payloadId},
            {"token", "PRIVATE_TOKEN_SENTINEL"}, {"files", QJsonArray{QJsonObject{{"source", "codex-original-events"},
                {"events", QJsonArray{QJsonObject{{"messageId", "call-async"}, {"delivery", "async"},
                    {"text", "PRIVATE_BODY_SENTINEL"}, {"detail", QJsonObject{{"ID", "sdk-id"}}}}}}}}}};
        const auto path = temp_.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + ".json");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QJsonDocument(payload).toJson()); file.close();
        RemoteIMMessage message;
        message.id = QUuid::createUuid().toString(); message.fromUserId = from; message.toUserId = "phone";
        message.hasFile = true; message.file.localPath = path;
        message.file.fileName = RemoteDiagnostics::attachmentFileName(id);
        message.createdAtMillis = 1; // Remote clock is deliberately unrelated.
        client_->deliver(message);
    }
private slots:
    void init() {
        auto client = std::make_unique<DiagnosticsClient>(); client_ = client.get();
        app_ = std::make_unique<RemoteIMApplication>("phone", std::move(client));
        for (const auto& peer : {"machine", "helper", "other"}) app_->addContact(peer, peer);
        app_->connectToService(1, "fake-credential"); app_->selectPeer("machine");
        controller_ = std::make_unique<RemoteDiagnosticsController>(app_.get());
        controller_->setTimeouts(1000, 1000);
    }
    void cleanup() { controller_.reset(); app_.reset(); }
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); QCoreApplication::setApplicationName("MaiChatDiagnosticsControllerTest"); }
    void cancellationBeforeDispatchSendsNothing() {
        controller_->start("machine", "helper"); controller_->cancel();
        QTest::qWait(120); QVERIFY(client_->texts.isEmpty()); QVERIFY(client_->reports.isEmpty());
    }
    void usesPersistedSendFacadeAndLocksRecipientWhileChatsChange() {
        controller_->start("machine", "helper");
        QTRY_COMPARE(client_->texts.size(), 1);
        QCOMPARE(client_->textTargets.first(), QString("machine"));
        QVERIFY(app_->chatState().messagesWith("machine").last().text.startsWith("/diagnostics "));
        app_->selectPeer("other"); response(); response();
        QTRY_VERIFY(!controller_->isRunning());
        QCOMPARE(client_->reportTargets, QStringList{"helper"});
        QVERIFY(client_->reports.first().contains("call-async"));
        QVERIFY(client_->reports.first().contains("sdk-id"));
        QVERIFY(!client_->reports.first().contains("PRIVATE_BODY_SENTINEL"));
        QVERIFY(!client_->reports.first().contains("PRIVATE_TOKEN_SENTINEL"));
        QCOMPARE(app_->chatState().selectedPeerId(), QString("other"));
        QCOMPARE(app_->chatState().messagesWith("helper").last().status, RemoteIMMessageStatus::Sent);
    }
    void wrongSenderAndNonceCannotReplaceTheValidReport() {
        controller_->start("machine", "helper"); QTRY_COMPARE(client_->texts.size(), 1);
        response("other"); response("machine", RemoteDiagnostics::newRequestId());
        QTest::qWait(150); QVERIFY(controller_->isRunning()); QVERIFY(client_->reports.isEmpty());
        response(); QTRY_VERIFY(!controller_->isRunning()); QCOMPARE(client_->reports.size(), 1);
    }
    void knownRefusalProducesAPartialReportWithoutRemoteErrorText() {
        controller_->start("machine", "helper"); QTRY_COMPARE(client_->texts.size(), 1);
        RemoteIMMessage message; message.id = "failure"; message.fromUserId = "machine"; message.toUserId = "phone";
        message.text = QStringLiteral("远程排障采集失败（编号 %1）：PRIVATE_ERROR_SENTINEL").arg(controller_->requestId());
        client_->deliver(message); QTRY_VERIFY(!controller_->isRunning());
        QVERIFY(client_->reports.first().contains(QStringLiteral("远端报告采集失败").toUtf8()));
        QVERIFY(!client_->reports.first().contains("PRIVATE_ERROR_SENTINEL"));
    }
    void stalledRequestStillEndsAndLateCompletionDoesNotSendTwice() {
        client_->holdText = true; controller_->setTimeouts(80, 20);
        controller_->start("machine", "helper"); QTRY_VERIFY(!controller_->isRunning());
        QCOMPARE(client_->reports.size(), 1);
        QVERIFY(client_->reports.first().contains(QStringLiteral("发送结果也尚未确认").toUtf8()));
        auto done = std::move(client_->pendingText); QVERIFY(bool(done)); done(true, {}, {});
        QTest::qWait(150); QCOMPARE(client_->reports.size(), 1);
    }
    void cancelledRequestRejectsLateSDKCallbackAndReport() {
        client_->holdText = true; controller_->start("machine", "helper"); QTRY_COMPARE(client_->texts.size(), 1);
        controller_->cancel(); response(); auto done = std::move(client_->pendingText); done(true, {}, {});
        QTest::qWait(150); QVERIFY(client_->reports.isEmpty());
    }
    void accountChangeAndFriendRemovalCancel() {
        controller_->start("machine", "helper"); QTRY_COMPARE(client_->texts.size(), 1);
        client_->account = {2, "phone"}; response(); QTRY_VERIFY(!controller_->isRunning()); QVERIFY(client_->reports.isEmpty());
        client_->account = {1, "phone"}; controller_->start("machine", "helper");
        app_->deleteContact("helper"); QTRY_VERIFY(!controller_->isRunning()); QVERIFY(client_->reports.isEmpty());
    }
    void stalledReportSendIsUnknownAndCannotBeAutomaticallyRepeated() {
        client_->holdFile = true; controller_->setTimeouts(1000, 20);
        controller_->start("machine", "helper"); QTRY_COMPARE(client_->texts.size(), 1); response();
        QTRY_VERIFY(!controller_->isRunning());
        QCOMPARE(client_->reports.size(), 1); QVERIFY(controller_->status().contains(QStringLiteral("尚未确认")));
        auto done = std::move(client_->pendingFile); done(true, {}, {}); QTest::qWait(150);
        QCOMPARE(client_->reports.size(), 1); QVERIFY(controller_->status().contains(QStringLiteral("尚未确认")));
    }
    void matchingDownloadFailureFinishesEarlyAndIsInTheActualAttachment() {
        controller_->setTimeouts(60000, 30000);
        controller_->start("machine", "helper"); QTRY_COMPARE(client_->texts.size(), 1);
        client_->recordFailure(controller_->requestId());
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isRunning(), 1000);
        QCOMPARE(client_->reports.size(), 1);
        QVERIFY(client_->reports.first().contains("sdk-download-id"));
        QVERIFY(client_->reports.first().contains(controller_->requestId().toUtf8()));
        QVERIFY(client_->reports.first().contains(QStringLiteral("附件下载失败").toUtf8()));
    }
    void otherDownloadsAccountsAndPeersCannotEndThisRequest() {
        controller_->start("machine", "helper"); QTRY_COMPARE(client_->texts.size(), 1);
        client_->recordFailure({});
        client_->recordFailure(RemoteDiagnostics::newRequestId());
        client_->recordFailure(controller_->requestId(), "other");
        client_->recordFailure(controller_->requestId(), "machine", {2, "phone"});
        QTest::qWait(150); QVERIFY(controller_->isRunning()); QVERIFY(client_->reports.isEmpty());
        response(); QTRY_VERIFY(!controller_->isRunning());
        QVERIFY(client_->reports.first().contains("call-async"));
    }
    void lateAcknowledgementAfterAccountChangeDoesNotModifyTheNewState() {
        client_->holdText = true; controller_->start("machine", "helper"); QTRY_COMPARE(client_->texts.size(), 1);
        const auto id = app_->chatState().messagesWith("machine").last().id;
        client_->account = {2, "phone"};
        auto done = std::move(client_->pendingText); done(true, {}, {"SHOULD_NOT_ADOPT", 1});
        QTRY_VERIFY(!controller_->isRunning());
        const auto message = app_->chatState().messagesWith("machine").last();
        QCOMPARE(message.id, id); QCOMPARE(message.status, RemoteIMMessageStatus::Pending);
        QVERIFY(client_->reports.isEmpty());
    }
    void deletingControllerBeforeSendCompletionDoesNotCrashOrForward() {
        client_->holdText = true; controller_->start("machine", "helper"); QTRY_COMPARE(client_->texts.size(), 1);
        controller_.reset(); auto done = std::move(client_->pendingText); done(true, {}, {});
        QTest::qWait(150); QVERIFY(client_->reports.isEmpty());
    }
};
QTEST_GUILESS_MAIN(RemoteDiagnosticsControllerTest)
#include "RemoteDiagnosticsControllerTest.moc"
