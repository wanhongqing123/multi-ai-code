// 文件作用：桌面端IM 会话、消息与登录；TimSdkRemoteIMClient 负责外部接口请求与结果转换，本文件给出其声明。
// 类型关系：TimSdkRemoteIMClient : public RemoteIMClient；审阅时可据此追踪父类、接口或回调契约。
// 对外边界：本文件给出 TimSdkRemoteIMClient 的类型和函数声明；参数、返回值和对象生命周期以此为准。
// 公开入口：complete、compactJson。
// 直接依赖：im/RemoteIMClient.h、im/TimSdkApi.h；实现与这些接口协作。
#pragma once

#include <QHash>
#include <QJsonObject>
#include <functional>
#include <memory>

#include "im/DesktopCurlAttachmentDownload.h"
#include "im/RemoteIMClient.h"
#include "im/TimSdkApi.h"

class TimSdkRemoteIMClient : public RemoteIMClient {
    Q_OBJECT

public:
    explicit TimSdkRemoteIMClient(QObject* parent = nullptr);
    explicit TimSdkRemoteIMClient(std::unique_ptr<TimSdkApi> api, QObject* parent = nullptr,
                                  QByteArray attachmentCaPem = {});
    ~TimSdkRemoteIMClient() override;

    void connectToService(int sdkAppId, const QString& userId, const QString& userSig,
                          RemoteIMCompletion completion) override;
    void disconnectFromService(RemoteIMCompletion completion) override;
    void deleteContact(const QString& userId, RemoteIMCompletion completion) override;
    void requestFriend(const QString& userId, RemoteIMCompletion completion) override;
    void refreshFriendList() override;
    void joinedGroups(RemoteIMGroupsCompletion completion) override;
    void createGroup(const QString& name, const QStringList& memberIds,
                     RemoteIMGroupCompletion completion) override;
    void sendGroupText(const QString& groupId, const QString& text,
                       RemoteIMSendCompletion completion) override;
    void groupHistory(const QString& groupId, int count, qint64 beforeSequence,
                      RemoteIMGroupHistoryCompletion completion) override;
    void markGroupRead(const QString& groupId, RemoteIMCompletion completion) override;
    void friendRequests(RemoteIMFriendRequestsCompletion completion) override;
    void decideFriendRequest(const QString& userId, bool accept,
                             RemoteIMCompletion completion) override;
    void sendText(const QString& peerId, const QString& text, RemoteIMSendCompletion completion) override;
    void sendActivity(const QString& peerId, const RemoteIMActivitySignal& signal,
                      RemoteIMCompletion completion) override;
    void sendTextWithQuote(const QString& peerId, const QString& text,
                           const RemoteIMQuote& quote, bool hasQuote,
                           RemoteIMSendCompletion completion) override;
    void sendApprovalDecision(const QString& peerId,
                              const QString& token,
                              RemoteIMApprovalAction action,
                              RemoteIMSendCompletion completion) override;
    void sendMachineText(const QString& peerId, const QString& text, RemoteIMSendCompletion completion) override;
    void sendImage(const QString& peerId, const QString& localPath, RemoteIMSendCompletion completion) override;
    void sendVoice(const QString& peerId, const QString& localPath, int durationSeconds,
                   RemoteIMCompletion completion) override;
    void sendFile(const QString& peerId, const QString& localPath, const QString& fileName,
                  RemoteIMSendCompletion completion) override;
    void sendImageWithText(const QString& peerId, const QString& imagePath, const QString& text, bool captionAbove,
                           RemoteIMSendCompletion completion) override;
    void sendFileWithText(const QString& peerId, const QString& localPath, const QString& fileName, const QString& text,
                          bool captionAbove, RemoteIMSendCompletion completion) override;
    void sendVideo(const QString& peerId, const RemoteIMVideoPayload& video,
                   RemoteIMSendCompletion completion) override;
    void sendVideoWithText(const QString& peerId, const RemoteIMVideoPayload& video, const QString& text,
                           bool captionAbove, RemoteIMSendCompletion completion) override;

private:
    void sendTextWithOrigin(const QString& peerId,
                            const QString& text,
                            RemoteIMMessageOrigin origin,
                            RemoteIMSendCompletion completion,
                            const QJsonObject& interaction = QJsonObject(),
                            const RemoteIMQuote& quote = RemoteIMQuote(),
                            bool hasQuote = false);
    void syncInitialData();
    void handleSelfProfilePayload(const QString& payload);
    void fetchFriendList();
    void fetchConversationList();
    void fetchRecentMessages(const QString& conversationId, int conversationType);
    void handleFriendListPayload(const QString& jsonPayload);
    void handleConversationListPayload(const QString& jsonPayload, bool includeDirectChats = true);
    void handleHistoryMessagesPayload(const QString& jsonPayload);
    qint64 orderedMessageTime(const QString& peerId, qint64 sdkTimeMillis);
    QString sdkConfigJson() const;
    void handleIncomingMessages(const QString& jsonMessages);
    void handleIncomingMessage(const QJsonObject& message);
    static RemoteIMGroupMessage groupTextMessage(const QJsonObject& message, const QString& groupId,
                                                 const QString& ownerUserId);
    void handleGroupHistoryPayload(const QString& groupId, const QString& payload, bool live);
    // message 需预填 id/方向/时间等字段（附件 localPath 由下载完成后补上），
    // 下载完经 messagesReceived 通道送出，保持稳定 id 供本地库去重。
    // live=true 表示来自实时推送（RecvNewMsg），送 liveMessagesReceived（计未读红点）；
    // false 表示漫游/历史解析，送 messagesReceived。下载助手被两条链路共用，须透传。
    void handleIncomingImageUrl(RemoteIMMessage message, const QString& url, bool live);
    void handleIncomingFileUrl(RemoteIMMessage message, const QString& url, bool live);
    // 视频要下两个文件（视频本体 + 封面）。封面失败不阻塞：气泡会退化成深色底 +
    // 播放角标，仍然能点开播放。
    // 语音：SDK 给的是 HTTPS URL，本端下载后才有本地文件可播。
    void handleIncomingVoiceUrl(RemoteIMMessage message, const QString& url, bool live);
    void handleIncomingVideoUrls(RemoteIMMessage message, const QString& videoUrl,
                                 const QString& coverUrl, bool live);
    void fetchAttachment(const QString& url, const QString& targetPath, qint64 maximumBytes,
                         std::function<void(DesktopCurlAttachmentResult)> completion);
    // origin 有效时表示「这批消息是某次异步请求发起时那个账号的」。
    // 与当前账号不符就整批丢弃：换账号之后晚到的下载结果不能进新账号的库。
    // 同步路径不传 origin（调用期间账号不可能变）。
    // 返回是否真的把信号发出去了。调用方据此决定记 Delivered 还是
    // DroppedForAccountSwitch——「下完了」和「进了会话」是两件事。
    bool emitReceivedMessages(const QList<RemoteIMMessage>& messages, bool live,
                              const RemoteDiagnostics::AccountTag& origin = {});
    void recordAttachmentPhase(const RemoteDiagnostics::AccountTag& account,
                               const RemoteIMMessage& message,
                               RemoteDiagnostics::AttachmentPhase phase,
                               int code = 0);
    static void complete(RemoteIMCompletion completion, int code, const QString& description);
    static QString compactJson(const QJsonObject& object);

    std::unique_ptr<TimSdkApi> api_;
    QByteArray attachmentCaPem_;
    std::shared_ptr<MaiDownloadCancel> downloadsCanceled_;
    QString currentUserId_;
    // AccountTag 的另一半。以前没存过——只有连接日志里出现过一次。
    quint64 sdkAppId_ = 0;
    QHash<QString, qint64> orderedSecondByPeer_;
    QHash<QString, int> nextOrderInSecondByPeer_;
    bool connected_ = false;
    quint64 profileSession_ = 0;
    quint64 selfProfileRevision_ = 0;
    quint64 conversationRevision_ = 0;
    quint64 conversationFetchGeneration_ = 0;

public:
    RemoteDiagnostics::AccountTag currentAccount() const override;
};
