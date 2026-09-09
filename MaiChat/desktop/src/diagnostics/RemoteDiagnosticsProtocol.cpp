#include "diagnostics/RemoteDiagnosticsProtocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>
#include <QUuid>

namespace RemoteDiagnostics {
namespace {

const QString kCommandPrefix = QStringLiteral("/diagnostics ");

// 小写 UUID，无花括号。两端逐字符比对，所以这里写死形状。
const QRegularExpression& requestIdPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
    return pattern;
}

// ---- 白名单 ----
//
// 这些表就是协议本身。写在一处，便于和对端逐条对齐；
// 表以外的字段一律丢弃，不做"看起来无害就放行"的判断。

const QStringList& topLevelStringFields()
{
    static const QStringList fields{
        QStringLiteral("requestId"),
        QStringLiteral("appVersion"),
        QStringLiteral("platform"),
        QStringLiteral("projectId"),
        QStringLiteral("electronVersion"),
        QStringLiteral("timeZone")};
    return fields;
}

const QStringList& fileStringFields()
{
    static const QStringList fields{
        QStringLiteral("source"), QStringLiteral("status")};
    return fields;
}

// events / detail 里允许的**字符串** ID 类字段。
//
// 注意 ID 与 messageId 是两个不同字段，不是同一个值的两种写法：
// ID 是 send:created 里的 SDK 大写字符串标识；messageId 见下，
// 合法地有数字与字符串两种形态。混用会让两端日志对不上号。
const QStringList& eventStringFields()
{
    static const QStringList fields{
        QStringLiteral("ID"),
        QStringLiteral("remoteMessageId"),
        QStringLiteral("callId"),
        QStringLiteral("id"),
        QStringLiteral("phase"),
        QStringLiteral("delivery"),
        QStringLiteral("type"),
        QStringLiteral("threadId"),
        QStringLiteral("turnId"),
        QStringLiteral("direction"),
        QStringLiteral("status")};
    // Kept in sync with the actual host producer, not an IM prose summary.
    static const QStringList all = fields + QStringList{
        "event", "kind", "sourceKind", "cli", "sessionId", "taskId", "replyId", "partId",
        "eventTaskId", "eventReplyId", "sourceCommit", "onDiskBinarySha256", "terminalKind",
        "hostStopReason", "stopReasonRequested", "spawnErrorCode", "signal", "exitCodeHex",
        "appVersion", "onDiskBinaryStatus"};
    return all;
}

const QStringList& eventNumberFields()
{
    static const QStringList fields{
        QStringLiteral("attempt"),
        QStringLiteral("code"),
        QStringLiteral("errorCode"),
        QStringLiteral("at"), "createdAt", "startedAt", "pid", "hostPid", "lifetimeMs", "lastInputAt",
        "lastOutputAt", "lastEtxAt", "stopRequestedAt", "exitCode", "textLength", "inputLength",
        "resolvedLength", "forwardedChunks", "onDiskBinaryBytes"};
    return fields;
}

// messageId 合法地有两种类型，不能只收一种：
//
//   事件顶层 messageId = 1637              本地入库的数字编号
//   detail.messageId   = "msg_..."         原生消息标识
//   codex-original-events 的 messageId     = "call-async..." 调用标识
//
// 只收数字会把后两种静默丢掉，而那正是两端日志对得上号的关键。
// 布尔/对象/数组仍然拒绝——那些不是这个字段的合法形态。
const QStringList& eventStringOrNumberFields()
{
    static const QStringList fields{QStringLiteral("messageId")};
    return fields;
}

const QStringList& eventBoolFields()
{
    static const QStringList fields{
        QStringLiteral("sdkReady"), QStringLiteral("isReady"), QStringLiteral("accepted"),
        "ok", "sourceStarted", "autoReplyToIm"};
    return fields;
}

const QStringList& sessionStringFields()
{
    return eventStringFields();
}

const QStringList& sessionNumberFields()
{
    return eventNumberFields();
}

// sourceCoverage.aicliOriginalEvents 只认两个取值。
// 别的取值丢弃而不是透传：这个字段的作用正是区分"采到了"和"没采到"，
// 放进第三种取值就等于让报告显得比它实际知道的更确定。
bool isKnownCoverageValue(const QString& value)
{
    return value == QStringLiteral("see-codex-original-events")
        || value == QStringLiteral("unavailable");
}

// 按类型分表挑字段。命中表但类型不对同样丢弃——类型对不上的值
// 拿去和对端比对只会得出错误结论。
QJsonObject pickWhitelisted(const QJsonObject& source,
                            const QStringList& stringFields,
                            const QStringList& numberFields,
                            const QStringList& boolFields,
                            const QString& path,
                            QStringList* dropped, int depth = 0)
{
    QJsonObject out;
    for (auto it = source.begin(); it != source.end(); ++it) {
        const QString key = it.key();
        const QJsonValue value = it.value();
        const bool wantString = stringFields.contains(key);
        const bool wantNumber = numberFields.contains(key);
        const bool wantBool = boolFields.contains(key);
        const bool wantEither = eventStringOrNumberFields().contains(key);
        if (key == "detail" && value.isObject() && depth < 3) {
            out.insert(key, pickWhitelisted(value.toObject(), eventStringFields(), eventNumberFields(),
                eventBoolFields(), path + ".detail", dropped, depth + 1));
        } else if (key == "candidates" && value.isArray() && depth < 3) {
            QJsonArray candidates;
            for (const auto& candidate : value.toArray()) {
                if (candidates.size() >= 20) break;
                if (candidate.isObject()) candidates.append(pickWhitelisted(candidate.toObject(), eventStringFields(),
                    eventNumberFields(), eventBoolFields(), path + ".candidates", dropped, depth + 1));
            }
            out.insert(key, candidates);
        } else if (wantEither && value.isString()) {
            out.insert(key, value.toString());
        } else if (wantEither && value.isDouble()) {
            out.insert(key, value.toDouble());
        } else if (wantString && value.isString()) {
            out.insert(key, value.toString());
        } else if (wantNumber && value.isDouble()) {
            out.insert(key, value.toDouble());
        } else if (wantBool && value.isBool()) {
            out.insert(key, value.toBool());
        } else {
            dropped->append(path.isEmpty() ? key : path + QLatin1Char('.') + key);
        }
    }
    return out;
}

QJsonArray pickEventArray(const QJsonValue& value, const QString& path, QStringList* dropped)
{
    QJsonArray out;
    if (!value.isArray()) {
        dropped->append(path);
        return out;
    }
    const QJsonArray source = value.toArray();
    for (int i = 0; i < qMin(source.size(), 1000); ++i) {
        if (!source.at(i).isObject()) {
            dropped->append(QStringLiteral("%1[%2]").arg(path).arg(i));
            continue;
        }
        const QString itemPath = QStringLiteral("%1[%2]").arg(path).arg(i);
        QJsonObject event = pickWhitelisted(source.at(i).toObject(), eventStringFields(),
                                            eventNumberFields(), eventBoolFields(), itemPath,
                                            dropped);
        if (!event.isEmpty()) out.append(event);
    }
    return out;
}

}  // namespace

QString newRequestId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
}

bool isValidRequestId(const QString& requestId)
{
    return requestIdPattern().match(requestId).hasMatch();
}

QString formatCommand(const QString& requestId)
{
    return kCommandPrefix + requestId;
}

QString parseCommand(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (!trimmed.startsWith(kCommandPrefix)) return QString();
    const QString candidate = trimmed.mid(kCommandPrefix.size()).trimmed();
    return isValidRequestId(candidate) ? candidate : QString();
}

QString attachmentFileName(const QString& requestId)
{
    return QStringLiteral("remote-diagnostics-%1.json").arg(requestId);
}

bool matchesAttachmentFileName(const QString& fileName, const QString& requestId)
{
    if (!isValidRequestId(requestId)) return false;
    return fileName == attachmentFileName(requestId);
}

FailureReceipt parseFailureReceipt(const QString& text, const QString& expectedRequestId)
{
    FailureReceipt receipt;
    if (!isValidRequestId(expectedRequestId)) return receipt;

    // requestId 必须出现在**句子里它该在的位置**，不是「文本里某处含有它」。
    // 后者可以被 "远程排障采集失败（编号 <别人的>）：... <我们的>" 骗过去：
    // 编号出现了，但那条回绝根本不是给本次请求的。
    const QString id = expectedRequestId;

    if (text == QStringLiteral("远程排障请求过于频繁，请稍后重试（%1）。").arg(id)) {
        receipt.recognized = true;
        receipt.reason = FailureReason::RateLimited;
        return receipt;
    }
    if (text.startsWith(QStringLiteral("远程排障采集失败（编号 %1）：").arg(id))) {
        receipt.recognized = true;
        receipt.reason = FailureReason::CollectionFailed;
        return receipt;
    }
    if (text == QStringLiteral("当前 MultiAICode 尚未接入远程排障（%1），请升级后重试。").arg(id)) {
        receipt.recognized = true;
        receipt.reason = FailureReason::ServiceUnavailable;
        return receipt;
    }
    // 旧版 B：只比对首行，后面还跟着可用命令列表。
    if (text.split(QLatin1Char('\n')).value(0)
        == QStringLiteral("不支持的 IM 控制命令：/diagnostics %1").arg(id)) {
        receipt.recognized = true;
        receipt.reason = FailureReason::Unsupported;
        return receipt;
    }
    return receipt;
}

QString describeFailureReason(FailureReason reason)
{
    // 与 iOS / 宿主逐字一致：同一件事在三端应当说同一句话。
    switch (reason) {
    case FailureReason::Unsupported:
        return QStringLiteral("远端版本不支持远程排障；本报告仅含本地现场。");
    case FailureReason::RateLimited:
        return QStringLiteral("远端拒绝了过于频繁的采集请求；本报告仅含本地现场。");
    case FailureReason::CollectionFailed:
        return QStringLiteral("远端报告采集失败；本报告仅含本地现场。");
    case FailureReason::ServiceUnavailable:
        return QStringLiteral("远端排障服务尚未接入，暂未取得远端现场；本报告仅含本地现场。");
    case FailureReason::None:
        break;
    }
    return QString();
}

QString requestIdFromAttachmentFileName(const QString& fileName)
{
    static const QString prefix = QStringLiteral("remote-diagnostics-");
    static const QString suffix = QStringLiteral(".json");
    if (!fileName.startsWith(prefix) || !fileName.endsWith(suffix)) return QString();
    const QString candidate =
        fileName.mid(prefix.size(), fileName.size() - prefix.size() - suffix.size());
    // 形状不对就当普通文件。这里不放宽：放宽等于把别的文件认成排障报告。
    return isValidRequestId(candidate) ? candidate : QString();
}

ParsedReport parseReport(const QByteArray& payload,
                         const QString& fileName,
                         const QString& expectedRequestId,
                         const QString& senderId,
                         const QString& expectedSenderId)
{
    ParsedReport result;

    // 来源先于内容。别人发来的同名文件不能因为格式正确就被采信。
    if (senderId.isEmpty() || senderId != expectedSenderId) {
        result.rejectionReason =
            QStringLiteral("报告不是来自故障客户端本人，已拒收");
        return result;
    }
    if (payload.size() > kMaxReportBytes) {
        result.rejectionReason = QStringLiteral("报告超过 %1 字节上限，已拒收")
                                     .arg(kMaxReportBytes);
        return result;
    }
    if (!matchesAttachmentFileName(fileName, expectedRequestId)) {
        result.rejectionReason = QStringLiteral("附件名与本次请求编号不符，已拒收");
        return result;
    }

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        result.rejectionReason = QStringLiteral("报告不是合法 JSON 对象，已拒收");
        return result;
    }
    const QJsonObject root = document.object();

    if (!root.value("schemaVersion").isDouble() || root.value("schemaVersion").toDouble() != kSchemaVersion) {
        result.rejectionReason = QStringLiteral("报告 schemaVersion 不是 %1，已拒收")
                                     .arg(kSchemaVersion);
        return result;
    }
    if (!root.value("files").isArray() || root.value("files").toArray().size() > 16) {
        result.rejectionReason = QStringLiteral("报告来源列表缺失、无效或超过上限，已拒收");
        return result;
    }
    // 文件名对了，内容里的编号也必须对——两处都查，少查一处就能被绕过。
    if (root.value(QStringLiteral("requestId")).toString() != expectedRequestId) {
        result.rejectionReason =
            QStringLiteral("报告内的请求编号与本次请求不符，已拒收");
        return result;
    }

    QJsonObject out;
    out.insert(QStringLiteral("schemaVersion"), kSchemaVersion);
    for (const QString& key : topLevelStringFields()) {
        const QJsonValue value = root.value(key);
        if (value.isString()) out.insert(key, value.toString());
    }
    for (const QString& key : {QStringLiteral("from"), QStringLiteral("exportedAt")}) {
        const QJsonValue value = root.value(key);
        if (value.isDouble()) out.insert(key, value.toDouble());
    }

    if (root.contains(QStringLiteral("files"))) {
        QJsonArray files;
        const QJsonValue filesValue = root.value(QStringLiteral("files"));
        if (filesValue.isArray()) {
            const QJsonArray source = filesValue.toArray();
            for (int i = 0; i < source.size(); ++i) {
                if (!source.at(i).isObject()) continue;
                const QJsonObject entry = source.at(i).toObject();
                const QString path = QStringLiteral("files[%1]").arg(i);
                QJsonObject file = pickWhitelisted(entry, fileStringFields(),
                                                   {QStringLiteral("invalidLines")},
                                                   {QStringLiteral("truncated")}, path,
                                                   &result.droppedFields);
                if (entry.contains(QStringLiteral("events"))) {
                    const QJsonArray events =
                        pickEventArray(entry.value(QStringLiteral("events")),
                                       path + QStringLiteral(".events"), &result.droppedFields);
                    file.insert(QStringLiteral("events"), events);
                    if (entry.value("events").toArray().size() > 1000) file.insert("truncated", true);
                }
                files.append(file);
            }
        }
        out.insert(QStringLiteral("files"), files);
    }

    if (root.contains(QStringLiteral("activeSessions"))) {
        QJsonArray sessions;
        const QJsonValue value = root.value(QStringLiteral("activeSessions"));
        if (value.isArray()) {
            const QJsonArray source = value.toArray();
            for (int i = 0; i < qMin(source.size(), 32); ++i) {
                if (!source.at(i).isObject()) continue;
                sessions.append(pickWhitelisted(
                    source.at(i).toObject(), sessionStringFields(), sessionNumberFields(), eventBoolFields(),
                    QStringLiteral("activeSessions[%1]").arg(i), &result.droppedFields));
            }
        }
        out.insert(QStringLiteral("activeSessions"), sessions);
    }

    if (root.contains(QStringLiteral("sourceCoverage"))) {
        const QJsonValue value = root.value(QStringLiteral("sourceCoverage"));
        QJsonObject coverage;
        if (value.isObject()) {
            const QString events =
                value.toObject().value(QStringLiteral("aicliOriginalEvents")).toString();
            if (isKnownCoverageValue(events)) {
                coverage.insert(QStringLiteral("aicliOriginalEvents"), events);
            } else if (!events.isEmpty()) {
                result.droppedFields.append(
                    QStringLiteral("sourceCoverage.aicliOriginalEvents"));
            }
        }
        out.insert(QStringLiteral("sourceCoverage"), coverage);
    }

    result.accepted = true;
    result.report = out;
    return result;
}

}  // namespace RemoteDiagnostics
