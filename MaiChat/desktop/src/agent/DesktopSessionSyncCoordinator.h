// 文件作用：保持桌面端会话在线，接收同账号设备的操作并回传历史与状态。
#pragma once

#include <QHash>
#include <QJsonObject>
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

    DesktopSessionSyncCoordinator(Requester requester, AgentProvider agent, QObject* parent = nullptr,
                                  QString deviceId = {}, QString deviceName = {});
    ~DesktopSessionSyncCoordinator() override;

    void start();

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

    DesktopSessionSyncHost* host();
    void publishPresence();
    void poll();
    void handleEnvelope(const QJsonObject& envelope);
    void acknowledge(int id);
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
    QTimer retryTimer_;
    QTimer snapshotTimer_;
    QString deviceId_;
    QString deviceName_;
    QSet<QString> pendingSnapshotSessions_;
    QHash<QString, Subscriber> subscribers_;
    QQueue<Outgoing> outbound_;
    bool started_ = false;
    bool polling_ = false;
    bool sending_ = false;
};
