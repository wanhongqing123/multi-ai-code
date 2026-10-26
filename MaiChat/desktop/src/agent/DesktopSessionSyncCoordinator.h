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
    using DeviceHandler = std::function<void(const QJsonArray&)>;
    using PayloadHandler = std::function<void(const QJsonObject&, const QString&, const QJsonObject&)>;

    DesktopSessionSyncCoordinator(Requester requester, AgentProvider agent, QObject* parent = nullptr,
                                  QString deviceId = {}, QString deviceName = {});
    ~DesktopSessionSyncCoordinator() override;

    void start();
    void connectionReady();
    void connectionLost();
    void acceptCatalog(const QJsonArray& sessions);
    void acceptDevices(const QJsonArray& devices);
    void acceptInbox(const QJsonArray& messages);
    void setCatalogHandler(CatalogHandler handler);
    void setDeviceHandler(DeviceHandler handler);
    void setPayloadHandler(PayloadHandler handler);
    void requestRemoteHistory(const QJsonObject& session, const QString& before, int limit, Completion completion);
    void sendRemoteCommand(const QJsonObject& session, const QJsonObject& payload, Completion completion);
    void sendRemoteLifecycle(const QJsonObject& device, const QJsonObject& payload, Completion completion);
    void setDefaultWorkspaceDirectory(const QString& directory);
    QJsonArray remoteDevices() const { return remoteDevices_; }
    QJsonArray remoteSessions() const { return remoteSessions_; }

private:
    struct Outgoing {
        QString recipientSlot;
        QString recipientLogin;
        QString sessionId;
        QString kind;
        QJsonObject payload;
        QString id;
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
    struct PendingLifecycle {
        QString slot;
        QString login;
        Completion completion;
    };

    DesktopSessionSyncHost* host();
    void publishPresence();
    void handleEnvelope(const QJsonObject& envelope);
    void acknowledge(int id);
    void handleRemoteChunk(const QJsonObject& envelope);
    QJsonObject matchingRemote(const QString& slot, const QString& login, const QString& session) const;
    QJsonObject matchingDevice(const QJsonObject& device) const;
    void queuePayload(const QString& slot, const QString& login, const QString& sessionId, const QString& kind,
                      const QJsonObject& payload);
    void flushOutbound();
    void scheduleSnapshot(const QString& sessionId);
    void publishSnapshot(const QString& sessionId);
    void publishPage(const QString& sessionId, const QJsonObject& page);
    void processBackfillPage();

    Requester requester_;
    AgentProvider agentProvider_;
    QPointer<AgentController> agent_;
    std::unique_ptr<DesktopSessionSyncHost> host_;
    QTimer presenceTimer_;
    QTimer snapshotTimer_;
    QTimer backfillTimer_;
    QString deviceId_;
    QString deviceName_;
    QSet<QString> pendingSnapshotSessions_;
    QSet<QString> initiallyPublishedSessions_;
    QQueue<QString> backfillSessions_;
    QHash<QString, QString> backfillBefore_;
    QHash<QString, QByteArray> publishedMessageDigests_;
    QHash<QString, QByteArray> publishedStateDigests_;
    QHash<QString, Incoming> incoming_;
    QHash<QString, QString> retryIdByFingerprint_;
    QHash<QString, QString> fingerprintByCommandId_;
    QHash<QString, PendingLifecycle> pendingLifecycle_;
    QJsonArray remoteSessions_;
    QJsonArray remoteDevices_;
    QString defaultWorkspaceDirectory_;
    CatalogHandler catalogHandler_;
    DeviceHandler deviceHandler_;
    PayloadHandler payloadHandler_;
    QQueue<Outgoing> outbound_;
    bool started_ = false;
    bool sending_ = false;
};
