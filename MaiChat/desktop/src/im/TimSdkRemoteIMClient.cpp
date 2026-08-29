#include "im/TimSdkRemoteIMClient.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
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
constexpr int kElemSound = 2;
constexpr int kElemFile = 4;
// TIMElemType 里 Video 排在 Text/Image/Sound/Custom/File/GroupTips/Face/Location/GroupReport
// 之后，值为 9（见 vendor/tencent-im/.../TIMMessageManager.h 的 enum TIMElemType）。
constexpr int kElemVideo = 9;
constexpr int kImageLevelOriginal = 0;
constexpr int kFriendTypeBoth = 1;
constexpr auto kCloudCustomDataKey = "message_cloud_custom_str";
constexpr auto kMetadataNamespace = "multi-ai-code";
constexpr int kMetadataVersion = 2;

struct ParsedCloudMetadata {
    RemoteIMMessageOrigin origin = RemoteIMMessageOrigin::Unknown;
    RemoteIMApprovalRequest approvalRequest;
    RemoteIMApprovalDecision approvalDecision;
    bool hasApprovalRequest = false;
    bool hasApprovalDecision = false;
};

QString originName(RemoteIMMessageOrigin origin) {
    return origin == RemoteIMMessageOrigin::Human
               ? QStringLiteral("human")
               : QStringLiteral("machine");
}

QString cloudCustomData(RemoteIMMessageOrigin origin,
                        const QJsonObject& interaction = QJsonObject()) {
    QJsonObject metadata;
    metadata[QStringLiteral("namespace")] = QString::fromLatin1(kMetadataNamespace);
    metadata[QStringLiteral("version")] = kMetadataVersion;
    metadata[QStringLiteral("origin")] = originName(origin);
    if (!interaction.isEmpty()) metadata[QStringLiteral("interaction")] = interaction;
    return QString::fromUtf8(QJsonDocument(metadata).toJson(QJsonDocument::Compact));
}

void setMessageMetadata(QJsonObject& message,
                        RemoteIMMessageOrigin origin,
                        const QJsonObject& interaction = QJsonObject()) {
    message[QString::fromLatin1(kCloudCustomDataKey)] = cloudCustomData(origin, interaction);
}

void setMessageOrigin(QJsonObject& message, RemoteIMMessageOrigin origin) {
    setMessageMetadata(message, origin);
}

ParsedCloudMetadata messageMetadata(const QJsonObject& message) {
    ParsedCloudMetadata parsed;
    const QString raw = message.value(QString::fromLatin1(kCloudCustomDataKey)).toString();
    if (raw.isEmpty()) return parsed;
    const QJsonDocument document = QJsonDocument::fromJson(raw.toUtf8());
    if (!document.isObject()) return parsed;
    const QJsonObject metadata = document.object();
    if (metadata.value(QStringLiteral("namespace")).toString() != QLatin1String(kMetadataNamespace)
        || metadata.value(QStringLiteral("version")).toInt(-1) != kMetadataVersion) {
        return parsed;
    }
    const QString origin = metadata.value(QStringLiteral("origin")).toString();
    if (origin == QStringLiteral("human")) {
        parsed.origin = RemoteIMMessageOrigin::Human;
    } else if (origin == QStringLiteral("machine")) {
        parsed.origin = RemoteIMMessageOrigin::Machine;
    } else {
        return ParsedCloudMetadata{};
    }

    if (!metadata.contains(QStringLiteral("interaction"))) return parsed;
    const QJsonObject interaction = metadata.value(QStringLiteral("interaction")).toObject();
    const QString kind = interaction.value(QStringLiteral("kind")).toString();
    const QString token = interaction.value(QStringLiteral("token")).toString();
    if (kind == QStringLiteral("approval-request")
        && parsed.origin == RemoteIMMessageOrigin::Machine
        && !interaction.contains(QStringLiteral("action"))) {
        RemoteIMApprovalRequest request;
        request.token = token;
        const QJsonArray actions = interaction.value(QStringLiteral("actions")).toArray();
        for (const QJsonValue& value : actions) {
            RemoteIMApprovalAction action;
            if (!remoteIMApprovalActionFromWireName(value.toString(), &action)) {
                return ParsedCloudMetadata{};
            }
            request.actions.append(action);
        }
        if (!request.isValid()) return ParsedCloudMetadata{};
        parsed.approvalRequest = request;
        parsed.hasApprovalRequest = true;
        return parsed;
    }
    if (kind == QStringLiteral("approval-decision")
        && parsed.origin == RemoteIMMessageOrigin::Human
        && !interaction.contains(QStringLiteral("actions"))
        && isValidRemoteIMApprovalToken(token)) {
        RemoteIMApprovalAction action;
        if (remoteIMApprovalActionFromWireName(
                interaction.value(QStringLiteral("action")).toString(), &action)
            && (action == RemoteIMApprovalAction::ApproveOnce
                || action == RemoteIMApprovalAction::ApprovePrefix
                || action == RemoteIMApprovalAction::Reject)) {
            parsed.approvalDecision = RemoteIMApprovalDecision{token, action};
            parsed.hasApprovalDecision = true;
            return parsed;
        }
    }
    if (kind == QStringLiteral("approval-resolved")
        && parsed.origin == RemoteIMMessageOrigin::Machine
        && !interaction.contains(QStringLiteral("actions"))
        && !interaction.contains(QStringLiteral("action"))
        && isValidRemoteIMApprovalToken(token)) {
        const QString outcome = interaction.value(QStringLiteral("outcome")).toString();
        const RemoteIMApprovalAction action = outcome == QStringLiteral("auto-declined")
            ? RemoteIMApprovalAction::AutoDeclined
            : (outcome == QStringLiteral("approved")
               || outcome == QStringLiteral("rejected")
               || outcome == QStringLiteral("resolved"))
                ? RemoteIMApprovalAction::Resolved
                : RemoteIMApprovalAction::Reject;
        if (outcome == QStringLiteral("auto-declined")
            || outcome == QStringLiteral("approved")
            || outcome == QStringLiteral("rejected")
            || outcome == QStringLiteral("resolved")) {
            parsed.approvalDecision = RemoteIMApprovalDecision{token, action};
            parsed.hasApprovalDecision = true;
            return parsed;
        }
    }
    return ParsedCloudMetadata{};
}

QString appDataDir(const QString& child) {
    QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (root.isEmpty()) root = QDir::homePath() + QStringLiteral("/.maichat-desktop");
    QDir dir(root);
    dir.mkpath(child);
    return dir.filePath(child);
}

QString cacheImagePathForUrl(const QString& url) {
    const QByteArray hash = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex();
    const QString suffix = QFileInfo(QUrl(url).path()).suffix().isEmpty() ? QStringLiteral("jpg") : QFileInfo(QUrl(url).path()).suffix();
    return QDir(appDataDir(QStringLiteral("RemoteIMImages"))).filePath(QString::fromUtf8(hash) + "." + suffix);
}

// 视频元素不带文件名，只能从本地路径或下载 URL 里抠；都抠不出来就用默认名——
// 气泡上总得有个能显示的标题。
QString videoDisplayName(const QJsonObject& elem) {
    const QString localPath = elem.value(QStringLiteral("video_elem_video_path")).toString().trimmed();
    if (!localPath.isEmpty()) {
        const QString name = QFileInfo(localPath).fileName();
        if (!name.isEmpty()) return name;
    }
    const QString url = elem.value(QStringLiteral("video_elem_video_url")).toString().trimmed();
    if (!url.isEmpty()) {
        const QString name = QFileInfo(QUrl(url).path()).fileName();
        if (!name.isEmpty()) return name;
    }
    return QStringLiteral("video.mp4");
}

QString cacheVoicePathForUrl(const QString& url) {
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex());
    const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                            .filePath(QStringLiteral("remote-im-voice"));
    QDir().mkpath(dir);
    // 腾讯 IM 的语音是 AMR/SILK 之类，扩展名统一给 .amr——播放侧按内容解码，
    // 这里只需要一个稳定且唯一的落盘名。
    return QDir(dir).filePath(hash + QStringLiteral(".amr"));
}

QString cacheVideoPathForUrl(const QString& url, const QString& suffix) {
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex());
    const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                            .filePath(QStringLiteral("remote-im-videos"));
    QDir().mkpath(dir);
    return QDir(dir).filePath(hash + suffix);
}

QString cacheFilePathForUrl(const QString& url, const QString& fileName) {
    const QByteArray hash = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex();
    QString suffix = QFileInfo(fileName).suffix();
    if (suffix.isEmpty()) suffix = QFileInfo(QUrl(url).path()).suffix();
    // 兜底不能用 md：那会把 zip/pdf 存成 .md，另存出去双击打不开。
    if (suffix.isEmpty()) suffix = QStringLiteral("bin");
    return QDir(appDataDir(QStringLiteral("RemoteIMFiles"))).filePath(QString::fromUtf8(hash) + "." + suffix);
}

// MIME 必须如实反映扩展名：MainWindow 的 isPreviewableDocument() 同时看 mimeType，
// 谎报 text/markdown 会让 pdf/zip 被当成文档去渲染。表与 electron/remote-im/localFile.ts 对齐。
QString mimeTypeForFileName(const QString& fileName) {
    static const QHash<QString, QString> kMimeBySuffix = {
        {QStringLiteral("md"), QStringLiteral("text/markdown")},
        {QStringLiteral("markdown"), QStringLiteral("text/markdown")},
        {QStringLiteral("html"), QStringLiteral("text/html")},
        {QStringLiteral("htm"), QStringLiteral("text/html")},
        {QStringLiteral("7z"), QStringLiteral("application/x-7z-compressed")},
        {QStringLiteral("csv"), QStringLiteral("text/csv")},
        {QStringLiteral("gif"), QStringLiteral("image/gif")},
        {QStringLiteral("gz"), QStringLiteral("application/gzip")},
        {QStringLiteral("jpeg"), QStringLiteral("image/jpeg")},
        {QStringLiteral("jpg"), QStringLiteral("image/jpeg")},
        {QStringLiteral("json"), QStringLiteral("application/json")},
        {QStringLiteral("log"), QStringLiteral("text/plain")},
        {QStringLiteral("mp3"), QStringLiteral("audio/mpeg")},
        {QStringLiteral("mp4"), QStringLiteral("video/mp4")},
        {QStringLiteral("pdf"), QStringLiteral("application/pdf")},
        {QStringLiteral("png"), QStringLiteral("image/png")},
        {QStringLiteral("txt"), QStringLiteral("text/plain")},
        {QStringLiteral("webp"), QStringLiteral("image/webp")},
        {QStringLiteral("xml"), QStringLiteral("application/xml")},
        {QStringLiteral("zip"), QStringLiteral("application/zip")}
    };
    return kMimeBySuffix.value(QFileInfo(fileName).suffix().toLower(),
                               QStringLiteral("application/octet-stream"));
}

QString fileDisplayName(const QString& fileName) {
    const QString name = QFileInfo(fileName).fileName().trimmed();
    return name.isEmpty() ? QStringLiteral("文件") : name;
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

    qInfo().noquote()
        << QStringLiteral("[im] connect: sdkAppId=%1 user=%2").arg(sdkAppId).arg(currentUserId_);
    const int initResult = api_->init(static_cast<quint64>(sdkAppId), sdkConfigJson());
    if (initResult != 0) {
        qWarning().noquote() << QStringLiteral("[im] SDK init failed: %1").arg(initResult);
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
        if (connected_) {
            qInfo().noquote() << QStringLiteral("[im] login ok: %1").arg(currentUserId_);
        } else {
            qWarning().noquote()
                << QStringLiteral("[im] login failed: code=%1 %2").arg(code).arg(description);
        }
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
    sendTextWithOrigin(peerId, text, RemoteIMMessageOrigin::Human, std::move(completion));
}

void TimSdkRemoteIMClient::sendMachineText(const QString& peerId, const QString& text, RemoteIMSendCompletion completion) {
    sendTextWithOrigin(peerId, text, RemoteIMMessageOrigin::Machine, std::move(completion));
}

void TimSdkRemoteIMClient::sendApprovalDecision(const QString& peerId,
                                                const QString& token,
                                                RemoteIMApprovalAction action,
                                                RemoteIMSendCompletion completion) {
    const QString cleanToken = token.trimmed();
    if (!isValidRemoteIMApprovalToken(cleanToken)) {
        if (completion) completion(false, QStringLiteral("审批请求已失效"), {});
        return;
    }
    if (action != RemoteIMApprovalAction::ApproveOnce
        && action != RemoteIMApprovalAction::ApprovePrefix
        && action != RemoteIMApprovalAction::Reject) {
        if (completion) completion(false, QStringLiteral("审批状态不能作为用户决定发送"), {});
        return;
    }
    QJsonObject interaction;
    interaction[QStringLiteral("kind")] = QStringLiteral("approval-decision");
    interaction[QStringLiteral("token")] = cleanToken;
    interaction[QStringLiteral("action")] = remoteIMApprovalActionWireName(action);
    sendTextWithOrigin(
        peerId,
        QStringLiteral("审批操作：%1").arg(remoteIMApprovalActionTitle(action)),
        RemoteIMMessageOrigin::Human,
        std::move(completion),
        interaction);
}

void TimSdkRemoteIMClient::sendTextWithOrigin(const QString& peerId,
                                              const QString& text,
                                              RemoteIMMessageOrigin origin,
                                              RemoteIMSendCompletion completion,
                                              const QJsonObject& interaction) {
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
    setMessageMetadata(message, origin, interaction);
    api_->sendMessage(cleanPeerId, kConversationTypeC2C, compactJson(message), [completion = std::move(completion)](int code,
                                                                                                                    const QString& description,
                                                                                                                    const QString& jsonPayload) mutable {
        if (!completion) return;
        const bool ok = code == 0;
        if (!ok) {
            qWarning().noquote()
                << QStringLiteral("[im] send failed: code=%1 %2").arg(code).arg(description);
        }
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
    setMessageOrigin(message, RemoteIMMessageOrigin::Human);
    api_->sendMessage(cleanPeerId, kConversationTypeC2C, compactJson(message), [completion = std::move(completion)](int code,
                                                                                                                    const QString& description,
                                                                                                                    const QString& jsonPayload) mutable {
        if (!completion) return;
        const bool ok = code == 0;
        if (!ok) {
            qWarning().noquote()
                << QStringLiteral("[im] send failed: code=%1 %2").arg(code).arg(description);
        }
        RemoteIMSendReceipt receipt = ok ? sentMessageReceipt(jsonPayload) : RemoteIMSendReceipt{};
        completion(ok,
                   ok ? QString() : (description.isEmpty() ? QStringLiteral("IM SDK 操作失败：%1").arg(code) : description),
                   receipt);
    });
}

void TimSdkRemoteIMClient::sendFile(const QString& peerId, const QString& localPath, const QString& fileName, RemoteIMSendCompletion completion) {
    const QString cleanPeerId = peerId.trimmed();
    const QString cleanPath = localPath.trimmed();
    if (cleanPeerId.isEmpty() || cleanPath.isEmpty()) {
        if (completion) completion(false, QStringLiteral("文件消息缺少接收人或文件路径"), {});
        return;
    }
    const QString displayName = fileName.trimmed().isEmpty() ? QFileInfo(cleanPath).fileName() : fileName.trimmed();

    QJsonObject elem;
    elem[QStringLiteral("elem_type")] = kElemFile;
    elem[QStringLiteral("file_elem_file_path")] = cleanPath;
    elem[QStringLiteral("file_elem_file_name")] = displayName;
    QJsonObject message;
    message[QStringLiteral("message_elem_array")] = QJsonArray{elem};
    setMessageOrigin(message, RemoteIMMessageOrigin::Human);
    api_->sendMessage(cleanPeerId, kConversationTypeC2C, compactJson(message), [completion = std::move(completion)](int code,
                                                                                                                    const QString& description,
                                                                                                                    const QString& jsonPayload) mutable {
        if (!completion) return;
        const bool ok = code == 0;
        if (!ok) {
            qWarning().noquote()
                << QStringLiteral("[im] send failed: code=%1 %2").arg(code).arg(description);
        }
        RemoteIMSendReceipt receipt = ok ? sentMessageReceipt(jsonPayload) : RemoteIMSendReceipt{};
        completion(ok,
                   ok ? QString() : (description.isEmpty() ? QStringLiteral("IM SDK 操作失败：%1").arg(code) : description),
                   receipt);
    });
}

void TimSdkRemoteIMClient::sendImageWithText(const QString& peerId, const QString& imagePath, const QString& text, bool captionAbove, RemoteIMSendCompletion completion) {
    const QString cleanPeerId = peerId.trimmed();
    const QString cleanPath = imagePath.trimmed();
    const QString cleanText = text.trimmed();
    if (cleanPeerId.isEmpty() || cleanPath.isEmpty()) {
        if (completion) completion(false, QStringLiteral("图片消息缺少接收人或图片路径"), {});
        return;
    }
    QJsonArray elems;
    auto appendCaption = [&elems, &cleanText]() {
        if (cleanText.isEmpty()) return;
        QJsonObject textElem;
        textElem[QStringLiteral("elem_type")] = kElemText;
        textElem[QStringLiteral("text_elem_content")] = cleanText;
        elems.append(textElem);
    };
    // 配文在上就先放文本元素。元素数组的顺序就是收发两端看到的排版顺序，
    // 不需要额外的协议字段来描述它。
    if (captionAbove) appendCaption();
    {
        QJsonObject imageElem;
        imageElem[QStringLiteral("elem_type")] = kElemImage;
        imageElem[QStringLiteral("image_elem_orig_path")] = cleanPath;
        imageElem[QStringLiteral("image_elem_level")] = kImageLevelOriginal;
        elems.append(imageElem);
    }
    if (!captionAbove) appendCaption();
    QJsonObject message;
    message[QStringLiteral("message_elem_array")] = elems;
    setMessageOrigin(message, RemoteIMMessageOrigin::Human);
    api_->sendMessage(cleanPeerId, kConversationTypeC2C, compactJson(message), [completion = std::move(completion)](int code,
                                                                                                                    const QString& description,
                                                                                                                    const QString& jsonPayload) mutable {
        if (!completion) return;
        const bool ok = code == 0;
        if (!ok) {
            qWarning().noquote()
                << QStringLiteral("[im] send failed: code=%1 %2").arg(code).arg(description);
        }
        RemoteIMSendReceipt receipt = ok ? sentMessageReceipt(jsonPayload) : RemoteIMSendReceipt{};
        completion(ok, ok ? QString() : (description.isEmpty() ? QStringLiteral("IM SDK 操作失败：%1").arg(code) : description), receipt);
    });
}

void TimSdkRemoteIMClient::sendFileWithText(const QString& peerId, const QString& localPath, const QString& fileName, const QString& text, bool captionAbove, RemoteIMSendCompletion completion) {
    const QString cleanPeerId = peerId.trimmed();
    const QString cleanPath = localPath.trimmed();
    const QString cleanText = text.trimmed();
    if (cleanPeerId.isEmpty() || cleanPath.isEmpty()) {
        if (completion) completion(false, QStringLiteral("文件消息缺少接收人或文件路径"), {});
        return;
    }
    const QString displayName = fileName.trimmed().isEmpty() ? QFileInfo(cleanPath).fileName() : fileName.trimmed();
    QJsonArray elems;
    auto appendCaption = [&elems, &cleanText]() {
        if (cleanText.isEmpty()) return;
        QJsonObject textElem;
        textElem[QStringLiteral("elem_type")] = kElemText;
        textElem[QStringLiteral("text_elem_content")] = cleanText;
        elems.append(textElem);
    };
    if (captionAbove) appendCaption();
    {
        QJsonObject fileElem;
        fileElem[QStringLiteral("elem_type")] = kElemFile;
        fileElem[QStringLiteral("file_elem_file_path")] = cleanPath;
        fileElem[QStringLiteral("file_elem_file_name")] = displayName;
        elems.append(fileElem);
    }
    if (!captionAbove) appendCaption();
    QJsonObject message;
    message[QStringLiteral("message_elem_array")] = elems;
    setMessageOrigin(message, RemoteIMMessageOrigin::Human);
    api_->sendMessage(cleanPeerId, kConversationTypeC2C, compactJson(message), [completion = std::move(completion)](int code,
                                                                                                                    const QString& description,
                                                                                                                    const QString& jsonPayload) mutable {
        if (!completion) return;
        const bool ok = code == 0;
        if (!ok) {
            qWarning().noquote()
                << QStringLiteral("[im] send failed: code=%1 %2").arg(code).arg(description);
        }
        RemoteIMSendReceipt receipt = ok ? sentMessageReceipt(jsonPayload) : RemoteIMSendReceipt{};
        completion(ok, ok ? QString() : (description.isEmpty() ? QStringLiteral("IM SDK 操作失败：%1").arg(code) : description), receipt);
    });
}

namespace {

// VideoElem 的九个「必填」字段一个都不能少：少一个 SDK 不会报错，只会发出一条
// 对端解析不出来的空视频消息——这是这套 C 接口最典型的静默失败方式。
QJsonObject videoElemJson(const RemoteIMVideoPayload& video) {
    QJsonObject elem;
    elem[QStringLiteral("elem_type")] = kElemVideo;
    elem[QStringLiteral("video_elem_video_path")] = video.videoPath;
    elem[QStringLiteral("video_elem_video_type")] = video.videoType;
    // QJsonValue 在 Qt5 里没有 qint64 重载，只有 int / double；用 double 序列化出来
    // 可能带小数点，jsoncpp 的 asUInt() 读到就废了。视频上限远小于 2GB，收敛成 int。
    elem[QStringLiteral("video_elem_video_size")] = static_cast<int>(video.videoSizeBytes);
    elem[QStringLiteral("video_elem_video_duration")] = video.durationSeconds;
    elem[QStringLiteral("video_elem_image_path")] = video.coverPath;
    elem[QStringLiteral("video_elem_image_type")] = video.coverType;
    elem[QStringLiteral("video_elem_image_size")] = static_cast<int>(video.coverSizeBytes);
    elem[QStringLiteral("video_elem_image_width")] = video.coverWidth;
    elem[QStringLiteral("video_elem_image_height")] = video.coverHeight;
    return elem;
}

}  // namespace

void TimSdkRemoteIMClient::sendVideo(const QString& peerId, const RemoteIMVideoPayload& video, RemoteIMSendCompletion completion) {
    sendVideoWithText(peerId, video, QString(), false, std::move(completion));
}

void TimSdkRemoteIMClient::sendVideoWithText(const QString& peerId, const RemoteIMVideoPayload& video, const QString& text, bool captionAbove, RemoteIMSendCompletion completion) {
    const QString cleanPeerId = peerId.trimmed();
    const QString cleanText = text.trimmed();
    if (cleanPeerId.isEmpty()) {
        if (completion) completion(false, QStringLiteral("视频消息缺少接收人"), {});
        return;
    }
    if (!video.isValid()) {
        if (completion) completion(false, QStringLiteral("视频消息缺少时长、尺寸或封面"), {});
        return;
    }

    QJsonArray elems;
    auto appendCaption = [&elems, &cleanText]() {
        if (cleanText.isEmpty()) return;
        QJsonObject textElem;
        textElem[QStringLiteral("elem_type")] = kElemText;
        textElem[QStringLiteral("text_elem_content")] = cleanText;
        elems.append(textElem);
    };
    if (captionAbove) appendCaption();
    elems.append(videoElemJson(video));
    if (!captionAbove) appendCaption();
    QJsonObject message;
    message[QStringLiteral("message_elem_array")] = elems;
    setMessageOrigin(message, RemoteIMMessageOrigin::Human);
    api_->sendMessage(cleanPeerId, kConversationTypeC2C, compactJson(message), [completion = std::move(completion)](int code,
                                                                                                                    const QString& description,
                                                                                                                    const QString& jsonPayload) mutable {
        if (!completion) return;
        const bool ok = code == 0;
        if (!ok) {
            qWarning().noquote()
                << QStringLiteral("[im] send failed: code=%1 %2").arg(code).arg(description);
        }
        RemoteIMSendReceipt receipt = ok ? sentMessageReceipt(jsonPayload) : RemoteIMSendReceipt{};
        completion(ok, ok ? QString() : (description.isEmpty() ? QStringLiteral("IM SDK 操作失败：%1").arg(code) : description), receipt);
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
    api_->getFriendList([this](int code, const QString& description, const QString& jsonPayload) {
        if (code != 0) {
            qWarning().noquote()
                << QStringLiteral("[im] fetch friend list failed: code=%1 %2").arg(code).arg(description);
            return;
        }
        if (jsonPayload.trimmed().isEmpty()) {
            qInfo().noquote() << QStringLiteral("[im] friend list: empty payload");
            return;
        }
        handleFriendListPayload(jsonPayload);
    });
}

void TimSdkRemoteIMClient::fetchConversationList() {
    api_->getConversationList([this](int code, const QString& description, const QString& jsonPayload) {
        if (code != 0) {
            qWarning().noquote()
                << QStringLiteral("[im] fetch conversation list failed: code=%1 %2").arg(code).arg(description);
            return;
        }
        if (jsonPayload.trimmed().isEmpty()) {
            qInfo().noquote() << QStringLiteral("[im] conversation list: empty payload");
            return;
        }
        handleConversationListPayload(jsonPayload);
    });
}

void TimSdkRemoteIMClient::fetchRecentMessages(const QString& conversationId, int conversationType) {
    if (conversationId.trimmed().isEmpty()) return;
    QJsonObject request;
    request[QStringLiteral("msg_getmsglist_param_count")] = 20;
    request[QStringLiteral("msg_getmsglist_param_is_ramble")] = true;
    request[QStringLiteral("msg_getmsglist_param_is_forward")] = false;
    api_->getMessageList(conversationId, conversationType, compactJson(request), [this, conversationId](int code,
                                                                                       const QString& description,
                                                                                       const QString& jsonPayload) {
        if (code != 0) {
            qWarning().noquote()
                << QStringLiteral("[im] fetch history failed: conv=%1 code=%2 %3")
                       .arg(conversationId).arg(code).arg(description);
            return;
        }
        if (jsonPayload.trimmed().isEmpty()) return;
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
        const QJsonObject userProfile = friendProfile.value(QStringLiteral("friend_profile_user_profile")).toObject();
        const QString displayName = friendDisplayName(friendProfile).trimmed();
        const QString avatarUrl = firstNonEmpty(userProfile, {
            QStringLiteral("user_profile_face_url")
        });
        contacts.append(RemoteIMContact{userId, displayName.isEmpty() ? userId : displayName, avatarUrl});
    }
    qInfo().noquote()
        << QStringLiteral("[im] friend list: %1 raw -> %2 contacts")
               .arg(friends.size()).arg(contacts.size());
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
            const QString avatarUrl = firstNonEmpty(conversation, {
                QStringLiteral("conv_face_url")
            });
            contacts.append(RemoteIMContact{
                conversationId,
                displayName.isEmpty() ? conversationId : displayName,
                avatarUrl
            });
            fetchRecentMessages(conversationId, conversationType);
        }
    }
    qInfo().noquote()
        << QStringLiteral("[im] conversation list: %1 raw -> %2 c2c")
               .arg(conversations.size()).arg(contacts.size());
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
        const ParsedCloudMetadata metadata = messageMetadata(sdkMessage);
        const RemoteIMMessageOrigin origin = metadata.origin;
        const QJsonArray elems = sdkMessage.value(QStringLiteral("message_elem_array")).toArray();

        // 与实时接收（handleIncomingMessage）保持一致：图片/文件 + 配文合并成一条，
        // 稳定 id 锚定在附件元素上，重登漫游拉取回来的 id 与实时投递一致，本地库去重不产生重复配文行。
        int captionElemIndex = -1;
        int firstAttachmentIndex = -1;
        QString caption;
        bool hasAttachment = false;
        for (int i = 0; i < elems.size(); ++i) {
            const QJsonObject elem = elems.at(i).toObject();
            const int elemType = elem.value(QStringLiteral("elem_type")).toInt(-1);
            if (elemType == kElemImage) {
                if (!elem.value(QStringLiteral("image_elem_orig_path")).toString().trimmed().isEmpty()) {
                    hasAttachment = true;
                    if (firstAttachmentIndex < 0) firstAttachmentIndex = i;
                }
            } else if (elemType == kElemFile || elemType == kElemVideo || elemType == kElemSound) {
                hasAttachment = true;
                if (firstAttachmentIndex < 0) firstAttachmentIndex = i;
            } else if (elemType == kElemText && captionElemIndex < 0) {
                const QString content = elem.value(QStringLiteral("text_elem_content")).toString();
                if (!content.trimmed().isEmpty()) {
                    caption = content;
                    captionElemIndex = i;
                }
            }
        }
        const QString attachmentCaption = hasAttachment ? caption : QString();
        // 配文元素排在附件元素之前，就说明发送方当时是「文字在上、附件在下」。
        // 元素顺序是发送方排版的唯一记录，这里不做别的猜测。
        const bool attachmentCaptionAbove =
            hasAttachment && captionElemIndex >= 0 && firstAttachmentIndex >= 0
            && captionElemIndex < firstAttachmentIndex;

        const auto makeBase = [&](int elemIndex) {
            RemoteIMMessage message;
            const QString stableId = sdkElemMessageId(sdkId, elemIndex);
            if (!stableId.isEmpty()) message.id = stableId;
            message.fromUserId = isFromSelf ? currentUserId_ : peerId;
            message.toUserId = isFromSelf ? peerId : currentUserId_;
            message.direction = isFromSelf ? RemoteIMMessageDirection::Outgoing : RemoteIMMessageDirection::Incoming;
            message.status = isFromSelf ? RemoteIMMessageStatus::Sent : RemoteIMMessageStatus::Received;
            message.origin = origin;
            message.createdAtMillis = orderedMessageTime(peerId, sdkTimeMillis);
            return message;
        };

        bool captionConsumed = false;
        for (int elemIndex = 0; elemIndex < elems.size(); ++elemIndex) {
            const QJsonObject elem = elems.at(elemIndex).toObject();
            const int elemType = elem.value(QStringLiteral("elem_type")).toInt(-1);
            if (elemType == kElemText) {
                if (hasAttachment && elemIndex == captionElemIndex) continue;
                RemoteIMMessage message = makeBase(elemIndex);
                message.text = elem.value(QStringLiteral("text_elem_content")).toString();
                if (!isFromSelf && metadata.hasApprovalRequest) {
                    message.approvalRequest = metadata.approvalRequest;
                    message.hasApprovalRequest = true;
                }
                if (metadata.hasApprovalDecision) {
                    message.approvalDecision = metadata.approvalDecision;
                    message.hasApprovalDecision = true;
                }
                if (!message.text.trimmed().isEmpty()) messages.append(message);
                continue;
            }
            if (elemType == kElemImage) {
                const QString localPath = elem.value(QStringLiteral("image_elem_orig_path")).toString().trimmed();
                if (localPath.isEmpty()) continue;
                RemoteIMMessage message = makeBase(elemIndex);
                message.hasImage = true;
                message.image = RemoteIMImageAttachment{
                    localPath,
                    elem.value(QStringLiteral("image_elem_orig_pic_width")).toInt(0),
                    elem.value(QStringLiteral("image_elem_orig_pic_height")).toInt(0),
                    static_cast<qint64>(elem.value(QStringLiteral("image_elem_orig_pic_size")).toDouble(0))
                };
                if (!attachmentCaption.isEmpty() && !captionConsumed) {
                    message.text = attachmentCaption;
                message.captionAbove = attachmentCaptionAbove;
                    captionConsumed = true;
                } else {
                    message.text = QStringLiteral("[图片消息] ") + QFileInfo(localPath).fileName();
                }
                messages.append(message);
                continue;
            }
            if (elemType == kElemSound) {
                const QString localPath = elem.value(QStringLiteral("sound_elem_file_path")).toString().trimmed();
                const QString url = elem.value(QStringLiteral("sound_elem_url")).toString().trimmed();
                const int duration = elem.value(QStringLiteral("sound_elem_file_time")).toInt(0);
                RemoteIMMessage message = makeBase(elemIndex);
                message.hasVoice = true;
                message.text = (!attachmentCaption.isEmpty() && !captionConsumed)
                                   ? attachmentCaption
                                   : QStringLiteral("[语音消息]");
                if (!attachmentCaption.isEmpty() && !captionConsumed) captionConsumed = true;
                message.voice = RemoteIMVoiceAttachment{localPath, duration > 0 ? duration : 1};
                if (!localPath.isEmpty()) {
                    messages.append(message);
                } else if (!url.isEmpty()) {
                    handleIncomingVoiceUrl(message, url, /*live=*/false);
                }
                continue;
            }
            if (elemType == kElemVideo) {
                const QString videoUrl = elem.value(QStringLiteral("video_elem_video_url")).toString().trimmed();
                const QString coverUrl = elem.value(QStringLiteral("video_elem_image_url")).toString().trimmed();
                const QString localPath = elem.value(QStringLiteral("video_elem_video_path")).toString().trimmed();
                const int duration = elem.value(QStringLiteral("video_elem_video_duration")).toInt(0);
                const qint64 sizeBytes = static_cast<qint64>(elem.value(QStringLiteral("video_elem_video_size")).toDouble(0));
                const QString displayName = videoDisplayName(elem);
                RemoteIMMessage message = makeBase(elemIndex);
                message.hasVideo = true;
                message.text = (!attachmentCaption.isEmpty() && !captionConsumed)
                                   ? attachmentCaption
                                   : QStringLiteral("[视频消息] ") + displayName;
                if (!attachmentCaption.isEmpty() && !captionConsumed) captionConsumed = true;
                message.video = RemoteIMVideoAttachment{localPath, displayName, QString(), duration, sizeBytes};
                if (!localPath.isEmpty()) {
                    messages.append(message);
                } else if (!videoUrl.isEmpty()) {
                    handleIncomingVideoUrls(message, videoUrl, coverUrl, /*live=*/false);
                }
                continue;
            }
            if (elemType == kElemFile) {
                const QString fileName = firstNonEmpty(elem, {
                    QStringLiteral("file_elem_file_name"),
                    QStringLiteral("file_elem_file_path")
                });
                const QString localPath = elem.value(QStringLiteral("file_elem_file_path")).toString().trimmed();
                const qint64 sizeBytes = static_cast<qint64>(elem.value(QStringLiteral("file_elem_file_size")).toDouble(0));
                const QString url = elem.value(QStringLiteral("file_elem_url")).toString().trimmed();
                const QString displayName = fileDisplayName(fileName);
                const QString captionText = (!attachmentCaption.isEmpty() && !captionConsumed)
                                                ? attachmentCaption
                                                : QStringLiteral("[文件消息] ") + displayName;
                if (localPath.isEmpty() && !url.isEmpty()) {
                    const QString cachedPath = cacheFilePathForUrl(url, fileName);
                    RemoteIMMessage message = makeBase(elemIndex);
                    message.hasFile = true;
                    message.text = captionText;
                    if (!attachmentCaption.isEmpty() && !captionConsumed) captionConsumed = true;
                    if (QFile::exists(cachedPath)) {
                        message.file = RemoteIMFileAttachment{cachedPath, displayName, mimeTypeForFileName(fileName), sizeBytes};
                        messages.append(message);
                    } else {
                        message.file = RemoteIMFileAttachment{QString(), displayName, mimeTypeForFileName(fileName), sizeBytes};
                        handleIncomingFileUrl(message, url, /*live=*/false);
                    }
                    continue;
                }
                if (!localPath.isEmpty()) {
                    RemoteIMMessage message = makeBase(elemIndex);
                    message.hasFile = true;
                    message.text = captionText;
                    if (!attachmentCaption.isEmpty() && !captionConsumed) captionConsumed = true;
                    message.file = RemoteIMFileAttachment{localPath, displayName, mimeTypeForFileName(fileName), sizeBytes};
                    messages.append(message);
                }
            }
        }
    }
    qInfo().noquote()
        << QStringLiteral("[im] recv history: %1 raw -> %2 messages%3")
               .arg(sdkMessages.size())
               .arg(messages.size())
               .arg(messages.isEmpty() && !sdkMessages.isEmpty()
                        ? QStringLiteral("  <- raw messages produced nothing; "
                                         "an elem_type branch may be missing")
                        : QString());
    emitReceivedMessages(messages, /*live=*/false);
}

QString TimSdkRemoteIMClient::sdkConfigJson() const {
    QJsonObject config;
    config[QStringLiteral("sdk_config_config_file_path")] = appDataDir(QStringLiteral("SdkConfig"));
    config[QStringLiteral("sdk_config_log_file_path")] = appDataDir(QStringLiteral("SdkLogs"));
    return compactJson(config);
}

void TimSdkRemoteIMClient::handleIncomingMessages(const QString& jsonMessages) {
    const QJsonDocument doc = QJsonDocument::fromJson(jsonMessages.toUtf8());
    if (doc.isNull()) {
        qWarning().noquote() << "[im] recv: callback JSON is not parseable; whole batch dropped";
        return;
    }
    const QJsonArray messages = doc.isArray() ? doc.array() : QJsonArray{doc.object()};
    qInfo().noquote() << QStringLiteral("[im] recv: %1 pushed by SDK").arg(messages.size());
    for (const QJsonValue& value : messages) {
        handleIncomingMessage(value.toObject());
    }
}

void TimSdkRemoteIMClient::handleIncomingMessage(const QJsonObject& message) {
    // 这两个 return 以前是完全静默的：消息在这里消失，日志里一个字都没有。
    if (message.value(QStringLiteral("message_is_from_self")).toBool(false)) return;
    const QString fromUserId = firstNonEmpty(message, {QStringLiteral("message_sender"), QStringLiteral("message_conv_id")});
    if (fromUserId.isEmpty()) {
        qWarning().noquote() << "[im] recv: message has no sender id; dropped";
        return;
    }

    // 实时消息与漫游是同一条消息的两次投递：构造与漫游路径完全一致的
    // RemoteIMMessage（含稳定 SDK id 与服务器时间），经 messagesReceived 通道
    // 送出，本地库据 id 去重，重登时不会与漫游重复。
    const QString sdkId = sdkMessageId(message);
    const qint64 sdkTimeMillis = messageTimeMillis(message);
    const ParsedCloudMetadata metadata = messageMetadata(message);
    const RemoteIMMessageOrigin origin = metadata.origin;
    const auto baseMessage = [this, &fromUserId, &sdkId, sdkTimeMillis, origin](int elemIndex) {
        RemoteIMMessage result;
        const QString stableId = sdkElemMessageId(sdkId, elemIndex);
        if (!stableId.isEmpty()) result.id = stableId;
        result.fromUserId = fromUserId;
        result.toUserId = currentUserId_;
        result.direction = RemoteIMMessageDirection::Incoming;
        result.status = RemoteIMMessageStatus::Received;
        result.origin = origin;
        result.createdAtMillis = orderedMessageTime(fromUserId, sdkTimeMillis);
        return result;
    };

    const QJsonArray elems = message.value(QStringLiteral("message_elem_array")).toArray();

    // 图片/文件 + 配文合并成「一条」消息：先取第一条非空文本作为附件配文，
    // 附件与配文共用同一条 RemoteIMMessage（稳定 id 锚定在附件元素上），
    // 使实时与漫游两路投递出的 id 一致，本地库不会把配文当成独立文本重复入库。
    int captionElemIndex = -1;
    int firstAttachmentIndex = -1;
    QString caption;
    bool hasAttachment = false;
    for (int i = 0; i < elems.size(); ++i) {
        const QJsonObject elem = elems.at(i).toObject();
        const int elemType = elem.value(QStringLiteral("elem_type")).toInt(-1);
        if (elemType == kElemImage) {
            hasAttachment = true;
            if (firstAttachmentIndex < 0) firstAttachmentIndex = i;
        } else if (elemType == kElemFile || elemType == kElemVideo || elemType == kElemSound) {
            hasAttachment = true;
            if (firstAttachmentIndex < 0) firstAttachmentIndex = i;
        } else if (elemType == kElemText && captionElemIndex < 0) {
            const QString content = elem.value(QStringLiteral("text_elem_content")).toString();
            if (!content.trimmed().isEmpty()) {
                caption = content;
                captionElemIndex = i;
            }
        }
    }
    const QString attachmentCaption = hasAttachment ? caption : QString();
    // 配文元素排在附件元素之前，就说明发送方当时是「文字在上、附件在下」。
    // 元素顺序是发送方排版的唯一记录，这里不做别的猜测。
    const bool attachmentCaptionAbove =
        hasAttachment && captionElemIndex >= 0 && firstAttachmentIndex >= 0
        && captionElemIndex < firstAttachmentIndex;

    QList<RemoteIMMessage> received;
    bool captionConsumed = false;
    for (int elemIndex = 0; elemIndex < elems.size(); ++elemIndex) {
        const QJsonObject elem = elems.at(elemIndex).toObject();
        const int elemType = elem.value(QStringLiteral("elem_type")).toInt(-1);
        if (elemType == kElemText) {
            // 已并入附件的配文不再作为独立文本消息重复投递。
            if (hasAttachment && elemIndex == captionElemIndex) continue;
            const QString text = elem.value(QStringLiteral("text_elem_content")).toString();
            if (text.trimmed().isEmpty()) continue;
            RemoteIMMessage textMessage = baseMessage(elemIndex);
            textMessage.text = text;
            if (metadata.hasApprovalRequest) {
                textMessage.approvalRequest = metadata.approvalRequest;
                textMessage.hasApprovalRequest = true;
            }
            if (metadata.hasApprovalDecision) {
                textMessage.approvalDecision = metadata.approvalDecision;
                textMessage.hasApprovalDecision = true;
            }
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
            if (!attachmentCaption.isEmpty() && !captionConsumed) {
                imageMessage.text = attachmentCaption;
                imageMessage.captionAbove = attachmentCaptionAbove;
                captionConsumed = true;
            }
            if (!localPath.isEmpty()) {
                if (imageMessage.text.trimmed().isEmpty())
                    imageMessage.text = QStringLiteral("[图片消息] ") + QFileInfo(localPath).fileName();
                received.append(imageMessage);
                continue;
            }
            const QString url = firstNonEmpty(elem, {
                QStringLiteral("image_elem_large_url"),
                QStringLiteral("image_elem_orig_url"),
                QStringLiteral("image_elem_thumb_url")
            });
            if (!url.isEmpty()) handleIncomingImageUrl(imageMessage, url, /*live=*/true);
            continue;
        }
        if (elemType == kElemSound) {
            const QString localPath = elem.value(QStringLiteral("sound_elem_file_path")).toString().trimmed();
            const QString url = elem.value(QStringLiteral("sound_elem_url")).toString().trimmed();
            const int duration = elem.value(QStringLiteral("sound_elem_file_time")).toInt(0);
            RemoteIMMessage voiceMessage = baseMessage(elemIndex);
            voiceMessage.hasVoice = true;
            if (!attachmentCaption.isEmpty() && !captionConsumed) {
                voiceMessage.text = attachmentCaption;
                voiceMessage.captionAbove = attachmentCaptionAbove;
                captionConsumed = true;
            } else {
                voiceMessage.text = QStringLiteral("[语音消息]");
            }
            voiceMessage.voice = RemoteIMVoiceAttachment{localPath, duration > 0 ? duration : 1};
            if (!localPath.isEmpty()) {
                received.append(voiceMessage);
            } else if (!url.isEmpty()) {
                handleIncomingVoiceUrl(voiceMessage, url, /*live=*/true);
            }
            continue;
        }
        if (elemType == kElemVideo) {
            const QString videoUrl = elem.value(QStringLiteral("video_elem_video_url")).toString().trimmed();
            const QString coverUrl = elem.value(QStringLiteral("video_elem_image_url")).toString().trimmed();
            const QString localPath = elem.value(QStringLiteral("video_elem_video_path")).toString().trimmed();
            const int duration = elem.value(QStringLiteral("video_elem_video_duration")).toInt(0);
            const qint64 sizeBytes = static_cast<qint64>(elem.value(QStringLiteral("video_elem_video_size")).toDouble(0));
            const QString displayName = videoDisplayName(elem);
            RemoteIMMessage videoMessage = baseMessage(elemIndex);
            videoMessage.hasVideo = true;
            if (!attachmentCaption.isEmpty() && !captionConsumed) {
                videoMessage.text = attachmentCaption;
                videoMessage.captionAbove = attachmentCaptionAbove;
                captionConsumed = true;
            } else {
                videoMessage.text = QStringLiteral("[视频消息] ") + displayName;
            }
            videoMessage.video = RemoteIMVideoAttachment{localPath, displayName, QString(), duration, sizeBytes};
            if (!localPath.isEmpty()) {
                received.append(videoMessage);
            } else if (!videoUrl.isEmpty()) {
                handleIncomingVideoUrls(videoMessage, videoUrl, coverUrl, /*live=*/true);
            }
            continue;
        }
        if (elemType == kElemFile) {
            const QString fileName = firstNonEmpty(elem, {
                QStringLiteral("file_elem_file_name"),
                QStringLiteral("file_elem_file_path")
            });
            const QString displayName = fileDisplayName(fileName);
            const QString localPath = elem.value(QStringLiteral("file_elem_file_path")).toString().trimmed();
            const qint64 sizeBytes = static_cast<qint64>(elem.value(QStringLiteral("file_elem_file_size")).toDouble(0));
            RemoteIMMessage fileMessage = baseMessage(elemIndex);
            fileMessage.hasFile = true;
            if (!attachmentCaption.isEmpty() && !captionConsumed) {
                fileMessage.text = attachmentCaption;
                fileMessage.captionAbove = attachmentCaptionAbove;
                captionConsumed = true;
            } else {
                fileMessage.text = QStringLiteral("[文件消息] ") + displayName;
            }
            if (!localPath.isEmpty()) {
                fileMessage.file = RemoteIMFileAttachment{localPath, displayName, mimeTypeForFileName(displayName), sizeBytes};
                received.append(fileMessage);
                continue;
            }
            const QString url = elem.value(QStringLiteral("file_elem_url")).toString().trimmed();
            if (!url.isEmpty()) {
                fileMessage.file = RemoteIMFileAttachment{QString(), displayName, mimeTypeForFileName(displayName), sizeBytes};
                handleIncomingFileUrl(fileMessage, url, /*live=*/true);
            }
        }
    }
    // 把「收到哪些元素类型」与「产出几条消息」一起记下来：某个 elem_type 没有
    // 对应分支时，这两个数字会对不上——语音收不到那次，正是缺这条日志才只能读代码。
    QStringList elemTypes;
    for (const QJsonValue& value : elems) {
        elemTypes << QString::number(value.toObject().value(QStringLiteral("elem_type")).toInt(-1));
    }
    qInfo().noquote()
        << QStringLiteral("[im] recv live: from=%1 id=%2 elems=[%3] -> %4 messages%5")
               .arg(fromUserId, sdkId, elemTypes.join(QLatin1Char(',')))
               .arg(received.size())
               .arg(received.isEmpty() && !elems.isEmpty()
                        ? QStringLiteral("  <- elems produced no message; "
                                         "an elem_type branch may be missing")
                        : QString());
    emitReceivedMessages(received, /*live=*/true);
}

void TimSdkRemoteIMClient::handleIncomingImageUrl(RemoteIMMessage message, const QString& url, bool live) {
    const QString targetPath = cacheImagePathForUrl(url);
    message.image.localPath = targetPath;
    // 保留调用方合并进来的配文；只有真正无配文时才补占位文字。
    if (message.text.trimmed().isEmpty())
        message.text = QStringLiteral("[图片消息] ") + QFileInfo(targetPath).fileName();
    if (QFile::exists(targetPath)) {
        emitReceivedMessages({message}, live);
        return;
    }

    QNetworkReply* reply = network_.get(QNetworkRequest(QUrl(url)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, message, url, live] {
        const QByteArray data = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError && !data.isEmpty();
        const QString err = reply->errorString();
        reply->deleteLater();
        if (!ok) {
            // 常见原因：未携带 OpenSSL 1.1 导致 HTTPS 请求失败（supportsSsl()==false）。
            qWarning().noquote()
                << QStringLiteral("[im] image download failed: %1 - %2").arg(err, url);
            return;
        }
        QFile file(message.image.localPath);
        if (!file.open(QIODevice::WriteOnly)) return;
        file.write(data);
        file.close();
        emitReceivedMessages({message}, live);
    });
}

void TimSdkRemoteIMClient::handleIncomingFileUrl(RemoteIMMessage message, const QString& url, bool live) {
    const QString targetPath = cacheFilePathForUrl(url, message.file.fileName);
    message.file.localPath = targetPath;
    if (QFile::exists(targetPath)) {
        emitReceivedMessages({message}, live);
        return;
    }

    QNetworkReply* reply = network_.get(QNetworkRequest(QUrl(url)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, message, live] {
        const QByteArray data = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError && !data.isEmpty();
        const QString replyError = reply->errorString();
        reply->deleteLater();
        if (!ok) {
            // 以前这里是静默 return：附件下载失败时界面上什么都不出现，日志里也没有痕迹。
            qWarning().noquote()
                << QStringLiteral("[im] attachment download failed: %1 (%2 bytes)")
                       .arg(replyError).arg(data.size());
            return;
        }
        QFile file(message.file.localPath);
        if (!file.open(QIODevice::WriteOnly)) return;
        file.write(data);
        file.close();
        emitReceivedMessages({message}, live);
    });
}

void TimSdkRemoteIMClient::handleIncomingVideoUrls(RemoteIMMessage message,
                                                   const QString& videoUrl,
                                                   const QString& coverUrl,
                                                   bool live) {
    const QString videoPath = cacheVideoPathForUrl(videoUrl, QStringLiteral(".mp4"));
    message.video.localPath = videoPath;

    // 先把封面拿下来（小、快），再下视频本体。封面失败只是没有缩略图，
    // 不能因此把整条视频消息拦住——气泡会退化成深色底 + 播放角标，仍可播放。
    // message 必须按参数传进来：如果在这里按值捕获，封面回调里补上的 coverPath
    // 改的是外层那一份，拷贝进 lambda 的那份永远是空的，封面白下了。
    const auto fetchVideo = [this, videoUrl, videoPath, live](RemoteIMMessage message) {
        if (QFile::exists(videoPath)) {
            emitReceivedMessages({message}, live);
            return;
        }
        QNetworkReply* reply = network_.get(QNetworkRequest(QUrl(videoUrl)));
        connect(reply, &QNetworkReply::finished, this, [this, reply, message, live] {
            const QByteArray data = reply->readAll();
            const bool ok = reply->error() == QNetworkReply::NoError && !data.isEmpty();
            const QString replyError = reply->errorString();
            reply->deleteLater();
            if (!ok) {
                qWarning().noquote()
                    << QStringLiteral("[im] video download failed: %1 (%2 bytes)")
                           .arg(replyError).arg(data.size());
                return;
            }
            QFile file(message.video.localPath);
            if (!file.open(QIODevice::WriteOnly)) {
                qWarning().noquote()
                    << QStringLiteral("[im] video write failed: %1").arg(message.video.localPath);
                return;
            }
            file.write(data);
            file.close();
            emitReceivedMessages({message}, live);
        });
    };

    if (coverUrl.trimmed().isEmpty()) {
        fetchVideo(message);
        return;
    }
    const QString coverPath = cacheVideoPathForUrl(coverUrl, QStringLiteral(".jpg"));
    if (QFile::exists(coverPath)) {
        message.video.coverPath = coverPath;
        fetchVideo(message);
        return;
    }
    QNetworkReply* coverReply = network_.get(QNetworkRequest(QUrl(coverUrl)));
    connect(coverReply, &QNetworkReply::finished, this,
            [coverReply, coverPath, message, fetchVideo]() mutable {
        const QByteArray data = coverReply->readAll();
        const bool ok = coverReply->error() == QNetworkReply::NoError && !data.isEmpty();
        coverReply->deleteLater();
        if (ok) {
            QFile file(coverPath);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(data);
                file.close();
                message.video.coverPath = coverPath;
            }
        }
        fetchVideo(message);
    });
}

void TimSdkRemoteIMClient::handleIncomingVoiceUrl(RemoteIMMessage message, const QString& url, bool live) {
    const QString targetPath = cacheVoicePathForUrl(url);
    message.voice.localPath = targetPath;
    if (QFile::exists(targetPath)) {
        emitReceivedMessages({message}, live);
        return;
    }

    QNetworkReply* reply = network_.get(QNetworkRequest(QUrl(url)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, message, live] {
        const QByteArray data = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError && !data.isEmpty();
        const QString replyError = reply->errorString();
        reply->deleteLater();
        if (!ok) {
            // 以前这里是静默 return：附件下载失败时界面上什么都不出现，日志里也没有痕迹。
            qWarning().noquote()
                << QStringLiteral("[im] attachment download failed: %1 (%2 bytes)")
                       .arg(replyError).arg(data.size());
            return;
        }
        QFile file(message.voice.localPath);
        if (!file.open(QIODevice::WriteOnly)) return;
        file.write(data);
        file.close();
        emitReceivedMessages({message}, live);
    });
}

void TimSdkRemoteIMClient::emitReceivedMessages(const QList<RemoteIMMessage>& messages, bool live) {
    if (messages.isEmpty()) return;
    if (live) {
        emit liveMessagesReceived(messages);
    } else {
        emit messagesReceived(messages);
    }
}

void TimSdkRemoteIMClient::complete(RemoteIMCompletion completion, int code, const QString& description) {
    if (!completion) return;
    completion(code == 0, code == 0 ? QString() : (description.isEmpty() ? QStringLiteral("IM SDK 操作失败：%1").arg(code) : description));
}

QString TimSdkRemoteIMClient::compactJson(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}
