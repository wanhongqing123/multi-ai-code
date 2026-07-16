#pragma once

#include "im/RemoteIMClient.h"

class UnsupportedRemoteIMClient : public RemoteIMClient {
public:
    explicit UnsupportedRemoteIMClient(QObject* parent = nullptr);

    void connectToService(int sdkAppId, const QString& userId, const QString& userSig, RemoteIMCompletion completion) override;
    void disconnectFromService(RemoteIMCompletion completion) override;
    void deleteContact(const QString& userId, RemoteIMCompletion completion) override;
    void sendText(const QString& peerId, const QString& text, RemoteIMCompletion completion) override;
    void sendImage(const QString& peerId, const QString& localPath, RemoteIMCompletion completion) override;
    void sendVoice(const QString& peerId, const QString& localPath, int durationSeconds, RemoteIMCompletion completion) override;

protected:
    void fail(RemoteIMCompletion completion, const QString& action) const;
};
