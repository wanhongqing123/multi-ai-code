#include "app/RemoteIMApplication.h"

#include <QFileInfo>
#include <QMimeDatabase>
#include <stdexcept>
#include <utility>

namespace {

// 分页启动：每会话只载最近一页，避免大历史全量进内存/上屏。
constexpr int kMessagesPageSize = 200;

QString peerOf(const RemoteIMMessage& message) {
    return message.direction == RemoteIMMessageDirection::Outgoing ? message.toUserId : message.fromUserId;
}

}  // namespace

RemoteIMApplication::RemoteIMApplication(QString ownerUserId,
                                         std::unique_ptr<RemoteIMClient> client,
                                         std::unique_ptr<LocalMessageDatabase> database,
                                         QObject* parent)
    : QObject(parent), state_(std::move(ownerUserId)), client_(std::move(client)), database_(std::move(database)) {
    if (!client_) {
        throw std::invalid_argument("RemoteIMClient is required");
    }
    client_->setParent(this);
    bindClientSignals();
    // 登录前先把本地库的全部历史恢复进内存：消息列表不再依赖 SDK 漫游
    //（只有几条），漫游随后按 id 去重合并进来。
    if (database_ && database_->isOpen()) {
        hasEarlierMessages_ = database_->loadRecentInto(state_, kMessagesPageSize);
    }
}

const ChatState& RemoteIMApplication::chatState() const { return state_; }
ChatState& RemoteIMApplication::chatState() { return state_; }
RemoteIMClient& RemoteIMApplication::client() { return *client_; }
bool RemoteIMApplication::isConnected() const { return connected_; }

bool RemoteIMApplication::hasEarlierMessages(const QString& peerId) const {
    return hasEarlierMessages_.value(peerId.trimmed(), false);
}

int RemoteIMApplication::loadEarlierMessages(const QString& peerId) {
    const QString peer = peerId.trimmed();
    if (peer.isEmpty() || !database_ || !hasEarlierMessages_.value(peer, false)) return 0;
    const QList<RemoteIMMessage> visible = state_.messagesWith(peer);
    if (visible.isEmpty()) return 0;
    const RemoteIMMessage& oldest = visible.first();
    const QList<RemoteIMMessage> earlier =
        database_->loadMessagesBefore(peer, oldest.createdAtMillis, oldest.id, kMessagesPageSize);
    for (const RemoteIMMessage& message : earlier) {
        state_.appendMessageForRestore(message);  // 展示顺序由 messagesWith 排序保证
    }
    hasEarlierMessages_.insert(peer, earlier.size() == kMessagesPageSize);
    if (!earlier.isEmpty()) emit stateChanged();
    return earlier.size();
}

void RemoteIMApplication::connectToService(int sdkAppId, const QString& userSig) {
    client_->connectToService(sdkAppId, state_.ownerUserId(), userSig, [this](bool ok, const QString& error) {
        connected_ = ok;
        emit connectionChanged(connected_);
        if (!ok) emit errorMessage(error.isEmpty() ? QStringLiteral("IM 连接失败") : error);
    });
}

void RemoteIMApplication::addContact(const QString& userId, const QString& displayName) {
    state_.upsertContact(RemoteIMContact{userId, displayName});
    state_.selectPeer(userId);
    if (database_) database_->upsertContact(RemoteIMContact{userId, displayName});
    emit stateChanged();
}

void RemoteIMApplication::deleteContact(const QString& userId) {
    const QString cleanUserId = userId.trimmed();
    if (cleanUserId.isEmpty()) return;
    client_->deleteContact(cleanUserId, [this, cleanUserId](bool ok, const QString& error) {
        if (!ok) {
            emit errorMessage(error.isEmpty() ? QStringLiteral("删除好友失败") : error);
            return;
        }
        state_.removeContactAndMessages(cleanUserId);
        if (database_) database_->removeContactCascade(cleanUserId);
        emit stateChanged();
    });
}

void RemoteIMApplication::selectPeer(const QString& userId) {
    state_.selectPeer(userId);
    emit stateChanged();
}

void RemoteIMApplication::sendText(const QString& text) {
    if (text.trimmed().isEmpty() || state_.selectedPeerId().isEmpty()) return;
    RemoteIMMessage message = state_.queueOutgoingText(text);
    persistMessage(message);
    emit stateChanged();

    client_->sendText(message.toUserId, message.text,
                      [this, messageId = message.id](bool ok, const QString& error, const RemoteIMSendReceipt& receipt) {
        // 发送成功且 SDK 给了稳定 id：内存与库同步换 id，漫游重投同一条消息
        // 时按主键去重（否则临时 UUID 与漫游 id 对不上，重启后会重复显示）。
        const QString effectiveId = adoptRemoteMessageId(messageId, ok ? receipt.remoteMessageId : QString());
        if (ok) {
            state_.updateMessageTime(effectiveId, receipt.createdAtMillis);
            if (database_) database_->updateMessageTime(effectiveId, receipt.createdAtMillis);
        }
        markMessage(effectiveId, ok ? RemoteIMMessageStatus::Sent : RemoteIMMessageStatus::Failed);
        if (!ok) emit errorMessage(error.isEmpty() ? QStringLiteral("文本消息发送失败") : error);
    });
}

void RemoteIMApplication::sendImage(const QString& localPath, const QString& text) {
    const QString cleanPath = localPath.trimmed();
    if (cleanPath.isEmpty() || state_.selectedPeerId().isEmpty()) return;

    QFileInfo info(cleanPath);
    const QString caption = text.trimmed();
    RemoteIMMessage message = state_.queueOutgoingImage(cleanPath, 0, 0, info.size(), caption);
    persistMessage(message);
    emit stateChanged();

    RemoteIMSendCompletion onDone =
        [this, messageId = message.id](bool ok, const QString& error, const RemoteIMSendReceipt& receipt) {
        const QString effectiveId = adoptRemoteMessageId(messageId, ok ? receipt.remoteMessageId : QString());
        if (ok) {
            state_.updateMessageTime(effectiveId, receipt.createdAtMillis);
            if (database_) database_->updateMessageTime(effectiveId, receipt.createdAtMillis);
        }
        markMessage(effectiveId, ok ? RemoteIMMessageStatus::Sent : RemoteIMMessageStatus::Failed);
        if (!ok) emit errorMessage(error.isEmpty() ? QStringLiteral("图片消息发送失败") : error);
    };
    if (caption.isEmpty()) {
        client_->sendImage(message.toUserId, cleanPath, std::move(onDone));
    } else {
        client_->sendImageWithText(message.toUserId, cleanPath, caption, std::move(onDone));
    }
}

void RemoteIMApplication::sendFile(const QString& localPath, const QString& text) {
    const QString cleanPath = localPath.trimmed();
    if (cleanPath.isEmpty() || state_.selectedPeerId().isEmpty()) return;

    QFileInfo info(cleanPath);
    if (!info.exists() || !info.isFile()) {
        emit errorMessage(QStringLiteral("文件不存在或不可读：%1").arg(cleanPath));
        return;
    }
    const QString fileName = info.fileName();
    const QString mimeType = QMimeDatabase().mimeTypeForFile(info).name();
    const QString caption = text.trimmed();
    RemoteIMMessage message = state_.queueOutgoingFile(cleanPath, fileName, mimeType, info.size(), caption);
    persistMessage(message);
    emit stateChanged();

    RemoteIMSendCompletion onDone =
        [this, messageId = message.id](bool ok, const QString& error, const RemoteIMSendReceipt& receipt) {
        const QString effectiveId = adoptRemoteMessageId(messageId, ok ? receipt.remoteMessageId : QString());
        if (ok) {
            state_.updateMessageTime(effectiveId, receipt.createdAtMillis);
            if (database_) database_->updateMessageTime(effectiveId, receipt.createdAtMillis);
        }
        markMessage(effectiveId, ok ? RemoteIMMessageStatus::Sent : RemoteIMMessageStatus::Failed);
        if (!ok) emit errorMessage(error.isEmpty() ? QStringLiteral("文件消息发送失败") : error);
    };
    if (caption.isEmpty()) {
        client_->sendFile(message.toUserId, cleanPath, fileName, std::move(onDone));
    } else {
        client_->sendFileWithText(message.toUserId, cleanPath, fileName, caption, std::move(onDone));
    }
}

void RemoteIMApplication::sendVoicePlaceholder() {
    emit errorMessage(QStringLiteral("语音消息需要接入桌面端原生录音与 IM SDK 后启用"));
}

void RemoteIMApplication::markMessage(const QString& messageId, RemoteIMMessageStatus status) {
    state_.updateMessageStatus(messageId, status);
    if (database_) database_->updateMessageStatus(messageId, status);
    emit stateChanged();
}

QString RemoteIMApplication::adoptRemoteMessageId(const QString& localId, const QString& remoteMessageId) {
    if (remoteMessageId.isEmpty() || remoteMessageId == localId) return localId;
    state_.adoptMessageId(localId, remoteMessageId);
    if (database_) database_->adoptMessageId(localId, remoteMessageId);
    return remoteMessageId;
}

void RemoteIMApplication::persistMessage(const RemoteIMMessage& message) {
    if (!database_) return;
    database_->upsertContact(RemoteIMContact{peerOf(message), peerOf(message)});
    database_->insertMessageIfAbsent(message, peerOf(message));
}

void RemoteIMApplication::bindClientSignals() {
    connect(client_.get(), &RemoteIMClient::contactsReceived, this, [this](const QList<RemoteIMContact>& contacts) {
        const bool shouldSelectFirstContact = state_.selectedPeerId().isEmpty();
        for (const RemoteIMContact& contact : contacts) {
            state_.upsertContact(contact);
            if (database_) database_->upsertContact(contact);
        }
        if (shouldSelectFirstContact && !contacts.isEmpty()) {
            state_.selectPeer(contacts.first().userId);
        }
        emit stateChanged();
    });
    // 漫游/历史与实时推送分两条同构通道（都带稳定 SDK 消息 id、按 id 去重落库），
    // 区别仅在实时通道的新入站消息会累计会话未读红点。
    connect(client_.get(), &RemoteIMClient::messagesReceived, this, [this](const QList<RemoteIMMessage>& messages) {
        ingestMessages(messages, /*live=*/false);
    });
    connect(client_.get(), &RemoteIMClient::liveMessagesReceived, this, [this](const QList<RemoteIMMessage>& messages) {
        ingestMessages(messages, /*live=*/true);
    });
    connect(client_.get(), &RemoteIMClient::incomingText, this, [this](const QString& fromUserId, const QString& text) {
        persistMessage(state_.receiveText(fromUserId, text));
        emit stateChanged();
    });
    connect(client_.get(), &RemoteIMClient::incomingImage, this, [this](const QString& fromUserId,
                                                                        const QString& localPath,
                                                                        int width,
                                                                        int height,
                                                                        qint64 sizeBytes) {
        persistMessage(state_.receiveImage(fromUserId, localPath, width, height, sizeBytes));
        emit stateChanged();
    });
    connect(client_.get(), &RemoteIMClient::incomingVoice, this, [this](const QString& fromUserId,
                                                                        const QString& localPath,
                                                                        int durationSeconds) {
        persistMessage(state_.receiveVoice(fromUserId, localPath, durationSeconds));
        emit stateChanged();
    });
    connect(client_.get(), &RemoteIMClient::incomingFile, this, [this](const QString& fromUserId,
                                                                       const QString& localPath,
                                                                       const QString& fileName,
                                                                       const QString& mimeType,
                                                                       qint64 sizeBytes) {
        persistMessage(state_.receiveFile(fromUserId, localPath, fileName, mimeType, sizeBytes));
        emit stateChanged();
    });
    connect(client_.get(), &RemoteIMClient::disconnected, this, [this] {
        connected_ = false;
        emit connectionChanged(false);
    });
}

void RemoteIMApplication::ingestMessages(const QList<RemoteIMMessage>& messages, bool live) {
    const bool shouldSelectFirstPeer = state_.selectedPeerId().isEmpty();
    QString firstPeerId;
    for (const RemoteIMMessage& message : messages) {
        const QString peerId = peerOf(message);
        if (peerId.isEmpty()) continue;
        if (firstPeerId.isEmpty()) firstPeerId = peerId;
        state_.upsertContact(RemoteIMContact{peerId, peerId});
        if (database_) {
            database_->upsertContact(RemoteIMContact{peerId, peerId});
            database_->insertMessageIfAbsent(message, peerId);
        }
        if (live) {
            state_.appendLiveMessage(message);
        } else {
            state_.appendMessageForRestore(message);
        }
    }
    if (shouldSelectFirstPeer && !firstPeerId.isEmpty()) state_.selectPeer(firstPeerId);
    emit stateChanged();
}
