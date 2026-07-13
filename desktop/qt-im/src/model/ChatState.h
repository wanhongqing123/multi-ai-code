#pragma once

#include <QList>
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
    RemoteIMMessage queueOutgoingImage(const QString& localPath, int width, int height, qint64 sizeBytes);
    RemoteIMMessage queueOutgoingVoice(const QString& localPath, int durationSeconds);
    RemoteIMMessage receiveText(const QString& fromUserId, const QString& text);
    RemoteIMMessage receiveImage(const QString& fromUserId, const QString& localPath, int width, int height, qint64 sizeBytes);
    RemoteIMMessage receiveVoice(const QString& fromUserId, const QString& localPath, int durationSeconds);
    QList<RemoteIMMessage> messagesWith(const QString& peerId) const;
    bool updateMessageStatus(const QString& messageId, RemoteIMMessageStatus status);
    void appendMessageForRestore(const RemoteIMMessage& message);

private:
    QString requireSelectedPeer() const;
    static QString clean(const QString& value);
    static QString fileName(const QString& path);

    QString ownerUserId_;
    QString selectedPeerId_;
    QList<RemoteIMContact> contacts_;
    QList<RemoteIMMessage> messages_;
};
