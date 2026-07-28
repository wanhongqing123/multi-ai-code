#include "remote/RemoteDesktopSignal.h"

#include <QJsonDocument>
#include <QJsonObject>

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
        case Type::Unknown: break;
    }
    return QString();
}

Type typeFromString(const QString& value) {
    if (value == QStringLiteral("invite")) return Type::Invite;
    if (value == QStringLiteral("accept")) return Type::Accept;
    if (value == QStringLiteral("reject")) return Type::Reject;
    if (value == QStringLiteral("stop")) return Type::Stop;
    return Type::Unknown;
}

}  // namespace

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

    signal.type = type;
    signal.protocolVersion = kProtocolVersion;
    signal.sessionId = object.value(QStringLiteral("sessionId")).toString();
    signal.roomId = object.value(QStringLiteral("roomId")).toString();
    signal.authProof = object.value(QStringLiteral("authProof")).toString();
    signal.reason = object.value(QStringLiteral("reason")).toString();
    return signal;
}

}  // namespace RemoteDesktopSignals
