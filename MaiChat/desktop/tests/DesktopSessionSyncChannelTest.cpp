// Local TLS WebSocket coverage for Desktop account-token renewal.
#include <QCryptographicHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTest>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509v3.h>

#include <cstring>
#include <memory>
#include <utility>

#include "agent/DesktopSessionSyncChannel.h"

namespace {

struct TestIdentity {
    QByteArray certificate;
    QByteArray privateKey;
};

TestIdentity createTestIdentity() {
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

class LocalWssServer final : public QTcpServer {
public:
    LocalWssServer(QByteArray certificate, QByteArray privateKey)
        : certificate_(std::move(certificate)), privateKey_(std::move(privateKey)) {}

    bool start() { return listen(QHostAddress::LocalHost); }
    QUrl url() const {
        return QUrl(QStringLiteral("wss://localhost:%1/v1/session-sync/ws").arg(serverPort()));
    }
    int connectionCount() const { return connections_.size(); }
    QByteArray authorization(int connection) const {
        return connections_.at(connection).authorization;
    }
    int operationCount(const QString& operation) const {
        int count = 0;
        for (const Connection& connection : connections_)
            for (const QJsonObject& frame : connection.frames)
                if (frame.value(QStringLiteral("op")).toString() == operation) ++count;
        return count;
    }
    QJsonObject operationFrame(const QString& operation) const {
        for (const Connection& connection : connections_)
            for (const QJsonObject& frame : connection.frames)
                if (frame.value(QStringLiteral("op")).toString() == operation) return frame;
        return {};
    }
    void respond(int connection, const QString& id, int status) {
        const QJsonObject response{
            {QStringLiteral("type"), QStringLiteral("response")},
            {QStringLiteral("id"), id},
            {QStringLiteral("status"), status},
            {QStringLiteral("body"), QJsonObject{}}};
        sendJson(connection, response);
    }
    void sendCatalog(int connection) {
        sendJson(connection, {{QStringLiteral("type"), QStringLiteral("catalog")},
                              {QStringLiteral("sessions"), QJsonArray{}}});
    }

private:
    void sendJson(int connection, const QJsonObject& message) {
        const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
        QByteArray frame;
        frame.append(char(0x81));
        if (payload.size() < 126) {
            frame.append(char(payload.size()));
        } else {
            frame.append(char(126));
            frame.append(char((payload.size() >> 8) & 0xff));
            frame.append(char(payload.size() & 0xff));
        }
        frame += payload;
        connections_.at(connection).socket->write(frame);
        connections_.at(connection).socket->flush();
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
        const int index = connections_.size();
        connections_.append({socket, {}, false, {}, {}});
        connect(socket, &QSslSocket::readyRead, this, [this, index] { receive(index); });
        socket->startServerEncryption();
    }

private:
    struct Connection {
        QSslSocket* socket = nullptr;
        QByteArray bytes;
        bool upgraded = false;
        QByteArray authorization;
        QList<QJsonObject> frames;
    };

    void receive(int index) {
        Connection& connection = connections_[index];
        connection.bytes += connection.socket->readAll();
        if (!connection.upgraded) {
            const int end = connection.bytes.indexOf("\r\n\r\n");
            if (end < 0) return;
            const QByteArray request = connection.bytes.left(end + 4);
            connection.bytes.remove(0, end + 4);
            QByteArray key;
            for (const QByteArray& line : request.split('\n')) {
                const QByteArray trimmed = line.trimmed();
                if (trimmed.toLower().startsWith("sec-websocket-key:"))
                    key = trimmed.mid(int(strlen("sec-websocket-key:"))).trimmed();
                if (trimmed.toLower().startsWith("authorization:"))
                    connection.authorization = trimmed.mid(int(strlen("authorization:"))).trimmed();
            }
            if (key.isEmpty()) {
                connection.socket->abort();
                return;
            }
            const QByteArray digest = QCryptographicHash::hash(
                key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", QCryptographicHash::Sha1).toBase64();
            connection.socket->write(
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: " + digest + "\r\n\r\n");
            connection.socket->flush();
            connection.upgraded = true;
        }
        while (connection.bytes.size() >= 2) {
            const auto* bytes = reinterpret_cast<const uchar*>(connection.bytes.constData());
            quint64 length = bytes[1] & 0x7f;
            int offset = 2;
            if (length == 126) {
                if (connection.bytes.size() < 4) return;
                length = (quint64(bytes[2]) << 8) | bytes[3];
                offset = 4;
            } else if (length == 127) {
                connection.socket->abort();
                return;
            }
            if (!(bytes[1] & 0x80) || length > 96 * 1024) {
                connection.socket->abort();
                return;
            }
            if (connection.bytes.size() < offset + 4 + int(length)) return;
            const QByteArray mask = connection.bytes.mid(offset, 4);
            QByteArray payload = connection.bytes.mid(offset + 4, int(length));
            for (int i = 0; i < payload.size(); ++i)
                payload[i] = char(payload.at(i) ^ mask.at(i % 4));
            const int opcode = bytes[0] & 0x0f;
            connection.bytes.remove(0, offset + 4 + int(length));
            if (opcode == 1) {
                const QJsonDocument document = QJsonDocument::fromJson(payload);
                if (document.isObject()) connection.frames.append(document.object());
            }
        }
    }

    QByteArray certificate_;
    QByteArray privateKey_;
    QList<Connection> connections_;
};

}  // namespace

class DesktopSessionSyncChannelTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void connectsToLocalWssWithBearer();
    void reauthenticatesWithoutRepeatingAnInFlightCommand();
    void failedReauthReconnectsWithoutRepeatingAnInFlightCommand();

private:
    TestIdentity identity_;
};

void DesktopSessionSyncChannelTest::initTestCase() {
    identity_ = createTestIdentity();
    QVERIFY(!identity_.certificate.isEmpty());
    QVERIFY(!identity_.privateKey.isEmpty());
}

void DesktopSessionSyncChannelTest::connectsToLocalWssWithBearer() {
    LocalWssServer server(identity_.certificate, identity_.privateKey);
    QVERIFY(server.start());
    const QString token(43, QLatin1Char('A'));
    DesktopSessionSyncChannel channel(server.url(), [token] { return token; }, nullptr,
                                      identity_.certificate);
    int connected = 0;
    channel.setConnectedHandler([&] { ++connected; });
    channel.start();
    QTRY_COMPARE_WITH_TIMEOUT(connected, 1, 5000);
    QCOMPARE(server.authorization(0), QByteArray("Bearer ") + token.toUtf8());
}

void DesktopSessionSyncChannelTest::reauthenticatesWithoutRepeatingAnInFlightCommand() {
    LocalWssServer server(identity_.certificate, identity_.privateKey);
    QVERIFY(server.start());
    QString token(43, QLatin1Char('A'));
    DesktopSessionSyncChannel channel(server.url(), [&token] { return token; }, nullptr,
                                      identity_.certificate);
    int connected = 0;
    channel.setConnectedHandler([&] { ++connected; });
    channel.start();
    QTRY_COMPARE_WITH_TIMEOUT(connected, 1, 5000);
    int completions = 0;
    QString commandError;
    channel.request(QStringLiteral("session-sync/send"), "POST",
                    {{QStringLiteral("kind"), QStringLiteral("command")},
                     {QStringLiteral("command_id"), QStringLiteral("same-paid-command")}},
                    [&](const QJsonObject&, const QString& error) {
                        ++completions;
                        commandError = error;
                    });
    QTRY_COMPARE_WITH_TIMEOUT(server.operationCount(QStringLiteral("send")), 1, 5000);
    const QString commandId = server.operationFrame(QStringLiteral("send"))
                                  .value(QStringLiteral("id")).toString();
    token = QString(43, QLatin1Char('B'));
    channel.refreshBearerNow();
    QTRY_COMPARE_WITH_TIMEOUT(server.operationCount(QStringLiteral("reauth")), 1, 5000);
    const QJsonObject reauth = server.operationFrame(QStringLiteral("reauth"));
    QCOMPARE(reauth.value(QStringLiteral("body")).toObject()
                 .value(QStringLiteral("access_token")).toString(), token);
    QCOMPARE(server.connectionCount(), 1);
    QCOMPARE(server.operationCount(QStringLiteral("send")), 1);
    QCOMPARE(completions, 0);
    int blocked = 0;
    QString blockedError;
    channel.request(QStringLiteral("session-sync/send"), "POST",
                    {{QStringLiteral("kind"), QStringLiteral("command")},
                     {QStringLiteral("command_id"), QStringLiteral("do-not-submit-yet")}},
                    [&](const QJsonObject&, const QString& error) {
                        ++blocked;
                        blockedError = error;
                    });
    QCOMPARE(blocked, 1);
    QVERIFY(!blockedError.isEmpty());
    QCOMPARE(server.operationCount(QStringLiteral("send")), 1);
    int catalogs = 0;
    channel.setCatalogHandler([&](const QJsonArray&) { ++catalogs; });
    server.respond(0, reauth.value(QStringLiteral("id")).toString(), 200);
    server.sendCatalog(0);
    QTRY_COMPARE_WITH_TIMEOUT(catalogs, 1, 5000);
    channel.refreshBearerNow();
    QTest::qWait(100);
    QCOMPARE(server.operationCount(QStringLiteral("reauth")), 1);
    server.respond(0, commandId, 200);
    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 5000);
    QVERIFY(commandError.isEmpty());
    QCOMPARE(server.operationCount(QStringLiteral("send")), 1);
    QCOMPARE(server.connectionCount(), 1);
}

void DesktopSessionSyncChannelTest::failedReauthReconnectsWithoutRepeatingAnInFlightCommand() {
    LocalWssServer server(identity_.certificate, identity_.privateKey);
    QVERIFY(server.start());
    QString token(43, QLatin1Char('A'));
    DesktopSessionSyncChannel channel(server.url(), [&token] { return token; }, nullptr,
                                      identity_.certificate);
    int connected = 0;
    channel.setConnectedHandler([&] { ++connected; });
    channel.start();
    QTRY_COMPARE_WITH_TIMEOUT(connected, 1, 5000);
    int completions = 0;
    QString commandError;
    channel.request(QStringLiteral("session-sync/send"), "POST",
                    {{QStringLiteral("kind"), QStringLiteral("command")},
                     {QStringLiteral("command_id"), QStringLiteral("must-not-repeat")}},
                    [&](const QJsonObject&, const QString& error) {
                        ++completions;
                        commandError = error;
                    });
    QTRY_COMPARE_WITH_TIMEOUT(server.operationCount(QStringLiteral("send")), 1, 5000);
    token = QString(43, QLatin1Char('B'));
    channel.refreshBearerNow();
    QTRY_COMPARE_WITH_TIMEOUT(server.operationCount(QStringLiteral("reauth")), 1, 5000);
    const QString reauthId = server.operationFrame(QStringLiteral("reauth"))
                                 .value(QStringLiteral("id")).toString();
    server.respond(0, reauthId, 401);
    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 5000);
    QVERIFY(!commandError.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(connected, 2, 8000);
    QCOMPARE(server.authorization(1), QByteArray("Bearer ") + token.toUtf8());
    QCOMPARE(server.operationCount(QStringLiteral("send")), 1);
    QTest::qWait(200);
    QCOMPARE(completions, 1);
    QCOMPARE(server.operationCount(QStringLiteral("send")), 1);
}

QTEST_GUILESS_MAIN(DesktopSessionSyncChannelTest)
#include "DesktopSessionSyncChannelTest.moc"
