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
        QStringLiteral("schemaVersion"),  // 数字，单独校验，这里不列
        QStringLiteral("requestId"),
        QStringLiteral("appVersion"),
        QStringLiteral("platform"),
        QStringLiteral("exportedAt"),
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
    return fields;
}

const QStringList& eventNumberFields()
{
    static const QStringList fields{
        QStringLiteral("attempt"),
        QStringLiteral("code"),
        QStringLiteral("errorCode"),
        QStringLiteral("at")};
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
        QStringLiteral("sdkReady"), QStringLiteral("isReady"), QStringLiteral("accepted")};
    return fields;
}

const QStringList& sessionStringFields()
{
    static const QStringList fields{
        QStringLiteral("appVersion"),
        QStringLiteral("sourceCommit"),
        QStringLiteral("startedAt"),
        QStringLiteral("onDiskBinarySha256"),
        QStringLiteral("onDiskBinaryStatus")};
    return fields;
}

const QStringList& sessionNumberFields()
{
    static const QStringList fields{
        QStringLiteral("pid"), QStringLiteral("onDiskBinaryBytes")};
    return fields;
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
                            QStringList* dropped)
{
    QJsonObject out;
    for (auto it = source.begin(); it != source.end(); ++it) {
        const QString key = it.key();
        const QJsonValue value = it.value();
        const bool wantString = stringFields.contains(key);
        const bool wantNumber = numberFields.contains(key);
        const bool wantBool = boolFields.contains(key);
        const bool wantEither = eventStringOrNumberFields().contains(key);
        if (wantEither && value.isString()) {
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
    for (int i = 0; i < source.size(); ++i) {
        if (!source.at(i).isObject()) {
            dropped->append(QStringLiteral("%1[%2]").arg(path).arg(i));
            continue;
        }
        const QString itemPath = QStringLiteral("%1[%2]").arg(path).arg(i);
        QJsonObject event = pickWhitelisted(source.at(i).toObject(), eventStringFields(),
                                            eventNumberFields(), eventBoolFields(), itemPath,
                                            dropped);
        // detail 是嵌套的一层，字段表与 events 相同。
        const QJsonValue detail = source.at(i).toObject().value(QStringLiteral("detail"));
        if (detail.isObject()) {
            const QJsonObject picked =
                pickWhitelisted(detail.toObject(), eventStringFields(), eventNumberFields(),
                                eventBoolFields(), itemPath + QStringLiteral(".detail"), dropped);
            if (!picked.isEmpty()) event.insert(QStringLiteral("detail"), picked);
        }
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
    // 必须带本次的 requestId。少了这条，B 的任何一句普通话都可能被当成回绝，
    // 而且别的请求的回绝会串到这次来。
    if (!text.contains(expectedRequestId)) return receipt;

    const QString firstLine = text.split(QLatin1Char('\n')).value(0).trimmed();

    // 旧版 B 不认识这条命令时的固定首行。这条是「对端根本不支持」，
    // 与「支持但这次失败」要分开，报告里的措辞不一样。
    if (firstLine.startsWith(QStringLiteral("不支持的 IM 控制命令："))) {
        receipt.recognized = true;
        receipt.reason = FailureReason::Unsupported;
        return receipt;
    }

    // 已知固定提示 -> 固定原因。逐条精确匹配，不做包含式的模糊猜测：
    // 猜错会把用户的正常发言当成回绝，比多等 60 秒更糟。
    static const QList<QPair<QString, FailureReason>> kKnownPrefixes{
        {QStringLiteral("排障请求过于频繁"), FailureReason::RateLimited},
        {QStringLiteral("排障采集失败"), FailureReason::CollectionFailed},
        {QStringLiteral("排障服务不可用"), FailureReason::ServiceUnavailable}};
    for (const auto& entry : kKnownPrefixes) {
        if (firstLine.startsWith(entry.first)) {
            receipt.recognized = true;
            receipt.reason = entry.second;
            return receipt;
        }
    }
    return receipt;
}

QString describeFailureReason(FailureReason reason)
{
    switch (reason) {
    case FailureReason::Unsupported:
        return QStringLiteral("对方客户端版本较旧，不支持远程排障");
    case FailureReason::RateLimited:
        return QStringLiteral("对方限制了排障请求频率，本次未采集");
    case FailureReason::CollectionFailed:
        return QStringLiteral("对方采集失败，本次没有拿到数据");
    case FailureReason::ServiceUnavailable:
        return QStringLiteral("对方的排障服务不可用，本次未采集");
    case FailureReason::None:
        break;
    }
    return QString();
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

    if (root.value(QStringLiteral("schemaVersion")).toInt(-1) != kSchemaVersion) {
        result.rejectionReason = QStringLiteral("报告 schemaVersion 不是 %1，已拒收")
                                     .arg(kSchemaVersion);
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
    for (const QString& key : {QStringLiteral("from"), QStringLiteral("projectId")}) {
        const QJsonValue value = root.value(key);
        if (value.isDouble()) out.insert(key, value.toDouble());
        else if (value.isString()) out.insert(key, value.toString());
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
            for (int i = 0; i < source.size(); ++i) {
                if (!source.at(i).isObject()) continue;
                sessions.append(pickWhitelisted(
                    source.at(i).toObject(), sessionStringFields(), sessionNumberFields(), {},
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
