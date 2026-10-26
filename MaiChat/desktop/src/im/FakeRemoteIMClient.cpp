// 文件作用：桌面端IM 会话、消息与登录；FakeRemoteIMClient 负责外部接口请求与结果转换，本文件给出其实现。
// 主要入口：connectToService、disconnectFromService、deleteContact、sendText。
// 实现边界：与 FakeRemoteIMClient 的公开声明配套，调用方通过相应接口进入。
#include "im/FakeRemoteIMClient.h"

FakeRemoteIMClient::FakeRemoteIMClient(QObject* parent) : RemoteIMClient(parent) {}

void FakeRemoteIMClient::connectToService(int sdkAppId, const QString& userId, const QString&,
                                          RemoteIMCompletion completion) {
    sdkAppId_ = sdkAppId > 0 ? quint64(sdkAppId) : 0;
    connectedUserId_ = userId.trimmed();
    complete(std::move(completion));
}

void FakeRemoteIMClient::disconnectFromService(RemoteIMCompletion completion) {
    connectedUserId_.clear();
    sdkAppId_ = 0;
    complete(std::move(completion));
}

void FakeRemoteIMClient::deleteContact(const QString& userId, RemoteIMCompletion completion) {
    lastDeletedContactId_ = userId.trimmed();
    complete(std::move(completion));
}

void FakeRemoteIMClient::requestFriend(const QString& userId,
                                       RemoteIMCompletion completion) {
    lastFriendRequestId_ = userId;
    complete(std::move(completion));
}

void FakeRemoteIMClient::joinedGroups(RemoteIMGroupsCompletion completion) {
    if (completion) completion(true, {}, groups_);
}

void FakeRemoteIMClient::createGroup(const QString& name, const QStringList& memberIds,
                                     RemoteIMGroupCompletion completion) {
    if (!nextError_.isEmpty()) {
        const QString error = nextError_;
        nextError_.clear();
        if (completion) completion(false, error, {});
        return;
    }
    const RemoteIMGroup group{QStringLiteral("fake-group-%1").arg(groups_.size() + 1),
                              name.trimmed(), memberIds.size() + 1};
    groups_.append(group);
    emit groupsReceived({group});
    if (completion) completion(true, {}, group);
}

void FakeRemoteIMClient::sendGroupText(const QString& groupId, const QString& text,
                                       RemoteIMSendCompletion completion) {
    lastGroupId_ = groupId;
    lastGroupText_ = text;
    completeSend(std::move(completion));
}

void FakeRemoteIMClient::groupHistory(const QString&, int, qint64,
                                      RemoteIMGroupHistoryCompletion completion) {
    ++groupHistoryRequestCount_;
    if (deferGroupHistory_) {
        deferGroupHistory_ = false;
        deferredGroupHistoryCompletion_ = std::move(completion);
        return;
    }
    if (completion) completion(true, {}, {});
}

void FakeRemoteIMClient::deferNextGroupHistory() {
    deferGroupHistory_ = true;
}

void FakeRemoteIMClient::finishDeferredGroupHistory(const QList<RemoteIMGroupMessage>& messages) {
    auto completion = std::move(deferredGroupHistoryCompletion_);
    if (completion) completion(true, {}, RemoteIMGroupHistoryPage{messages, 0, false});
}

void FakeRemoteIMClient::markGroupRead(const QString& groupId, RemoteIMCompletion completion) {
    lastMarkedGroupId_ = groupId;
    if (completion) completion(true, {});
}

void FakeRemoteIMClient::emitIncomingGroupText(const QString& groupId, const QString& senderId,
                                                const QString& text, const QString& messageId) {
    RemoteIMGroupMessage message;
    message.id = messageId;
    message.groupId = groupId;
    message.senderId = senderId;
    message.text = text;
    message.createdAtMillis = QDateTime::currentMSecsSinceEpoch();
    emit groupMessagesReceived({message}, true);
}

void FakeRemoteIMClient::removeGroupForTest(const QString& groupId) {
    for (int index = groups_.size() - 1; index >= 0; --index)
        if (groups_.at(index).groupId == groupId) groups_.removeAt(index);
    emit groupMembershipChanged();
}

QString FakeRemoteIMClient::lastFriendRequestId() const {
    return lastFriendRequestId_;
}

void FakeRemoteIMClient::sendText(const QString& peerId, const QString& text, RemoteIMSendCompletion completion) {
    lastTextPeerId_ = peerId.trimmed();
    lastText_ = text.trimmed();
    completeSend(std::move(completion));
}

void FakeRemoteIMClient::sendApprovalDecision(const QString& peerId,
                                              const QString& token,
                                              RemoteIMApprovalAction action,
                                              RemoteIMSendCompletion completion) {
    lastTextPeerId_ = peerId.trimmed();
    lastApprovalToken_ = token.trimmed();
    lastApprovalAction_ = action;
    lastText_ = QStringLiteral("审批操作：%1").arg(remoteIMApprovalActionTitle(action));
    completeSend(std::move(completion));
}

void FakeRemoteIMClient::sendImage(const QString& peerId, const QString& localPath, RemoteIMSendCompletion completion) {
    lastImagePeerId_ = peerId.trimmed();
    lastImagePath_ = localPath.trimmed();
    completeSend(std::move(completion));
}

void FakeRemoteIMClient::sendVoice(const QString&, const QString&, int, RemoteIMCompletion completion) {
    complete(std::move(completion));
}

void FakeRemoteIMClient::sendFile(const QString& peerId, const QString& localPath, const QString& fileName,
                                  RemoteIMSendCompletion completion) {
    lastFilePeerId_ = peerId.trimmed();
    lastFilePath_ = localPath.trimmed();
    lastFileName_ = fileName.trimmed();
    completeSend(std::move(completion));
}

void FakeRemoteIMClient::sendVideo(const QString& peerId, const RemoteIMVideoPayload& video,
                                   RemoteIMSendCompletion completion) {
    lastVideoPeerId_ = peerId.trimmed();
    lastVideo_ = video;
    completeSend(std::move(completion));
}

QString FakeRemoteIMClient::connectedUserId() const { return connectedUserId_; }
QString FakeRemoteIMClient::lastDeletedContactId() const { return lastDeletedContactId_; }
QString FakeRemoteIMClient::lastTextPeerId() const { return lastTextPeerId_; }
QString FakeRemoteIMClient::lastText() const { return lastText_; }
QString FakeRemoteIMClient::lastApprovalToken() const { return lastApprovalToken_; }
RemoteIMApprovalAction FakeRemoteIMClient::lastApprovalAction() const { return lastApprovalAction_; }
QString FakeRemoteIMClient::lastImagePeerId() const { return lastImagePeerId_; }
QString FakeRemoteIMClient::lastImagePath() const { return lastImagePath_; }
QString FakeRemoteIMClient::lastFilePeerId() const { return lastFilePeerId_; }
QString FakeRemoteIMClient::lastFilePath() const { return lastFilePath_; }
QString FakeRemoteIMClient::lastFileName() const { return lastFileName_; }
QString FakeRemoteIMClient::lastVideoPeerId() const { return lastVideoPeerId_; }
RemoteIMVideoPayload FakeRemoteIMClient::lastVideo() const { return lastVideo_; }

void FakeRemoteIMClient::failNext(const QString& error) {
    nextError_ = error;
}

void FakeRemoteIMClient::deferNextSend() {
    deferNextSend_ = true;
}

void FakeRemoteIMClient::finishDeferredSend() {
    if (!deferredSendCompletion_) return;
    completeSend(std::move(deferredSendCompletion_));
}

void FakeRemoteIMClient::emitIncomingText(const QString& fromUserId, const QString& text) {
    emit incomingText(fromUserId.trimmed(), text.trimmed());
}

void FakeRemoteIMClient::emitIncomingImage(const QString& fromUserId, const QString& localPath, int width, int height,
                                           qint64 sizeBytes) {
    emit incomingImage(fromUserId.trimmed(), localPath.trimmed(), width, height, sizeBytes);
}

void FakeRemoteIMClient::complete(RemoteIMCompletion completion) {
    if (!completion) return;
    if (nextError_.isEmpty()) {
        completion(true, QString());
        return;
    }
    const QString error = nextError_;
    nextError_.clear();
    completion(false, error);
}

void FakeRemoteIMClient::completeSend(RemoteIMSendCompletion completion) {
    if (!completion) return;
    if (deferNextSend_) {
        deferNextSend_ = false;
        deferredSendCompletion_ = std::move(completion);
        return;
    }
    if (nextError_.isEmpty()) {
        // 模拟 SDK 回执的稳定消息 id（与真实实现的 <msg_id>#<elem> 结构对齐）。
        const qint64 sequence = ++sentSequence_;
        completion(true, QString(), RemoteIMSendReceipt{
            QStringLiteral("fake-remote-%1#0").arg(sequence),
            QDateTime::currentMSecsSinceEpoch()
        });
        return;
    }
    const QString error = nextError_;
    nextError_.clear();
    completion(false, error, {});
}
