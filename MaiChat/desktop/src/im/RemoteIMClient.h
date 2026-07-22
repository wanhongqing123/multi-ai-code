#pragma once

#include <QObject>
#include <QList>
#include <QString>
#include <functional>

#include "model/RemoteIMContact.h"
#include "model/RemoteIMMessage.h"

using RemoteIMCompletion = std::function<void(bool ok, const QString& error)>;
struct RemoteIMSendReceipt {
    QString remoteMessageId;
    qint64 createdAtMillis = 0;
};

// 发送类操作的回执：成功时带 SDK 确认的消息 id 和规范化时间。本地库据此
// 替换临时 UUID，并与漫游/实时消息使用同一排序键。
using RemoteIMSendCompletion =
    std::function<void(bool ok, const QString& error, const RemoteIMSendReceipt& receipt)>;

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
    // 发送任意文件。默认不支持（Fake/Unsupported 客户端据此优雅降级），仅 TimSdk 真正实现。
    virtual void sendFile(const QString& peerId, const QString& localPath, const QString& fileName, RemoteIMSendCompletion completion) {
        Q_UNUSED(peerId);
        Q_UNUSED(localPath);
        Q_UNUSED(fileName);
        if (completion) completion(false, QStringLiteral("当前 IM 客户端不支持发送文件"), {});
    }
    // 图片/文件 + 配文合并成「一条」多元素消息发送。默认降级为分两条发（图片/文件 + 文本），
    // 仅 TimSdk 真正合并成一条。
    virtual void sendImageWithText(const QString& peerId, const QString& imagePath, const QString& text, RemoteIMSendCompletion completion) {
        if (text.trimmed().isEmpty()) { sendImage(peerId, imagePath, std::move(completion)); return; }
        sendImage(peerId, imagePath, {});
        sendText(peerId, text, std::move(completion));
    }
    virtual void sendFileWithText(const QString& peerId, const QString& localPath, const QString& fileName, const QString& text, RemoteIMSendCompletion completion) {
        if (text.trimmed().isEmpty()) { sendFile(peerId, localPath, fileName, std::move(completion)); return; }
        sendFile(peerId, localPath, fileName, {});
        sendText(peerId, text, std::move(completion));
    }

signals:
    void contactsReceived(const QList<RemoteIMContact>& contacts);
    void messagesReceived(const QList<RemoteIMMessage>& messages);
    void incomingText(const QString& fromUserId, const QString& text);
    void incomingImage(const QString& fromUserId, const QString& localPath, int width, int height, qint64 sizeBytes);
    void incomingVoice(const QString& fromUserId, const QString& localPath, int durationSeconds);
    void incomingFile(const QString& fromUserId, const QString& localPath, const QString& fileName, const QString& mimeType, qint64 sizeBytes);
    void disconnected();
};
