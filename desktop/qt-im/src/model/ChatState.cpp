#include "model/ChatState.h"

#include <QFileInfo>
#include <QtGlobal>
#include <algorithm>
#include <stdexcept>

ChatState::ChatState(QString ownerUserId)
    : ownerUserId_(clean(ownerUserId)) {
    if (ownerUserId_.isEmpty()) {
        throw std::invalid_argument("ownerUserId is required");
    }
}

QString ChatState::ownerUserId() const { return ownerUserId_; }
QString ChatState::selectedPeerId() const { return selectedPeerId_; }
QList<RemoteIMContact> ChatState::contacts() const { return contacts_; }
QList<RemoteIMMessage> ChatState::messages() const { return messages_; }

void ChatState::upsertContact(const RemoteIMContact& contact) {
    const QString userId = clean(contact.userId);
    if (userId.isEmpty()) return;
    const QString displayName = clean(contact.displayName).isEmpty() ? userId : clean(contact.displayName);
    for (RemoteIMContact& existing : contacts_) {
        if (existing.userId == userId) {
            if (displayName == userId && !existing.displayName.isEmpty() && existing.displayName != userId) {
                return;
            }
            existing.displayName = displayName;
            return;
        }
    }
    contacts_.append(RemoteIMContact{userId, displayName});
}

void ChatState::removeContactAndMessages(const QString& userId) {
    const QString cleanUserId = clean(userId);
    if (cleanUserId.isEmpty()) return;
    contacts_.erase(std::remove_if(contacts_.begin(), contacts_.end(), [&cleanUserId](const RemoteIMContact& contact) {
        return contact.userId == cleanUserId;
    }), contacts_.end());
    messages_.erase(std::remove_if(messages_.begin(), messages_.end(), [&cleanUserId](const RemoteIMMessage& message) {
        return message.fromUserId == cleanUserId || message.toUserId == cleanUserId;
    }), messages_.end());
    const bool selectedMissing = !selectedPeerId_.isEmpty()
        && std::none_of(contacts_.cbegin(), contacts_.cend(), [this](const RemoteIMContact& contact) {
               return contact.userId == selectedPeerId_;
           });
    if (selectedPeerId_ == cleanUserId || selectedMissing) {
        selectedPeerId_ = contacts_.isEmpty() ? QString() : contacts_.first().userId;
    }
}

void ChatState::selectPeer(const QString& userId) {
    const QString peerId = clean(userId);
    if (peerId.isEmpty()) return;
    bool exists = false;
    for (const RemoteIMContact& contact : contacts_) {
        if (contact.userId == peerId) {
            exists = true;
            break;
        }
    }
    if (!exists) upsertContact(RemoteIMContact{peerId, peerId});
    selectedPeerId_ = peerId;
}

RemoteIMMessage ChatState::queueOutgoingText(const QString& text) {
    const QString cleanText = clean(text);
    if (cleanText.isEmpty()) throw std::invalid_argument("text is required");
    RemoteIMMessage message;
    message.fromUserId = ownerUserId_;
    message.toUserId = requireSelectedPeer();
    message.text = cleanText;
    message.direction = RemoteIMMessageDirection::Outgoing;
    message.status = RemoteIMMessageStatus::Pending;
    messages_.append(message);
    return message;
}

RemoteIMMessage ChatState::queueOutgoingImage(const QString& localPath, int width, int height, qint64 sizeBytes) {
    const QString cleanPath = clean(localPath);
    if (cleanPath.isEmpty()) throw std::invalid_argument("localPath is required");
    RemoteIMMessage message;
    message.fromUserId = ownerUserId_;
    message.toUserId = requireSelectedPeer();
    message.text = "[图片消息] " + fileName(cleanPath);
    message.direction = RemoteIMMessageDirection::Outgoing;
    message.status = RemoteIMMessageStatus::Pending;
    message.hasImage = true;
    message.image = RemoteIMImageAttachment{cleanPath, width, height, sizeBytes};
    messages_.append(message);
    return message;
}

RemoteIMMessage ChatState::queueOutgoingVoice(const QString& localPath, int durationSeconds) {
    const QString cleanPath = clean(localPath);
    if (cleanPath.isEmpty()) throw std::invalid_argument("localPath is required");
    RemoteIMMessage message;
    message.fromUserId = ownerUserId_;
    message.toUserId = requireSelectedPeer();
    message.text = QString("[语音消息 %1s]").arg(qMax(1, durationSeconds));
    message.direction = RemoteIMMessageDirection::Outgoing;
    message.status = RemoteIMMessageStatus::Pending;
    message.hasVoice = true;
    message.voice = RemoteIMVoiceAttachment{cleanPath, qMax(1, durationSeconds)};
    messages_.append(message);
    return message;
}

RemoteIMMessage ChatState::receiveText(const QString& fromUserId, const QString& text) {
    const QString peerId = clean(fromUserId);
    if (peerId.isEmpty()) throw std::invalid_argument("fromUserId is required");
    upsertContact(RemoteIMContact{peerId, peerId});
    if (selectedPeerId_.isEmpty()) selectedPeerId_ = peerId;
    RemoteIMMessage message;
    message.fromUserId = peerId;
    message.toUserId = ownerUserId_;
    message.text = clean(text);
    message.direction = RemoteIMMessageDirection::Incoming;
    message.status = RemoteIMMessageStatus::Received;
    messages_.append(message);
    return message;
}

RemoteIMMessage ChatState::receiveImage(const QString& fromUserId, const QString& localPath, int width, int height, qint64 sizeBytes) {
    const QString peerId = clean(fromUserId);
    const QString cleanPath = clean(localPath);
    if (peerId.isEmpty()) throw std::invalid_argument("fromUserId is required");
    if (cleanPath.isEmpty()) throw std::invalid_argument("localPath is required");
    upsertContact(RemoteIMContact{peerId, peerId});
    if (selectedPeerId_.isEmpty()) selectedPeerId_ = peerId;
    RemoteIMMessage message;
    message.fromUserId = peerId;
    message.toUserId = ownerUserId_;
    message.text = "[图片消息] " + fileName(cleanPath);
    message.direction = RemoteIMMessageDirection::Incoming;
    message.status = RemoteIMMessageStatus::Received;
    message.hasImage = true;
    message.image = RemoteIMImageAttachment{cleanPath, width, height, sizeBytes};
    messages_.append(message);
    return message;
}

RemoteIMMessage ChatState::receiveVoice(const QString& fromUserId, const QString& localPath, int durationSeconds) {
    const QString peerId = clean(fromUserId);
    if (peerId.isEmpty()) throw std::invalid_argument("fromUserId is required");
    upsertContact(RemoteIMContact{peerId, peerId});
    if (selectedPeerId_.isEmpty()) selectedPeerId_ = peerId;
    RemoteIMMessage message;
    message.fromUserId = peerId;
    message.toUserId = ownerUserId_;
    message.text = QString("[语音消息 %1s]").arg(qMax(1, durationSeconds));
    message.direction = RemoteIMMessageDirection::Incoming;
    message.status = RemoteIMMessageStatus::Received;
    message.hasVoice = true;
    message.voice = RemoteIMVoiceAttachment{clean(localPath), qMax(1, durationSeconds)};
    messages_.append(message);
    return message;
}

QList<RemoteIMMessage> ChatState::messagesWith(const QString& peerId) const {
    QList<RemoteIMMessage> result;
    const QString cleanPeerId = clean(peerId);
    for (const RemoteIMMessage& message : messages_) {
        if (message.fromUserId == cleanPeerId || message.toUserId == cleanPeerId) {
            result.append(message);
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const RemoteIMMessage& lhs, const RemoteIMMessage& rhs) {
        return lhs.createdAtMillis < rhs.createdAtMillis;
    });
    return result;
}

bool ChatState::updateMessageStatus(const QString& messageId, RemoteIMMessageStatus status) {
    for (RemoteIMMessage& message : messages_) {
        if (message.id == messageId) {
            message.status = status;
            return true;
        }
    }
    return false;
}

void ChatState::appendMessageForRestore(const RemoteIMMessage& message) {
    messages_.append(message);
}

QString ChatState::requireSelectedPeer() const {
    if (selectedPeerId_.isEmpty()) throw std::logic_error("selected peer is required");
    return selectedPeerId_;
}

QString ChatState::clean(const QString& value) {
    return value.trimmed();
}

QString ChatState::fileName(const QString& path) {
    const QString name = QFileInfo(path).fileName();
    return name.isEmpty() ? QStringLiteral("file") : name;
}
