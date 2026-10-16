// 文件作用：验证桌面端扫码登录窗布局和无账号绕过入口。
// 运行边界：由测试进程驱动实际组件或替身，不作为正式 App 的用户入口。
#include <QLabel>
#include <QLineEdit>
#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPixmap>
#include <QPushButton>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QWidget>
#include <memory>

#include "ui/LoginDialog.h"
#include "auth/DesktopCurlAuthTransport.h"

namespace {

QString testPlatformName() {
#ifdef Q_OS_MACOS
    return QStringLiteral("mac");
#elif defined(Q_OS_WIN)
    return QStringLiteral("windows");
#else
    return {};
#endif
}

class FakeQrServer : public QObject {
public:
    // 用一个本地 HTTP 服务覆盖扫码、会话、IM 和模型端点；测试可以延迟 claim
    // 回复来固定“用户更换/关闭二维码时服务器刚好批准”的并发顺序。
    FakeQrServer() {
        const QString qrFixture = qEnvironmentVariable("MAICHAT_TEST_QR_PNG");
        if (!qrFixture.isEmpty()) {
            QFile file(qrFixture);
            if (file.open(QIODevice::ReadOnly)) png_ = file.readAll();
        }
        if (png_.isEmpty()) {
            QImage image(220, 220, QImage::Format_RGB32);
            image.fill(Qt::white);
            for (int y = 0; y < 11; ++y)
                for (int x = 0; x < 11; ++x)
                    if ((x * 7 + y * 3) % 5 < 2)
                        for (int dy = 0; dy < 20; ++dy)
                            for (int dx = 0; dx < 20; ++dx)
                                image.setPixelColor(x * 20 + dx, y * 20 + dy, Qt::black);
            QBuffer buffer(&png_);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
        }
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            auto* socket = server_.nextPendingConnection();
            auto bytes = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes] {
                bytes->append(socket->readAll());
                const int separator = bytes->indexOf("\r\n\r\n");
                if (separator < 0) return;
                const QByteArray headers = bytes->left(separator).toLower();
                int contentLength = 0;
                for (const QByteArray& line : headers.split('\n')) {
                    if (line.startsWith("content-length:"))
                        contentLength = line.mid(sizeof("content-length:") - 1).trimmed().toInt();
                }
                if (bytes->size() < separator + 4 + contentLength) return;
                const QJsonObject body = QJsonDocument::fromJson(
                    bytes->mid(separator + 4, contentLength)).object();
                QJsonObject response;
                int status = 200;
                if (headers.startsWith("post /v1/auth/desktop/request ")) {
                    ++qrRequestCount;
                    challenge = body.value(QStringLiteral("code_challenge")).toString();
                    platform = body.value(QStringLiteral("platform")).toString();
                    response = {{QStringLiteral("request_id"), requestId},
                                {QStringLiteral("qr_uri"),
                                 QStringLiteral("maichat://auth/desktop?request_id=") + requestId},
                                {QStringLiteral("qr_png_base64"), QString::fromLatin1(png_.toBase64())},
                                {QStringLiteral("display_code"), QStringLiteral("123456")},
                                {QStringLiteral("expires_in"), 120}};
                    if (oversizeQrResponse)
                        response.insert(QStringLiteral("padding"), QString(70 * 1024, QLatin1Char('x')));
                } else if (headers.startsWith("post /v1/auth/desktop/claim ")) {
                    const QByteArray verifier = QByteArray::fromBase64(
                        body.value(QStringLiteral("verifier")).toString().toLatin1(),
                        QByteArray::Base64UrlEncoding);
                    const QByteArray expected = QCryptographicHash::hash(
                        verifier, QCryptographicHash::Sha256).toBase64(
                        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
                    verifierValid = verifier.size() == 32 &&
                        body.value(QStringLiteral("request_id")).toString() == requestId &&
                        expected == challenge.toLatin1();
                    if (deferClaimResponse) {
                        deferredClaimSocket = socket;
                        return;
                    }
                    status = claimStatus;
                    if (status == 200) {
                        response = approvedSession();
                    } else if (status == 202) {
                        response = {{QStringLiteral("status"), QStringLiteral("pending")},
                                    {QStringLiteral("retry_after"), 2}};
                    } else {
                        response = {{QStringLiteral("error"), status == 403 ?
                            (approvalRevoked ? QStringLiteral("Desktop login approval revoked") :
                                               QStringLiteral("Desktop login request denied")) :
                            QStringLiteral("test claim failure")}};
                    }
                } else if (headers.startsWith("post /v1/auth/refresh ")) {
                    ++refreshCount;
                    validRefreshToken = body.value(QStringLiteral("refresh_token")).toString() ==
                                        QString(43, QLatin1Char('R'));
                    status = refreshStatus;
                    if (status == 200) {
                        response = {{QStringLiteral("access_token"), QString(43, QLatin1Char('B'))},
                                    {QStringLiteral("refresh_token"), QString(43, QLatin1Char('S'))},
                                    {QStringLiteral("expires_in"), 900},
                                    {QStringLiteral("device_slot"), testPlatformName()},
                                    {QStringLiteral("user"), QJsonObject{
                                        {QStringLiteral("im_user_id"), QStringLiteral("im-auth-test")}}}};
                    } else {
                        response = {{QStringLiteral("error"), QStringLiteral("temporary network failure")}};
                    }
                } else if (headers.startsWith("get /v1/im/credential ")) {
                    ++credentialRequestCount;
                    lastCredentialBearer = headers.contains("authorization: bearer " +
                        QByteArray(43, refreshCount > 0 ? 'b' : 'a'));
                    if ((firstCredentialUnauthorized && refreshCount == 0) ||
                        imCredentialStatus != 200) {
                        status = imCredentialStatus == 200 ? 401 : imCredentialStatus;
                        response = {{QStringLiteral("error"), QStringLiteral("IM unavailable")}};
                    } else {
                        response = {{QStringLiteral("sdk_app_id"), 1600148979},
                                    {QStringLiteral("user_id"),
                                     credentialUserMismatch ? QStringLiteral("wrong-user") :
                                                              QStringLiteral("im-auth-test")},
                                    {QStringLiteral("user_sig"), QStringLiteral("server-signed-user-sig")},
                                    {QStringLiteral("expires_at"),
                                     double(QDateTime::currentDateTimeUtc().addDays(7).toSecsSinceEpoch())}};
                    }
                } else if (headers.startsWith("get /v1/auth/me ")) {
                    ++meRequestCount;
                    meRequestNoStore = headers.contains("cache-control: no-store");
                    status = meStatus;
                    if (status == 200) {
                        response = {{QStringLiteral("user"), QJsonObject{
                                        {QStringLiteral("im_user_id"), QStringLiteral("im-auth-test")}}},
                                    {QStringLiteral("device_slot"), testPlatformName()}};
                    } else if (status == 401) {
                        response = {{QStringLiteral("error"),
                                     QStringLiteral("Session replaced by another device")},
                                    {QStringLiteral("code"), QStringLiteral("SESSION_REPLACED")}};
                    }
                } else if (headers.startsWith("post /v1/auth/logout ")) {
                    ++logoutRequestCount;
                    response = {{QStringLiteral("logged_out"), true}};
                } else if (headers.startsWith("post /v1/auth/desktop/cancel ")) {
                    ++cancelRequestCount;
                    cancelHadProof = body.value(QStringLiteral("request_id")).toString() ==
                                         requestId &&
                                     body.value(QStringLiteral("verifier")).toString().size() == 43;
                    response = {{QStringLiteral("status"), QStringLiteral("canceled")}};
                } else if (headers.startsWith("get /v1/model/seedance-credential ")) {
                    ++arkCredentialRequestCount;
                    if (modelAccessDenied) {
                        status = 403;
                        response = {{QStringLiteral("error"),
                                     QStringLiteral("Model access is not enabled for this account")}};
                    } else {
                        response = {{QStringLiteral("api_key"),
                                     QStringLiteral("ark-temporary-key-%1")
                                         .arg(arkCredentialRequestCount)},
                                    {QStringLiteral("expires_at"), double(
                                         QDateTime::currentDateTimeUtc()
                                             .addSecs(modelCredentialExpiresIn).toSecsSinceEpoch())}};
                    }
                } else if (headers.startsWith("get /v1/model/bailian-credential ")) {
                    ++bailianCredentialRequestCount;
                    if (modelAccessDenied) {
                        status = 403;
                        response = {{QStringLiteral("error"),
                                     QStringLiteral("Model access is not enabled for this account")}};
                    } else {
                        response = {{QStringLiteral("api_key"),
                                     invalidBailianKey ? QStringLiteral("wrong-key") :
                                     QStringLiteral("st-temporary-key-%1")
                                         .arg(bailianCredentialRequestCount)},
                                    {QStringLiteral("wan_workspace_id"),
                                     invalidWorkspace ? QStringLiteral("bad-space") :
                                                        QStringLiteral("ws-test")},
                                    {QStringLiteral("expires_at"), double(
                                         QDateTime::currentDateTimeUtc()
                                             .addSecs(modelCredentialExpiresIn).toSecsSinceEpoch())}};
                    }
                } else {
                    status = 404;
                }
                const QByteArray payload = QJsonDocument(response).toJson(QJsonDocument::Compact);
                const QByteArray redirectHeader = status == 302 ?
                    QByteArray("Location: /v1/im/credential\r\n") : QByteArray();
                socket->write("HTTP/1.1 " + QByteArray::number(status) +
                              " OK\r\nContent-Type: application/json\r\nContent-Length: " +
                              QByteArray::number(payload.size()) +
                              "\r\n" + redirectHeader + "Connection: close\r\n\r\n" + payload);
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
        server_.listen(QHostAddress::LocalHost);
    }

    QUrl url() const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/v1/").arg(server_.serverPort()));
    }
    QJsonObject approvedSession() const {
        return {{QStringLiteral("access_token"), QString(43, QLatin1Char('A'))},
                {QStringLiteral("refresh_token"), QString(43, QLatin1Char('R'))},
                {QStringLiteral("expires_in"), initialAccessExpiresIn},
                {QStringLiteral("device_slot"), testPlatformName()},
                {QStringLiteral("user"), QJsonObject{
                    {QStringLiteral("im_user_id"), QStringLiteral("im-auth-test")}}}};
    }
    void approveDeferredClaim() {
        if (!deferredClaimSocket) return;
        const QByteArray payload = QJsonDocument(approvedSession()).toJson(QJsonDocument::Compact);
        deferredClaimSocket->write(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
            QByteArray::number(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload);
        deferredClaimSocket->disconnectFromHost();
        deferredClaimSocket = nullptr;
        deferClaimResponse = false;
    }
    QString challenge;
    QString platform;
    int qrRequestCount = 0;
    bool verifierValid = false;
    int claimStatus = 202;
    bool approvalRevoked = false;
    bool credentialUserMismatch = false;
    bool firstCredentialUnauthorized = false;
    int imCredentialStatus = 200;
    bool deferClaimResponse = false;
    QTcpSocket* deferredClaimSocket = nullptr;
    int initialAccessExpiresIn = 900;
    int refreshCount = 0;
    int refreshStatus = 200;
    bool validRefreshToken = false;
    bool lastCredentialBearer = false;
    bool oversizeQrResponse = false;
    int credentialRequestCount = 0;
    int meStatus = 200;
    int meRequestCount = 0;
    bool meRequestNoStore = false;
    int logoutRequestCount = 0;
    int cancelRequestCount = 0;
    bool cancelHadProof = false;
    int arkCredentialRequestCount = 0;
    int bailianCredentialRequestCount = 0;
    bool modelAccessDenied = false;
    int modelCredentialExpiresIn = 300;
    bool invalidBailianKey = false;
    bool invalidWorkspace = false;
    const QString requestId = QStringLiteral("12345678-1234-1234-1234-123456789abc");

private:
    QTcpServer server_;
    QByteArray png_;
};

}  // namespace

class LoginDialogTest : public QObject {
    Q_OBJECT

private slots:
    void liveProductionAuthHealthCheck();
    void liveProductionQrCreationAndCancellation();
    void usesDesktopLandscapeDefaultSize();
    void showsQrLoginHierarchyWithoutManualUserId();
    void displaysServerQrAndSendsMatchingVerifier();
    void replacingQrCancelsPreviousRequest();
    void approvedQrUsesServerSignedCredential();
    void rejectsMismatchedServerCredential();
    void refreshesShortLivedAccessToken();
    void retriesCredentialAfterAccessIsRejected();
    void showsDeniedAndExpiredStates();
    void distinguishesRevokedApprovalFromExplicitDenial();
    void detectsReplacedSessionDuringForegroundCheck();
    void logoutRevokesServerSessionAndClearsMemory();
    void refusesOversizedQrResponseAndAuthRedirect();
    void refreshesTemporaryModelCredentialsThroughLiveProviders();
    void refusesMalformedTemporaryModelCredentials();
    void retriesStoredSessionAfterTransientRestoreFailure();
    void revokesLateApprovedClaimBeforeReplacingQr();
    void waitsForLateClaimBeforeClosing();
    void cancelAfterApprovalRevokesAdoptedSession();
};

// 显式启用时，用正式账号的无凭据只读端点验收最终链接的 curl/OpenSSL。
// 不创建二维码、不修改账号状态，也不触发模型或付费任务。
void LoginDialogTest::liveProductionAuthHealthCheck() {
    if (!qEnvironmentVariableIsSet("MAICHAT_AUTH_LIVE_PROBE")) QSKIP("Live probe is opt-in");
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    const DesktopCurlAuthResult result = DesktopCurlAuthTransport::perform(
        QUrl(QStringLiteral("https://ichat.life/api/v1/auth/me")), "GET", {}, {}, cancelled);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.status, 401);
    QVERIFY(!result.body.isEmpty());
}

// Opt-in production check: create one QR request, validate its PNG, then cancel
// that request before asserting the returned content. Never log the verifier or QR.
void LoginDialogTest::liveProductionQrCreationAndCancellation() {
    if (!qEnvironmentVariableIsSet("MAICHAT_AUTH_LIVE_QR")) QSKIP("Live QR probe is opt-in");
    DesktopAuthClient auth(nullptr, QUrl(QStringLiteral("https://ichat.life/api/v1/")), false);
    DesktopLoginRequest login;
    QString error;
    bool completed = false;
    auth.createLoginRequest([&](const DesktopLoginRequest& result, const QString& failure) {
        login = result;
        error = failure;
        completed = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(completed, 20000);
    if (!login.requestId.isEmpty()) {
        bool cancelled = false;
        auth.cancelLoginRequest(login.requestId, login.verifier, [&] { cancelled = true; });
        QTRY_VERIFY_WITH_TIMEOUT(cancelled, 20000);
    }
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!login.requestId.isEmpty());
    QVERIFY(!login.verifier.isEmpty());
    QVERIFY(login.expiresIn > 0);
    QImage qr;
    QVERIFY(qr.loadFromData(login.qrPng, "PNG"));
    QVERIFY(qr.width() > 0 && qr.height() > 0);
}

void LoginDialogTest::usesDesktopLandscapeDefaultSize() {
    LoginDialog dialog;
    if (qEnvironmentVariableIsSet("MAICHAT_CAPTURE_LOGIN")) {
        dialog.show();
        QCoreApplication::processEvents();
        dialog.grab().save(qEnvironmentVariable("MAICHAT_CAPTURE_LOGIN"));
    }

    QVERIFY(dialog.width() > dialog.height());
    QVERIFY(dialog.minimumWidth() > dialog.minimumHeight());
}

void LoginDialogTest::showsQrLoginHierarchyWithoutManualUserId() {
    LoginDialog dialog;

    auto* loginPanel = dialog.findChild<QWidget*>(QStringLiteral("loginPanel"));
    auto* logo = dialog.findChild<QLabel*>(QStringLiteral("loginLogo"));
    auto* title = dialog.findChild<QLabel*>(QStringLiteral("welcomeTitle"));
    auto* qr = dialog.findChild<QLabel*>(QStringLiteral("loginQr"));
    auto* status = dialog.findChild<QLabel*>(QStringLiteral("loginStatus"));
    auto* retryButton = dialog.findChild<QPushButton*>(QStringLiteral("loginRetryButton"));
    auto* cancelButton = dialog.findChild<QPushButton*>(QStringLiteral("loginCancelButton"));

    QVERIFY(loginPanel != nullptr);
    QVERIFY(logo != nullptr);
    QVERIFY(title != nullptr);
    QVERIFY(qr != nullptr);
    QVERIFY(status != nullptr);
    QVERIFY(retryButton != nullptr);
    QVERIFY(cancelButton != nullptr);
    QVERIFY(dialog.findChild<QWidget*>(QStringLiteral("introPane")) == nullptr);
    QCOMPARE(dialog.windowTitle(), QStringLiteral("MaiChat"));
    const QPixmap logoPixmap = logo->pixmap(Qt::ReturnByValue);
    QVERIFY(!logoPixmap.isNull());
    QCOMPARE(title->text(), QStringLiteral("欢迎使用 MaiChat"));
    QCOMPARE(status->text(), QStringLiteral("正在生成登录二维码"));
    QVERIFY(!retryButton->isEnabled());
    QCOMPARE(cancelButton->text(), QStringLiteral("取消登录"));
    QVERIFY(dialog.findChild<QLineEdit*>() == nullptr);
}

void LoginDialogTest::displaysServerQrAndSendsMatchingVerifier() {
    FakeQrServer server;
    DesktopAuthClient auth(nullptr, server.url(), false);
    LoginDialog dialog(&auth);
    auto* status = dialog.findChild<QLabel*>(QStringLiteral("loginStatus"));
    auto* code = dialog.findChild<QLabel*>(QStringLiteral("loginCode"));
    auto* qr = dialog.findChild<QLabel*>(QStringLiteral("loginQr"));
    auto* cancel = dialog.findChild<QPushButton*>(QStringLiteral("loginCancelButton"));
    QSignalSpy rejected(&dialog, &QDialog::rejected);

    dialog.show();
    dialog.start();
    QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("等待手机扫码并确认"), 5000);
    QCOMPARE(server.platform, testPlatformName());
    QCOMPARE(server.challenge.size(), 43);
    QCOMPARE(code->text(), QStringLiteral("核对码  123456"));
    QVERIFY(!qr->pixmap(Qt::ReturnByValue).isNull());
    if (qEnvironmentVariableIsSet("MAICHAT_CAPTURE_QR_LOGIN"))
        dialog.grab().save(qEnvironmentVariable("MAICHAT_CAPTURE_QR_LOGIN"));
    QTRY_VERIFY_WITH_TIMEOUT(server.verifierValid, 5000);
    QTest::mouseClick(cancel, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(rejected.size(), 1, 5000);
    QCOMPARE(server.cancelRequestCount, 1);
    QVERIFY(server.cancelHadProof);
}

void LoginDialogTest::replacingQrCancelsPreviousRequest() {
    FakeQrServer server;
    DesktopAuthClient auth(nullptr, server.url(), false);
    LoginDialog dialog(&auth);
    auto* status = dialog.findChild<QLabel*>(QStringLiteral("loginStatus"));
    auto* replace = dialog.findChild<QPushButton*>(QStringLiteral("loginRetryButton"));
    dialog.show();
    dialog.start();
    QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("等待手机扫码并确认"), 5000);
    replace->click();
    QTRY_VERIFY_WITH_TIMEOUT(server.cancelRequestCount >= 1, 5000);
    QVERIFY(server.cancelHadProof);
    QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("等待手机扫码并确认"), 5000);
    dialog.reject();
    QTRY_VERIFY_WITH_TIMEOUT(server.cancelRequestCount >= 2, 5000);
}

void LoginDialogTest::approvedQrUsesServerSignedCredential() {
    FakeQrServer server;
    server.claimStatus = 200;
    DesktopAuthClient auth(nullptr, server.url(), false);
    LoginDialog dialog(&auth);
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    dialog.show();
    dialog.start();
    QTRY_COMPARE_WITH_TIMEOUT(accepted.size(), 1, 5000);
    QVERIFY(server.verifierValid);
    QCOMPARE(auth.authenticatedUserId(), QStringLiteral("im-auth-test"));
    QCOMPARE(auth.imCredential().userSig, QStringLiteral("server-signed-user-sig"));
}

void LoginDialogTest::rejectsMismatchedServerCredential() {
    FakeQrServer server;
    server.claimStatus = 200;
    server.credentialUserMismatch = true;
    DesktopAuthClient auth(nullptr, server.url(), false);
    LoginDialog dialog(&auth);
    auto* status = dialog.findChild<QLabel*>(QStringLiteral("loginStatus"));
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    dialog.show();
    dialog.start();
    QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("连接聊天服务失败"), 5000);
    QCOMPARE(accepted.size(), 0);
    QVERIFY(!auth.imCredential().isValid());
}

void LoginDialogTest::refreshesShortLivedAccessToken() {
    FakeQrServer server;
    server.claimStatus = 200;
    server.initialAccessExpiresIn = 2;
    DesktopAuthClient auth(nullptr, server.url(), false);
    LoginDialog dialog(&auth);
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    dialog.show();
    dialog.start();
    QTRY_COMPARE_WITH_TIMEOUT(accepted.size(), 1, 7000);
    QTRY_COMPARE_WITH_TIMEOUT(server.refreshCount, 1, 5000);
    QVERIFY(server.validRefreshToken);
    bool completed = false;
    auth.fetchImCredential([&completed](bool ok, const QString&) { completed = ok; });
    QTRY_VERIFY_WITH_TIMEOUT(completed, 5000);
    QVERIFY(server.lastCredentialBearer);
}

void LoginDialogTest::retriesCredentialAfterAccessIsRejected() {
    FakeQrServer server;
    server.claimStatus = 200;
    server.firstCredentialUnauthorized = true;
    DesktopAuthClient auth(nullptr, server.url(), false);
    LoginDialog dialog(&auth);
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    dialog.show();
    dialog.start();
    QTRY_COMPARE_WITH_TIMEOUT(accepted.size(), 1, 5000);
    QCOMPARE(server.refreshCount, 1);
    QVERIFY(server.validRefreshToken);
    QVERIFY(server.lastCredentialBearer);
}

void LoginDialogTest::showsDeniedAndExpiredStates() {
    for (const auto& expected : {
             qMakePair(403, QStringLiteral("手机拒绝了这次登录")),
             qMakePair(410, QStringLiteral("二维码已过期")),
             qMakePair(503, QStringLiteral("网络连接失败"))}) {
        FakeQrServer server;
        server.claimStatus = expected.first;
        DesktopAuthClient auth(nullptr, server.url(), false);
        LoginDialog dialog(&auth);
        auto* status = dialog.findChild<QLabel*>(QStringLiteral("loginStatus"));
        dialog.show();
        dialog.start();
        QTRY_COMPARE_WITH_TIMEOUT(status->text(), expected.second, 5000);
    }
}

void LoginDialogTest::distinguishesRevokedApprovalFromExplicitDenial() {
    FakeQrServer server;
    server.claimStatus = 403;
    server.approvalRevoked = true;
    DesktopAuthClient auth(nullptr, server.url(), false);
    LoginDialog dialog(&auth);
    auto* status = dialog.findChild<QLabel*>(QStringLiteral("loginStatus"));
    dialog.show();
    dialog.start();
    QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("手机授权已失效"), 5000);
}

void LoginDialogTest::detectsReplacedSessionDuringForegroundCheck() {
    FakeQrServer server;
    server.claimStatus = 200;
    server.meStatus = 401;
    DesktopAuthClient auth(nullptr, server.url(), false);
    LoginDialog dialog(&auth);
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    dialog.show();
    dialog.start();
    QTRY_COMPARE_WITH_TIMEOUT(accepted.size(), 1, 5000);

    QString invalidationReason;
    auth.setSessionInvalidatedHandler([&invalidationReason](const QString& reason) {
        invalidationReason = reason;
    });
    auth.startSessionMonitoring();
    auth.checkSessionNow();
    QTRY_VERIFY_WITH_TIMEOUT(!invalidationReason.isEmpty(), 5000);
    QVERIFY(invalidationReason.contains(QStringLiteral("替换")));
    QVERIFY(server.meRequestNoStore);
    QVERIFY(auth.authenticatedUserId().isEmpty());
    QVERIFY(!auth.imCredential().isValid());
}

void LoginDialogTest::logoutRevokesServerSessionAndClearsMemory() {
    FakeQrServer server;
    server.claimStatus = 200;
    DesktopAuthClient auth(nullptr, server.url(), false);
    LoginDialog dialog(&auth);
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    dialog.show();
    dialog.start();
    QTRY_COMPARE_WITH_TIMEOUT(accepted.size(), 1, 5000);

    bool finished = false;
    bool revoked = false;
    auth.logout([&](bool ok, const QString&) {
        revoked = ok;
        finished = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(finished, 5000);
    QVERIFY(revoked);
    QCOMPARE(server.logoutRequestCount, 1);
    QVERIFY(auth.authenticatedUserId().isEmpty());
    QVERIFY(!auth.imCredential().isValid());
}

void LoginDialogTest::refusesOversizedQrResponseAndAuthRedirect() {
    FakeQrServer oversize;
    oversize.oversizeQrResponse = true;
    DesktopAuthClient requestClient(nullptr, oversize.url(), false);
    bool requestDone = false;
    QString requestError;
    requestClient.createLoginRequest([&](const DesktopLoginRequest&, const QString& error) {
        requestError = error;
        requestDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(requestDone, 5000);
    QVERIFY(requestError.contains(QStringLiteral("数据过大")));

    FakeQrServer redirected;
    redirected.claimStatus = 200;
    redirected.meStatus = 302;
    DesktopAuthClient auth(nullptr, redirected.url(), false);
    LoginDialog dialog(&auth);
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    dialog.show();
    dialog.start();
    QTRY_COMPARE_WITH_TIMEOUT(accepted.size(), 1, 5000);
    const int credentialRequests = redirected.credentialRequestCount;
    auth.startSessionMonitoring();
    auth.checkSessionNow();
    QTRY_VERIFY_WITH_TIMEOUT(redirected.meRequestCount > 0, 5000);
    QTest::qWait(100);
    QCOMPARE(redirected.credentialRequestCount, credentialRequests);
    QCOMPARE(auth.authenticatedUserId(), QStringLiteral("im-auth-test"));
}

void LoginDialogTest::refreshesTemporaryModelCredentialsThroughLiveProviders() {
    FakeQrServer server;
    server.claimStatus = 200;
    DesktopAuthClient auth(nullptr, server.url(), false);
    LoginDialog dialog(&auth);
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    dialog.show();
    dialog.start();
    QTRY_COMPARE_WITH_TIMEOUT(accepted.size(), 1, 5000);

    const auto arkProvider = auth.arkApiKeyProvider();
    const auto bailianProvider = auth.bailianCredentialProvider();
    auth.startModelCredentialMonitoring();
    QTRY_COMPARE_WITH_TIMEOUT(arkProvider(), QStringLiteral("ark-temporary-key-1"), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(bailianProvider().first,
                              QStringLiteral("st-temporary-key-1"), 5000);
    QCOMPARE(bailianProvider().second, QStringLiteral("ws-test"));
    auth.refreshModelCredentialsNow(true);
    QTRY_COMPARE_WITH_TIMEOUT(arkProvider(), QStringLiteral("ark-temporary-key-2"), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(bailianProvider().first,
                              QStringLiteral("st-temporary-key-2"), 5000);

    server.modelAccessDenied = true;
    auth.refreshModelCredentialsNow(true);
    QTRY_VERIFY_WITH_TIMEOUT(arkProvider().isEmpty() &&
                              bailianProvider().first.isEmpty(), 5000);

    bool finished = false;
    auth.logout([&finished](bool, const QString&) { finished = true; });
    QTRY_VERIFY_WITH_TIMEOUT(finished, 5000);
    QVERIFY(arkProvider().isEmpty());
    QVERIFY(bailianProvider().first.isEmpty());
}

void LoginDialogTest::refusesMalformedTemporaryModelCredentials() {
    FakeQrServer server;
    server.claimStatus = 200;
    server.modelCredentialExpiresIn = 600;
    server.invalidBailianKey = true;
    server.invalidWorkspace = true;
    DesktopAuthClient auth(nullptr, server.url(), false);
    LoginDialog dialog(&auth);
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    dialog.show();
    dialog.start();
    QTRY_COMPARE_WITH_TIMEOUT(accepted.size(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(server.arkCredentialRequestCount > 0 &&
                              server.bailianCredentialRequestCount > 0, 5000);
    QTest::qWait(200);
    QVERIFY(auth.arkApiKeyProvider()().isEmpty());
    QVERIFY(auth.bailianCredentialProvider()().first.isEmpty());

    server.modelCredentialExpiresIn = 300;
    server.invalidBailianKey = false;
    server.invalidWorkspace = false;
    auth.refreshModelCredentialsNow(true);
    QTRY_VERIFY_WITH_TIMEOUT(!auth.arkApiKeyProvider()().isEmpty(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!auth.bailianCredentialProvider()().first.isEmpty(), 5000);
}

void LoginDialogTest::retriesStoredSessionAfterTransientRestoreFailure() {
    FakeQrServer server;
    DesktopAuthClient auth(nullptr, server.url(), false);
    const QJsonObject session = {
        {QStringLiteral("access_token"), QString(43, QLatin1Char('A'))},
        {QStringLiteral("refresh_token"), QString(43, QLatin1Char('R'))},
        {QStringLiteral("expires_in"), 900},
        {QStringLiteral("device_slot"), testPlatformName()},
        {QStringLiteral("user"), QJsonObject{
            {QStringLiteral("im_user_id"), QStringLiteral("im-auth-test")}}}
    };
    bool adopted = false;
    auth.adoptApprovedSession(session, [&adopted](bool ok, const QString&) {
        adopted = ok;
    });
    QTRY_VERIFY_WITH_TIMEOUT(adopted, 5000);

    server.refreshStatus = 503;
    LoginDialog dialog(&auth);
    auto* status = dialog.findChild<QLabel*>(QStringLiteral("loginStatus"));
    auto* retry = dialog.findChild<QPushButton*>(QStringLiteral("loginRetryButton"));
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    dialog.show();
    dialog.start();
    QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("恢复登录失败"), 5000);
    QCOMPARE(retry->text(), QStringLiteral("重试恢复登录"));
    QCOMPARE(server.qrRequestCount, 0);
    QVERIFY(auth.hasRestorableSession());

    server.refreshStatus = 200;
    retry->click();
    QTRY_COMPARE_WITH_TIMEOUT(accepted.size(), 1, 5000);
    QCOMPARE(server.qrRequestCount, 0);
}

void LoginDialogTest::revokesLateApprovedClaimBeforeReplacingQr() {
    FakeQrServer server;
    server.claimStatus = 200;
    server.deferClaimResponse = true;
    DesktopAuthClient auth(nullptr, server.url(), false);
    LoginDialog dialog(&auth);
    auto* status = dialog.findChild<QLabel*>(QStringLiteral("loginStatus"));
    auto* replace = dialog.findChild<QPushButton*>(QStringLiteral("loginRetryButton"));
    dialog.show();
    dialog.start();
    QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("等待手机扫码并确认"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(server.deferredClaimSocket != nullptr, 5000);

    replace->click();
    QCOMPARE(server.qrRequestCount, 1);
    server.approveDeferredClaim();
    QTRY_COMPARE_WITH_TIMEOUT(server.logoutRequestCount, 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(server.qrRequestCount, 2, 5000);
    QVERIFY(auth.authenticatedUserId().isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("等待手机扫码并确认"), 5000);
    QSignalSpy rejected(&dialog, &QDialog::rejected);
    dialog.reject();
    QTRY_COMPARE_WITH_TIMEOUT(rejected.size(), 1, 5000);
}

void LoginDialogTest::waitsForLateClaimBeforeClosing() {
    FakeQrServer server;
    server.claimStatus = 200;
    server.deferClaimResponse = true;
    DesktopAuthClient auth(nullptr, server.url(), false);
    LoginDialog dialog(&auth);
    auto* status = dialog.findChild<QLabel*>(QStringLiteral("loginStatus"));
    QSignalSpy rejected(&dialog, &QDialog::rejected);
    dialog.show();
    dialog.start();
    QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("等待手机扫码并确认"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(server.deferredClaimSocket != nullptr, 5000);

    dialog.reject();
    QCOMPARE(rejected.size(), 0);
    server.approveDeferredClaim();
    QTRY_COMPARE_WITH_TIMEOUT(server.logoutRequestCount, 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(rejected.size(), 1, 5000);
    QVERIFY(auth.authenticatedUserId().isEmpty());
}

void LoginDialogTest::cancelAfterApprovalRevokesAdoptedSession() {
    FakeQrServer server;
    server.claimStatus = 200;
    server.imCredentialStatus = 503;
    DesktopAuthClient auth(nullptr, server.url(), false);
    LoginDialog dialog(&auth);
    auto* status = dialog.findChild<QLabel*>(QStringLiteral("loginStatus"));
    auto* qr = dialog.findChild<QLabel*>(QStringLiteral("loginQr"));
    QSignalSpy rejected(&dialog, &QDialog::rejected);
    dialog.show();
    dialog.start();
    QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("连接聊天服务失败"), 5000);
    QVERIFY(qr->pixmap(Qt::ReturnByValue).isNull());
    QCOMPARE(auth.authenticatedUserId(), QStringLiteral("im-auth-test"));

    dialog.reject();
    QTRY_COMPARE_WITH_TIMEOUT(server.logoutRequestCount, 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(rejected.size(), 1, 5000);
    QVERIFY(auth.authenticatedUserId().isEmpty());
    QVERIFY(!auth.imCredential().isValid());
}

QTEST_MAIN(LoginDialogTest)
#include "LoginDialogTest.moc"
