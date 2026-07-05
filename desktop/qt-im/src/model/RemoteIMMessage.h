#pragma once

#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QUuid>

enum class RemoteIMMessageDirection {
    Incoming,
    Outgoing
};

enum class RemoteIMMessageStatus {
    Pending,
    Sent,
    Received,
    Failed
};

struct RemoteIMImageAttachment {
    QString localPath;
    int width = 0;
    int height = 0;
    qint64 sizeBytes = 0;
};

struct RemoteIMVoiceAttachment {
    QString localPath;
    int durationSeconds = 1;
};

struct RemoteIMMessage {
    QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString fromUserId;
    QString toUserId;
    QString text;
    RemoteIMMessageDirection direction = RemoteIMMessageDirection::Incoming;
    RemoteIMMessageStatus status = RemoteIMMessageStatus::Received;
    qint64 createdAtMillis = QDateTime::currentMSecsSinceEpoch();
    RemoteIMImageAttachment image;
    RemoteIMVoiceAttachment voice;
    bool hasImage = false;
    bool hasVoice = false;
};

Q_DECLARE_METATYPE(RemoteIMMessage)
Q_DECLARE_METATYPE(QList<RemoteIMMessage>)
