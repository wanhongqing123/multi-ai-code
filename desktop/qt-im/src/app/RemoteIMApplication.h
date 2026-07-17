#pragma once

#include <QObject>
#include <QString>
#include <memory>

#include "im/RemoteIMClient.h"
#include "model/ChatState.h"
#include "storage/LocalMessageDatabase.h"

class RemoteIMApplication final : public QObject {
    Q_OBJECT

public:
    // database 可空：不带本地库时行为与从前一致（仅内存 + SDK 漫游）。
    // 带库时构造即加载全部本地历史；收发消息即时落库；SDK 漫游拉取降级为
    // 补充源，按消息 id 去重合并。
    RemoteIMApplication(QString ownerUserId,
                        std::unique_ptr<RemoteIMClient> client,
                        std::unique_ptr<LocalMessageDatabase> database = nullptr,
                        QObject* parent = nullptr);

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
    void persistMessage(const RemoteIMMessage& message);

    ChatState state_;
    std::unique_ptr<RemoteIMClient> client_;
    std::unique_ptr<LocalMessageDatabase> database_;
    bool connected_ = false;
};
