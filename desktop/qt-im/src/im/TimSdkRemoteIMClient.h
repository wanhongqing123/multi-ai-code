#pragma once

#include <QNetworkAccessManager>
#include <memory>

#include "im/RemoteIMClient.h"
#include "im/TimSdkApi.h"

class TimSdkRemoteIMClient : public RemoteIMClient {
    Q_OBJECT

public:
    explicit TimSdkRemoteIMClient(QObject* parent = nullptr);
    explicit TimSdkRemoteIMClient(std::unique_ptr<TimSdkApi> api, QObject* parent = nullptr);
    ~TimSdkRemoteIMClient() override;

    void connectToService(int sdkAppId, const QString& userId, const QString& userSig, RemoteIMCompletion completion) override;
    void disconnectFromService(RemoteIMCompletion completion) override;
    void deleteContact(const QString& userId, RemoteIMCompletion completion) override;
    void sendText(const QString& peerId, const QString& text, RemoteIMCompletion completion) override;
    void sendImage(const QString& peerId, const QString& localPath, RemoteIMCompletion completion) override;
    void sendVoice(const QString& peerId, const QString& localPath, int durationSeconds, RemoteIMCompletion completion) override;

private:
    void syncInitialData();
    void fetchFriendList();
    void fetchConversationList();
    void fetchRecentMessages(const QString& conversationId, int conversationType);
    void handleFriendListPayload(const QString& jsonPayload);
    void handleConversationListPayload(const QString& jsonPayload);
    void handleHistoryMessagesPayload(const QString& jsonPayload);
    QString sdkConfigJson() const;
    void handleIncomingMessages(const QString& jsonMessages);
    void handleIncomingMessage(const QJsonObject& message);
    void handleIncomingImageUrl(const QString& fromUserId, const QString& url, int width, int height, qint64 sizeBytes);
    void handleIncomingFileUrl(const QString& fromUserId, const QString& url, const QString& fileName, qint64 sizeBytes);
    static void complete(RemoteIMCompletion completion, int code, const QString& description);
    static QString compactJson(const QJsonObject& object);

    std::unique_ptr<TimSdkApi> api_;
    QNetworkAccessManager network_;
    QString currentUserId_;
    bool connected_ = false;
};
