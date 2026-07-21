#pragma once

#include <QList>
#include <QSet>
#include <QString>

#include "model/RemoteIMContact.h"
#include "model/RemoteIMMessage.h"

class ChatState {
public:
    explicit ChatState(QString ownerUserId);

    QString ownerUserId() const;
    QString selectedPeerId() const;
    QList<RemoteIMContact> contacts() const;
    QList<RemoteIMMessage> messages() const;

    void upsertContact(const RemoteIMContact& contact);
    void removeContactAndMessages(const QString& userId);
    void selectPeer(const QString& userId);
    RemoteIMMessage queueOutgoingText(const QString& text);
    RemoteIMMessage queueOutgoingImage(const QString& localPath, int width, int height, qint64 sizeBytes, const QString& text = QString());
    RemoteIMMessage queueOutgoingVoice(const QString& localPath, int durationSeconds);
    RemoteIMMessage queueOutgoingFile(const QString& localPath, const QString& fileName, const QString& mimeType, qint64 sizeBytes, const QString& text = QString());
    RemoteIMMessage receiveText(const QString& fromUserId, const QString& text);
    RemoteIMMessage receiveImage(const QString& fromUserId, const QString& localPath, int width, int height, qint64 sizeBytes);
    RemoteIMMessage receiveVoice(const QString& fromUserId, const QString& localPath, int durationSeconds);
    RemoteIMMessage receiveFile(const QString& fromUserId, const QString& localPath, const QString& fileName, const QString& mimeType, qint64 sizeBytes);
    QList<RemoteIMMessage> messagesWith(const QString& peerId) const;
    bool updateMessageStatus(const QString& messageId, RemoteIMMessageStatus status);
    bool updateMessageTime(const QString& messageId, qint64 createdAtMillis);
    // 出站消息发送成功后，把本地临时 UUID 换成 SDK 确认的稳定 id（漫游重投可
    // 据此去重）。新 id 已存在（漫游先到）时删除旧临时消息，返回 false。
    bool adoptMessageId(const QString& oldId, const QString& newId);
    void appendMessageForRestore(const RemoteIMMessage& message);

private:
    QString requireSelectedPeer() const;
    // 统一的消息追加入口：登记 id（供恢复/漫游合并去重）后追加。
    void appendTracked(const RemoteIMMessage& message);
    static QString clean(const QString& value);
    static QString incomingDisplayText(const QString& value);
    static QString fileName(const QString& path);

    QString ownerUserId_;
    QString selectedPeerId_;
    QList<RemoteIMContact> contacts_;
    QList<RemoteIMMessage> messages_;
    QSet<QString> messageIds_;
};
