#include "im/TimSdkRemoteIMClient.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrl>

#include "im/DynamicTimSdkApi.h"

namespace {

constexpr int kConversationTypeC2C = 1;
constexpr int kElemText = 0;
constexpr int kElemImage = 1;
constexpr int kElemFile = 4;
constexpr int kImageLevelOriginal = 0;
constexpr int kFriendTypeBoth = 1;

QString appDataDir(const QString& child) {
    QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (root.isEmpty()) root = QDir::homePath() + QStringLiteral("/.multi-ai-im-desktop");
    QDir dir(root);
    dir.mkpath(child);
    return dir.filePath(child);
}

QString cacheImagePathForUrl(const QString& url) {
    const QByteArray hash = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex();
    const QString suffix = QFileInfo(QUrl(url).path()).suffix().isEmpty() ? QStringLiteral("jpg") : QFileInfo(QUrl(url).path()).suffix();
    return QDir(appDataDir(QStringLiteral("RemoteIMImages"))).filePath(QString::fromUtf8(hash) + "." + suffix);
}

QString cacheFilePathForUrl(const QString& url, const QString& fileName) {
    const QByteArray hash = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex();
    QString suffix = QFileInfo(fileName).suffix();
    if (suffix.isEmpty()) suffix = QFileInfo(QUrl(url).path()).suffix();
    if (suffix.isEmpty()) suffix = QStringLiteral("md");
    return QDir(appDataDir(QStringLiteral("RemoteIMFiles"))).filePath(QString::fromUtf8(hash) + "." + suffix);
}

QString mimeTypeForFileName(const QString& fileName) {
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    if (suffix == QStringLiteral("html") || suffix == QStringLiteral("htm")) return QStringLiteral("text/html");
    return QStringLiteral("text/markdown");
}

bool isSupportedPreviewFileName(const QString& fileName) {
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    return suffix == QStringLiteral("md")
        || suffix == QStringLiteral("markdown")
        || suffix == QStringLiteral("html")
        || suffix == QStringLiteral("htm");
}

QString firstNonEmpty(const QJsonObject& object, std::initializer_list<QString> keys) {
    for (const QString& key : keys) {
        const QString value = object.value(key).toString().trimmed();
        if (!value.isEmpty()) return value;
    }
    return {};
}

QJsonArray arrayPayload(const QString& jsonPayload, std::initializer_list<QString> arrayKeys = {}) {
    const QJsonDocument doc = QJsonDocument::fromJson(jsonPayload.toUtf8());
    if (doc.isArray()) return doc.array();
    const QJsonObject object = doc.object();
    for (const QString& key : arrayKeys) {
        const QJsonArray array = object.value(key).toArray();
        if (!array.isEmpty()) return array;
    }
    return {};
}

QString userProfileDisplayName(const QJsonObject& userProfile) {
    return firstNonEmpty(userProfile, {
        QStringLiteral("user_profile_nick_name"),
        QStringLiteral("user_profile_identifier")
    });
}

QString friendDisplayName(const QJsonObject& friendProfile) {
    const QString remark = friendProfile.value(QStringLiteral("friend_profile_remark")).toString().trimmed();
    if (!remark.isEmpty()) return remark;
    return userProfileDisplayName(friendProfile.value(QStringLiteral("friend_profile_user_profile")).toObject());
}

qint64 messageTimeMillis(const QJsonObject& message) {
    const qint64 seconds = static_cast<qint64>(message.value(QStringLiteral("message_server_time")).toDouble(
        message.value(QStringLiteral("message_client_time")).toDouble(0)));
    return seconds > 0 ? seconds * 1000 : QDateTime::currentMSecsSinceEpoch();
}

QString jsonValueAsString(const QJsonValue& value) {
    if (value.isString()) return value.toString().trimmed();
    if (value.isDouble() && value.toDouble() != 0) return QString::number(static_cast<qint64>(value.toDouble()));
    return QString();
}

// SDK 消息 id：漫游与实时是同一条消息的两次投递，取相同的 SDK id 才能让
// 本地库按主键去重（此前两条路径都落到随机 UUID，重复消息无从识别）。
QString sdkMessageId(const QJsonObject& message) {
    QString id = jsonValueAsString(message.value(QStringLiteral("message_msg_id")));
    if (id.isEmpty()) id = jsonValueAsString(message.value(QStringLiteral("message_unique_id")));
    return id;
}

// 一条 SDK 消息可含多个 elem（拆成多条 RemoteIMMessage），按 elem 下标编号，
// 两条路径的编号规则一致。SDK id 缺失时返回空串，调用方保留默认 UUID。
QString sdkElemMessageId(const QString& sdkId, int elemIndex) {
    if (sdkId.isEmpty()) return QString();
    return sdkId + QLatin1Char('#') + QString::number(elemIndex);
}

// TIMMsgSendMessage 回调的 json_param 是服务端确认后的消息对象。出站记录采纳
// 同一稳定 id 和服务端时间，避免本地毫秒时间与 SDK 秒级时间混排。
RemoteIMSendReceipt sentMessageReceipt(const QString& jsonPayload) {
    const QJsonDocument doc = QJsonDocument::fromJson(jsonPayload.toUtf8());
    const QJsonObject object = doc.isArray() ? doc.array().at(0).toObject() : doc.object();
    RemoteIMSendReceipt receipt;
    receipt.remoteMessageId = sdkElemMessageId(sdkMessageId(object), 0);
    const qint64 seconds = static_cast<qint64>(object.value(QStringLiteral("message_server_time")).toDouble(
        object.value(QStringLiteral("message_client_time")).toDouble(0)));
    if (seconds > 0) receipt.createdAtMillis = seconds * 1000;
    return receipt;
}

}  // namespace

TimSdkRemoteIMClient::TimSdkRemoteIMClient(QObject* parent)
    : TimSdkRemoteIMClient(std::make_unique<DynamicTimSdkApi>(), parent) {}

TimSdkRemoteIMClient::TimSdkRemoteIMClient(std::unique_ptr<TimSdkApi> api, QObject* parent)
    : RemoteIMClient(parent), api_(std::move(api)) {}

qint64 TimSdkRemoteIMClient::orderedMessageTime(const QString& peerId, qint64 sdkTimeMillis) {
    const qint64 second = sdkTimeMillis / 1000;
    if (orderedSecondByPeer_.value(peerId, -1) != second) {
        orderedSecondByPeer_.insert(peerId, second);
        nextOrderInSecondByPeer_.insert(peerId, 0);
    }
    const int orderInSecond = nextOrderInSecondByPeer_.value(peerId, 0);
    nextOrderInSecondByPeer_.insert(peerId, orderInSecond + 1);
    return second * 1000 + orderInSecond;
}

TimSdkRemoteIMClient::~TimSdkRemoteIMClient() {
    if (api_) {
        api_->removeReceiveMessageCallback();
        api_->uninit();
    }
}

void TimSdkRemoteIMClient::connectToService(int sdkAppId, const QString& userId, const QString& userSig, RemoteIMCompletion completion) {
    if (sdkAppId <= 0) {
        if (completion) completion(false, QStringLiteral("SDK AppID 不能为空"));
        return;
    }
    if (userId.trimmed().isEmpty()) {
        if (completion) completion(false, QStringLiteral("IM 账号不能为空"));
        return;
    }
    if (userSig.trimmed().isEmpty()) {
        if (completion) completion(false, QStringLiteral("UserSig 不能为空"));
        return;
    }
    if (!api_ || !api_->isReady()) {
        if (completion) {
            completion(false, QStringLiteral("未加载桌面 IM SDK：%1").arg(api_ ? api_->diagnosticError() : QStringLiteral("SDK API 为空")));
        }
        return;
    }
    currentUserId_ = userId.trimmed();

    const int initResult = api_->init(static_cast<quint64>(sdkAppId), sdkConfigJson());
    if (initResult != 0) {
        if (completion) completion(false, QStringLiteral("IM SDK 初始化失败：%1").arg(initResult));
        return;
    }

    api_->addReceiveMessageCallback([this](const QString& jsonMessages) {
        handleIncomingMessages(jsonMessages);
    });
    api_->login(userId.trimmed(), userSig.trimmed(), [this, completion = std::move(completion)](int code,
                                                                                                const QString& description,
                                                                                                const QString&) mutable {
        connected_ = code == 0;
        if (!connected_) api_->removeReceiveMessageCallback();
        if (connected_) syncInitialData();
        complete(std::move(completion), code, description);
    });
}

void TimSdkRemoteIMClient::disconnectFromService(RemoteIMCompletion completion) {
    if (!api_) {
        if (completion) completion(true, QString());
        return;
    }
    api_->removeReceiveMessageCallback();
    api_->logout([this, completion = std::move(completion)](int code, const QString& description, const QString&) mutable {
        connected_ = false;
        api_->uninit();
        complete(std::move(completion), code, description);
        emit disconnected();
    });
}

void TimSdkRemoteIMClient::deleteContact(const QString& userId, RemoteIMCompletion completion) {
    const QString cleanUserId = userId.trimmed();
    if (cleanUserId.isEmpty()) {
        if (completion) completion(false, QStringLiteral("好友账号不能为空"));
        return;
    }
    if (!connected_ || !api_) {
        if (completion) completion(false, QStringLiteral("IM 未连接，无法删除好友"));
        return;
    }

    QJsonObject request;
    request[QStringLiteral("friendship_delete_friend_param_friend_type")] = kFriendTypeBoth;
    request[QStringLiteral("friendship_delete_friend_param_identifier_array")] = QJsonArray{cleanUserId};
    api_->deleteFriend(compactJson(request), [this, cleanUserId, completion = std::move(completion)](
                                                   int code,
                                                   const QString& description,
                                                   const QString&) mutable {
        if (code != 0) {
            complete(std::move(completion), code, description);
            return;
        }
        api_->deleteConversation(cleanUserId,
                                 kConversationTypeC2C,
                                 [completion = std::move(completion)](int conversationCode,
                                                                      const QString& conversationDescription,
                                                                      const QString&) mutable {
            complete(std::move(completion), conversationCode, conversationDescription);
        });
    });
}

void TimSdkRemoteIMClient::sendText(const QString& peerId, const QString& text, RemoteIMSendCompletion completion) {
    const QString cleanPeerId = peerId.trimmed();
    const QString cleanText = text.trimmed();
    if (cleanPeerId.isEmpty() || cleanText.isEmpty()) {
        if (completion) completion(false, QStringLiteral("文本消息缺少接收人或内容"), {});
        return;
    }

    QJsonObject elem;
    elem[QStringLiteral("elem_type")] = kElemText;
    elem[QStringLiteral("text_elem_content")] = cleanText;
    QJsonObject message;
    message[QStringLiteral("message_elem_array")] = QJsonArray{elem};
    api_->sendMessage(cleanPeerId, kConversationTypeC2C, compactJson(message), [completion = std::move(completion)](int code,
                                                                                                                    const QString& description,
                                                                                                                    const QString& jsonPayload) mutable {
        if (!completion) return;
        const bool ok = code == 0;
        RemoteIMSendReceipt receipt = ok ? sentMessageReceipt(jsonPayload) : RemoteIMSendReceipt{};
        completion(ok,
                   ok ? QString() : (description.isEmpty() ? QStringLiteral("IM SDK 操作失败：%1").arg(code) : description),
                   receipt);
    });
}

void TimSdkRemoteIMClient::sendImage(const QString& peerId, const QString& localPath, RemoteIMSendCompletion completion) {
    const QString cleanPeerId = peerId.trimmed();
    const QString cleanPath = localPath.trimmed();
    if (cleanPeerId.isEmpty() || cleanPath.isEmpty()) {
        if (completion) completion(false, QStringLiteral("图片消息缺少接收人或图片路径"), {});
        return;
    }

    QJsonObject elem;
    elem[QStringLiteral("elem_type")] = kElemImage;
    elem[QStringLiteral("image_elem_orig_path")] = cleanPath;
    elem[QStringLiteral("image_elem_level")] = kImageLevelOriginal;
    QJsonObject message;
    message[QStringLiteral("message_elem_array")] = QJsonArray{elem};
    api_->sendMessage(cleanPeerId, kConversationTypeC2C, compactJson(message), [completion = std::move(completion)](int code,
                                                                                                                    const QString& description,
                                                                                                                    const QString& jsonPayload) mutable {
        if (!completion) return;
        const bool ok = code == 0;
        RemoteIMSendReceipt receipt = ok ? sentMessageReceipt(jsonPayload) : RemoteIMSendReceipt{};
        completion(ok,
                   ok ? QString() : (description.isEmpty() ? QStringLiteral("IM SDK 操作失败：%1").arg(code) : description),
                   receipt);
    });
}

void TimSdkRemoteIMClient::sendVoice(const QString&, const QString&, int, RemoteIMCompletion completion) {
    if (completion) completion(false, QStringLiteral("桌面端语音消息还未接入原生录音与 SDK 声音元素"));
}

void TimSdkRemoteIMClient::syncInitialData() {
    fetchFriendList();
    fetchConversationList();
}

void TimSdkRemoteIMClient::fetchFriendList() {
    api_->getFriendList([this](int code, const QString&, const QString& jsonPayload) {
        if (code != 0 || jsonPayload.trimmed().isEmpty()) return;
        handleFriendListPayload(jsonPayload);
    });
}

void TimSdkRemoteIMClient::fetchConversationList() {
    api_->getConversationList([this](int code, const QString&, const QString& jsonPayload) {
        if (code != 0 || jsonPayload.trimmed().isEmpty()) return;
        handleConversationListPayload(jsonPayload);
    });
}

void TimSdkRemoteIMClient::fetchRecentMessages(const QString& conversationId, int conversationType) {
    if (conversationId.trimmed().isEmpty()) return;
    QJsonObject request;
    request[QStringLiteral("msg_getmsglist_param_count")] = 20;
    request[QStringLiteral("msg_getmsglist_param_is_ramble")] = true;
    request[QStringLiteral("msg_getmsglist_param_is_forward")] = false;
    api_->getMessageList(conversationId, conversationType, compactJson(request), [this](int code,
                                                                                       const QString&,
                                                                                       const QString& jsonPayload) {
        if (code != 0 || jsonPayload.trimmed().isEmpty()) return;
        handleHistoryMessagesPayload(jsonPayload);
    });
}

void TimSdkRemoteIMClient::handleFriendListPayload(const QString& jsonPayload) {
    QList<RemoteIMContact> contacts;
    const QJsonArray friends = arrayPayload(jsonPayload);
    for (const QJsonValue& value : friends) {
        const QJsonObject friendProfile = value.toObject();
        const QString userId = firstNonEmpty(friendProfile, {
            QStringLiteral("friend_profile_identifier"),
            QStringLiteral("friendship_friend_info_get_result_userid")
        });
        if (userId.isEmpty()) continue;
        const QString displayName = friendDisplayName(friendProfile).trimmed();
        contacts.append(RemoteIMContact{userId, displayName.isEmpty() ? userId : displayName});
    }
    if (!contacts.isEmpty()) emit contactsReceived(contacts);
}

void TimSdkRemoteIMClient::handleConversationListPayload(const QString& jsonPayload) {
    QList<RemoteIMContact> contacts;
    const QJsonArray conversations = arrayPayload(jsonPayload, {
        QStringLiteral("conversation_list_result_conv_list")
    });
    for (const QJsonValue& value : conversations) {
        const QJsonObject conversation = value.toObject();
        const int conversationType = conversation.value(QStringLiteral("conv_type")).toInt(0);
        const QString conversationId = conversation.value(QStringLiteral("conv_id")).toString().trimmed();
        if (conversationId.isEmpty()) continue;
        const QString displayName = firstNonEmpty(conversation, {
            QStringLiteral("conv_show_name"),
            QStringLiteral("conv_id")
        });
        if (conversationType == kConversationTypeC2C) {
            contacts.append(RemoteIMContact{conversationId, displayName.isEmpty() ? conversationId : displayName});
            fetchRecentMessages(conversationId, conversationType);
        }
    }
    if (!contacts.isEmpty()) emit contactsReceived(contacts);
}

void TimSdkRemoteIMClient::handleHistoryMessagesPayload(const QString& jsonPayload) {
    QList<RemoteIMMessage> messages;
    const QJsonArray sdkMessages = arrayPayload(jsonPayload);
    // is_forward=false 的历史结果按“新到旧”返回；反向遍历后为同一秒消息分配
    // 递增毫秒，保留 SDK 已确定的会话顺序。
    for (int messageIndex = sdkMessages.size() - 1; messageIndex >= 0; --messageIndex) {
        const QJsonObject sdkMessage = sdkMessages.at(messageIndex).toObject();
        const bool isFromSelf = sdkMessage.value(QStringLiteral("message_is_from_self")).toBool(false);
        const QString peerId = isFromSelf
                                   ? sdkMessage.value(QStringLiteral("message_conv_id")).toString().trimmed()
                                   : firstNonEmpty(sdkMessage, {QStringLiteral("message_sender"), QStringLiteral("message_conv_id")});
        if (peerId.isEmpty()) continue;

        const qint64 sdkTimeMillis = messageTimeMillis(sdkMessage);
        const QString sdkId = sdkMessageId(sdkMessage);
        const QJsonArray elems = sdkMessage.value(QStringLiteral("message_elem_array")).toArray();
        for (int elemIndex = 0; elemIndex < elems.size(); ++elemIndex) {
            const QJsonObject elem = elems.at(elemIndex).toObject();
            RemoteIMMessage message;
            const QString stableId = sdkElemMessageId(sdkId, elemIndex);
            if (!stableId.isEmpty()) message.id = stableId;
            message.fromUserId = isFromSelf ? currentUserId_ : peerId;
            message.toUserId = isFromSelf ? peerId : currentUserId_;
            message.direction = isFromSelf ? RemoteIMMessageDirection::Outgoing : RemoteIMMessageDirection::Incoming;
            message.status = isFromSelf ? RemoteIMMessageStatus::Sent : RemoteIMMessageStatus::Received;
            message.createdAtMillis = orderedMessageTime(peerId, sdkTimeMillis);

            const int elemType = elem.value(QStringLiteral("elem_type")).toInt(-1);
            if (elemType == kElemText) {
                message.text = elem.value(QStringLiteral("text_elem_content")).toString();
                if (!message.text.trimmed().isEmpty()) messages.append(message);
                continue;
            }
            if (elemType == kElemImage) {
                const QString localPath = elem.value(QStringLiteral("image_elem_orig_path")).toString().trimmed();
                if (localPath.isEmpty()) continue;
                message.text = QStringLiteral("[图片消息] ") + QFileInfo(localPath).fileName();
                message.hasImage = true;
                message.image = RemoteIMImageAttachment{
                    localPath,
                    elem.value(QStringLiteral("image_elem_orig_pic_width")).toInt(0),
                    elem.value(QStringLiteral("image_elem_orig_pic_height")).toInt(0),
                    static_cast<qint64>(elem.value(QStringLiteral("image_elem_orig_pic_size")).toDouble(0))
                };
                messages.append(message);
                continue;
            }
            if (elemType == kElemFile) {
                const QString fileName = firstNonEmpty(elem, {
                    QStringLiteral("file_elem_file_name"),
                    QStringLiteral("file_elem_file_path")
                });
                if (!isSupportedPreviewFileName(fileName)) continue;
                const QString localPath = elem.value(QStringLiteral("file_elem_file_path")).toString().trimmed();
                const qint64 sizeBytes = static_cast<qint64>(elem.value(QStringLiteral("file_elem_file_size")).toDouble(0));
                const QString url = elem.value(QStringLiteral("file_elem_url")).toString().trimmed();
                if (localPath.isEmpty() && !url.isEmpty()) {
                    const QString cachedPath = cacheFilePathForUrl(url, fileName);
                    if (QFile::exists(cachedPath)) {
                        message.text = QStringLiteral("[文件消息] ") + QFileInfo(fileName).fileName();
                        message.hasFile = true;
                        message.file = RemoteIMFileAttachment{cachedPath, QFileInfo(fileName).fileName(), mimeTypeForFileName(fileName), sizeBytes};
                        messages.append(message);
                    } else {
                        message.hasFile = true;
                        message.text = QStringLiteral("[文件消息] ") + QFileInfo(fileName).fileName();
                        message.file = RemoteIMFileAttachment{QString(), QFileInfo(fileName).fileName(), mimeTypeForFileName(fileName), sizeBytes};
                        handleIncomingFileUrl(message, url);
                    }
                    continue;
                }
                if (!localPath.isEmpty()) {
                    message.text = QStringLiteral("[文件消息] ") + QFileInfo(fileName).fileName();
                    message.hasFile = true;
                    message.file = RemoteIMFileAttachment{localPath, QFileInfo(fileName).fileName(), mimeTypeForFileName(fileName), sizeBytes};
                    messages.append(message);
                }
            }
        }
    }
    if (!messages.isEmpty()) emit messagesReceived(messages);
}

QString TimSdkRemoteIMClient::sdkConfigJson() const {
    QJsonObject config;
    config[QStringLiteral("sdk_config_config_file_path")] = appDataDir(QStringLiteral("SdkConfig"));
    config[QStringLiteral("sdk_config_log_file_path")] = appDataDir(QStringLiteral("SdkLogs"));
    return compactJson(config);
}

void TimSdkRemoteIMClient::handleIncomingMessages(const QString& jsonMessages) {
    const QJsonDocument doc = QJsonDocument::fromJson(jsonMessages.toUtf8());
    const QJsonArray messages = doc.isArray() ? doc.array() : QJsonArray{doc.object()};
    for (const QJsonValue& value : messages) {
        handleIncomingMessage(value.toObject());
    }
}

void TimSdkRemoteIMClient::handleIncomingMessage(const QJsonObject& message) {
    if (message.value(QStringLiteral("message_is_from_self")).toBool(false)) return;
    const QString fromUserId = firstNonEmpty(message, {QStringLiteral("message_sender"), QStringLiteral("message_conv_id")});
    if (fromUserId.isEmpty()) return;

    // 实时消息与漫游是同一条消息的两次投递：构造与漫游路径完全一致的
    // RemoteIMMessage（含稳定 SDK id 与服务器时间），经 messagesReceived 通道
    // 送出，本地库据 id 去重，重登时不会与漫游重复。
    const QString sdkId = sdkMessageId(message);
    const qint64 sdkTimeMillis = messageTimeMillis(message);
    const auto baseMessage = [this, &fromUserId, &sdkId, sdkTimeMillis](int elemIndex) {
        RemoteIMMessage result;
        const QString stableId = sdkElemMessageId(sdkId, elemIndex);
        if (!stableId.isEmpty()) result.id = stableId;
        result.fromUserId = fromUserId;
        result.toUserId = currentUserId_;
        result.direction = RemoteIMMessageDirection::Incoming;
        result.status = RemoteIMMessageStatus::Received;
        result.createdAtMillis = orderedMessageTime(fromUserId, sdkTimeMillis);
        return result;
    };

    QList<RemoteIMMessage> received;
    const QJsonArray elems = message.value(QStringLiteral("message_elem_array")).toArray();
    for (int elemIndex = 0; elemIndex < elems.size(); ++elemIndex) {
        const QJsonObject elem = elems.at(elemIndex).toObject();
        const int elemType = elem.value(QStringLiteral("elem_type")).toInt(-1);
        if (elemType == kElemText) {
            const QString text = elem.value(QStringLiteral("text_elem_content")).toString();
            if (text.trimmed().isEmpty()) continue;
            RemoteIMMessage textMessage = baseMessage(elemIndex);
            textMessage.text = text;
            received.append(textMessage);
            continue;
        }
        if (elemType == kElemImage) {
            const QString localPath = elem.value(QStringLiteral("image_elem_orig_path")).toString().trimmed();
            const int width = elem.value(QStringLiteral("image_elem_orig_pic_width")).toInt(0);
            const int height = elem.value(QStringLiteral("image_elem_orig_pic_height")).toInt(0);
            const qint64 sizeBytes = static_cast<qint64>(elem.value(QStringLiteral("image_elem_orig_pic_size")).toDouble(0));
            RemoteIMMessage imageMessage = baseMessage(elemIndex);
            imageMessage.hasImage = true;
            imageMessage.image = RemoteIMImageAttachment{localPath, width, height, sizeBytes};
            if (!localPath.isEmpty()) {
                imageMessage.text = QStringLiteral("[图片消息] ") + QFileInfo(localPath).fileName();
                received.append(imageMessage);
                continue;
            }
            const QString url = firstNonEmpty(elem, {
                QStringLiteral("image_elem_large_url"),
                QStringLiteral("image_elem_orig_url"),
                QStringLiteral("image_elem_thumb_url")
            });
            if (!url.isEmpty()) handleIncomingImageUrl(imageMessage, url);
            continue;
        }
        if (elemType == kElemFile) {
            const QString fileName = firstNonEmpty(elem, {
                QStringLiteral("file_elem_file_name"),
                QStringLiteral("file_elem_file_path")
            });
            if (!isSupportedPreviewFileName(fileName)) continue;
            const QString displayName = QFileInfo(fileName).fileName();
            const QString localPath = elem.value(QStringLiteral("file_elem_file_path")).toString().trimmed();
            const qint64 sizeBytes = static_cast<qint64>(elem.value(QStringLiteral("file_elem_file_size")).toDouble(0));
            RemoteIMMessage fileMessage = baseMessage(elemIndex);
            fileMessage.hasFile = true;
            fileMessage.text = QStringLiteral("[文件消息] ") + displayName;
            if (!localPath.isEmpty()) {
                fileMessage.file = RemoteIMFileAttachment{localPath, displayName, mimeTypeForFileName(displayName), sizeBytes};
                received.append(fileMessage);
                continue;
            }
            const QString url = elem.value(QStringLiteral("file_elem_url")).toString().trimmed();
            if (!url.isEmpty()) {
                fileMessage.file = RemoteIMFileAttachment{QString(), displayName, mimeTypeForFileName(displayName), sizeBytes};
                handleIncomingFileUrl(fileMessage, url);
            }
        }
    }
    if (!received.isEmpty()) emit messagesReceived(received);
}

void TimSdkRemoteIMClient::handleIncomingImageUrl(RemoteIMMessage message, const QString& url) {
    const QString targetPath = cacheImagePathForUrl(url);
    message.image.localPath = targetPath;
    message.text = QStringLiteral("[图片消息] ") + QFileInfo(targetPath).fileName();
    if (QFile::exists(targetPath)) {
        emit messagesReceived({message});
        return;
    }

    QNetworkReply* reply = network_.get(QNetworkRequest(QUrl(url)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, message] {
        const QByteArray data = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError && !data.isEmpty();
        reply->deleteLater();
        if (!ok) return;
        QFile file(message.image.localPath);
        if (!file.open(QIODevice::WriteOnly)) return;
        file.write(data);
        file.close();
        emit messagesReceived({message});
    });
}

void TimSdkRemoteIMClient::handleIncomingFileUrl(RemoteIMMessage message, const QString& url) {
    const QString targetPath = cacheFilePathForUrl(url, message.file.fileName);
    message.file.localPath = targetPath;
    if (QFile::exists(targetPath)) {
        emit messagesReceived({message});
        return;
    }

    QNetworkReply* reply = network_.get(QNetworkRequest(QUrl(url)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, message] {
        const QByteArray data = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError && !data.isEmpty();
        reply->deleteLater();
        if (!ok) return;
        QFile file(message.file.localPath);
        if (!file.open(QIODevice::WriteOnly)) return;
        file.write(data);
        file.close();
        emit messagesReceived({message});
    });
}

void TimSdkRemoteIMClient::complete(RemoteIMCompletion completion, int code, const QString& description) {
    if (!completion) return;
    completion(code == 0, code == 0 ? QString() : (description.isEmpty() ? QStringLiteral("IM SDK 操作失败：%1").arg(code) : description));
}

QString TimSdkRemoteIMClient::compactJson(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}
