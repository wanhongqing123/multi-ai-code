#include "storage/LocalChatHistoryStore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <utility>

namespace {
QString directionToString(RemoteIMMessageDirection direction) {
    return direction == RemoteIMMessageDirection::Outgoing ? "outgoing" : "incoming";
}

RemoteIMMessageDirection directionFromString(const QString& direction) {
    return direction == "outgoing" ? RemoteIMMessageDirection::Outgoing : RemoteIMMessageDirection::Incoming;
}

QJsonObject contactToJson(const RemoteIMContact& contact) {
    QJsonObject object;
    object["userId"] = contact.userId;
    object["displayName"] = contact.displayName;
    return object;
}

RemoteIMContact contactFromJson(const QJsonObject& object) {
    return RemoteIMContact{object["userId"].toString(), object["displayName"].toString()};
}

QJsonObject messageToJson(const RemoteIMMessage& message) {
    QJsonObject object;
    object["id"] = message.id;
    object["from"] = message.fromUserId;
    object["to"] = message.toUserId;
    object["text"] = message.text;
    object["direction"] = directionToString(message.direction);
    object["status"] = static_cast<int>(message.status);
    object["createdAtMillis"] = QString::number(message.createdAtMillis);
    object["hasImage"] = message.hasImage;
    object["imagePath"] = message.image.localPath;
    object["imageWidth"] = message.image.width;
    object["imageHeight"] = message.image.height;
    object["imageSizeBytes"] = QString::number(message.image.sizeBytes);
    object["hasVoice"] = message.hasVoice;
    object["voicePath"] = message.voice.localPath;
    object["voiceDurationSeconds"] = message.voice.durationSeconds;
    return object;
}

RemoteIMMessage messageFromJson(const QJsonObject& object) {
    RemoteIMMessage message;
    message.id = object["id"].toString();
    message.fromUserId = object["from"].toString();
    message.toUserId = object["to"].toString();
    message.text = object["text"].toString();
    message.direction = directionFromString(object["direction"].toString());
    message.status = static_cast<RemoteIMMessageStatus>(object["status"].toInt());
    message.createdAtMillis = object["createdAtMillis"].toString().toLongLong();
    message.hasImage = object["hasImage"].toBool();
    message.image = RemoteIMImageAttachment{
        object["imagePath"].toString(),
        object["imageWidth"].toInt(),
        object["imageHeight"].toInt(),
        object["imageSizeBytes"].toString().toLongLong()
    };
    message.hasVoice = object["hasVoice"].toBool();
    message.voice = RemoteIMVoiceAttachment{
        object["voicePath"].toString(),
        object["voiceDurationSeconds"].toInt()
    };
    return message;
}
}

LocalChatHistoryStore::LocalChatHistoryStore(QString rootDir) : rootDir_(std::move(rootDir)) {}

bool LocalChatHistoryStore::save(const ChatState& state) const {
    QDir().mkpath(rootDir_);
    QJsonObject root;
    root["ownerUserId"] = state.ownerUserId();
    root["selectedPeerId"] = state.selectedPeerId();

    QJsonArray contacts;
    for (const RemoteIMContact& contact : state.contacts()) {
        contacts.append(contactToJson(contact));
    }
    root["contacts"] = contacts;

    QJsonArray messages;
    for (const RemoteIMMessage& message : state.messages()) {
        messages.append(messageToJson(message));
    }
    root["messages"] = messages;

    QFile file(filePath(state.ownerUserId()));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    return true;
}

bool LocalChatHistoryStore::load(const QString& ownerUserId, ChatState& state) const {
    QFile file(filePath(ownerUserId));
    if (!file.exists()) return true;
    if (!file.open(QIODevice::ReadOnly)) return false;

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) return false;
    const QJsonObject root = document.object();

    ChatState loaded(ownerUserId);
    for (const QJsonValue& value : root["contacts"].toArray()) {
        loaded.upsertContact(contactFromJson(value.toObject()));
    }
    const QString selectedPeerId = root["selectedPeerId"].toString();
    if (!selectedPeerId.isEmpty()) loaded.selectPeer(selectedPeerId);

    for (const QJsonValue& value : root["messages"].toArray()) {
        loaded.appendMessageForRestore(messageFromJson(value.toObject()));
    }
    state = loaded;
    return true;
}

QString LocalChatHistoryStore::filePath(const QString& ownerUserId) const {
    return QDir(rootDir_).filePath(ownerUserId.trimmed() + ".json");
}
