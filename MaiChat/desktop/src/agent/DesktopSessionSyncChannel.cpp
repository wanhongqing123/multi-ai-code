#include "agent/DesktopSessionSyncChannel.h"

#include <QFile>
#include <QJsonDocument>
#include <QMetaObject>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QPointer>
#include <QUuid>

namespace {

QString requestOperation(const QString& path, const QByteArray& method) {
    if (method == "POST" && path == QStringLiteral("session-sync/presence")) return QStringLiteral("presence");
    if (method == "POST" && path == QStringLiteral("session-sync/send")) return QStringLiteral("send");
    if (method == "POST" && path == QStringLiteral("session-sync/ack")) return QStringLiteral("ack");
    return {};
}

QString proxyForUrl(const QUrl& socketUrl) {
    QUrl lookup = socketUrl;
    lookup.setScheme(QStringLiteral("https"));
    const auto proxies = QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(lookup));
    if (proxies.isEmpty() || proxies.first().type() == QNetworkProxy::NoProxy ||
        proxies.first().type() == QNetworkProxy::DefaultProxy)
        return {};
    const QNetworkProxy proxy = proxies.first();
    if (proxy.type() != QNetworkProxy::HttpProxy || proxy.hostName().isEmpty() || proxy.port() == 0)
        return QStringLiteral("unsupported");
    QUrl address;
    address.setScheme(QStringLiteral("http"));
    address.setHost(proxy.hostName());
    address.setPort(proxy.port());
    address.setPath(QStringLiteral("/"));
    return address.toString(QUrl::FullyEncoded);
}

}  // namespace

DesktopSessionSyncChannel::DesktopSessionSyncChannel(QUrl url, std::function<QString()> bearerProvider, QObject* parent)
    : QObject(parent), url_(std::move(url)), bearerProvider_(std::move(bearerProvider)) {
    retryTimer_.setInterval(2000);
    connect(&retryTimer_, &QTimer::timeout, this, [this] { startWhenReady(); });
    tokenTimer_.setInterval(10000);
    connect(&tokenTimer_, &QTimer::timeout, this, [this] {
        if (!socket_ || !bearerProvider_) return;
        const QByteArray token = bearerProvider_().toUtf8();
        if (token.isEmpty()) {
            ++generation_;
            socket_->stop();
            socket_.reset();
            connected_ = false;
            failPending(QStringLiteral("账号登录已失效"));
            if (disconnectedHandler_) disconnectedHandler_();
            return;
        }
        socket_->updateBearer(token.toStdString());
    });
}

DesktopSessionSyncChannel::~DesktopSessionSyncChannel() {
    if (socket_) socket_->stop();
}

void DesktopSessionSyncChannel::setCatalogHandler(CatalogHandler handler) {
    catalogHandler_ = std::move(handler);
}

void DesktopSessionSyncChannel::setInboxHandler(InboxHandler handler) {
    inboxHandler_ = std::move(handler);
}

void DesktopSessionSyncChannel::setConnectedHandler(StateHandler handler) {
    connectedHandler_ = std::move(handler);
}

void DesktopSessionSyncChannel::setDisconnectedHandler(StateHandler handler) {
    disconnectedHandler_ = std::move(handler);
}

void DesktopSessionSyncChannel::start() {
    if (started_) return;
    started_ = true;
    retryTimer_.start();
    tokenTimer_.start();
    startWhenReady();
}

void DesktopSessionSyncChannel::startWhenReady() {
    if (socket_ || !bearerProvider_) return;
    const QByteArray token = bearerProvider_().toUtf8();
    if (token.isEmpty() || url_.scheme() != QStringLiteral("wss")) return;
    const QString proxy = proxyForUrl(url_);
    if (proxy == QStringLiteral("unsupported")) return;
    Q_INIT_RESOURCE(AuthCertificates);
    QFile certificates(QStringLiteral(":/maichat/auth/cacert.pem"));
    if (!certificates.open(QIODevice::ReadOnly)) return;
    const QByteArray ca = certificates.readAll();
    QPointer<DesktopSessionSyncChannel> guard(this);
    const quint64 generation = ++generation_;
    socket_ = std::make_unique<MaiSessionSyncSocket>(
        [guard, generation](MaiSessionSyncSocket::Event event, const std::string& detail) {
            if (!guard) return;
            const QByteArray bytes(detail.data(), int(detail.size()));
            QMetaObject::invokeMethod(
                guard,
                [guard, event, bytes, generation] {
                    if (guard && guard->generation_ == generation) guard->consume(event, bytes);
                },
                Qt::QueuedConnection);
        });
    const MaiSessionSyncSocket::Configuration settings{url_.toString(QUrl::FullyEncoded).toStdString(),
                                                       token.toStdString(), ca.toStdString(), proxy.toStdString()};
    if (!socket_->start(settings)) socket_.reset();
}

void DesktopSessionSyncChannel::request(const QString& path, const QByteArray& method, const QJsonObject& body,
                                        Completion completion) {
    const QString operation = requestOperation(path, method);
    if (operation.isEmpty() || !connected_ || !socket_) {
        completion({}, QStringLiteral("设备同步连接尚未建立"));
        return;
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-'));
    const QJsonObject frame{
        {QStringLiteral("id"), id}, {QStringLiteral("op"), operation}, {QStringLiteral("body"), body}};
    const QByteArray bytes = QJsonDocument(frame).toJson(QJsonDocument::Compact);
    if (bytes.isEmpty() || bytes.size() > 96 * 1024) {
        completion({}, QStringLiteral("设备同步请求过大"));
        return;
    }
    pending_.insert(id, std::move(completion));
    if (!socket_->send(bytes.toStdString())) {
        const Completion failed = pending_.take(id);
        failed({}, QStringLiteral("设备同步连接中断"));
        return;
    }
    QTimer::singleShot(15000, this, [this, id] {
        if (!pending_.contains(id)) return;
        const Completion expired = pending_.take(id);
        expired({}, QStringLiteral("操作状态未知，请先查看所属设备会话记录"));
    });
}

void DesktopSessionSyncChannel::consume(MaiSessionSyncSocket::Event event, const QByteArray& bytes) {
    if (event == MaiSessionSyncSocket::Event::Connected) {
        connected_ = true;
        if (connectedHandler_) connectedHandler_();
        return;
    }
    if (event == MaiSessionSyncSocket::Event::Disconnected || event == MaiSessionSyncSocket::Event::Error) {
        connected_ = false;
        failPending(QStringLiteral("设备同步连接中断"));
        if (disconnectedHandler_) disconnectedHandler_();
        return;
    }
    if (event != MaiSessionSyncSocket::Event::Message) return;
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    if (!document.isObject()) return;
    const QJsonObject frame = document.object();
    const QString type = frame.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("catalog")) {
        if (catalogHandler_) catalogHandler_(frame.value(QStringLiteral("sessions")).toArray());
    } else if (type == QStringLiteral("inbox")) {
        if (inboxHandler_) inboxHandler_(frame.value(QStringLiteral("messages")).toArray());
    } else if (type == QStringLiteral("response")) {
        const QString id = frame.value(QStringLiteral("id")).toString();
        if (!pending_.contains(id)) return;
        const Completion completion = pending_.take(id);
        const QJsonObject body = frame.value(QStringLiteral("body")).toObject();
        const int status = frame.value(QStringLiteral("status")).toInt();
        completion(status == 200 ? body : QJsonObject{},
                   status == 200 ? QString()
                                 : body.value(QStringLiteral("error")).toString(QStringLiteral("设备同步请求失败")));
    }
}

void DesktopSessionSyncChannel::failPending(const QString& error) {
    const auto pending = pending_;
    pending_.clear();
    for (const Completion& completion : pending) completion({}, error);
}
