// 文件作用：验证桌面端附件下载结果的交付记录、失败回退和账号切换隔离。
// 运行边界：由测试进程驱动实际组件或替身，不作为正式 App 的用户入口。
#include <QCryptographicHash>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTest>
#include <QUrl>
#include <QUuid>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509v3.h>

#include <memory>
#include <utility>

#include "diagnostics/RemoteDiagnosticsEvidence.h"
#include "im/TimSdkApi.h"
#include "im/TimSdkRemoteIMClient.h"

using namespace RemoteDiagnostics;

namespace {

// 只实现下载用例真正要用到的部分；其余返回成功即可。
class FakeApi final : public TimSdkApi {
public:
    int init(quint64, const QString&) override { return 0; }
    void uninit() override {}
    int login(const QString&, const QString&, TimSdkCompletion completion) override {
        if (completion) completion(0, QString(), QString());
        return 0;
    }
    int logout(TimSdkCompletion completion) override {
        if (completion) completion(0, QString(), QString());
        return 0;
    }
    int sendMessage(const QString&, int, const QString&, TimSdkCompletion completion) override {
        if (completion) completion(0, QString(), QString());
        return 0;
    }
    int getConversationList(TimSdkCompletion completion) override {
        if (completion) completion(0, QString(), QStringLiteral("[]"));
        return 0;
    }
    int getFriendList(TimSdkCompletion completion) override {
        if (completion) completion(0, QString(), QStringLiteral("[]"));
        return 0;
    }
    int deleteFriend(const QString&, TimSdkCompletion completion) override {
        if (completion) completion(0, QString(), QString());
        return 0;
    }
    int deleteConversation(const QString&, int, TimSdkCompletion completion) override {
        if (completion) completion(0, QString(), QString());
        return 0;
    }
    int getMessageList(const QString&, int, const QString&, TimSdkCompletion completion) override {
        if (completion) completion(0, QString(), QStringLiteral("[]"));
        return 0;
    }
    void addReceiveMessageCallback(TimSdkReceiveMessagesCallback callback) override {
        receive = std::move(callback);
    }
    void removeReceiveMessageCallback() override { receive = nullptr; }
    bool isReady() const override { return true; }
    QString diagnosticError() const override { return {}; }

    void deliver(const QJsonArray& messages) {
        if (receive) {
            receive(QString::fromUtf8(QJsonDocument(messages).toJson(QJsonDocument::Compact)));
        }
    }
    TimSdkReceiveMessagesCallback receive;
};

struct TestIdentity {
    QByteArray certificate;
    QByteArray privateKey;
};

// 证书和私钥仅在测试进程内生成，不把测试私钥提交到仓库。
TestIdentity createTestIdentity()
{
    using KeyContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
    using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;
    using Extension = std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)>;
    using Buffer = std::unique_ptr<BIO, decltype(&BIO_free)>;
    KeyContext context(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    if (!context || EVP_PKEY_keygen_init(context.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) <= 0) return {};
    EVP_PKEY* rawKey = nullptr;
    if (EVP_PKEY_keygen(context.get(), &rawKey) <= 0) return {};
    Key key(rawKey, EVP_PKEY_free);
    Certificate certificate(X509_new(), X509_free);
    if (!certificate || X509_set_version(certificate.get(), 2) <= 0 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) <= 0 ||
        !X509_gmtime_adj(X509_get_notBefore(certificate.get()), -3600) ||
        !X509_gmtime_adj(X509_get_notAfter(certificate.get()), 24 * 3600) ||
        X509_set_pubkey(certificate.get(), key.get()) <= 0) return {};
    X509_NAME* name = X509_get_subject_name(certificate.get());
    if (!name || X509_NAME_add_entry_by_txt(
            name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) <= 0 ||
        X509_set_issuer_name(certificate.get(), name) <= 0) return {};
    X509V3_CTX extensionContext;
    X509V3_set_ctx(&extensionContext, certificate.get(), certificate.get(), nullptr, nullptr, 0);
    const auto addExtension = [&](int type, const char* value) {
        QByteArray writableValue(value);
        Extension extension(
            X509V3_EXT_conf_nid(nullptr, &extensionContext, type, writableValue.data()),
            X509_EXTENSION_free);
        return extension && X509_add_ext(certificate.get(), extension.get(), -1) > 0;
    };
    if (!addExtension(NID_basic_constraints, "critical,CA:TRUE") ||
        !addExtension(NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1") ||
        X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0) return {};
    Buffer certificateBuffer(BIO_new(BIO_s_mem()), BIO_free);
    Buffer keyBuffer(BIO_new(BIO_s_mem()), BIO_free);
    if (!certificateBuffer || !keyBuffer ||
        PEM_write_bio_X509(certificateBuffer.get(), certificate.get()) <= 0 ||
        PEM_write_bio_PrivateKey(keyBuffer.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) <= 0)
        return {};
    BUF_MEM* certificateBytes = nullptr;
    BUF_MEM* keyBytes = nullptr;
    BIO_get_mem_ptr(certificateBuffer.get(), &certificateBytes);
    BIO_get_mem_ptr(keyBuffer.get(), &keyBytes);
    if (!certificateBytes || !keyBytes) return {};
    return {QByteArray(certificateBytes->data, int(certificateBytes->length)),
            QByteArray(keyBytes->data, int(keyBytes->length))};
}

// 本地 HTTPS 服务让测试真正经过 TLS、HTTP、libcurl 与文件缓存全链路。
class LocalHttpsServer final : public QTcpServer {
public:
    LocalHttpsServer(QByteArray certificate, QByteArray privateKey)
        : certificate_(std::move(certificate)), privateKey_(std::move(privateKey)) {}

    struct Route {
        QByteArray body;
        int status = 200;
        bool holdOpen = false;
        bool headersOnly = false;
        qint64 advertisedLength = -1;
    };

    bool start() { return listen(QHostAddress::LocalHost); }
    void addRoute(const QString& path, Route route) { routes_.insert(path, std::move(route)); }
    QString url(const QString& name) const {
        return QStringLiteral("https://localhost:%1/%2").arg(serverPort()).arg(name);
    }
    int requestCount(const QString& path) const { return requests_.count(path); }
    void release(const QString& path) {
        for (const Pending& pending : pending_) {
            if (pending.path == path && pending.socket && pending.socket->state() == QAbstractSocket::ConnectedState)
                respond(pending.socket, path);
        }
        pending_.clear();
    }

protected:
    void incomingConnection(qintptr descriptor) override {
        auto* socket = new QSslSocket(this);
        socket->setLocalCertificate(QSslCertificate(certificate_, QSsl::Pem));
        socket->setPrivateKey(QSslKey(privateKey_, QSsl::Rsa, QSsl::Pem));
        socket->setPeerVerifyMode(QSslSocket::VerifyNone);
        if (!socket->setSocketDescriptor(descriptor)) {
            delete socket;
            return;
        }
        connect(socket, &QSslSocket::readyRead, this, [this, socket] {
            QByteArray& incoming = buffers_[socket];
            incoming += socket->readAll();
            const int end = incoming.indexOf("\r\n\r\n");
            if (end < 0) return;
            const QByteArray firstLine = incoming.left(incoming.indexOf("\r\n"));
            incoming.clear();
            const QList<QByteArray> parts = firstLine.split(' ');
            if (parts.size() < 2 || parts.at(0) != "GET") {
                socket->disconnectFromHost();
                return;
            }
            const QString path = QString::fromUtf8(parts.at(1));
            requests_.append(path);
            if (routes_.value(path).holdOpen) {
                pending_.append({socket, path});
                return;
            }
            respond(socket, path);
        });
        socket->startServerEncryption();
    }

private:
    struct Pending {
        QPointer<QSslSocket> socket;
        QString path;
    };

    void respond(QSslSocket* socket, const QString& path) {
        const Route route = routes_.value(path, Route{QByteArray("missing"), 404});
        const qint64 length = route.advertisedLength >= 0 ? route.advertisedLength : route.body.size();
        const QByteArray header =
            QStringLiteral("HTTP/1.1 %1 %2\r\nContent-Length: %3\r\n"
                           "Content-Type: application/octet-stream\r\nConnection: close\r\n\r\n")
                .arg(route.status)
                .arg(route.status == 200 ? QStringLiteral("OK") : QStringLiteral("Failure"))
                .arg(length)
                .toUtf8();
        socket->write(header);
        if (!route.headersOnly) socket->write(route.body);
        socket->flush();
        if (!route.headersOnly) socket->disconnectFromHost();
    }

    QByteArray certificate_;
    QByteArray privateKey_;
    QHash<QSslSocket*, QByteArray> buffers_;
    QHash<QString, Route> routes_;
    QList<QString> requests_;
    QList<Pending> pending_;
};

QString cachePathFor(const QString& url, const QString& fileName)
{
    const QByteArray hash = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex();
    QString suffix = QFileInfo(fileName).suffix();
    if (suffix.isEmpty()) suffix = QStringLiteral("bin");
    QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir dir(root);
    dir.mkpath(QStringLiteral("RemoteIMFiles"));
    return QDir(dir.filePath(QStringLiteral("RemoteIMFiles")))
        .filePath(QString::fromUtf8(hash) + "." + suffix);
}

QJsonArray fileMessage(const QString& url, const QString& fileName, const QString& msgId)
{
    return QJsonArray{QJsonObject{
        {QStringLiteral("message_is_from_self"), false},
        {QStringLiteral("message_sender"), QStringLiteral("phone-user")},
        {QStringLiteral("message_msg_id"), msgId},
        {QStringLiteral("message_server_time"), 1700000000},
        {QStringLiteral("message_elem_array"),
         QJsonArray{QJsonObject{{QStringLiteral("elem_type"), 4},
                                {QStringLiteral("file_elem_url"), url},
                                {QStringLiteral("file_elem_file_name"), fileName},
                                {QStringLiteral("file_elem_file_size"), 4}}}}}};
}

QJsonArray mediaMessage(const QJsonObject& elem, const QString& msgId)
{
    return QJsonArray{QJsonObject{
        {QStringLiteral("message_is_from_self"), false},
        {QStringLiteral("message_sender"), QStringLiteral("phone-user")},
        {QStringLiteral("message_msg_id"), msgId},
        {QStringLiteral("message_server_time"), 1700000000},
        {QStringLiteral("message_elem_array"), QJsonArray{elem}}}};
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

bool hasPhase(const QList<AttachmentEvent>& events, AttachmentPhase phase)
{
    for (const AttachmentEvent& e : events) {
        if (e.phase == phase) return true;
    }
    return false;
}

}  // namespace

class AttachmentDownloadEvidenceTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void normalDownloadIsRecordedAsDeliveredAndReachesTheConversation();
    void networkFailureIsRecordedAndNoMessageAppears();
    void untrustedCertificateIsRejected();
    void writeFailureIsNotRecordedAsDelivered();
    void directoryAtCachePathIsNotAFileHit();
    void lateDownloadAfterAnAccountSwitchIsDroppedNotDelivered();
    void oversizedResponseIsRejectedBeforeBody();
    void destroyingClientCancelsHeldDownload();
    void imageVoiceAndVideoCoverDownloadOverTls();

private:
    TestIdentity identity_;
};

void AttachmentDownloadEvidenceTest::initTestCase()
{
    // 别往真实用户目录里写缓存。
    QStandardPaths::setTestModeEnabled(true);
    identity_ = createTestIdentity();
    QVERIFY(!identity_.certificate.isEmpty());
    QVERIFY(!identity_.privateKey.isEmpty());
}

void AttachmentDownloadEvidenceTest::normalDownloadIsRecordedAsDeliveredAndReachesTheConversation()
{
    LocalHttpsServer server(identity_.certificate, identity_.privateKey);
    QVERIFY(server.start());
    auto api = std::make_unique<FakeApi>();
    FakeApi* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api), nullptr, identity_.certificate);

    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig"), nullptr);
    QSignalSpy live(&client, &RemoteIMClient::liveMessagesReceived);

    const QString name = QStringLiteral("remote-diagnostics-"
                                        "65de6748-3a4e-4d08-8ffd-bc78e1804ff9.json");
    const QString url = server.url(name);
    server.addRoute(QStringLiteral("/") + name, {QByteArray("{\"a\":1}")});
    QFile::remove(cachePathFor(url, name));
    fake->deliver(fileMessage(url, name, QStringLiteral("m-normal")));

    QVERIFY(live.wait(5000));
    const auto events = client.diagnosticsEvidence().exportFor(
        AccountTag{123456ULL, QStringLiteral("desktop-user")}, QStringLiteral("phone-user"));
    QVERIFY(hasPhase(events, AttachmentPhase::DownloadStarted));
    QVERIFY(hasPhase(events, AttachmentPhase::Delivered));
    QVERIFY(!hasPhase(events, AttachmentPhase::DownloadFailed));
    // 报告名要被反解成 requestId，普通附件才留空。
    QCOMPARE(events.last().requestId, QStringLiteral("65de6748-3a4e-4d08-8ffd-bc78e1804ff9"));
    const auto first = qvariant_cast<QList<RemoteIMMessage>>(live.at(0).at(0));
    QCOMPARE(first.size(), 1);
    QVERIFY(first.first().hasFile);
    QCOMPARE(readFile(first.first().file.localPath), QByteArray("{\"a\":1}"));

    // 相同 URL 的下一条消息必须命中缓存，不能再次向服务器发请求。
    fake->deliver(fileMessage(url, name, QStringLiteral("m-cached")));
    QTRY_COMPARE_WITH_TIMEOUT(live.count(), 2, 5000);
    QCOMPARE(server.requestCount(QStringLiteral("/") + name), 1);
    QVERIFY(hasPhase(client.diagnosticsEvidence().exportFor(
                         AccountTag{123456ULL, QStringLiteral("desktop-user")}, QStringLiteral("phone-user")),
                     AttachmentPhase::CacheHit));
}

// 下载失败时消息**整条不出现**——所以只能靠这条记录知道附件卡在哪。
void AttachmentDownloadEvidenceTest::networkFailureIsRecordedAndNoMessageAppears()
{
    LocalHttpsServer server(identity_.certificate, identity_.privateKey);
    QVERIFY(server.start());
    auto api = std::make_unique<FakeApi>();
    FakeApi* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api), nullptr, identity_.certificate);

    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig"), nullptr);
    QSignalSpy live(&client, &RemoteIMClient::liveMessagesReceived);

    // HTTPS 服务器真实返回 503，验证非成功状态不可交付。
    server.addRoute(QStringLiteral("/missing.json"), {QByteArray("unavailable"), 503});
    const QString url = server.url(QStringLiteral("missing.json"));
    QFile::remove(cachePathFor(url, QStringLiteral("missing.json")));
    fake->deliver(fileMessage(url, QStringLiteral("missing.json"), QStringLiteral("m-net")));

    QTRY_VERIFY_WITH_TIMEOUT(
        hasPhase(client.diagnosticsEvidence().exportFor(
                     AccountTag{123456ULL, QStringLiteral("desktop-user")},
                     QStringLiteral("phone-user")),
                 AttachmentPhase::DownloadFailed),
        5000);
    QCOMPARE(live.count(), 0);
    QCOMPARE(server.requestCount(QStringLiteral("/missing.json")), 1);
}

void AttachmentDownloadEvidenceTest::untrustedCertificateIsRejected()
{
    LocalHttpsServer server(identity_.certificate, identity_.privateKey);
    QVERIFY(server.start());
    server.addRoute(QStringLiteral("/untrusted.json"), {QByteArray("data")});
    auto api = std::make_unique<FakeApi>();
    FakeApi* fake = api.get();
    // 不注入测试 CA：真实证书校验必须拒绝这个临时自签证书。
    TimSdkRemoteIMClient client(std::move(api));
    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig"), nullptr);
    QSignalSpy live(&client, &RemoteIMClient::liveMessagesReceived);
    const QString url = server.url(QStringLiteral("untrusted.json"));
    const QString target = cachePathFor(url, QStringLiteral("untrusted.json"));
    QFile::remove(target);
    fake->deliver(fileMessage(url, QStringLiteral("untrusted.json"), QStringLiteral("m-cert")));

    QTRY_VERIFY_WITH_TIMEOUT(
        hasPhase(client.diagnosticsEvidence().exportFor(
                     AccountTag{123456ULL, QStringLiteral("desktop-user")},
                     QStringLiteral("phone-user")), AttachmentPhase::DownloadFailed),
        5000);
    QCOMPARE(live.count(), 0);
    QVERIFY(!QFileInfo::exists(target));
}

// 下载成功但存不住：不能记成 Delivered，否则下游会把「我们没存住」
// 误判成「对端给了个坏报告」，归错因、排查方向全偏。
void AttachmentDownloadEvidenceTest::writeFailureIsNotRecordedAsDelivered()
{
    LocalHttpsServer server(identity_.certificate, identity_.privateKey);
    QVERIFY(server.start());
    auto api = std::make_unique<FakeApi>();
    FakeApi* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api), nullptr, identity_.certificate);

    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig"), nullptr);
    QSignalSpy live(&client, &RemoteIMClient::liveMessagesReceived);

    const QString name = QStringLiteral("write-fail.json");
    const QString url = server.url(name);
    server.addRoute(QStringLiteral("/") + name, {QByteArray("{\"a\":1}")});
    const QString target = cachePathFor(url, name);
    QFile::remove(target);

    // 不能在目标路径上放目录：`QFile::exists()` 对目录返回 true，代码会走
    // 缓存命中分支，根本不下载。改成把缓存目录本身换成一个**文件**——
    // 这样目标路径不存在（exists 为 false，会真的去下载），但父级不是目录，
    // open(WriteOnly) 必然失败。
    const QString cacheDir = QFileInfo(target).absolutePath();
    QDir(cacheDir).removeRecursively();
    QFile blocker(cacheDir);
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.write("not a directory");
    blocker.close();

    fake->deliver(fileMessage(url, name, QStringLiteral("m-write")));

    QTRY_VERIFY_WITH_TIMEOUT(
        hasPhase(client.diagnosticsEvidence().exportFor(
                     AccountTag{123456ULL, QStringLiteral("desktop-user")},
                     QStringLiteral("phone-user")),
                 AttachmentPhase::WriteFailed),
        5000);
    const auto events = client.diagnosticsEvidence().exportFor(
        AccountTag{123456ULL, QStringLiteral("desktop-user")}, QStringLiteral("phone-user"));
    QVERIFY(!hasPhase(events, AttachmentPhase::Delivered));
    QVERIFY(!hasPhase(events, AttachmentPhase::CacheHit));
    QCOMPARE(live.count(), 0);
    // 还原，后面的用例还要用这个目录。
    QFile::remove(cacheDir);
    QDir().mkpath(cacheDir);
}

void AttachmentDownloadEvidenceTest::directoryAtCachePathIsNotAFileHit()
{
    LocalHttpsServer server(identity_.certificate, identity_.privateKey);
    QVERIFY(server.start());
    auto api = std::make_unique<FakeApi>(); auto* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api), nullptr, identity_.certificate);
    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig"), nullptr);
    QSignalSpy live(&client, &RemoteIMClient::liveMessagesReceived);
    const auto name = QStringLiteral("directory-not-file.json");
    const auto url = server.url(name); const auto target = cachePathFor(url, name);
    server.addRoute(QStringLiteral("/") + name, {QByteArray("{\"a\":1}")});
    QVERIFY(QDir().mkdir(target));
    fake->deliver(fileMessage(url, name, QStringLiteral("m-directory")));
    const AccountTag account{123456, QStringLiteral("desktop-user")};
    QTRY_VERIFY_WITH_TIMEOUT(
        hasPhase(client.diagnosticsEvidence().exportFor(account, "phone-user"), AttachmentPhase::WriteFailed), 1500);
    const auto events = client.diagnosticsEvidence().exportFor(account, "phone-user");
    QVERIFY(!hasPhase(events, AttachmentPhase::CacheHit));
    QVERIFY(!hasPhase(events, AttachmentPhase::Delivered)); QCOMPARE(live.count(), 0);
    QVERIFY(QDir().rmdir(target));
}

// 甲开始下载 -> 切到乙 -> 甲完成。文件确实下完了，但它不能进乙的会话，
// 也不能记成「已交付」——那会让乙的报告里出现甲的数据。
void AttachmentDownloadEvidenceTest::lateDownloadAfterAnAccountSwitchIsDroppedNotDelivered()
{
    LocalHttpsServer server(identity_.certificate, identity_.privateKey);
    QVERIFY(server.start());
    auto api = std::make_unique<FakeApi>();
    FakeApi* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api), nullptr, identity_.certificate);
    server.addRoute(QStringLiteral("/late.json"), {QByteArray("{\"a\":1}"), 200, true});

    client.connectToService(123456, QStringLiteral("alice"), QStringLiteral("sig"), nullptr);
    QSignalSpy live(&client, &RemoteIMClient::liveMessagesReceived);

    const QString name = QStringLiteral("late.json");
    const QString url = server.url(name);
    QFile::remove(cachePathFor(url, name));
    fake->deliver(fileMessage(url, name, QStringLiteral("m-late")));

    const AccountTag alice{123456ULL, QStringLiteral("alice")};
    QTRY_VERIFY_WITH_TIMEOUT(
        hasPhase(client.diagnosticsEvidence().exportFor(alice, QStringLiteral("phone-user")),
                 AttachmentPhase::DownloadStarted),
        5000);
    QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(QStringLiteral("/late.json")), 1, 5000);

    // 换账号，然后让下载完成。
    client.connectToService(123456, QStringLiteral("bob"), QStringLiteral("sig"), nullptr);
    server.release(QStringLiteral("/late.json"));

    const AccountTag bob{123456ULL, QStringLiteral("bob")};
    QTRY_VERIFY_WITH_TIMEOUT(
        hasPhase(client.diagnosticsEvidence().exportFor(alice, QStringLiteral("phone-user")),
                 AttachmentPhase::DroppedForAccountSwitch),
        5000);

    const auto aliceEvents = client.diagnosticsEvidence().exportFor(alice, QStringLiteral("phone-user"));
    QVERIFY2(!hasPhase(aliceEvents, AttachmentPhase::Delivered),
             "the file downloaded, but it never entered any conversation");
    // 乙那边既没有消息，也没有甲的任何记录。
    QCOMPARE(live.count(), 0);
    QVERIFY(client.diagnosticsEvidence().exportFor(bob, QStringLiteral("phone-user")).isEmpty());
    QVERIFY(!QFileInfo::exists(cachePathFor(url, name)));
}

void AttachmentDownloadEvidenceTest::oversizedResponseIsRejectedBeforeBody()
{
    LocalHttpsServer server(identity_.certificate, identity_.privateKey);
    QVERIFY(server.start());
    constexpr qint64 kFileLimit = 512LL * 1024 * 1024;
    server.addRoute(QStringLiteral("/oversized.bin"),
                    {QByteArray(), 200, false, true, kFileLimit + 1});
    auto api = std::make_unique<FakeApi>();
    FakeApi* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api), nullptr, identity_.certificate);
    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig"), nullptr);
    QSignalSpy live(&client, &RemoteIMClient::liveMessagesReceived);

    const QString url = server.url(QStringLiteral("oversized.bin"));
    const QString target = cachePathFor(url, QStringLiteral("oversized.bin"));
    QFile::remove(target);
    fake->deliver(fileMessage(url, QStringLiteral("oversized.bin"), QStringLiteral("m-oversized")));

    // 服务端只发响应头且保持连接，不送正文；通过说明上限在下载前生效。
    QTRY_VERIFY_WITH_TIMEOUT(
        hasPhase(client.diagnosticsEvidence().exportFor(
                     AccountTag{123456ULL, QStringLiteral("desktop-user")},
                     QStringLiteral("phone-user")), AttachmentPhase::DownloadFailed),
        5000);
    QCOMPARE(server.requestCount(QStringLiteral("/oversized.bin")), 1);
    QCOMPARE(live.count(), 0);
    QVERIFY(!QFileInfo::exists(target));
}

void AttachmentDownloadEvidenceTest::destroyingClientCancelsHeldDownload()
{
    LocalHttpsServer server(identity_.certificate, identity_.privateKey);
    QVERIFY(server.start());
    server.addRoute(QStringLiteral("/cancel.bin"), {QByteArray("late data"), 200, true});
    auto api = std::make_unique<FakeApi>();
    FakeApi* fake = api.get();
    auto client = std::make_unique<TimSdkRemoteIMClient>(std::move(api), nullptr,
                                                          identity_.certificate);
    client->connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig"), nullptr);
    const QString url = server.url(QStringLiteral("cancel.bin"));
    const QString target = cachePathFor(url, QStringLiteral("cancel.bin"));
    QFile::remove(target);
    fake->deliver(fileMessage(url, QStringLiteral("cancel.bin"), QStringLiteral("m-cancel")));
    QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(QStringLiteral("/cancel.bin")), 1, 5000);

    client.reset();
    server.release(QStringLiteral("/cancel.bin"));
    QTest::qWait(200);
    // 销毁客户端后，后台结果不能再将未完成下载提交为缓存。
    QVERIFY(!QFileInfo::exists(target));
}

void AttachmentDownloadEvidenceTest::imageVoiceAndVideoCoverDownloadOverTls()
{
    LocalHttpsServer server(identity_.certificate, identity_.privateKey);
    QVERIFY(server.start());
    const QString suffix = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString imageName = QStringLiteral("image-") + suffix + QStringLiteral(".jpg");
    const QString voiceName = QStringLiteral("voice-") + suffix + QStringLiteral(".amr");
    const QString coverName = QStringLiteral("cover-") + suffix + QStringLiteral(".jpg");
    const QString videoName = QStringLiteral("video-") + suffix + QStringLiteral(".mp4");
    server.addRoute(QStringLiteral("/") + imageName, {QByteArray("image-bytes")});
    server.addRoute(QStringLiteral("/") + voiceName, {QByteArray("voice-bytes")});
    server.addRoute(QStringLiteral("/") + coverName, {QByteArray("cover-bytes")});
    server.addRoute(QStringLiteral("/") + videoName, {QByteArray("video-bytes")});
    auto api = std::make_unique<FakeApi>();
    FakeApi* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api), nullptr, identity_.certificate);
    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig"), nullptr);
    QSignalSpy live(&client, &RemoteIMClient::liveMessagesReceived);

    fake->deliver(mediaMessage(
        {{QStringLiteral("elem_type"), 1},
         {QStringLiteral("image_elem_large_url"), server.url(imageName)},
         {QStringLiteral("image_elem_orig_pic_width"), 4},
         {QStringLiteral("image_elem_orig_pic_height"), 3}}, QStringLiteral("m-image")));
    QTRY_COMPARE_WITH_TIMEOUT(live.count(), 1, 5000);
    const auto images = qvariant_cast<QList<RemoteIMMessage>>(live.at(0).at(0));
    QCOMPARE(images.size(), 1);
    QVERIFY(images.first().hasImage);
    QCOMPARE(readFile(images.first().image.localPath), QByteArray("image-bytes"));

    fake->deliver(mediaMessage(
        {{QStringLiteral("elem_type"), 2},
         {QStringLiteral("sound_elem_url"), server.url(voiceName)},
         {QStringLiteral("sound_elem_file_time"), 3}}, QStringLiteral("m-voice")));
    QTRY_COMPARE_WITH_TIMEOUT(live.count(), 2, 5000);
    const auto voices = qvariant_cast<QList<RemoteIMMessage>>(live.at(1).at(0));
    QCOMPARE(voices.size(), 1);
    QVERIFY(voices.first().hasVoice);
    QCOMPARE(readFile(voices.first().voice.localPath), QByteArray("voice-bytes"));

    fake->deliver(mediaMessage(
        {{QStringLiteral("elem_type"), 9},
         {QStringLiteral("video_elem_video_url"), server.url(videoName)},
         {QStringLiteral("video_elem_image_url"), server.url(coverName)},
         {QStringLiteral("video_elem_video_duration"), 4}}, QStringLiteral("m-video")));
    QTRY_COMPARE_WITH_TIMEOUT(live.count(), 3, 5000);
    const auto videos = qvariant_cast<QList<RemoteIMMessage>>(live.at(2).at(0));
    QCOMPARE(videos.size(), 1);
    QVERIFY(videos.first().hasVideo);
    QCOMPARE(readFile(videos.first().video.localPath), QByteArray("video-bytes"));
    QCOMPARE(readFile(videos.first().video.coverPath), QByteArray("cover-bytes"));
    QCOMPARE(server.requestCount(QStringLiteral("/") + imageName), 1);
    QCOMPARE(server.requestCount(QStringLiteral("/") + voiceName), 1);
    QCOMPARE(server.requestCount(QStringLiteral("/") + coverName), 1);
    QCOMPARE(server.requestCount(QStringLiteral("/") + videoName), 1);
}

QTEST_MAIN(AttachmentDownloadEvidenceTest)
#include "AttachmentDownloadEvidenceTest.moc"
