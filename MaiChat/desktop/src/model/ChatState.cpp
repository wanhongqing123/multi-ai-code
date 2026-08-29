#include "model/ChatState.h"

#include "model/ContactGroups.h"

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

int ChatState::unreadCount(const QString& peerId) const {
    return unreadCounts_.value(clean(peerId), 0);
}

void ChatState::upsertContact(const RemoteIMContact& contact) {
    const QString userId = clean(contact.userId);
    if (userId.isEmpty()) return;
    const QString displayName = clean(contact.displayName).isEmpty() ? userId : clean(contact.displayName);
    const QString avatarUrl = clean(contact.avatarUrl);
    for (RemoteIMContact& existing : contacts_) {
        if (existing.userId == userId) {
            if (displayName != userId || existing.displayName.isEmpty() || existing.displayName == userId) {
                existing.displayName = displayName;
            }
            if (!avatarUrl.isEmpty()) existing.avatarUrl = avatarUrl;
            // groupName 有意不在这里更新。这个入口也被 SDK 资料刷新走，
            // 而资料对象里从来没有分组信息——跟着覆盖就会让用户分好的组
            // 在下一次刷新时被静默清空。分组只能由分组相关的接口改。
            return;
        }
    }
    contacts_.append(RemoteIMContact{userId, displayName, avatarUrl,
                                     ContactGroups::normalize(contact.groupName)});
}

QStringList ChatState::contactGroups() const { return contactGroups_; }

void ChatState::setContactGroups(const QStringList& groups) { contactGroups_ = groups; }

bool ChatState::addContactGroup(const QString& name) {
    const QString clean = ContactGroups::normalize(name);
    if (!ContactGroups::isAcceptableName(clean)) return false;
    if (contactGroups_.contains(clean)) return false;
    contactGroups_.append(clean);
    return true;
}

bool ChatState::renameContactGroup(const QString& from, const QString& to) {
    const QString oldName = ContactGroups::normalize(from);
    const QString newName = ContactGroups::normalize(to);
    if (!ContactGroups::isAcceptableName(newName)) return false;
    const int index = contactGroups_.indexOf(oldName);
    if (index < 0) return false;
    if (oldName == newName) return true;
    if (contactGroups_.contains(newName)) return false;
    contactGroups_[index] = newName;
    for (RemoteIMContact& contact : contacts_) {
        if (contact.groupName == oldName) contact.groupName = newName;
    }
    return true;
}

void ChatState::removeContactGroup(const QString& name) {
    const QString clean = ContactGroups::normalize(name);
    if (clean.isEmpty()) return;
    contactGroups_.removeAll(clean);
    // 成员回到未分组——删组不删人。
    for (RemoteIMContact& contact : contacts_) {
        if (contact.groupName == clean) contact.groupName.clear();
    }
}

void ChatState::setContactGroup(const QString& userId, const QString& groupName) {
    const QString id = clean(userId);
    const QString group = ContactGroups::normalize(groupName);
    // 未知分组一律落到未分组：内存里也不该出现指向不存在分组的联系人。
    const QString target = contactGroups_.contains(group) ? group : QString();
    for (RemoteIMContact& contact : contacts_) {
        if (contact.userId == id) {
            contact.groupName = target;
            return;
        }
    }
}

void ChatState::removeContactAndMessages(const QString& userId) {
    const QString cleanUserId = clean(userId);
    if (cleanUserId.isEmpty()) return;
    contacts_.erase(std::remove_if(contacts_.begin(), contacts_.end(), [&cleanUserId](const RemoteIMContact& contact) {
        return contact.userId == cleanUserId;
    }), contacts_.end());
    messages_.erase(std::remove_if(messages_.begin(), messages_.end(), [&cleanUserId](const RemoteIMMessage& message) {
        const bool removing = message.fromUserId == cleanUserId || message.toUserId == cleanUserId;
        return removing;
    }), messages_.end());
    rebuildMessageIndexes();
    unreadCounts_.remove(cleanUserId);
    const bool selectedMissing = !selectedPeerId_.isEmpty()
        && std::none_of(contacts_.cbegin(), contacts_.cend(), [this](const RemoteIMContact& contact) {
               return contact.userId == selectedPeerId_;
           });
    if (selectedPeerId_ == cleanUserId || selectedMissing) {
        selectedPeerId_ = contacts_.isEmpty() ? QString() : contacts_.first().userId;
        // 删除会话后选中焦点回落到首个会话，等同用户打开该会话，红点随之清零。
        if (!selectedPeerId_.isEmpty()) unreadCounts_.remove(selectedPeerId_);
    }
}

void ChatState::removeMessagesWith(const QString& peerId) {
    const QString cleanPeerId = clean(peerId);
    if (cleanPeerId.isEmpty()) return;
    messages_.erase(std::remove_if(messages_.begin(), messages_.end(), [&cleanPeerId](const RemoteIMMessage& message) {
        const bool removing = message.fromUserId == cleanPeerId || message.toUserId == cleanPeerId;
        return removing;
    }), messages_.end());
    rebuildMessageIndexes();
    unreadCounts_.remove(cleanPeerId);
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
    // 打开（选中）会话即视为已读，红点清零。
    unreadCounts_.remove(peerId);
}

RemoteIMMessage ChatState::queueOutgoingText(const QString& text) {
    return queueOutgoingTextTo(requireSelectedPeer(), text);
}

RemoteIMMessage ChatState::queueOutgoingTextTo(const QString& peerId, const QString& text) {
    const QString cleanText = clean(text);
    if (cleanText.isEmpty()) throw std::invalid_argument("text is required");
    const QString peer = clean(peerId);
    if (peer.isEmpty()) throw std::invalid_argument("peerId is required");
    RemoteIMMessage message;
    message.fromUserId = ownerUserId_;
    message.toUserId = peer;
    message.text = cleanText;
    message.direction = RemoteIMMessageDirection::Outgoing;
    message.status = RemoteIMMessageStatus::Pending;
    message.origin = RemoteIMMessageOrigin::Human;
    message.createdAtMillis = (message.createdAtMillis / 1000) * 1000;
    appendTracked(message);
    return message;
}

RemoteIMMessage ChatState::queueOutgoingApprovalDecision(
    const QString& token,
    RemoteIMApprovalAction action) {
    const QString cleanToken = clean(token);
    if (!isValidRemoteIMApprovalToken(cleanToken)) {
        throw std::invalid_argument("approval token is required");
    }
    if (action != RemoteIMApprovalAction::ApproveOnce
        && action != RemoteIMApprovalAction::ApprovePrefix
        && action != RemoteIMApprovalAction::Reject) {
        throw std::invalid_argument("approval resolution is not a user decision");
    }
    RemoteIMMessage message;
    message.fromUserId = ownerUserId_;
    message.toUserId = requireSelectedPeer();
    message.text = QStringLiteral("审批操作：%1").arg(remoteIMApprovalActionTitle(action));
    message.direction = RemoteIMMessageDirection::Outgoing;
    message.status = RemoteIMMessageStatus::Pending;
    message.origin = RemoteIMMessageOrigin::Human;
    message.hasApprovalDecision = true;
    message.approvalDecision = RemoteIMApprovalDecision{cleanToken, action};
    message.createdAtMillis = (message.createdAtMillis / 1000) * 1000;
    appendTracked(message);
    return message;
}

RemoteIMMessage ChatState::queueOutgoingImage(const QString& localPath, int width, int height, qint64 sizeBytes, const QString& text, bool captionAbove) {
    const QString cleanPath = clean(localPath);
    if (cleanPath.isEmpty()) throw std::invalid_argument("localPath is required");
    RemoteIMMessage message;
    message.fromUserId = ownerUserId_;
    message.toUserId = requireSelectedPeer();
    // 有配文时 text 存配文（气泡里图下显示、会话列表预览也显示配文）；无配文回退占位摘要。
    message.text = clean(text).isEmpty() ? (QStringLiteral("[图片消息] ") + fileName(cleanPath)) : clean(text);
    message.direction = RemoteIMMessageDirection::Outgoing;
    message.status = RemoteIMMessageStatus::Pending;
    message.origin = RemoteIMMessageOrigin::Human;
    message.hasImage = true;
    message.captionAbove = captionAbove;
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
    message.origin = RemoteIMMessageOrigin::Human;
    message.hasVoice = true;
    message.voice = RemoteIMVoiceAttachment{cleanPath, qMax(1, durationSeconds)};
    appendTracked(message);
    return message;
}

RemoteIMMessage ChatState::queueOutgoingFile(const QString& localPath, const QString& fileName, const QString& mimeType, qint64 sizeBytes, const QString& text, bool captionAbove) {
    const QString cleanPath = clean(localPath);
    if (cleanPath.isEmpty()) throw std::invalid_argument("localPath is required");
    const QString cleanFileName = clean(fileName).isEmpty() ? ChatState::fileName(cleanPath) : clean(fileName);
    RemoteIMMessage message;
    message.fromUserId = ownerUserId_;
    message.toUserId = requireSelectedPeer();
    message.text = clean(text).isEmpty()
        ? QStringLiteral("[文件消息] %1").arg(cleanFileName.isEmpty() ? QStringLiteral("file") : cleanFileName)
        : clean(text);
    message.direction = RemoteIMMessageDirection::Outgoing;
    message.status = RemoteIMMessageStatus::Pending;
    message.origin = RemoteIMMessageOrigin::Human;
    message.hasFile = true;
    message.captionAbove = captionAbove;
    message.file = RemoteIMFileAttachment{cleanPath, cleanFileName, clean(mimeType), sizeBytes};
    appendTracked(message);
    return message;
}

RemoteIMMessage ChatState::queueOutgoingVideo(const QString& localPath, const QString& fileName, const QString& coverPath, int durationSeconds, qint64 sizeBytes, const QString& text, bool captionAbove) {
    const QString cleanPath = clean(localPath);
    if (cleanPath.isEmpty()) throw std::invalid_argument("localPath is required");
    const QString cleanFileName = clean(fileName).isEmpty() ? ChatState::fileName(cleanPath) : clean(fileName);
    RemoteIMMessage message;
    message.fromUserId = ownerUserId_;
    message.toUserId = requireSelectedPeer();
    // 与图片/文件一致：有配文存配文（气泡里图下显示、会话列表也预览配文），无配文回退占位摘要。
    message.text = clean(text).isEmpty()
        ? QStringLiteral("[视频消息] %1").arg(cleanFileName.isEmpty() ? QStringLiteral("video") : cleanFileName)
        : clean(text);
    message.direction = RemoteIMMessageDirection::Outgoing;
    message.status = RemoteIMMessageStatus::Pending;
    message.origin = RemoteIMMessageOrigin::Human;
    message.hasVideo = true;
    message.captionAbove = captionAbove;
    message.video = RemoteIMVideoAttachment{cleanPath, cleanFileName, clean(coverPath),
                                            durationSeconds > 0 ? durationSeconds : 0, sizeBytes};
    appendTracked(message);
    return message;
}

RemoteIMMessage ChatState::receiveText(const QString& fromUserId, const QString& text) {
    const QString peerId = clean(fromUserId);
    if (peerId.isEmpty()) throw std::invalid_argument("fromUserId is required");
    upsertContact(RemoteIMContact{peerId, peerId});
    if (selectedPeerId_.isEmpty()) selectedPeerId_ = peerId;
    bumpUnreadIfBackground(peerId);
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
    bumpUnreadIfBackground(peerId);
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
    bumpUnreadIfBackground(peerId);
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
    bumpUnreadIfBackground(peerId);
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

void ChatState::forEachMessageWith(
    const QString& peerId, const std::function<void(const RemoteIMMessage&)>& visit) const {
    const QString cleanPeerId = clean(peerId);
    const auto conversation = messageIdsByPeer_.constFind(cleanPeerId);
    if (conversation == messageIdsByPeer_.cend()) return;
    for (const QString& messageId : conversation.value()) {
        const int index = messageIndexById_.value(messageId, -1);
        if (index >= 0 && index < messages_.size()) visit(messages_.at(index));
    }
}

QList<RemoteIMMessage> ChatState::messagesWith(const QString& peerId) const {
    QList<RemoteIMMessage> result;
    const QString cleanPeerId = clean(peerId);
    const auto conversation = messageIdsByPeer_.constFind(cleanPeerId);
    if (conversation == messageIdsByPeer_.cend()) return result;
    result.reserve(conversation->size());
    for (const QString& messageId : conversation.value()) {
        const int index = messageIndexById_.value(messageId, -1);
        if (index >= 0 && index < messages_.size()) result.append(messages_.at(index));
    }
    return result;
}

int ChatState::messageCountWith(const QString& peerId) const {
    const auto conversation = messageIdsByPeer_.constFind(clean(peerId));
    return conversation == messageIdsByPeer_.cend() ? 0 : conversation->size();
}

bool ChatState::latestMessageWith(const QString& peerId, RemoteIMMessage* message) const {
    const auto conversation = messageIdsByPeer_.constFind(clean(peerId));
    if (conversation == messageIdsByPeer_.cend() || conversation->isEmpty()) return false;
    const int index = messageIndexById_.value(conversation->last(), -1);
    if (index < 0 || index >= messages_.size()) return false;
    if (message) *message = messages_.at(index);
    return true;
}

bool ChatState::updateMessageStatus(const QString& messageId, RemoteIMMessageStatus status) {
    const int index = messageIndexById_.value(messageId, -1);
    if (index < 0 || index >= messages_.size()) return false;
    messages_[index].status = status;
    return true;
}

bool ChatState::updateMessageTime(const QString& messageId, qint64 createdAtMillis) {
    const int index = messageIndexById_.value(messageId, -1);
    if (index < 0 || index >= messages_.size()) return false;
    RemoteIMMessage& message = messages_[index];
    if (createdAtMillis <= 0 || createdAtMillis == message.createdAtMillis) return true;
    const QString peerId = peerIdForMessage(message);
    messageIdsByPeer_[peerId].removeAll(messageId);
    message.createdAtMillis = createdAtMillis;
    insertMessageIntoConversation(messageId);
    return true;
}

bool ChatState::adoptMessageId(const QString& oldId, const QString& newId) {
    if (oldId.isEmpty() || newId.isEmpty() || oldId == newId) return false;
    if (messageIds_.contains(newId)) {
        // 漫游副本已先入内存：临时消息是重复项，移除之。
        messages_.erase(std::remove_if(messages_.begin(), messages_.end(), [&oldId](const RemoteIMMessage& message) {
            return message.id == oldId;
        }), messages_.end());
        rebuildMessageIndexes();
        return false;
    }
    const int index = messageIndexById_.value(oldId, -1);
    if (index < 0 || index >= messages_.size()) return false;
    RemoteIMMessage& message = messages_[index];
    const QString peerId = peerIdForMessage(message);
    message.id = newId;
    messageIds_.remove(oldId);
    messageIds_.insert(newId);
    messageIndexById_.remove(oldId);
    messageIndexById_.insert(newId, index);
    QList<QString>& conversation = messageIdsByPeer_[peerId];
    const int conversationIndex = conversation.indexOf(oldId);
    if (conversationIndex >= 0) conversation[conversationIndex] = newId;
    return true;
}

void ChatState::appendMessageForRestore(const RemoteIMMessage& message) {
    // 本地库加载与 SDK 漫游补充共用此入口：按消息 id 去重，重复直接丢弃
    // （展示顺序由 messagesWith 的稳定排序保证，无需在此排序）。
    if (messageIds_.contains(message.id)) {
        // SDK 漫游命中同一消息时，用规范化时间替换旧版保存的本机毫秒时间，
        // 使同一秒内的消息无需清库也能恢复真实先后顺序。
        updateMessageTime(message.id, message.createdAtMillis);
        return;
    }
    RemoteIMMessage restored = message;
    if (restored.direction == RemoteIMMessageDirection::Incoming) {
        restored.text = incomingDisplayText(restored.text);
    }
    appendTracked(restored);
}

bool ChatState::appendLiveMessage(const RemoteIMMessage& message) {
    const bool isNew = !messageIds_.contains(message.id);
    appendMessageForRestore(message);
    // 只有真正首次入内存的入站消息计红点；漫游重投/去重命中不重复累计。
    if (isNew && message.direction == RemoteIMMessageDirection::Incoming) {
        bumpUnreadIfBackground(clean(message.fromUserId));
    }
    return isNew;
}

void ChatState::appendTracked(const RemoteIMMessage& message) {
    if (messageIds_.contains(message.id)) return;
    messageIds_.insert(message.id);
    messages_.append(message);
    messageIndexById_.insert(message.id, messages_.size() - 1);
    insertMessageIntoConversation(message.id);
}

QString ChatState::peerIdForMessage(const RemoteIMMessage& message) const {
    if (message.fromUserId == ownerUserId_) return clean(message.toUserId);
    if (message.toUserId == ownerUserId_) return clean(message.fromUserId);
    return message.direction == RemoteIMMessageDirection::Outgoing
        ? clean(message.toUserId)
        : clean(message.fromUserId);
}

void ChatState::insertMessageIntoConversation(const QString& messageId) {
    const int messageIndex = messageIndexById_.value(messageId, -1);
    if (messageIndex < 0 || messageIndex >= messages_.size()) return;
    const QString peerId = peerIdForMessage(messages_.at(messageIndex));
    if (peerId.isEmpty()) return;
    QList<QString>& conversation = messageIdsByPeer_[peerId];
    const auto insertionPoint = std::lower_bound(
        conversation.begin(), conversation.end(), messageIndex,
        [this](const QString& existingId, int candidateIndex) {
            const int existingIndex = messageIndexById_.value(existingId, -1);
            if (existingIndex < 0 || existingIndex >= messages_.size()) return true;
            const RemoteIMMessage& existing = messages_.at(existingIndex);
            const RemoteIMMessage& candidate = messages_.at(candidateIndex);
            if (existing.createdAtMillis != candidate.createdAtMillis) {
                return existing.createdAtMillis < candidate.createdAtMillis;
            }
            // 与原来的 stable_sort 一致：同一毫秒按进入内存的先后顺序展示。
            return existingIndex < candidateIndex;
        });
    conversation.insert(insertionPoint, messageId);
}

void ChatState::rebuildMessageIndexes() {
    messageIds_.clear();
    messageIdsByPeer_.clear();
    messageIndexById_.clear();
    for (int index = 0; index < messages_.size(); ++index) {
        const QString& messageId = messages_.at(index).id;
        messageIds_.insert(messageId);
        messageIndexById_.insert(messageId, index);
        insertMessageIntoConversation(messageId);
    }
}

void ChatState::bumpUnreadIfBackground(const QString& peerId) {
    if (peerId != selectedPeerId_) ++unreadCounts_[peerId];
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
