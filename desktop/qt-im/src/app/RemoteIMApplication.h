#pragma once

#include <QObject>
#include <QString>
#include <memory>

#include "im/RemoteIMClient.h"
#include "model/ChatState.h"

class RemoteIMApplication final : public QObject {
    Q_OBJECT

public:
    RemoteIMApplication(QString ownerUserId, std::unique_ptr<RemoteIMClient> client, QObject* parent = nullptr);

    const ChatState& chatState() const;
    ChatState& chatState();
    RemoteIMClient& client();
    bool isConnected() const;

    void connectToService(int sdkAppId, const QString& userSig);
    void addContact(const QString& userId, const QString& displayName);
    void deleteContact(const QString& userId);
    void selectPeer(const QString& userId);
    void sendText(const QString& text);
    void sendImage(const QString& localPath);
    void sendVoicePlaceholder();

signals:
    void stateChanged();
    void connectionChanged(bool connected);
    void errorMessage(const QString& message);

private:
    void markMessage(const QString& messageId, RemoteIMMessageStatus status);
    void bindClientSignals();

    ChatState state_;
    std::unique_ptr<RemoteIMClient> client_;
    bool connected_ = false;
};
