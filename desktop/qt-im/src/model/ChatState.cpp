#include "model/ChatState.h"

#include <QFileInfo>
#include <QStringList>
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
    messages_.erase(std::remove_if(messages_.begin(), messages_.end(), [this, &cleanUserId](const RemoteIMMessage& message) {
        const bool removing = message.fromUserId == cleanUserId || message.toUserId == cleanUserId;
        if (removing) messageIds_.remove(message.id);
        return removing;
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
    message.createdAtMillis = (message.createdAtMillis / 1000) * 1000;
    appendTracked(message);
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
    message.createdAtMillis = (message.createdAtMillis / 1000) * 1000;
    message.image = RemoteIMImageAttachment{cleanPath, width, height, sizeBytes};
    appendTracked(message);
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
    appendTracked(message);
    return message;
}

RemoteIMMessage ChatState::queueOutgoingFile(const QString& localPath, const QString& fileName, const QString& mimeType, qint64 sizeBytes) {
    const QString cleanPath = clean(localPath);
    if (cleanPath.isEmpty()) throw std::invalid_argument("localPath is required");
    const QString cleanFileName = clean(fileName).isEmpty() ? ChatState::fileName(cleanPath) : clean(fileName);
    RemoteIMMessage message;
    message.fromUserId = ownerUserId_;
    message.toUserId = requireSelectedPeer();
    message.text = QString("[文件消息] %1").arg(cleanFileName.isEmpty() ? QStringLiteral("file") : cleanFileName);
    message.direction = RemoteIMMessageDirection::Outgoing;
    message.status = RemoteIMMessageStatus::Pending;
    message.hasFile = true;
    message.file = RemoteIMFileAttachment{cleanPath, cleanFileName, clean(mimeType), sizeBytes};
    appendTracked(message);
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
    message.text = incomingDisplayText(text);
    message.direction = RemoteIMMessageDirection::Incoming;
    message.status = RemoteIMMessageStatus::Received;
    appendTracked(message);
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
    appendTracked(message);
    return message;
}

RemoteIMMessage ChatState::receiveFile(const QString& fromUserId, const QString& localPath, const QString& fileName, const QString& mimeType, qint64 sizeBytes) {
    const QString peerId = clean(fromUserId);
    const QString cleanPath = clean(localPath);
    if (peerId.isEmpty()) throw std::invalid_argument("fromUserId is required");
    if (cleanPath.isEmpty()) throw std::invalid_argument("localPath is required");
    const QString cleanFileName = clean(fileName).isEmpty() ? ChatState::fileName(cleanPath) : clean(fileName);
    upsertContact(RemoteIMContact{peerId, peerId});
    if (selectedPeerId_.isEmpty()) selectedPeerId_ = peerId;
    RemoteIMMessage message;
    message.fromUserId = peerId;
    message.toUserId = ownerUserId_;
    message.text = QString("[文件消息] %1").arg(cleanFileName.isEmpty() ? QStringLiteral("file") : cleanFileName);
    message.direction = RemoteIMMessageDirection::Incoming;
    message.status = RemoteIMMessageStatus::Received;
    message.hasFile = true;
    message.file = RemoteIMFileAttachment{cleanPath, cleanFileName, clean(mimeType), sizeBytes};
    appendTracked(message);
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
    appendTracked(message);
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

bool ChatState::updateMessageTime(const QString& messageId, qint64 createdAtMillis) {
    for (RemoteIMMessage& message : messages_) {
        if (message.id != messageId) continue;
        if (createdAtMillis > 0) message.createdAtMillis = createdAtMillis;
        return true;
    }
    return false;
}

bool ChatState::adoptMessageId(const QString& oldId, const QString& newId) {
    if (oldId.isEmpty() || newId.isEmpty() || oldId == newId) return false;
    if (messageIds_.contains(newId)) {
        // 漫游副本已先入内存：临时消息是重复项，移除之。
        messages_.erase(std::remove_if(messages_.begin(), messages_.end(), [&oldId](const RemoteIMMessage& message) {
            return message.id == oldId;
        }), messages_.end());
        messageIds_.remove(oldId);
        return false;
    }
    for (RemoteIMMessage& message : messages_) {
        if (message.id == oldId) {
            message.id = newId;
            messageIds_.remove(oldId);
            messageIds_.insert(newId);
            return true;
        }
    }
    return false;
}

void ChatState::appendMessageForRestore(const RemoteIMMessage& message) {
    // 本地库加载与 SDK 漫游补充共用此入口：按消息 id 去重，重复直接丢弃
    // （展示顺序由 messagesWith 的稳定排序保证，无需在此排序）。
    if (messageIds_.contains(message.id)) {
        // SDK 漫游命中同一消息时，用规范化时间替换旧版保存的本机毫秒时间，
        // 使同一秒内的消息无需清库也能恢复真实先后顺序。
        for (RemoteIMMessage& existing : messages_) {
            if (existing.id != message.id) continue;
            if (message.createdAtMillis > 0) existing.createdAtMillis = message.createdAtMillis;
            break;
        }
        return;
    }
    RemoteIMMessage restored = message;
    if (restored.direction == RemoteIMMessageDirection::Incoming) {
        restored.text = incomingDisplayText(restored.text);
    }
    appendTracked(restored);
}

void ChatState::appendTracked(const RemoteIMMessage& message) {
    messageIds_.insert(message.id);
    messages_.append(message);
}

QString ChatState::requireSelectedPeer() const {
    if (selectedPeerId_.isEmpty()) throw std::logic_error("selected peer is required");
    return selectedPeerId_;
}

QString ChatState::clean(const QString& value) {
    return value.trimmed();
}

QString ChatState::incomingDisplayText(const QString& value) {
    QString text = clean(value);
    static const QString invisibleAicliPrefix = QStringLiteral("\u2063\u200B\u200C\u200D\u2063");
    if (text.startsWith(invisibleAicliPrefix)) {
        return clean(text.mid(invisibleAicliPrefix.size()));
    }

    static const QStringList legacyPrefixes = {
        QStringLiteral("【AICLI 输出】"),
        QStringLiteral("[AICLI 输出]"),
        QStringLiteral("【AICLI输出】"),
        QStringLiteral("[AICLI输出]")
    };
    for (const QString& prefix : legacyPrefixes) {
        if (text.startsWith(prefix)) {
            return clean(text.mid(prefix.size()));
        }
    }
    return text;
}

QString ChatState::fileName(const QString& path) {
    const QString name = QFileInfo(path).fileName();
    return name.isEmpty() ? QStringLiteral("file") : name;
}
