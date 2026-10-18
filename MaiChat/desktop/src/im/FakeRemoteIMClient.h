// 文件作用：桌面端IM 会话、消息与登录；FakeRemoteIMClient 负责外部接口请求与结果转换，本文件给出其声明。
// 类型关系：FakeRemoteIMClient final : public RemoteIMClient；审阅时可据此追踪父类、接口或回调契约。
// 对外边界：本文件给出 FakeRemoteIMClient 的类型和函数声明；参数、返回值和对象生命周期以此为准。
// 直接依赖：im/RemoteIMClient.h；实现与这些接口协作。
#pragma once

#include "im/RemoteIMClient.h"

class FakeRemoteIMClient final : public RemoteIMClient {
    Q_OBJECT

public:
    explicit FakeRemoteIMClient(QObject* parent = nullptr);
    RemoteDiagnostics::AccountTag currentAccount() const override { return {sdkAppId_, connectedUserId_}; }

    void connectToService(int sdkAppId, const QString& userId, const QString& userSig,
                          RemoteIMCompletion completion) override;
    void disconnectFromService(RemoteIMCompletion completion) override;
    void deleteContact(const QString& userId, RemoteIMCompletion completion) override;
    void requestFriend(const QString& userId, RemoteIMCompletion completion) override;
    void sendText(const QString& peerId, const QString& text, RemoteIMSendCompletion completion) override;
    void sendApprovalDecision(const QString& peerId,
                              const QString& token,
                              RemoteIMApprovalAction action,
                              RemoteIMSendCompletion completion) override;
    void sendImage(const QString& peerId, const QString& localPath, RemoteIMSendCompletion completion) override;
    void sendVoice(const QString& peerId, const QString& localPath, int durationSeconds,
                   RemoteIMCompletion completion) override;
    void sendFile(const QString& peerId, const QString& localPath, const QString& fileName,
                  RemoteIMSendCompletion completion) override;
    void sendVideo(const QString& peerId, const RemoteIMVideoPayload& video,
                   RemoteIMSendCompletion completion) override;

    QString connectedUserId() const;
    QString lastDeletedContactId() const;
    QString lastTextPeerId() const;
    QString lastText() const;
    QString lastApprovalToken() const;
    RemoteIMApprovalAction lastApprovalAction() const;
    QString lastImagePeerId() const;
    QString lastImagePath() const;
    QString lastFilePeerId() const;
    QString lastFilePath() const;
    QString lastFileName() const;
    QString lastVideoPeerId() const;
    RemoteIMVideoPayload lastVideo() const;
    void failNext(const QString& error);
    void deferNextSend();
    void finishDeferredSend();
    void emitIncomingText(const QString& fromUserId, const QString& text);
    void emitIncomingImage(const QString& fromUserId, const QString& localPath, int width, int height,
                           qint64 sizeBytes);

private:
    void complete(RemoteIMCompletion completion);
    void completeSend(RemoteIMSendCompletion completion);

    QString connectedUserId_;
    quint64 sdkAppId_ = 0;
    QString lastDeletedContactId_;
    QString lastTextPeerId_;
    QString lastText_;
    QString lastApprovalToken_;
    RemoteIMApprovalAction lastApprovalAction_ = RemoteIMApprovalAction::Reject;
    QString lastImagePeerId_;
    QString lastImagePath_;
    QString lastFilePeerId_;
    QString lastFilePath_;
    QString lastFileName_;
    QString lastVideoPeerId_;
    RemoteIMVideoPayload lastVideo_;
    QString nextError_;
    bool deferNextSend_ = false;
    RemoteIMSendCompletion deferredSendCompletion_;
    int sentSequence_ = 0;
};
