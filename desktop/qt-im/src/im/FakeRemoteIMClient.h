#pragma once

#include "im/RemoteIMClient.h"

class FakeRemoteIMClient final : public RemoteIMClient {
    Q_OBJECT

public:
    explicit FakeRemoteIMClient(QObject* parent = nullptr);

    void connectToService(int sdkAppId, const QString& userId, const QString& userSig, RemoteIMCompletion completion) override;
    void disconnectFromService(RemoteIMCompletion completion) override;
    void deleteContact(const QString& userId, RemoteIMCompletion completion) override;
    void sendText(const QString& peerId, const QString& text, RemoteIMCompletion completion) override;
    void sendImage(const QString& peerId, const QString& localPath, RemoteIMCompletion completion) override;
    void sendVoice(const QString& peerId, const QString& localPath, int durationSeconds, RemoteIMCompletion completion) override;

    QString connectedUserId() const;
    QString lastDeletedContactId() const;
    QString lastTextPeerId() const;
    QString lastText() const;
    QString lastImagePeerId() const;
    QString lastImagePath() const;
    void failNext(const QString& error);
    void emitIncomingText(const QString& fromUserId, const QString& text);
    void emitIncomingImage(const QString& fromUserId, const QString& localPath, int width, int height, qint64 sizeBytes);

private:
    void complete(RemoteIMCompletion completion);

    QString connectedUserId_;
    QString lastDeletedContactId_;
    QString lastTextPeerId_;
    QString lastText_;
    QString lastImagePeerId_;
    QString lastImagePath_;
    QString nextError_;
};
