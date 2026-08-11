#include "remote/RemoteDesktopSignal.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>

namespace RemoteDesktopSignals {
namespace {

// 不可见字符前缀 + 可读标记。前者保证信令即使被误显示也几乎不可见，
// 后者便于日志排查。
const QString kInvisiblePrefix = QStringLiteral("⁣​");
const QString kMarker = QStringLiteral("[remote-desktop]");

QString typeToString(Type type) {
    switch (type) {
        case Type::Invite: return QStringLiteral("invite");
        case Type::Accept: return QStringLiteral("accept");
        case Type::Reject: return QStringLiteral("reject");
        case Type::Stop:   return QStringLiteral("stop");
        case Type::Notice: return QStringLiteral("notice");
        case Type::Unknown: break;
    }
    return QString();
}

Type typeFromString(const QString& value) {
    if (value == QStringLiteral("invite")) return Type::Invite;
    if (value == QStringLiteral("accept")) return Type::Accept;
    if (value == QStringLiteral("reject")) return Type::Reject;
    if (value == QStringLiteral("stop")) return Type::Stop;
    if (value == QStringLiteral("notice")) return Type::Notice;
    return Type::Unknown;
}

bool exactInteger(const QJsonObject& object, const QString& key, int minimum, int maximum,
                  int* result) {
    if (result == nullptr) return false;
    const QJsonValue value = object.value(key);
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number || number < minimum
        || number > maximum) {
        return false;
    }
    *result = static_cast<int>(number);
    return true;
}

QJsonObject encodeCaptureGeometry(const RemoteDesktop::CaptureGeometry& geometry) {
    QJsonObject object;
    object.insert(QStringLiteral("sourceWidth"), geometry.sourceSize.width());
    object.insert(QStringLiteral("sourceHeight"), geometry.sourceSize.height());
    object.insert(QStringLiteral("captureX"), geometry.captureRect.x());
    object.insert(QStringLiteral("captureY"), geometry.captureRect.y());
    object.insert(QStringLiteral("captureWidth"), geometry.captureRect.width());
    object.insert(QStringLiteral("captureHeight"), geometry.captureRect.height());
    object.insert(QStringLiteral("contentMode"),
                  RemoteDesktop::captureContentModeName(geometry.contentMode));
    object.insert(QStringLiteral("revision"), static_cast<int>(geometry.revision));
    return object;
}

std::optional<RemoteDesktop::CaptureGeometry> decodeCaptureGeometry(const QJsonValue& value) {
    if (!value.isObject()) return std::nullopt;
    const QJsonObject object = value.toObject();

    int sourceWidth = 0;
    int sourceHeight = 0;
    int captureX = 0;
    int captureY = 0;
    int captureWidth = 0;
    int captureHeight = 0;
    int revision = 0;
    const int maxDimension = RemoteDesktop::kMaxCaptureGeometryDimension;
    if (!exactInteger(object, QStringLiteral("sourceWidth"), 1, maxDimension, &sourceWidth)
        || !exactInteger(object, QStringLiteral("sourceHeight"), 1, maxDimension,
                         &sourceHeight)
        || !exactInteger(object, QStringLiteral("captureX"), 0, maxDimension, &captureX)
        || !exactInteger(object, QStringLiteral("captureY"), 0, maxDimension, &captureY)
        || !exactInteger(object, QStringLiteral("captureWidth"), 1, maxDimension,
                         &captureWidth)
        || !exactInteger(object, QStringLiteral("captureHeight"), 1, maxDimension,
                         &captureHeight)
        || !exactInteger(object, QStringLiteral("revision"), 1,
                         static_cast<int>(RemoteDesktop::kMaxCaptureGeometryRevision),
                         &revision)) {
        return std::nullopt;
    }

    const QString mode = object.value(QStringLiteral("contentMode")).toString();
    RemoteDesktop::CaptureGeometry geometry;
    geometry.sourceSize = QSize(sourceWidth, sourceHeight);
    geometry.captureRect = QRect(captureX, captureY, captureWidth, captureHeight);
    geometry.contentMode = mode == QStringLiteral("fit")
        ? RemoteDesktop::CaptureContentMode::Fit
        : RemoteDesktop::CaptureContentMode::Unknown;
    geometry.revision = static_cast<quint64>(revision);
    return geometry.isValid()
        ? std::optional<RemoteDesktop::CaptureGeometry>(geometry)
        : std::nullopt;
}

}  // namespace

namespace NoticeCodes {
const char kSecureDesktopEntered[] = "secure-desktop-entered";
const char kSecureDesktopLeft[] = "secure-desktop-left";
}  // namespace NoticeCodes

QString signalPrefix() {
    return kInvisiblePrefix + kMarker;
}

bool isSignalText(const QString& text) {
    return text.startsWith(signalPrefix());
}

QString encodeSignal(const Signal& signal) {
    const QString type = typeToString(signal.type);
    if (type.isEmpty()) return QString();

    QJsonObject object;
    object.insert(QStringLiteral("v"), kProtocolVersion);
    object.insert(QStringLiteral("type"), type);
    if (!signal.sessionId.isEmpty()) object.insert(QStringLiteral("sessionId"), signal.sessionId);
    if (!signal.roomId.isEmpty()) object.insert(QStringLiteral("roomId"), signal.roomId);
    if (!signal.authProof.isEmpty()) object.insert(QStringLiteral("authProof"), signal.authProof);
    if (!signal.reason.isEmpty()) object.insert(QStringLiteral("reason"), signal.reason);
    if (!signal.noticeCode.isEmpty()) {
        object.insert(QStringLiteral("noticeCode"), signal.noticeCode);
    }
    if (signal.type == Type::Accept && signal.captureGeometry
        && signal.captureGeometry->isValid()) {
        object.insert(QStringLiteral("captureGeometry"),
                      encodeCaptureGeometry(*signal.captureGeometry));
    }

    return signalPrefix()
         + QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

Signal decodeSignal(const QString& text) {
    Signal signal;
    if (!isSignalText(text)) return signal;

    const QByteArray payload = text.mid(signalPrefix().size()).toUtf8();
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return signal;

    const QJsonObject object = document.object();
    // 版本不认识就当普通消息处理：未来协议升级时老客户端不会误解语义。
    if (object.value(QStringLiteral("v")).toInt(0) != kProtocolVersion) return signal;

    const Type type = typeFromString(object.value(QStringLiteral("type")).toString());
    if (type == Type::Unknown) return signal;

    const QString noticeCode = object.value(QStringLiteral("noticeCode")).toString();
    // 没带 code 的 Notice 没有任何意义，当成不认识的信令丢弃——否则控制端会
    // 收到一条自己不知道该显示什么的空播报。
    if (type == Type::Notice && noticeCode.isEmpty()) return signal;

    signal.type = type;
    signal.noticeCode = noticeCode;
    signal.protocolVersion = kProtocolVersion;
    signal.sessionId = object.value(QStringLiteral("sessionId")).toString();
    signal.roomId = object.value(QStringLiteral("roomId")).toString();
    signal.authProof = object.value(QStringLiteral("authProof")).toString();
    signal.reason = object.value(QStringLiteral("reason")).toString();
    // captureGeometry 是 v1 的兼容扩展。坏掉的可选对象只影响坐标增强，不能把
    // 一条本来合法的 Accept 整体打成 Unknown，否则老/新版本混连会进不了房。
    if (type == Type::Accept) {
        signal.captureGeometry =
            decodeCaptureGeometry(object.value(QStringLiteral("captureGeometry")));
    }
    return signal;
}

}  // namespace RemoteDesktopSignals
