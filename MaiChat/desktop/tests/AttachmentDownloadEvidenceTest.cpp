#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <memory>

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

// 一次性的本地 HTTP 服务：真正走 QNetworkAccessManager，不桩掉网络层。
// 桩掉的话，测的就只是我们自己的 if/else，测不到「下载这件事」。
class OneShotServer : public QObject {
public:
    explicit OneShotServer(QByteArray body) : body_(std::move(body)) {
        server_.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&server_, &QTcpServer::newConnection, this, [this] {
            QTcpSocket* socket = server_.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                socket->readAll();
                if (holdOpen) return;  // 故意不回应：模拟卡住的下载
                const QByteArray header =
                    QStringLiteral("HTTP/1.1 200 OK\r\nContent-Length: %1\r\n"
                                   "Content-Type: application/json\r\n\r\n")
                        .arg(body_.size())
                        .toUtf8();
                socket->write(header + body_);
                socket->flush();
                socket->disconnectFromHost();
            });
        });
    }
    QString url(const QString& name) const {
        return QStringLiteral("http://127.0.0.1:%1/%2").arg(server_.serverPort()).arg(name);
    }
    void release() {
        holdOpen = false;
        for (QTcpSocket* socket : server_.findChildren<QTcpSocket*>()) emit socket->readyRead();
    }
    bool holdOpen = false;

private:
    QTcpServer server_;
    QByteArray body_;
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
    void writeFailureIsNotRecordedAsDelivered();
    void lateDownloadAfterAnAccountSwitchIsDroppedNotDelivered();
};

void AttachmentDownloadEvidenceTest::initTestCase()
{
    // 别往真实用户目录里写缓存。
    QStandardPaths::setTestModeEnabled(true);
}

void AttachmentDownloadEvidenceTest::normalDownloadIsRecordedAsDeliveredAndReachesTheConversation()
{
    auto api = std::make_unique<FakeApi>();
    FakeApi* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api));
    OneShotServer server(QByteArray("{\"a\":1}"));

    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig"), nullptr);
    QSignalSpy live(&client, &RemoteIMClient::liveMessagesReceived);

    const QString name = QStringLiteral("remote-diagnostics-"
                                        "65de6748-3a4e-4d08-8ffd-bc78e1804ff9.json");
    const QString url = server.url(name);
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
}

// 下载失败时消息**整条不出现**——所以只能靠这条记录知道附件卡在哪。
void AttachmentDownloadEvidenceTest::networkFailureIsRecordedAndNoMessageAppears()
{
    auto api = std::make_unique<FakeApi>();
    FakeApi* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api));

    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig"), nullptr);
    QSignalSpy live(&client, &RemoteIMClient::liveMessagesReceived);

    // 关闭的端口：真实的连接失败，不是桩出来的。
    const QString url = QStringLiteral("http://127.0.0.1:1/missing.json");
    fake->deliver(fileMessage(url, QStringLiteral("missing.json"), QStringLiteral("m-net")));

    QTRY_VERIFY_WITH_TIMEOUT(
        hasPhase(client.diagnosticsEvidence().exportFor(
                     AccountTag{123456ULL, QStringLiteral("desktop-user")},
                     QStringLiteral("phone-user")),
                 AttachmentPhase::DownloadFailed),
        5000);
    QCOMPARE(live.count(), 0);
}

// 下载成功但存不住：不能记成 Delivered，否则下游会把「我们没存住」
// 误判成「对端给了个坏报告」，归错因、排查方向全偏。
void AttachmentDownloadEvidenceTest::writeFailureIsNotRecordedAsDelivered()
{
    auto api = std::make_unique<FakeApi>();
    FakeApi* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api));
    OneShotServer server(QByteArray("{\"a\":1}"));

    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig"), nullptr);
    QSignalSpy live(&client, &RemoteIMClient::liveMessagesReceived);

    const QString name = QStringLiteral("write-fail.json");
    const QString url = server.url(name);
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

// 甲开始下载 -> 切到乙 -> 甲完成。文件确实下完了，但它不能进乙的会话，
// 也不能记成「已交付」——那会让乙的报告里出现甲的数据。
void AttachmentDownloadEvidenceTest::lateDownloadAfterAnAccountSwitchIsDroppedNotDelivered()
{
    auto api = std::make_unique<FakeApi>();
    FakeApi* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api));
    OneShotServer server(QByteArray("{\"a\":1}"));
    server.holdOpen = true;  // 先卡住，等换完账号再放行

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

    // 换账号，然后让下载完成。
    client.connectToService(123456, QStringLiteral("bob"), QStringLiteral("sig"), nullptr);
    server.release();

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
}

QTEST_MAIN(AttachmentDownloadEvidenceTest)
#include "AttachmentDownloadEvidenceTest.moc"
