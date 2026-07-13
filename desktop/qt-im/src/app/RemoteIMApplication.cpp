#include "app/RemoteIMApplication.h"

#include <QFileInfo>
#include <stdexcept>
#include <utility>

RemoteIMApplication::RemoteIMApplication(QString ownerUserId, std::unique_ptr<RemoteIMClient> client, QObject* parent)
    : QObject(parent), state_(std::move(ownerUserId)), client_(std::move(client)) {
    if (!client_) {
        throw std::invalid_argument("RemoteIMClient is required");
    }
    client_->setParent(this);
    bindClientSignals();
}

const ChatState& RemoteIMApplication::chatState() const { return state_; }
ChatState& RemoteIMApplication::chatState() { return state_; }
RemoteIMClient& RemoteIMApplication::client() { return *client_; }
bool RemoteIMApplication::isConnected() const { return connected_; }

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
    emit stateChanged();
}

void RemoteIMApplication::deleteContact(const QString& userId) {
    state_.removeContactAndMessages(userId);
    emit stateChanged();
}

void RemoteIMApplication::selectPeer(const QString& userId) {
    state_.selectPeer(userId);
    emit stateChanged();
}

void RemoteIMApplication::sendText(const QString& text) {
    if (text.trimmed().isEmpty() || state_.selectedPeerId().isEmpty()) return;
    RemoteIMMessage message = state_.queueOutgoingText(text);
    emit stateChanged();

    client_->sendText(message.toUserId, message.text, [this, messageId = message.id](bool ok, const QString& error) {
        markMessage(messageId, ok ? RemoteIMMessageStatus::Sent : RemoteIMMessageStatus::Failed);
        if (!ok) emit errorMessage(error.isEmpty() ? QStringLiteral("文本消息发送失败") : error);
    });
}

void RemoteIMApplication::sendImage(const QString& localPath) {
    const QString cleanPath = localPath.trimmed();
    if (cleanPath.isEmpty() || state_.selectedPeerId().isEmpty()) return;

    QFileInfo info(cleanPath);
    RemoteIMMessage message = state_.queueOutgoingImage(cleanPath, 0, 0, info.size());
    emit stateChanged();

    client_->sendImage(message.toUserId, cleanPath, [this, messageId = message.id](bool ok, const QString& error) {
        markMessage(messageId, ok ? RemoteIMMessageStatus::Sent : RemoteIMMessageStatus::Failed);
        if (!ok) emit errorMessage(error.isEmpty() ? QStringLiteral("图片消息发送失败") : error);
    });
}

void RemoteIMApplication::sendVoicePlaceholder() {
    emit errorMessage(QStringLiteral("语音消息需要接入桌面端原生录音与 IM SDK 后启用"));
}

void RemoteIMApplication::markMessage(const QString& messageId, RemoteIMMessageStatus status) {
    state_.updateMessageStatus(messageId, status);
    emit stateChanged();
}

void RemoteIMApplication::bindClientSignals() {
    connect(client_.get(), &RemoteIMClient::contactsReceived, this, [this](const QList<RemoteIMContact>& contacts) {
        const bool shouldSelectFirstContact = state_.selectedPeerId().isEmpty();
        for (const RemoteIMContact& contact : contacts) {
            state_.upsertContact(contact);
        }
        if (shouldSelectFirstContact && !contacts.isEmpty()) {
            state_.selectPeer(contacts.first().userId);
        }
        emit stateChanged();
    });
    connect(client_.get(), &RemoteIMClient::messagesReceived, this, [this](const QList<RemoteIMMessage>& messages) {
        const bool shouldSelectFirstPeer = state_.selectedPeerId().isEmpty();
        QString firstPeerId;
        for (const RemoteIMMessage& message : messages) {
            const QString peerId = message.direction == RemoteIMMessageDirection::Outgoing ? message.toUserId : message.fromUserId;
            if (peerId.isEmpty()) continue;
            if (firstPeerId.isEmpty()) firstPeerId = peerId;
            state_.upsertContact(RemoteIMContact{peerId, peerId});
            state_.appendMessageForRestore(message);
        }
        if (shouldSelectFirstPeer && !firstPeerId.isEmpty()) state_.selectPeer(firstPeerId);
        emit stateChanged();
    });
    connect(client_.get(), &RemoteIMClient::incomingText, this, [this](const QString& fromUserId, const QString& text) {
        state_.receiveText(fromUserId, text);
        emit stateChanged();
    });
    connect(client_.get(), &RemoteIMClient::incomingImage, this, [this](const QString& fromUserId,
                                                                        const QString& localPath,
                                                                        int width,
                                                                        int height,
                                                                        qint64 sizeBytes) {
        state_.receiveImage(fromUserId, localPath, width, height, sizeBytes);
        emit stateChanged();
    });
    connect(client_.get(), &RemoteIMClient::incomingVoice, this, [this](const QString& fromUserId,
                                                                        const QString& localPath,
                                                                        int durationSeconds) {
        state_.receiveVoice(fromUserId, localPath, durationSeconds);
        emit stateChanged();
    });
    connect(client_.get(), &RemoteIMClient::disconnected, this, [this] {
        connected_ = false;
        emit connectionChanged(false);
    });
}
