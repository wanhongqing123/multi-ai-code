#pragma once

#include <QObject>
#include <QList>
#include <QString>
#include <functional>

#include "model/RemoteIMContact.h"
#include "model/RemoteIMMessage.h"

using RemoteIMCompletion = std::function<void(bool ok, const QString& error)>;
// 发送类操作的回执：成功时带 SDK 确认的消息 id（<msg_id>#<elem下标>，与漫游/
// 实时投递同一编号规则）。本地库把出站消息的临时 UUID 换成它，漫游重投同一条
// 消息时才能按主键去重。SDK 未返回 id 时为空串。
using RemoteIMSendCompletion =
    std::function<void(bool ok, const QString& error, const QString& remoteMessageId)>;

class RemoteIMClient : public QObject {
    Q_OBJECT

public:
    explicit RemoteIMClient(QObject* parent = nullptr) : QObject(parent) {}
    ~RemoteIMClient() override = default;

    virtual void connectToService(int sdkAppId, const QString& userId, const QString& userSig, RemoteIMCompletion completion) = 0;
    virtual void disconnectFromService(RemoteIMCompletion completion) = 0;
    virtual void deleteContact(const QString& userId, RemoteIMCompletion completion) = 0;
    virtual void sendText(const QString& peerId, const QString& text, RemoteIMSendCompletion completion) = 0;
    virtual void sendImage(const QString& peerId, const QString& localPath, RemoteIMSendCompletion completion) = 0;
    virtual void sendVoice(const QString& peerId, const QString& localPath, int durationSeconds, RemoteIMCompletion completion) = 0;

signals:
    void contactsReceived(const QList<RemoteIMContact>& contacts);
    void messagesReceived(const QList<RemoteIMMessage>& messages);
    void incomingText(const QString& fromUserId, const QString& text);
    void incomingImage(const QString& fromUserId, const QString& localPath, int width, int height, qint64 sizeBytes);
    void incomingVoice(const QString& fromUserId, const QString& localPath, int durationSeconds);
    void incomingFile(const QString& fromUserId, const QString& localPath, const QString& fileName, const QString& mimeType, qint64 sizeBytes);
    void disconnected();
};
