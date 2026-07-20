#pragma once

#include <QHash>
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
    void sendText(const QString& peerId, const QString& text, RemoteIMSendCompletion completion) override;
    void sendImage(const QString& peerId, const QString& localPath, RemoteIMSendCompletion completion) override;
    void sendVoice(const QString& peerId, const QString& localPath, int durationSeconds, RemoteIMCompletion completion) override;

private:
    void syncInitialData();
    void fetchFriendList();
    void fetchConversationList();
    void fetchRecentMessages(const QString& conversationId, int conversationType);
    void handleFriendListPayload(const QString& jsonPayload);
    void handleConversationListPayload(const QString& jsonPayload);
    void handleHistoryMessagesPayload(const QString& jsonPayload);
    qint64 orderedMessageTime(const QString& peerId, qint64 sdkTimeMillis);
    QString sdkConfigJson() const;
    void handleIncomingMessages(const QString& jsonMessages);
    void handleIncomingMessage(const QJsonObject& message);
    // message 需预填 id/方向/时间等字段（附件 localPath 由下载完成后补上），
    // 下载完经 messagesReceived 通道送出，保持稳定 id 供本地库去重。
    void handleIncomingImageUrl(RemoteIMMessage message, const QString& url);
    void handleIncomingFileUrl(RemoteIMMessage message, const QString& url);
    static void complete(RemoteIMCompletion completion, int code, const QString& description);
    static QString compactJson(const QJsonObject& object);

    std::unique_ptr<TimSdkApi> api_;
    QNetworkAccessManager network_;
    QString currentUserId_;
    QHash<QString, qint64> orderedSecondByPeer_;
    QHash<QString, int> nextOrderInSecondByPeer_;
    bool connected_ = false;
};
