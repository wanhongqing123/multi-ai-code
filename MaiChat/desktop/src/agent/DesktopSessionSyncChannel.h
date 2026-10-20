// Qt bridge around the shared C++ Session WebSocket client.
#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <QUrl>

#include <functional>
#include <memory>

#include "MaiSessionSyncSocket.h"

class DesktopSessionSyncChannel final : public QObject {
public:
    using Completion = std::function<void(const QJsonObject&, const QString&)>;
    using CatalogHandler = std::function<void(const QJsonArray&)>;
    using InboxHandler = std::function<void(const QJsonArray&)>;
    using StateHandler = std::function<void()>;

    DesktopSessionSyncChannel(QUrl url, std::function<QString()> bearerProvider, QObject* parent = nullptr);
    ~DesktopSessionSyncChannel() override;

    void setCatalogHandler(CatalogHandler handler);
    void setInboxHandler(InboxHandler handler);
    void setConnectedHandler(StateHandler handler);
    void setDisconnectedHandler(StateHandler handler);
    void start();
    void request(const QString& path, const QByteArray& method, const QJsonObject& body, Completion completion);

private:
    void startWhenReady();
    void consume(MaiSessionSyncSocket::Event event, const QByteArray& bytes);
    void failPending(const QString& error);

    QUrl url_;
    std::function<QString()> bearerProvider_;
    std::unique_ptr<MaiSessionSyncSocket> socket_;
    QTimer retryTimer_;
    QTimer tokenTimer_;
    QHash<QString, Completion> pending_;
    CatalogHandler catalogHandler_;
    InboxHandler inboxHandler_;
    StateHandler connectedHandler_;
    StateHandler disconnectedHandler_;
    bool started_ = false;
    bool connected_ = false;
    quint64 generation_ = 0;
};
