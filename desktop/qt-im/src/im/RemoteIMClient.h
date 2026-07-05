#pragma once

#include <QObject>
#include <QList>
#include <QString>
#include <functional>

#include "model/RemoteIMContact.h"
#include "model/RemoteIMMessage.h"

using RemoteIMCompletion = std::function<void(bool ok, const QString& error)>;

class RemoteIMClient : public QObject {
    Q_OBJECT

public:
    explicit RemoteIMClient(QObject* parent = nullptr) : QObject(parent) {}
    ~RemoteIMClient() override = default;

    virtual void connectToService(int sdkAppId, const QString& userId, const QString& userSig, RemoteIMCompletion completion) = 0;
    virtual void disconnectFromService(RemoteIMCompletion completion) = 0;
    virtual void sendText(const QString& peerId, const QString& text, RemoteIMCompletion completion) = 0;
    virtual void sendImage(const QString& peerId, const QString& localPath, RemoteIMCompletion completion) = 0;
    virtual void sendVoice(const QString& peerId, const QString& localPath, int durationSeconds, RemoteIMCompletion completion) = 0;

signals:
    void contactsReceived(const QList<RemoteIMContact>& contacts);
    void messagesReceived(const QList<RemoteIMMessage>& messages);
    void incomingText(const QString& fromUserId, const QString& text);
    void incomingImage(const QString& fromUserId, const QString& localPath, int width, int height, qint64 sizeBytes);
    void incomingVoice(const QString& fromUserId, const QString& localPath, int durationSeconds);
    void disconnected();
};
