// 文件作用：保持桌面端会话在线，接收同账号设备的操作并回传历史与状态。
#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>

class AgentController;
class DesktopSessionSyncHost;

class DesktopSessionSyncCoordinator final : public QObject {
public:
    using Completion = std::function<void(const QJsonObject&, const QString&)>;
    using Requester = std::function<void(const QString&, const QByteArray&, const QJsonObject&, Completion)>;
    using AgentProvider = std::function<AgentController*()>;
    using CatalogHandler = std::function<void(const QJsonArray&)>;
    using PayloadHandler = std::function<void(const QJsonObject&, const QString&, const QJsonObject&)>;

    DesktopSessionSyncCoordinator(Requester requester, AgentProvider agent, QObject* parent = nullptr,
                                  QString deviceId = {}, QString deviceName = {});
    ~DesktopSessionSyncCoordinator() override;

    void start();
    void connectionReady();
    void connectionLost();
    void acceptCatalog(const QJsonArray& sessions);
    void acceptInbox(const QJsonArray& messages);
    void setCatalogHandler(CatalogHandler handler);
    void setPayloadHandler(PayloadHandler handler);
    void requestRemoteHistory(const QJsonObject& session, const QString& before, int limit, Completion completion);
    void sendRemoteCommand(const QJsonObject& session, const QJsonObject& payload, Completion completion);

private:
    struct Outgoing {
        QString recipientSlot;
        QString recipientLogin;
        QString sessionId;
        QString kind;
        QJsonObject payload;
        QString id;
    };
    struct Subscriber {
        QString slot;
        QString login;
        qint64 expiresAt = 0;
    };
    struct Incoming {
        QString ownerSlot;
        QString ownerLogin;
        QString sessionId;
        QString kind;
        QString digest;
        int count = 0;
        qint64 started = 0;
        QMap<int, QByteArray> chunks;
    };

    DesktopSessionSyncHost* host();
    void publishPresence();
    void handleEnvelope(const QJsonObject& envelope);
    void acknowledge(int id);
    void handleRemoteChunk(const QJsonObject& envelope);
    QJsonObject matchingRemote(const QString& slot, const QString& login, const QString& session) const;
    void queuePayload(const QString& slot, const QString& login, const QString& sessionId, const QString& kind,
                      const QJsonObject& payload);
    void flushOutbound();
    void scheduleSnapshot(const QString& sessionId);
    void publishSnapshot(const QString& sessionId);

    Requester requester_;
    AgentProvider agentProvider_;
    QPointer<AgentController> agent_;
    std::unique_ptr<DesktopSessionSyncHost> host_;
    QTimer presenceTimer_;
    QTimer snapshotTimer_;
    QString deviceId_;
    QString deviceName_;
    QSet<QString> pendingSnapshotSessions_;
    QHash<QString, Subscriber> subscribers_;
    QHash<QString, Incoming> incoming_;
    QHash<QString, QString> retryIdByFingerprint_;
    QHash<QString, QString> fingerprintByCommandId_;
    QJsonArray remoteSessions_;
    CatalogHandler catalogHandler_;
    PayloadHandler payloadHandler_;
    QQueue<Outgoing> outbound_;
    bool started_ = false;
    bool sending_ = false;
};
