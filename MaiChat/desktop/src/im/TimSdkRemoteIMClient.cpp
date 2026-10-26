// 文件作用：桌面端IM 会话、消息与登录；TimSdkRemoteIMClient 负责外部接口请求与结果转换，本文件给出其实现。
// 主要入口：orderedMessageTime、connectToService、disconnectFromService、deleteContact。
// 直接依赖：diagnostics/RemoteDiagnosticsProtocol.h、im/DynamicTimSdkApi.h；实现与这些接口协作。
#include "im/TimSdkRemoteIMClient.h"

#include "diagnostics/RemoteDiagnosticsProtocol.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QPointer>
#include <QRegularExpression>
#include <QtMath>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

#include "im/DynamicTimSdkApi.h"

namespace {

constexpr int kConversationTypeC2C = 1;
constexpr int kConversationTypeGroup = 2;
constexpr int kGroupTypeWork = 1;
constexpr int kElemText = 0;
constexpr int kElemImage = 1;
constexpr int kElemSound = 2;
constexpr int kElemCustom = 3;
constexpr int kElemFile = 4;
// TIMElemType 里 Video 排在 Text/Image/Sound/Custom/File/GroupTips/Face/Location/GroupReport
// 之后，值为 9（见 vendor/tencent-im/.../TIMMessageManager.h 的 enum TIMElemType）。
constexpr int kElemVideo = 9;
constexpr int kElemGroupTips = 5;
constexpr auto kGroupActivation = "{\"type\":\"maichat_group_activation\",\"v\":1}";
constexpr int kImageLevelOriginal = 0;
constexpr int kFriendTypeBoth = 1;
constexpr auto kCloudCustomDataKey = "message_cloud_custom_str";
constexpr auto kMetadataNamespace = "multi-ai-code";
constexpr int kMetadataVersion = 2;
constexpr auto kActivityNamespace = "multi-ai-code-activity";
constexpr int kActivityVersion = 2;
// 引用块字段的长度上限。对端可以往 metadata 里塞任意长的串，
// 不设上限等于让别人决定我们的内存占用和渲染耗时。
constexpr int kQuoteFieldLimit = 256;
constexpr int kQuoteDigestLimit = 200;
constexpr qint64 kMaximumImageBytes = 32LL * 1024 * 1024;
constexpr qint64 kMaximumVoiceBytes = 64LL * 1024 * 1024;
constexpr qint64 kMaximumFileBytes = 512LL * 1024 * 1024;
constexpr qint64 kMaximumVideoBytes = 1024LL * 1024 * 1024;
constexpr qint64 kMaximumVideoCoverBytes = 16LL * 1024 * 1024;

struct ParsedCloudMetadata {
    RemoteIMMessageOrigin origin = RemoteIMMessageOrigin::Unknown;
    bool captionAbove = false;
    RemoteIMQuote quote;
    bool hasQuote = false;
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

QString activityKindName(RemoteIMActivityKind kind) {
    switch (kind) {
    case RemoteIMActivityKind::HumanTyping: return QStringLiteral("human-typing");
    case RemoteIMActivityKind::MachineWorking: return QStringLiteral("machine-working");
    case RemoteIMActivityKind::MachineThinking: return QStringLiteral("machine-thinking");
    case RemoteIMActivityKind::MachineTool: return QStringLiteral("machine-tool");
    case RemoteIMActivityKind::MachineWaiting: return QStringLiteral("machine-waiting");
    }
    return QStringLiteral("human-typing");
}

bool activityKindFromName(const QString& value, RemoteIMActivityKind* out) {
    if (value == QStringLiteral("human-typing")) *out = RemoteIMActivityKind::HumanTyping;
    else if (value == QStringLiteral("machine-working")) *out = RemoteIMActivityKind::MachineWorking;
    else if (value == QStringLiteral("machine-thinking")) *out = RemoteIMActivityKind::MachineThinking;
    else if (value == QStringLiteral("machine-tool")) *out = RemoteIMActivityKind::MachineTool;
    else if (value == QStringLiteral("machine-waiting")) *out = RemoteIMActivityKind::MachineWaiting;
    else return false;
    return true;
}

QString activityData(const RemoteIMActivitySignal& signal) {
    const qint64 startedAt = signal.startedAtMs > 0
        ? signal.startedAtMs
        : QDateTime::currentMSecsSinceEpoch();
    const qint64 taskStartedAt = signal.taskStartedAtMs > 0
        ? qMin(signal.taskStartedAtMs, startedAt)
        : startedAt;
    QJsonObject object;
    object[QStringLiteral("namespace")] = QString::fromLatin1(kActivityNamespace);
    object[QStringLiteral("version")] = kActivityVersion;
    object[QStringLiteral("activityId")] = signal.activityId;
    object[QStringLiteral("sequence")] = static_cast<double>(signal.sequence);
    object[QStringLiteral("kind")] = activityKindName(signal.kind);
    object[QStringLiteral("active")] = signal.active;
    object[QStringLiteral("startedAtMs")] = static_cast<double>(startedAt);
    object[QStringLiteral("taskStartedAtMs")] = static_cast<double>(taskStartedAt);
    object[QStringLiteral("ttlMs")] = qBound(1000, signal.ttlMs, 30000);
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

bool parseActivityData(const QString& data, RemoteIMActivitySignal* out) {
    const QJsonObject object = QJsonDocument::fromJson(data.toUtf8()).object();
    if (object.value(QStringLiteral("namespace")).toString() != QLatin1String(kActivityNamespace)
        || object.value(QStringLiteral("version")).toInt(-1) != kActivityVersion) return false;
    const QString id = object.value(QStringLiteral("activityId")).toString();
    static const QRegularExpression idPattern(QStringLiteral("^[A-Za-z0-9._:-]{1,192}$"));
    if (!idPattern.match(id).hasMatch()) return false;
    RemoteIMActivityKind kind;
    if (!activityKindFromName(object.value(QStringLiteral("kind")).toString(), &kind)) return false;
    if (!object.value(QStringLiteral("active")).isBool()
        || !object.value(QStringLiteral("ttlMs")).isDouble()) return false;
    const double sequence = object.value(QStringLiteral("sequence")).toDouble(-1);
    if (sequence < 0 || sequence > 9007199254740991.0 || sequence != qFloor(sequence)) return false;
    const QJsonValue startedAtValue = object.value(QStringLiteral("startedAtMs"));
    if (!startedAtValue.isDouble()) return false;
    const double startedAt = startedAtValue.toDouble(-1);
    if (startedAt <= 0 || startedAt > 9007199254740991.0
        || startedAt != static_cast<double>(static_cast<qint64>(startedAt))) {
        return false;
    }
    const QJsonValue taskStartedAtValue = object.value(QStringLiteral("taskStartedAtMs"));
    if (!taskStartedAtValue.isDouble()) return false;
    const double taskStartedAt = taskStartedAtValue.toDouble(-1);
    if (taskStartedAt <= 0 || taskStartedAt > startedAt
        || taskStartedAt != static_cast<double>(static_cast<qint64>(taskStartedAt))) {
        return false;
    }
    out->sequence = static_cast<qint64>(sequence);
    out->activityId = id;
    out->kind = kind;
    out->active = object.value(QStringLiteral("active")).toBool();
    out->startedAtMs = static_cast<qint64>(startedAt);
    out->taskStartedAtMs = static_cast<qint64>(taskStartedAt);
    out->ttlMs = qBound(1000, object.value(QStringLiteral("ttlMs")).toInt(), 30000);
    return true;
}

// 引用块编码。msgId 为空时**整个键都不写**，而不是写一个空串：
// 空串会让对端以为「有 ID 但解析不出来」，而事实是这条消息压根没有跨端可解析的 ID。
QJsonObject quoteObject(const RemoteIMQuote& quote) {
    QJsonObject object;
    if (!quote.msgId.isEmpty()) object[QStringLiteral("msgId")] = quote.msgId;
    object[QStringLiteral("sender")] = quote.senderId;
    object[QStringLiteral("digest")] = quote.digest;
    object[QStringLiteral("kind")] = quote.kind;
    return object;
}

QString cloudCustomData(RemoteIMMessageOrigin origin,
                        const QJsonObject& interaction = QJsonObject(),
                        bool captionAbove = false,
                        const QJsonObject& quote = QJsonObject()) {
    QJsonObject metadata;
    metadata[QStringLiteral("namespace")] = QString::fromLatin1(kMetadataNamespace);
    metadata[QStringLiteral("version")] = kMetadataVersion;
    metadata[QStringLiteral("origin")] = originName(origin);
    if (!interaction.isEmpty()) metadata[QStringLiteral("interaction")] = interaction;
    // 配文排在附件上方。走元数据而不是靠元素顺序：元素顺序是破坏性变更——
    // 把文本元素放到附件前面，已经装出去的旧客户端（判到文本元素就 return）
    // 会把附件整条丢掉，而它们不会因为我们改了源码就自动变好。
    // 元数据是加性的，不认识这个键的客户端直接忽略。
    if (captionAbove) metadata[QStringLiteral("captionAbove")] = true;
    // 引用同样走可选键：不认识它的客户端忽略，照常显示回复正文，不会出错。
    if (!quote.isEmpty()) metadata[QStringLiteral("quote")] = quote;
    return QString::fromUtf8(QJsonDocument(metadata).toJson(QJsonDocument::Compact));
}

void setMessageMetadata(QJsonObject& message,
                        RemoteIMMessageOrigin origin,
                        const QJsonObject& interaction = QJsonObject(),
                        bool captionAbove = false,
                        const QJsonObject& quote = QJsonObject()) {
    message[QString::fromLatin1(kCloudCustomDataKey)] =
        cloudCustomData(origin, interaction, captionAbove, quote);
}

void setMessageOriginWithQuote(QJsonObject& message,
                               RemoteIMMessageOrigin origin,
                               const RemoteIMQuote& quote) {
    setMessageMetadata(message, origin, QJsonObject(), false, quoteObject(quote));
}

void setMessageOrigin(QJsonObject& message, RemoteIMMessageOrigin origin) {
    setMessageMetadata(message, origin);
}

void setMessageOriginWithCaptionOrder(QJsonObject& message,
                                      RemoteIMMessageOrigin origin,
                                      bool captionAbove) {
    setMessageMetadata(message, origin, QJsonObject(), captionAbove);
}

ParsedCloudMetadata messageMetadata(const QJsonObject& message) {
    ParsedCloudMetadata parsed;
    const QString raw = message.value(QString::fromLatin1(kCloudCustomDataKey)).toString();
    if (raw.isEmpty()) return parsed;
    const QJsonDocument document = QJsonDocument::fromJson(raw.toUtf8());
    if (!document.isObject()) return parsed;
    const QJsonObject metadata = document.object();
    if (metadata.value(QStringLiteral("namespace")).toString() != QLatin1String(kMetadataNamespace)) {
        return parsed;
    }
    // 版本闸门只挡「比我们旧」的格式，不挡「比我们新」的。
    //
    // 原来这里是严格相等，后果是元数据格式**永远无法在不做 flag day 的情况下扩展**：
    // 只要有一端升了 version，另一端就把整块元数据丢掉——丢的不只是新字段，
    // 连 origin（human/machine，自动回环阻断靠它）和 captionAbove 一起丢。
    //
    // 改成 >= 之后，新增可选键对旧客户端是加性的：认识的读，不认识的忽略。
    // 真正破坏性的格式变更应该换 namespace，而不是继续靠版本号逼出 flag day。
    const int version = metadata.value(QStringLiteral("version")).toInt(-1);
    if (version < kMetadataVersion) {
        return parsed;
    }
    parsed.captionAbove = metadata.value(QStringLiteral("captionAbove")).toBool(false);
    const QString origin = metadata.value(QStringLiteral("origin")).toString();
    if (origin == QStringLiteral("human")) {
        parsed.origin = RemoteIMMessageOrigin::Human;
    } else if (origin == QStringLiteral("machine")) {
        parsed.origin = RemoteIMMessageOrigin::Machine;
    } else {
        return ParsedCloudMetadata{};
    }

    // 引用块。解析失败只丢 quote，不能连带丢 origin / captionAbove / interaction——
    // origin 是自动回环阻断的依据，为了一个装饰性的引用块把它丢掉是不划算的。
    // metadata 来自对端，一律当不可信输入：字段缺失、类型不对、超长都只是「没有引用」。
    if (metadata.contains(QStringLiteral("quote"))) {
        const QJsonObject quote = metadata.value(QStringLiteral("quote")).toObject();
        RemoteIMQuote parsedQuote;
        parsedQuote.msgId = quote.value(QStringLiteral("msgId")).toString().left(kQuoteFieldLimit);
        parsedQuote.senderId =
            quote.value(QStringLiteral("sender")).toString().left(kQuoteFieldLimit);
        parsedQuote.digest =
            quote.value(QStringLiteral("digest")).toString().left(kQuoteDigestLimit);
        parsedQuote.kind = quote.value(QStringLiteral("kind")).toString().left(kQuoteFieldLimit);
        // 没有摘要的引用块会渲染成空白，那比没有引用更难看。
        if (!parsedQuote.digest.isEmpty()) {
            parsed.quote = parsedQuote;
            parsed.hasQuote = true;
        }
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
    const QString suffix =
        QFileInfo(QUrl(url).path()).suffix().isEmpty() ? QStringLiteral("jpg") : QFileInfo(QUrl(url).path()).suffix();
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

TimSdkRemoteIMClient::TimSdkRemoteIMClient(std::unique_ptr<TimSdkApi> api, QObject* parent,
                                           QByteArray attachmentCaPem)
    : RemoteIMClient(parent), api_(std::move(api)), attachmentCaPem_(std::move(attachmentCaPem)),
      downloadsCanceled_(mai_download_cancel_create(), mai_download_cancel_destroy) {}

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
    mai_download_cancel_request(downloadsCanceled_.get());
    if (api_) {
        api_->setSelfInfoUpdatedCallback({});
        api_->removeReceiveMessageCallback();
        api_->uninit();
    }
}

// 作用：配置 SDK 并建立指定用户的 IM 连接。
void TimSdkRemoteIMClient::connectToService(int sdkAppId, const QString& userId, const QString& userSig,
                                            RemoteIMCompletion completion) {
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
            completion(false, QStringLiteral("未加载桌面 IM SDK：%1")
                                  .arg(api_ ? api_->diagnosticError() : QStringLiteral("SDK API 为空")));
        }
        return;
    }
    mai_download_cancel_request(downloadsCanceled_.get());
    downloadsCanceled_.reset(mai_download_cancel_create(), mai_download_cancel_destroy);
    currentUserId_ = userId.trimmed();
    sdkAppId_ = static_cast<quint64>(sdkAppId);
    const quint64 profileSession = ++profileSession_;

    qInfo().noquote()
        << QStringLiteral("[im] connect: sdkAppId=%1 user=%2").arg(sdkAppId).arg(currentUserId_);
    const int initResult = api_->init(static_cast<quint64>(sdkAppId), sdkConfigJson());
    if (initResult != 0) {
        qWarning().noquote() << QStringLiteral("[im] SDK init failed: %1").arg(initResult);
        if (completion) completion(false, QStringLiteral("IM SDK 初始化失败：%1").arg(initResult));
        return;
    }

    const QPointer<TimSdkRemoteIMClient> self(this);
    api_->addReceiveMessageCallback([self](const QString& jsonMessages) {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, jsonMessages] {
            if (self) self->handleIncomingMessages(jsonMessages);
        }, Qt::AutoConnection);
    });
    const auto account = currentAccount();
    api_->setSelfInfoUpdatedCallback([self, account, profileSession](const QString& payload) {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, account, profileSession, payload] {
            if (self && self->currentAccount() == account && self->connected_ &&
                self->profileSession_ == profileSession) {
                ++self->selfProfileRevision_;
                self->handleSelfProfilePayload(payload);
            }
        }, Qt::AutoConnection);
    });
    api_->login(
        userId.trimmed(), userSig.trimmed(),
        [this, completion = std::move(completion)](int code, const QString& description, const QString&) mutable {
            connected_ = code == 0;
            if (connected_) {
                qInfo().noquote() << QStringLiteral("[im] login ok: %1").arg(currentUserId_);
            } else {
                qWarning().noquote() << QStringLiteral("[im] login failed: code=%1 %2").arg(code).arg(description);
            }
            if (!connected_) {
                api_->setSelfInfoUpdatedCallback({});
                api_->removeReceiveMessageCallback();
            }
            if (connected_) {
                QJsonObject profile;
                profile[QStringLiteral("user_profile_item_add_permission")] = 2;
                api_->modifySelfProfile(compactJson(profile),
                                        [](int profileCode, const QString&, const QString&) {
                    if (profileCode != 0)
                        qWarning() << "[im] friend verification update failed:" << profileCode;
                });
                syncInitialData();
            }
            complete(std::move(completion), code, description);
        });
}

void TimSdkRemoteIMClient::disconnectFromService(RemoteIMCompletion completion) {
    mai_download_cancel_request(downloadsCanceled_.get());
    if (!api_) {
        if (completion) completion(true, QString());
        return;
    }
    api_->removeReceiveMessageCallback();
    api_->setSelfInfoUpdatedCallback({});
    ++profileSession_;
    api_->logout(
        [this, completion = std::move(completion)](int code, const QString& description, const QString&) mutable {
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

void TimSdkRemoteIMClient::requestFriend(const QString& userId, RemoteIMCompletion completion) {
    const QString cleanUserId = userId.trimmed();
    static const QRegularExpression accountId(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    if (!accountId.match(cleanUserId).hasMatch() || cleanUserId == currentUserId_) {
        if (completion) completion(false, QStringLiteral("好友账号无效或不能添加自己"));
        return;
    }
    if (!connected_ || !api_) {
        if (completion) completion(false, QStringLiteral("IM 未连接，无法发送好友申请"));
        return;
    }
    QJsonObject request;
    request[QStringLiteral("friendship_add_friend_param_identifier")] = cleanUserId;
    request[QStringLiteral("friendship_add_friend_param_friend_type")] = kFriendTypeBoth;
    request[QStringLiteral("friendship_add_friend_param_add_source")] = QStringLiteral("MaiChat");
    request[QStringLiteral("friendship_add_friend_param_add_wording")] =
        QStringLiteral("请求添加为好友");
    api_->addFriend(compactJson(request), [completion = std::move(completion)](
                                               int code, const QString& description,
                                               const QString&) mutable {
        if (code == 30539) {
            if (completion) completion(true, {});
            return;
        }
        complete(std::move(completion), code, description);
    });
}

void TimSdkRemoteIMClient::refreshFriendList() {
    if (connected_ && api_) fetchFriendList();
}

void TimSdkRemoteIMClient::joinedGroups(RemoteIMGroupsCompletion completion) {
    const QPointer<TimSdkRemoteIMClient> self(this);
    const auto account = currentAccount();
    api_->getJoinedGroupList([self, account, completion = std::move(completion)](
        int code, const QString& description, const QString& payload) mutable {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, account, code, description, payload,
                                         completion = std::move(completion)]() mutable {
            if (!self || self->currentAccount() != account) return;
            QList<RemoteIMGroup> groups;
            const QJsonDocument document = QJsonDocument::fromJson(payload.toUtf8());
            if (code == 0 && !document.isArray()) {
                if (completion) completion(false, QStringLiteral("群列表响应格式错误"), {});
                return;
            }
            if (code == 0) {
                const QJsonArray raw = document.array();
                for (const QJsonValue& value : raw) {
                    const QJsonObject item = value.toObject();
                    const QJsonObject base = item.value(QStringLiteral("group_detail_info_group_base_info")).toObject();
                    const QJsonObject info = base.isEmpty() ? item : base;
                    const int type = info.value(QStringLiteral("group_base_info_group_type")).toInt(
                        info.value(QStringLiteral("group_detail_info_group_type")).toInt(-1));
                    if (type != kGroupTypeWork) continue;
                    const QString id = firstNonEmpty(info, {
                        QStringLiteral("group_base_info_group_id"),
                        QStringLiteral("group_detail_info_group_id")});
                    if (id.isEmpty()) continue;
                    const QString name = firstNonEmpty(info, {
                        QStringLiteral("group_base_info_group_name"),
                        QStringLiteral("group_detail_info_group_name")});
                    const int unread = info.value(QStringLiteral("group_base_info_self_info"))
                                           .toObject().value(QStringLiteral("group_self_info_unread_num"))
                                           .toInt(-1);
                    groups.append({id, name.isEmpty() ? id : name,
                                   item.value(QStringLiteral("group_detail_info_member_num")).toInt(),
                                   false, unread});
                }
            }
            if (code != 0) {
                if (completion) completion(false, description, {});
                return;
            }
            if (groups.isEmpty()) {
                emit self->groupsReceived(groups);
                if (completion) completion(true, {}, groups);
                return;
            }
            QJsonArray ids;
            for (const RemoteIMGroup& group : groups) ids.append(group.groupId);
            const QString request = QString::fromUtf8(QJsonDocument(ids).toJson(QJsonDocument::Compact));
            self->api_->getGroupInfoList(request,
                [self, account, groups, completion = std::move(completion)](
                    int detailCode, const QString&, const QString& detailPayload) mutable {
                if (!self) return;
                QMetaObject::invokeMethod(self, [self, account, groups, detailCode, detailPayload,
                                                 completion = std::move(completion)]() mutable {
                    if (!self || self->currentAccount() != account) return;
                    if (detailCode == 0) {
                        const QJsonDocument detail = QJsonDocument::fromJson(detailPayload.toUtf8());
                        if (detail.isArray()) {
                            for (const QJsonValue& value : detail.array()) {
                                const QJsonObject info = value.toObject();
                                const QString id = info.value(QStringLiteral("group_detail_info_group_id"))
                                                       .toString().trimmed();
                                for (RemoteIMGroup& group : groups) {
                                    if (group.groupId != id) continue;
                                    const QString name = info.value(QStringLiteral("group_detail_info_group_name"))
                                                             .toString().trimmed();
                                    if (!name.isEmpty()) group.name = name;
                                    group.memberCount = info.value(QStringLiteral("group_detail_info_member_num"))
                                                            .toInt(group.memberCount);
                                }
                            }
                        }
                    }
                    emit self->groupsReceived(groups);
                    if (completion) completion(true, {}, groups);
                }, Qt::AutoConnection);
            });
        }, Qt::AutoConnection);
    });
}

void TimSdkRemoteIMClient::createGroup(const QString& name, const QStringList& memberIds,
                                        RemoteIMGroupCompletion completion) {
    const QString cleanName = name.trimmed();
    QStringList members;
    for (const QString& id : memberIds) {
        const QString clean = id.trimmed();
        if (!clean.isEmpty() && !members.contains(clean)) members.append(clean);
    }
    if (cleanName.isEmpty() || cleanName.toUtf8().size() > 100 || members.isEmpty() ||
        members.size() > 19 || members.contains(currentUserId_)) {
        if (completion) completion(false, QStringLiteral("群名称需在 100 字节内，且须选择 1 至 19 位好友"), {});
        return;
    }
    QJsonArray memberArray;
    for (const QString& id : members) {
        memberArray.append(QJsonObject{{QStringLiteral("group_member_info_identifier"), id}});
    }
    QJsonObject request;
    request[QStringLiteral("create_group_param_group_type")] = kGroupTypeWork;
    request[QStringLiteral("create_group_param_group_name")] = cleanName;
    request[QStringLiteral("create_group_param_group_member_array")] = memberArray;
    const QPointer<TimSdkRemoteIMClient> self(this);
    const auto account = currentAccount();
    api_->createGroup(compactJson(request), [self, account, cleanName, memberCount = members.size() + 1,
                                              completion = std::move(completion)](
        int code, const QString& description, const QString& payload) mutable {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, account, cleanName, memberCount, code, description, payload,
                                         completion = std::move(completion)]() mutable {
            if (!self || self->currentAccount() != account) return;
            if (code != 0) {
                if (completion) completion(false, description, {});
                return;
            }
            const QJsonDocument doc = QJsonDocument::fromJson(payload.toUtf8());
            const QString id = doc.object().value(QStringLiteral("create_group_result_groupid")).toString().trimmed();
            if (id.isEmpty()) {
                if (completion) completion(false, QStringLiteral("群已创建，但 SDK 未返回群 ID；请刷新群列表"), {});
                self->joinedGroups({});
                return;
            }
            const RemoteIMGroup group{id, cleanName, memberCount};
            QJsonObject activationElem;
            activationElem[QStringLiteral("elem_type")] = kElemCustom;
            activationElem[QStringLiteral("custom_elem_data")] = QString::fromLatin1(kGroupActivation);
            QJsonObject activation;
            activation[QStringLiteral("message_elem_array")] = QJsonArray{activationElem};
            activation[QStringLiteral("message_is_excluded_from_unread_count")] = true;
            activation[QStringLiteral("message_excluded_from_last_message")] = true;
            self->api_->sendMessage(id, kConversationTypeGroup, compactJson(activation),
                [self, account, group, completion = std::move(completion)](
                    int activationCode, const QString& activationError, const QString&) mutable {
                if (!self) return;
                QMetaObject::invokeMethod(self, [self, account, group, activationCode, activationError,
                                                 completion = std::move(completion)]() mutable {
                    if (!self || self->currentAccount() != account) return;
                    if (activationCode != 0) {
                        if (completion) completion(false,
                            QStringLiteral("群已创建，但激活失败：%1；请打开该群发送文字重试")
                                .arg(activationError), group);
                        return;
                    }
                    emit self->groupsReceived({group});
                    if (completion) completion(true, {}, group);
                }, Qt::AutoConnection);
            });
        }, Qt::AutoConnection);
    });
}

void TimSdkRemoteIMClient::sendGroupText(const QString& groupId, const QString& text,
                                          RemoteIMSendCompletion completion) {
    const QString group = groupId.trimmed();
    const QString cleanText = text.trimmed();
    if (group.isEmpty() || cleanText.isEmpty()) {
        if (completion) completion(false, QStringLiteral("群 ID 和消息不能为空"), {});
        return;
    }
    QJsonObject elem{{QStringLiteral("elem_type"), kElemText},
                     {QStringLiteral("text_elem_content"), cleanText}};
    QJsonObject message{{QStringLiteral("message_elem_array"), QJsonArray{elem}}};
    api_->sendMessage(group, kConversationTypeGroup, compactJson(message),
        [completion = std::move(completion)](int code, const QString& description,
                                             const QString& payload) mutable {
        if (completion) completion(code == 0, code == 0 ? QString() : description,
                                   code == 0 ? sentMessageReceipt(payload) : RemoteIMSendReceipt{});
    });
}

void TimSdkRemoteIMClient::groupHistory(const QString& groupId, int count, qint64 beforeSequence,
                                         RemoteIMGroupHistoryCompletion completion) {
    const QString group = groupId.trimmed();
    if (group.isEmpty() || count <= 0) {
        if (completion) completion(false, QStringLiteral("群 ID 或分页数无效"), {});
        return;
    }
    const int pageSize = qMin(count, 20);
    QJsonObject request;
    request[QStringLiteral("msg_getmsglist_param_count")] = pageSize;
    request[QStringLiteral("msg_getmsglist_param_is_ramble")] = true;
    request[QStringLiteral("msg_getmsglist_param_is_forward")] = false;
    if (beforeSequence > 0) request[QStringLiteral("msg_getmsglist_param_last_msg_seq")] = beforeSequence;
    const QPointer<TimSdkRemoteIMClient> self(this);
    const auto account = currentAccount();
    api_->getMessageList(group, kConversationTypeGroup, compactJson(request),
        [self, account, group, pageSize, completion = std::move(completion)](
            int code, const QString& description, const QString& payload) mutable {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, account, group, pageSize, code, description, payload,
                                         completion = std::move(completion)]() mutable {
            if (!self || self->currentAccount() != account) return;
            RemoteIMGroupHistoryPage page;
            const QJsonDocument document = QJsonDocument::fromJson(payload.toUtf8());
            if (code == 0 && !document.isArray()) {
                if (completion) completion(false, QStringLiteral("群历史响应格式错误"), {});
                return;
            }
            if (code == 0) {
                const QJsonArray raw = document.array();
                qint64 oldest = 0;
                for (const QJsonValue& value : raw) {
                    const QJsonObject object = value.toObject();
                    const qint64 sequence = static_cast<qint64>(object.value(QStringLiteral("message_seq")).toDouble());
                    if (sequence > 0 && (oldest == 0 || sequence < oldest)) oldest = sequence;
                    const auto text = groupTextMessage(object, group, self->currentUserId_);
                    if (!text.id.isEmpty()) page.messages.prepend(text);
                }
                page.hasMore = raw.size() >= pageSize && oldest > 1;
                page.nextSequence = page.hasMore ? oldest - 1 : 0;
            }
            if (completion) completion(code == 0, code == 0 ? QString() : description, page);
        }, Qt::AutoConnection);
    });
}

void TimSdkRemoteIMClient::markGroupRead(const QString& groupId,
                                         RemoteIMCompletion completion) {
    const QString id = groupId.trimmed();
    if (id.isEmpty()) {
        if (completion) completion(false, QStringLiteral("群 ID 不能为空"));
        return;
    }
    const QPointer<TimSdkRemoteIMClient> self(this);
    const auto account = currentAccount();
    api_->cleanConversationUnread(QStringLiteral("group_") + id,
        [self, account, completion = std::move(completion)](
            int code, const QString& description, const QString&) mutable {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, account, code, description,
                                         completion = std::move(completion)]() mutable {
            if (!self || self->currentAccount() != account) return;
            if (completion) completion(code == 0, code == 0 ? QString() : description);
        }, Qt::AutoConnection);
    });
}

void TimSdkRemoteIMClient::friendRequests(RemoteIMFriendRequestsCompletion completion) {
    if (!connected_ || !api_) {
        if (completion) completion(false, QStringLiteral("IM 未连接"), {});
        return;
    }
    QJsonObject request;
    request[QStringLiteral("friendship_get_pendency_list_param_type")] = 0;
    request[QStringLiteral("friendship_get_pendency_list_param_start_seq")] = 0;
    api_->getFriendApplications(compactJson(request),
                                [completion = std::move(completion)](
                                    int code, const QString& description,
                                    const QString& payload) mutable {
        if (!completion) return;
        if (code != 0) { completion(false, description, {}); return; }
        const QJsonDocument document = QJsonDocument::fromJson(payload.toUtf8());
        if (!document.isObject()) {
            completion(false, QStringLiteral("好友申请数据无效"), {});
            return;
        }
        QList<RemoteIMFriendRequest> requests;
        const QJsonArray entries = document.object()
            .value(QStringLiteral("pendency_page_pendency_info_array")).toArray();
        for (const QJsonValue& value : entries) {
            const QJsonObject item = value.toObject();
            if (item.value(QStringLiteral("friend_add_pendency_info_type")).toInt(-1) != 0) continue;
            const QString userId = item.value(QStringLiteral("friend_add_pendency_info_identifier"))
                                       .toString().trimmed();
            if (userId.isEmpty()) continue;
            const QString nickname = item.value(QStringLiteral("friend_add_pendency_info_nick_name"))
                                         .toString().trimmed();
            requests.append(RemoteIMFriendRequest{
                userId, nickname.isEmpty() ? userId : nickname,
                item.value(QStringLiteral("friend_add_pendency_info_face_url")).toString(),
                item.value(QStringLiteral("friend_add_pendency_info_add_wording")).toString()
            });
        }
        completion(true, {}, requests);
    });
}

void TimSdkRemoteIMClient::decideFriendRequest(const QString& userId, bool accept,
                                               RemoteIMCompletion completion) {
    if (!connected_ || !api_) {
        if (completion) completion(false, QStringLiteral("IM 未连接"));
        return;
    }
    QJsonObject request;
    request[QStringLiteral("friend_response_identifier")] = userId.trimmed();
    request[QStringLiteral("friend_response_action")] = accept ? 1 : 2;
    api_->decideFriendApplication(compactJson(request),
                                  [completion = std::move(completion)](
                                      int code, const QString& description,
                                      const QString&) mutable {
        complete(std::move(completion), code, description);
    });
}

void TimSdkRemoteIMClient::sendText(const QString& peerId, const QString& text, RemoteIMSendCompletion completion) {
    sendTextWithOrigin(peerId, text, RemoteIMMessageOrigin::Human, std::move(completion));
}

void TimSdkRemoteIMClient::sendTextWithQuote(const QString& peerId, const QString& text,
                                             const RemoteIMQuote& quote, bool hasQuote,
                                             RemoteIMSendCompletion completion) {
    sendTextWithOrigin(peerId, text, RemoteIMMessageOrigin::Human, std::move(completion),
                       QJsonObject(), quote, hasQuote);
}

void TimSdkRemoteIMClient::sendMachineText(const QString& peerId, const QString& text,
                                           RemoteIMSendCompletion completion) {
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
                                              const QJsonObject& interaction,
                                              const RemoteIMQuote& quote,
                                              bool hasQuote) {
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
    // 引用只在确实有摘要时才写。没有摘要的引用块在对端会渲染成空白，
    // 那比不带引用更糟。
    setMessageMetadata(message, origin, interaction, false,
                       (hasQuote && !quote.digest.isEmpty()) ? quoteObject(quote) : QJsonObject());
    api_->sendMessage(
        cleanPeerId, kConversationTypeC2C, compactJson(message),
        [completion = std::move(completion)](int code, const QString& description, const QString& jsonPayload) mutable {
            if (!completion) return;
            const bool ok = code == 0;
            if (!ok) {
                qWarning().noquote() << QStringLiteral("[im] send failed: code=%1 %2").arg(code).arg(description);
            }
            RemoteIMSendReceipt receipt = ok ? sentMessageReceipt(jsonPayload) : RemoteIMSendReceipt{};
            completion(ok,
                       ok ? QString()
                          : (description.isEmpty() ? QStringLiteral("IM SDK 操作失败：%1").arg(code) : description),
                       receipt);
        });
}

void TimSdkRemoteIMClient::sendActivity(const QString& peerId,
                                        const RemoteIMActivitySignal& signal,
                                        RemoteIMCompletion completion) {
    const QString cleanPeerId = peerId.trimmed();
    static const QRegularExpression idPattern(QStringLiteral("^[A-Za-z0-9._:-]{1,192}$"));
    if (cleanPeerId.isEmpty() || !idPattern.match(signal.activityId).hasMatch()) {
        if (completion) completion(false, QStringLiteral("活动状态缺少接收人或有效 ID"));
        return;
    }
    QJsonObject elem;
    elem[QStringLiteral("elem_type")] = kElemCustom;
    elem[QStringLiteral("custom_elem_data")] = activityData(signal);
    elem[QStringLiteral("custom_elem_desc")] = QString();
    elem[QStringLiteral("custom_elem_ext")] = QString();
    QJsonObject message;
    message[QStringLiteral("message_elem_array")] = QJsonArray{elem};
    message[QStringLiteral("message_is_online_msg")] = true;
    message[QStringLiteral("message_is_excluded_from_unread_count")] = true;
    message[QStringLiteral("message_excluded_from_last_message")] = true;
    api_->sendMessage(cleanPeerId, kConversationTypeC2C, compactJson(message),
                      [completion = std::move(completion)](int code,
                                                           const QString& description,
                                                           const QString&) mutable {
        if (completion) {
            completion(code == 0,
                       code == 0 ? QString()
                                 : (description.isEmpty()
                                        ? QStringLiteral("IM SDK 操作失败：%1").arg(code)
                                        : description));
        }
    });
}

void TimSdkRemoteIMClient::sendImage(const QString& peerId, const QString& localPath,
                                     RemoteIMSendCompletion completion) {
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
    api_->sendMessage(
        cleanPeerId, kConversationTypeC2C, compactJson(message),
        [completion = std::move(completion)](int code, const QString& description, const QString& jsonPayload) mutable {
            if (!completion) return;
            const bool ok = code == 0;
            if (!ok) {
                qWarning().noquote() << QStringLiteral("[im] send failed: code=%1 %2").arg(code).arg(description);
            }
            RemoteIMSendReceipt receipt = ok ? sentMessageReceipt(jsonPayload) : RemoteIMSendReceipt{};
            completion(ok,
                       ok ? QString()
                          : (description.isEmpty() ? QStringLiteral("IM SDK 操作失败：%1").arg(code) : description),
                       receipt);
        });
}

void TimSdkRemoteIMClient::sendFile(const QString& peerId, const QString& localPath, const QString& fileName,
                                    RemoteIMSendCompletion completion) {
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
    api_->sendMessage(
        cleanPeerId, kConversationTypeC2C, compactJson(message),
        [completion = std::move(completion)](int code, const QString& description, const QString& jsonPayload) mutable {
            if (!completion) return;
            const bool ok = code == 0;
            if (!ok) {
                qWarning().noquote() << QStringLiteral("[im] send failed: code=%1 %2").arg(code).arg(description);
            }
            RemoteIMSendReceipt receipt = ok ? sentMessageReceipt(jsonPayload) : RemoteIMSendReceipt{};
            completion(ok,
                       ok ? QString()
                          : (description.isEmpty() ? QStringLiteral("IM SDK 操作失败：%1").arg(code) : description),
                       receipt);
        });
}

void TimSdkRemoteIMClient::sendImageWithText(const QString& peerId, const QString& imagePath, const QString& text,
                                             bool captionAbove, RemoteIMSendCompletion completion) {
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
    {
        QJsonObject imageElem;
        imageElem[QStringLiteral("elem_type")] = kElemImage;
        imageElem[QStringLiteral("image_elem_orig_path")] = cleanPath;
        imageElem[QStringLiteral("image_elem_level")] = kImageLevelOriginal;
        elems.append(imageElem);
    }
    appendCaption();
    QJsonObject message;
    message[QStringLiteral("message_elem_array")] = elems;
    setMessageOriginWithCaptionOrder(message, RemoteIMMessageOrigin::Human, captionAbove);
    api_->sendMessage(
        cleanPeerId, kConversationTypeC2C, compactJson(message),
        [completion = std::move(completion)](int code, const QString& description, const QString& jsonPayload) mutable {
            if (!completion) return;
            const bool ok = code == 0;
            if (!ok) {
                qWarning().noquote() << QStringLiteral("[im] send failed: code=%1 %2").arg(code).arg(description);
            }
            RemoteIMSendReceipt receipt = ok ? sentMessageReceipt(jsonPayload) : RemoteIMSendReceipt{};
            completion(ok,
                       ok ? QString()
                          : (description.isEmpty() ? QStringLiteral("IM SDK 操作失败：%1").arg(code) : description),
                       receipt);
        });
}

void TimSdkRemoteIMClient::sendFileWithText(const QString& peerId, const QString& localPath, const QString& fileName,
                                            const QString& text, bool captionAbove, RemoteIMSendCompletion completion) {
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
    {
        QJsonObject fileElem;
        fileElem[QStringLiteral("elem_type")] = kElemFile;
        fileElem[QStringLiteral("file_elem_file_path")] = cleanPath;
        fileElem[QStringLiteral("file_elem_file_name")] = displayName;
        elems.append(fileElem);
    }
    appendCaption();
    QJsonObject message;
    message[QStringLiteral("message_elem_array")] = elems;
    setMessageOriginWithCaptionOrder(message, RemoteIMMessageOrigin::Human, captionAbove);
    api_->sendMessage(
        cleanPeerId, kConversationTypeC2C, compactJson(message),
        [completion = std::move(completion)](int code, const QString& description, const QString& jsonPayload) mutable {
            if (!completion) return;
            const bool ok = code == 0;
            if (!ok) {
                qWarning().noquote() << QStringLiteral("[im] send failed: code=%1 %2").arg(code).arg(description);
            }
            RemoteIMSendReceipt receipt = ok ? sentMessageReceipt(jsonPayload) : RemoteIMSendReceipt{};
            completion(ok,
                       ok ? QString()
                          : (description.isEmpty() ? QStringLiteral("IM SDK 操作失败：%1").arg(code) : description),
                       receipt);
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

void TimSdkRemoteIMClient::sendVideo(const QString& peerId, const RemoteIMVideoPayload& video,
                                     RemoteIMSendCompletion completion) {
    sendVideoWithText(peerId, video, QString(), false, std::move(completion));
}

void TimSdkRemoteIMClient::sendVideoWithText(const QString& peerId, const RemoteIMVideoPayload& video,
                                             const QString& text, bool captionAbove,
                                             RemoteIMSendCompletion completion) {
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
    elems.append(videoElemJson(video));
    appendCaption();
    QJsonObject message;
    message[QStringLiteral("message_elem_array")] = elems;
    setMessageOriginWithCaptionOrder(message, RemoteIMMessageOrigin::Human, captionAbove);
    api_->sendMessage(
        cleanPeerId, kConversationTypeC2C, compactJson(message),
        [completion = std::move(completion)](int code, const QString& description, const QString& jsonPayload) mutable {
            if (!completion) return;
            const bool ok = code == 0;
            if (!ok) {
                qWarning().noquote() << QStringLiteral("[im] send failed: code=%1 %2").arg(code).arg(description);
            }
            RemoteIMSendReceipt receipt = ok ? sentMessageReceipt(jsonPayload) : RemoteIMSendReceipt{};
            completion(ok,
                       ok ? QString()
                          : (description.isEmpty() ? QStringLiteral("IM SDK 操作失败：%1").arg(code) : description),
                       receipt);
        });
}

void TimSdkRemoteIMClient::sendVoice(const QString&, const QString&, int, RemoteIMCompletion completion) {
    if (completion) completion(false, QStringLiteral("桌面端语音消息还未接入原生录音与 SDK 声音元素"));
}

void TimSdkRemoteIMClient::syncInitialData() {
    QJsonObject request;
    request[QStringLiteral("friendship_getprofilelist_param_identifier_array")] = QJsonArray{currentUserId_};
    request[QStringLiteral("friendship_getprofilelist_param_force_update")] = true;
    const QPointer<TimSdkRemoteIMClient> self(this);
    const auto account = currentAccount();
    const quint64 session = profileSession_;
    const quint64 revision = selfProfileRevision_;
    api_->getUserProfiles(compactJson(request), [self, account, session, revision](int code, const QString&,
                                                                                   const QString& payload) {
        if (code != 0 || !self) return;
        QMetaObject::invokeMethod(self, [self, account, session, revision, payload] {
            if (self && self->connected_ && self->currentAccount() == account &&
                self->profileSession_ == session && self->selfProfileRevision_ == revision)
                self->handleSelfProfilePayload(payload);
        }, Qt::AutoConnection);
    });
    fetchFriendList();
    fetchConversationList();
}

void TimSdkRemoteIMClient::handleSelfProfilePayload(const QString& payload) {
    const QJsonDocument document = QJsonDocument::fromJson(payload.toUtf8());
    QJsonArray profiles;
    if (document.isArray()) profiles = document.array();
    else if (document.isObject()) profiles.append(document.object());
    for (const QJsonValue& value : profiles) {
        const QJsonObject profile = value.toObject();
        const QString userId = profile.value(QStringLiteral("user_profile_identifier")).toString();
        if (userId != currentUserId_) continue;
        const QString name = profile.value(QStringLiteral("user_profile_nick_name")).toString();
        qInfo() << "[im] self profile updated; has avatar:"
                << !profile.value(QStringLiteral("user_profile_face_url")).toString().isEmpty();
        emit selfProfileReceived(userId, name.isEmpty() ? userId : name,
                                 profile.value(QStringLiteral("user_profile_face_url")).toString());
    }
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
    const QPointer<TimSdkRemoteIMClient> self(this);
    const auto account = currentAccount();
    api_->getMessageList(conversationId, conversationType, compactJson(request),
                         [self, account, conversationId](int code, const QString& description,
                                                         const QString& jsonPayload) {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, account, conversationId, code, description, jsonPayload] {
            if (!self || self->currentAccount() != account) return;
            if (code != 0) {
                qWarning().noquote()
                    << QStringLiteral("[im] fetch history failed: conv=%1 code=%2 %3")
                           .arg(conversationId).arg(code).arg(description);
                return;
            }
            if (!jsonPayload.trimmed().isEmpty()) self->handleHistoryMessagesPayload(jsonPayload);
        }, Qt::AutoConnection);
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
        } else if (conversationType == kConversationTypeGroup) {
            emit groupUnreadReceived(conversationId,
                qMax(0, conversation.value(QStringLiteral("conv_unread_num")).toInt()));
            const RemoteIMGroupMessage preview = groupTextMessage(
                conversation.value(QStringLiteral("conv_last_msg")).toObject(),
                conversationId, currentUserId_);
            if (!preview.id.isEmpty()) emit groupMessagesReceived({preview}, false);
        }
    }
    qInfo().noquote()
        << QStringLiteral("[im] conversation list: %1 raw -> %2 c2c")
               .arg(conversations.size()).arg(contacts.size());
    if (!contacts.isEmpty()) emit contactsReceived(contacts);
}

RemoteIMGroupMessage TimSdkRemoteIMClient::groupTextMessage(const QJsonObject& message,
                                                             const QString& groupId,
                                                             const QString& ownerUserId) {
    RemoteIMGroupMessage result;
    const QString actualGroup = message.value(QStringLiteral("message_conv_id")).toString().trimmed();
    if (!actualGroup.isEmpty() && actualGroup != groupId) return result;
    const QJsonArray elements = message.value(QStringLiteral("message_elem_array")).toArray();
    for (int index = 0; index < elements.size(); ++index) {
        const QJsonObject elem = elements.at(index).toObject();
        if (elem.value(QStringLiteral("elem_type")).toInt(-1) != kElemText) continue;
        const QString text = elem.value(QStringLiteral("text_elem_content")).toString();
        if (text.trimmed().isEmpty()) continue;
        const QString id = sdkElemMessageId(sdkMessageId(message), index);
        if (id.isEmpty()) continue;
        result.id = id;
        result.groupId = groupId;
        result.senderId = message.value(QStringLiteral("message_sender")).toString().trimmed();
        if (result.senderId.isEmpty() && message.value(QStringLiteral("message_is_from_self")).toBool())
            result.senderId = ownerUserId;
        result.text = text;
        result.createdAtMillis = messageTimeMillis(message);
        result.sequence = static_cast<qint64>(message.value(QStringLiteral("message_seq")).toDouble());
        result.fromSelf = result.senderId == ownerUserId ||
            message.value(QStringLiteral("message_is_from_self")).toBool();
        return result;
    }
    return result;
}

// 作用：解析 SDK 历史消息载荷并合并到对应会话。
// 先校验外部输入，再更新本地状态；失败路径不能覆盖已有有效结果。
void TimSdkRemoteIMClient::handleHistoryMessagesPayload(const QString& jsonPayload) {
    QList<RemoteIMMessage> messages;
    const QJsonArray sdkMessages = arrayPayload(jsonPayload);
    // is_forward=false 的历史结果按“新到旧”返回；反向遍历后为同一秒消息分配
    // 递增毫秒，保留 SDK 已确定的会话顺序。
    for (int messageIndex = sdkMessages.size() - 1; messageIndex >= 0; --messageIndex) {
        const QJsonObject sdkMessage = sdkMessages.at(messageIndex).toObject();
        const bool isFromSelf = sdkMessage.value(QStringLiteral("message_is_from_self")).toBool(false);
        const QString peerId =
            isFromSelf
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
        // 排版来自元数据，理由同实时路径。
        const bool attachmentCaptionAbove = hasAttachment && metadata.captionAbove;

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
                const qint64 sizeBytes =
                    static_cast<qint64>(elem.value(QStringLiteral("video_elem_video_size")).toDouble(0));
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
                const qint64 sizeBytes =
                    static_cast<qint64>(elem.value(QStringLiteral("file_elem_file_size")).toDouble(0));
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
                        message.file =
                            RemoteIMFileAttachment{cachedPath, displayName, mimeTypeForFileName(fileName), sizeBytes};
                        messages.append(message);
                    } else {
                        message.file =
                            RemoteIMFileAttachment{QString(), displayName, mimeTypeForFileName(fileName), sizeBytes};
                        handleIncomingFileUrl(message, url, /*live=*/false);
                    }
                    continue;
                }
                if (!localPath.isEmpty()) {
                    RemoteIMMessage message = makeBase(elemIndex);
                    message.hasFile = true;
                    message.text = captionText;
                    if (!attachmentCaption.isEmpty() && !captionConsumed) captionConsumed = true;
                    message.file =
                        RemoteIMFileAttachment{localPath, displayName, mimeTypeForFileName(fileName), sizeBytes};
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

// 作用：解析入站消息类型、元数据和附件后更新会话。
// 先校验外部输入，再更新本地状态；失败路径不能覆盖已有有效结果。
void TimSdkRemoteIMClient::handleIncomingMessage(const QJsonObject& message) {
    if (message.value(QStringLiteral("message_conv_type")).toInt() == kConversationTypeGroup) {
        const QString groupId = message.value(QStringLiteral("message_conv_id")).toString().trimmed();
        if (groupId.isEmpty()) return;
        bool refreshMembership = false;
        for (const QJsonValue& value : message.value(QStringLiteral("message_elem_array")).toArray()) {
            const QJsonObject elem = value.toObject();
            const int type = elem.value(QStringLiteral("elem_type")).toInt(-1);
            if (type == kElemGroupTips ||
                (type == kElemCustom && elem.value(QStringLiteral("custom_elem_data")).toString().toUtf8() ==
                    QByteArray(kGroupActivation))) refreshMembership = true;
        }
        if (refreshMembership) {
            emit groupMembershipChanged();
            joinedGroups({});
        }
        const RemoteIMGroupMessage text = groupTextMessage(message, groupId, currentUserId_);
        if (!text.id.isEmpty()) emit groupMessagesReceived({text}, true);
        return;
    }
    // 这两个 return 以前是完全静默的：消息在这里消失，日志里一个字都没有。
    if (message.value(QStringLiteral("message_is_from_self")).toBool(false)) return;
    const QString fromUserId =
        firstNonEmpty(message, {QStringLiteral("message_sender"), QStringLiteral("message_conv_id")});
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
    for (const QJsonValue& value : elems) {
        const QJsonObject elem = value.toObject();
        if (elem.value(QStringLiteral("elem_type")).toInt(-1) != kElemCustom) continue;
        RemoteIMActivitySignal signal;
        if (parseActivityData(elem.value(QStringLiteral("custom_elem_data")).toString(), &signal)) {
            emit activityReceived(fromUserId, signal);
            return;
        }
    }

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
    // 排版来自元数据，不从元素顺序推：元素顺序永远是「附件在前」，
    // 那是为了不让旧客户端丢附件。
    const bool attachmentCaptionAbove = hasAttachment && metadata.captionAbove;

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
            const qint64 sizeBytes =
                static_cast<qint64>(elem.value(QStringLiteral("image_elem_orig_pic_size")).toDouble(0));
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
            const qint64 sizeBytes =
                static_cast<qint64>(elem.value(QStringLiteral("video_elem_video_size")).toDouble(0));
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
                fileMessage.file =
                    RemoteIMFileAttachment{localPath, displayName, mimeTypeForFileName(displayName), sizeBytes};
                received.append(fileMessage);
                continue;
            }
            const QString url = elem.value(QStringLiteral("file_elem_url")).toString().trimmed();
            if (!url.isEmpty()) {
                fileMessage.file =
                    RemoteIMFileAttachment{QString(), displayName, mimeTypeForFileName(displayName), sizeBytes};
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

void TimSdkRemoteIMClient::fetchAttachment(
    const QString& url, const QString& targetPath, qint64 maximumBytes,
    std::function<void(DesktopCurlAttachmentResult)> completion) {
    if (QThread::currentThread() != thread()) {
        const QPointer<TimSdkRemoteIMClient> self(this);
        if (!self) return;
        QMetaObject::invokeMethod(self.data(),
            [self, url, targetPath, maximumBytes, completion = std::move(completion)]() mutable {
                if (self) self->fetchAttachment(url, targetPath, maximumBytes, std::move(completion));
            }, Qt::QueuedConnection);
        return;
    }
    const QUrl location(url);
    QNetworkProxy proxy(QNetworkProxy::DefaultProxy);
    if (location.host() == QStringLiteral("localhost") ||
        location.host() == QStringLiteral("127.0.0.1")) {
        proxy = QNetworkProxy(QNetworkProxy::NoProxy);
    } else {
        const auto candidates = QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(location));
        for (const QNetworkProxy& candidate : candidates) {
            if (candidate.type() == QNetworkProxy::NoProxy ||
                candidate.type() == QNetworkProxy::HttpProxy ||
                candidate.type() == QNetworkProxy::Socks5Proxy) {
                proxy = candidate;
                break;
            }
        }
    }
    const auto canceled = downloadsCanceled_;
    const QByteArray caPem = attachmentCaPem_;
    auto* watcher = new QFutureWatcher<DesktopCurlAttachmentResult>(this);
    connect(watcher, &QFutureWatcher<DesktopCurlAttachmentResult>::finished, this,
            [watcher, completion = std::move(completion)]() mutable {
        DesktopCurlAttachmentResult result = watcher->result();
        watcher->deleteLater();
        completion(result);
    });
    watcher->setFuture(QtConcurrent::run(
        [location, targetPath, maximumBytes, caPem, proxy, canceled] {
            return DesktopCurlAttachmentDownload::perform(
                location, targetPath, maximumBytes, caPem, proxy, canceled);
        }));
}

void TimSdkRemoteIMClient::handleIncomingImageUrl(RemoteIMMessage message, const QString& url, bool live) {
    const QString targetPath = cacheImagePathForUrl(url);
    message.image.localPath = targetPath;
    if (message.text.trimmed().isEmpty())
        message.text = QStringLiteral("[图片消息] ") + QFileInfo(targetPath).fileName();
    const auto origin = currentAccount();
    if (QFileInfo(targetPath).isFile() && QFileInfo(targetPath).isReadable()) {
        emitReceivedMessages({message}, live, origin);
        return;
    }
    fetchAttachment(url, targetPath, kMaximumImageBytes,
                    [this, message, live, origin](DesktopCurlAttachmentResult result) {
        if (currentAccount() != origin || !connected_) return;
        if (!result.ok()) {
            qWarning() << "[im] image download failed:" << int(result.status) << result.transferCode;
            return;
        }
        emitReceivedMessages({message}, live, origin);
    });
}

// 作用：将 SDK 文件地址与原消息附件绑定并处理下载状态。
void TimSdkRemoteIMClient::handleIncomingFileUrl(RemoteIMMessage message, const QString& url, bool live) {
    // 账号标签在**发起时**捕获，跟着这次下载走。回调里再取「当前账号」是错的：
    // 下载完成时用户可能已经换号，那样旧账号的附件会被算到新账号头上。
    const RemoteDiagnostics::AccountTag origin = currentAccount();
    recordAttachmentPhase(origin, message, RemoteDiagnostics::AttachmentPhase::MetadataReceived);

    const QString targetPath = cacheFilePathForUrl(url, message.file.fileName);
    message.file.localPath = targetPath;
    if (QFileInfo(targetPath).isFile() && QFileInfo(targetPath).isReadable()) {
        recordAttachmentPhase(origin, message, RemoteDiagnostics::AttachmentPhase::CacheHit);
        // 缓存命中也要走同一套判定：真的发出去才算交付。
        recordAttachmentPhase(origin, message,
                              emitReceivedMessages({message}, live, origin)
                                  ? RemoteDiagnostics::AttachmentPhase::Delivered
                                  : RemoteDiagnostics::AttachmentPhase::DroppedForAccountSwitch);
        return;
    }

    recordAttachmentPhase(origin, message, RemoteDiagnostics::AttachmentPhase::DownloadStarted);
    fetchAttachment(url, targetPath, kMaximumFileBytes,
                    [this, message, live, origin](DesktopCurlAttachmentResult result) {
        if (currentAccount() != origin) {
            recordAttachmentPhase(origin, message,
                                  RemoteDiagnostics::AttachmentPhase::DroppedForAccountSwitch);
            return;
        }
        if (result.status == DesktopCurlAttachmentResult::Status::WriteError) {
            recordAttachmentPhase(origin, message,
                                  RemoteDiagnostics::AttachmentPhase::WriteFailed);
            return;
        }
        if (!result.ok() || !connected_) {
            recordAttachmentPhase(origin, message,
                                  RemoteDiagnostics::AttachmentPhase::DownloadFailed,
                                  result.transferCode);
            qWarning() << "[im] attachment download failed:" << int(result.status)
                       << result.transferCode;
            return;
        }
        recordAttachmentPhase(origin, message,
                              emitReceivedMessages({message}, live, origin)
                                  ? RemoteDiagnostics::AttachmentPhase::Delivered
                                  : RemoteDiagnostics::AttachmentPhase::DroppedForAccountSwitch);
    });
}

// 视频和封面分别下载，交付以同一条消息的媒体状态更新，而不是再插入一条新消息。
// 下载期间若账号切换，旧回调只能丢弃，不能把旧账号附件写入当前会话。
void TimSdkRemoteIMClient::handleIncomingVideoUrls(RemoteIMMessage message,
                                                   const QString& videoUrl,
                                                   const QString& coverUrl,
                                                   bool live) {
    const QString videoPath = cacheVideoPathForUrl(videoUrl, QStringLiteral(".mp4"));
    message.video.localPath = videoPath;
    const auto origin = currentAccount();

    // 先把封面拿下来（小、快），再下视频本体。封面失败只是没有缩略图，
    // 不能因此把整条视频消息拦住——气泡会退化成深色底 + 播放角标，仍可播放。
    // message 必须按参数传进来：如果在这里按值捕获，封面回调里补上的 coverPath
    // 改的是外层那一份，拷贝进 lambda 的那份永远是空的，封面白下了。
    const auto fetchVideo = [this, videoUrl, videoPath, live, origin](RemoteIMMessage message) {
        if (currentAccount() != origin || !connected_) return;
        if (QFileInfo(videoPath).isFile() && QFileInfo(videoPath).isReadable()) {
            emitReceivedMessages({message}, live, origin);
            return;
        }
        fetchAttachment(videoUrl, videoPath, kMaximumVideoBytes,
                        [this, message, live, origin](DesktopCurlAttachmentResult result) {
            if (currentAccount() != origin || !connected_) return;
            if (!result.ok()) {
                qWarning() << "[im] video download failed:" << int(result.status)
                           << result.transferCode;
                return;
            }
            emitReceivedMessages({message}, live, origin);
        });
    };

    if (coverUrl.trimmed().isEmpty()) {
        fetchVideo(message);
        return;
    }
    const QString coverPath = cacheVideoPathForUrl(coverUrl, QStringLiteral(".jpg"));
    if (QFileInfo(coverPath).isFile() && QFileInfo(coverPath).isReadable()) {
        message.video.coverPath = coverPath;
        fetchVideo(message);
        return;
    }
    fetchAttachment(coverUrl, coverPath, kMaximumVideoCoverBytes,
                    [this, origin, coverPath, message, fetchVideo](DesktopCurlAttachmentResult result) mutable {
        if (currentAccount() != origin || !connected_) return;
        if (result.ok()) message.video.coverPath = coverPath;
        fetchVideo(message);
    });
}

void TimSdkRemoteIMClient::handleIncomingVoiceUrl(RemoteIMMessage message, const QString& url, bool live) {
    const QString targetPath = cacheVoicePathForUrl(url);
    message.voice.localPath = targetPath;
    const auto origin = currentAccount();
    if (QFileInfo(targetPath).isFile() && QFileInfo(targetPath).isReadable()) {
        emitReceivedMessages({message}, live, origin);
        return;
    }
    fetchAttachment(url, targetPath, kMaximumVoiceBytes,
                    [this, message, live, origin](DesktopCurlAttachmentResult result) {
        if (currentAccount() != origin || !connected_) return;
        if (!result.ok()) {
            qWarning() << "[im] voice download failed:" << int(result.status)
                       << result.transferCode;
            return;
        }
        emitReceivedMessages({message}, live, origin);
    });
}

RemoteDiagnostics::AccountTag TimSdkRemoteIMClient::currentAccount() const {
    return RemoteDiagnostics::AccountTag{sdkAppId_, currentUserId_};
}

void TimSdkRemoteIMClient::recordAttachmentPhase(const RemoteDiagnostics::AccountTag& account,
                                                 const RemoteIMMessage& message,
                                                 RemoteDiagnostics::AttachmentPhase phase,
                                                 int code) {
    RemoteDiagnostics::AttachmentEvent event;
    event.account = account;
    // RemoteIMMessage 没有 peerId：对端要按方向取。入站是发件人，出站是收件人。
    event.peerId = message.direction == RemoteIMMessageDirection::Incoming ? message.fromUserId
                                                                           : message.toUserId;
    event.messageId = message.id;
    // 只认完整匹配的报告名；普通附件留空，控制器据此把它们当现场元数据，
    // 而不是当成本次排障的失败证据。**不记文件名本身，也不记 URL。**
    event.requestId = RemoteDiagnostics::requestIdFromAttachmentFileName(message.file.fileName);
    event.phase = phase;
    event.code = code;
    event.atMs = QDateTime::currentMSecsSinceEpoch();
    evidence_.record(event);
}

bool TimSdkRemoteIMClient::emitReceivedMessages(const QList<RemoteIMMessage>& messages, bool live,
                                                const RemoteDiagnostics::AccountTag& origin) {
    if (messages.isEmpty()) return false;
    // 换账号之后晚到的异步结果不能进新账号的库。这条护栏放在**普通消息入口**，
    // 不只保护排障记录——否则甲的附件会静静地出现在乙的聊天里。
    // 同账号断线重连不受影响：AccountTag 只有 sdkAppId + ownerUserId，
    // 不含任何连接/会话序号。
    if (origin.isValid() && origin != currentAccount()) {
        qInfo().noquote()
            << QStringLiteral("[im] dropped %1 late message(s) from a previous account")
                   .arg(messages.size());
        return false;
    }
    if (live) {
        emit liveMessagesReceived(messages);
    } else {
        emit messagesReceived(messages);
    }
    return true;
}

void TimSdkRemoteIMClient::complete(RemoteIMCompletion completion, int code, const QString& description) {
    if (!completion) return;
    completion(code == 0,
               code == 0 ? QString()
                         : (description.isEmpty() ? QStringLiteral("IM SDK 操作失败：%1").arg(code) : description));
}

QString TimSdkRemoteIMClient::compactJson(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}
