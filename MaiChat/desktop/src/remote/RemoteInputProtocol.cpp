#include "remote/RemoteInputProtocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace RemoteInput {
namespace {

// 键名一律一到两个字符：单包 1KB 的预算下，短键名能多塞不少事件。
constexpr char kKeyVersion[] = "v";
constexpr char kKeySession[] = "s";
constexpr char kKeySequence[] = "n";
constexpr char kKeyEvents[] = "e";
constexpr char kKeyType[] = "t";
constexpr char kKeyX[] = "x";
constexpr char kKeyY[] = "y";
constexpr char kKeyButton[] = "b";
constexpr char kKeyPressed[] = "d";
constexpr char kKeyWheel[] = "w";
constexpr char kKeyCode[] = "k";
constexpr char kKeyText[] = "s";

QString eventTypeTag(EventType type) {
    switch (type) {
        case EventType::MouseMove: return QStringLiteral("m");
        case EventType::MouseButton: return QStringLiteral("b");
        case EventType::MouseWheel: return QStringLiteral("w");
        case EventType::Key: return QStringLiteral("k");
        case EventType::Text: return QStringLiteral("x");
        case EventType::ReleaseAll: return QStringLiteral("r");
        case EventType::Unknown: break;
    }
    return QString();
}

EventType eventTypeFromTag(const QString& tag) {
    if (tag == QLatin1String("m")) return EventType::MouseMove;
    if (tag == QLatin1String("b")) return EventType::MouseButton;
    if (tag == QLatin1String("w")) return EventType::MouseWheel;
    if (tag == QLatin1String("k")) return EventType::Key;
    if (tag == QLatin1String("x")) return EventType::Text;
    if (tag == QLatin1String("r")) return EventType::ReleaseAll;
    return EventType::Unknown;
}

int buttonToInt(MouseButton button) {
    switch (button) {
        case MouseButton::Left: return 0;
        case MouseButton::Right: return 1;
        case MouseButton::Middle: return 2;
    }
    return 0;
}

// 未知按键编号一律落回左键：宁可点错一个键，也不要把它当成"没有按键"
// 而漏掉配对的抬起，那会留下悬空状态。
MouseButton buttonFromInt(int value) {
    switch (value) {
        case 1: return MouseButton::Right;
        case 2: return MouseButton::Middle;
        default: return MouseButton::Left;
    }
}

QJsonObject encodeEvent(const Event& event) {
    QJsonObject object;
    object.insert(QLatin1String(kKeyType), eventTypeTag(event.type));
    switch (event.type) {
        case EventType::MouseMove:
            object.insert(QLatin1String(kKeyX), clampNormalized(event.x));
            object.insert(QLatin1String(kKeyY), clampNormalized(event.y));
            break;
        case EventType::MouseButton:
            object.insert(QLatin1String(kKeyX), clampNormalized(event.x));
            object.insert(QLatin1String(kKeyY), clampNormalized(event.y));
            object.insert(QLatin1String(kKeyButton), buttonToInt(event.button));
            object.insert(QLatin1String(kKeyPressed), event.pressed);
            break;
        case EventType::MouseWheel:
            object.insert(QLatin1String(kKeyX), clampNormalized(event.x));
            object.insert(QLatin1String(kKeyY), clampNormalized(event.y));
            object.insert(QLatin1String(kKeyWheel), event.wheelDelta);
            break;
        case EventType::Key:
            object.insert(QLatin1String(kKeyCode), static_cast<qint64>(event.keyCode));
            object.insert(QLatin1String(kKeyPressed), event.pressed);
            break;
        case EventType::Text:
            object.insert(QLatin1String(kKeyText), event.text);
            break;
        case EventType::ReleaseAll:
        case EventType::Unknown:
            break;
    }
    return object;
}

// 解不出来的事件返回 Unknown，由调用方跳过——一条坏事件不该让整包报废。
Event decodeEvent(const QJsonObject& object) {
    Event event;
    event.type = eventTypeFromTag(object.value(QLatin1String(kKeyType)).toString());
    switch (event.type) {
        case EventType::MouseMove:
            event.x = clampNormalized(object.value(QLatin1String(kKeyX)).toDouble());
            event.y = clampNormalized(object.value(QLatin1String(kKeyY)).toDouble());
            break;
        case EventType::MouseButton:
            event.x = clampNormalized(object.value(QLatin1String(kKeyX)).toDouble());
            event.y = clampNormalized(object.value(QLatin1String(kKeyY)).toDouble());
            event.button = buttonFromInt(object.value(QLatin1String(kKeyButton)).toInt());
            event.pressed = object.value(QLatin1String(kKeyPressed)).toBool();
            break;
        case EventType::MouseWheel:
            event.x = clampNormalized(object.value(QLatin1String(kKeyX)).toDouble());
            event.y = clampNormalized(object.value(QLatin1String(kKeyY)).toDouble());
            event.wheelDelta = object.value(QLatin1String(kKeyWheel)).toInt();
            break;
        case EventType::Key: {
            const qint64 code = static_cast<qint64>(
                object.value(QLatin1String(kKeyCode)).toDouble());
            // 键码越界的包按坏包处理：拿它去 SendInput 会点到完全无关的键。
            if (code < 0 || code > 0xFFFF) {
                event.type = EventType::Unknown;
                break;
            }
            event.keyCode = static_cast<quint32>(code);
            event.pressed = object.value(QLatin1String(kKeyPressed)).toBool();
            break;
        }
        case EventType::Text:
            event.text = object.value(QLatin1String(kKeyText)).toString();
            // 空文本没有注入意义，直接当无效事件丢掉。
            if (event.text.isEmpty()) event.type = EventType::Unknown;
            break;
        case EventType::ReleaseAll:
        case EventType::Unknown:
            break;
    }
    return event;
}

}  // namespace

double clampNormalized(double value) {
    // NaN 比不出大小，两个分支都不会命中，这里显式挡掉。
    if (!(value == value)) return 0.0;
    if (value < 0.0) return 0.0;
    if (value > 1.0) return 1.0;
    return value;
}

QByteArray encodePacket(const Packet& packet) {
    QJsonArray events;
    for (const auto& event : packet.events) {
        if (event.type == EventType::Unknown) continue;
        events.append(encodeEvent(event));
    }

    QJsonObject root;
    root.insert(QLatin1String(kKeyVersion), kProtocolVersion);
    root.insert(QLatin1String(kKeySession), packet.sessionId);
    root.insert(QLatin1String(kKeySequence), static_cast<qint64>(packet.sequence));
    root.insert(QLatin1String(kKeyEvents), events);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool decodePacket(const QByteArray& payload, Packet* out) {
    if (out == nullptr || payload.isEmpty()) return false;

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;

    const QJsonObject root = document.object();
    const QJsonValue version = root.value(QLatin1String(kKeyVersion));
    // 版本对不上一律整包丢弃：宁可不动，也不要用错误的语义去操作别人的电脑。
    if (!version.isDouble() || version.toInt() != kProtocolVersion) return false;

    const QJsonValue sequence = root.value(QLatin1String(kKeySequence));
    if (!sequence.isDouble()) return false;
    const qint64 sequenceValue = static_cast<qint64>(sequence.toDouble());
    if (sequenceValue < 0 || sequenceValue > 0xFFFFFFFFLL) return false;

    const QJsonValue events = root.value(QLatin1String(kKeyEvents));
    if (!events.isArray()) return false;

    Packet parsed;
    parsed.protocolVersion = version.toInt();
    parsed.sessionId = root.value(QLatin1String(kKeySession)).toString();
    parsed.sequence = static_cast<quint32>(sequenceValue);
    for (const auto& value : events.toArray()) {
        if (!value.isObject()) continue;
        const Event event = decodeEvent(value.toObject());
        if (event.type == EventType::Unknown) continue;
        parsed.events.append(event);
    }

    *out = parsed;
    return true;
}

bool fitsInOnePacket(const Packet& packet) {
    return encodePacket(packet).size() <= kMaxPacketBytes;
}

}  // namespace RemoteInput
