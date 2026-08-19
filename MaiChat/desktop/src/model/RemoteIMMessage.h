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

enum class RemoteIMMessageOrigin {
    Unknown,
    Human,
    Machine
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

struct RemoteIMFileAttachment {
    QString localPath;
    QString fileName;
    QString mimeType;
    qint64 sizeBytes = 0;
};

// 视频消息附件。coverPath 是封面在本地的路径：发送侧是我们自己生成的那张，
// 接收侧是从 video_elem_image_url 下载缓存下来的。durationSeconds 用于气泡上
// 显示时长，解不出来时为 0（不影响播放）。
struct RemoteIMVideoAttachment {
    QString localPath;
    QString fileName;
    QString coverPath;
    int durationSeconds = 0;
    qint64 sizeBytes = 0;
};

struct RemoteIMMessage {
    QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString fromUserId;
    QString toUserId;
    QString text;
    RemoteIMMessageDirection direction = RemoteIMMessageDirection::Incoming;
    RemoteIMMessageStatus status = RemoteIMMessageStatus::Received;
    // Versioned Tencent cloud metadata. Unknown means missing/invalid/foreign metadata.
    RemoteIMMessageOrigin origin = RemoteIMMessageOrigin::Unknown;
    qint64 createdAtMillis = QDateTime::currentMSecsSinceEpoch();
    RemoteIMImageAttachment image;
    RemoteIMVoiceAttachment voice;
    RemoteIMFileAttachment file;
    RemoteIMVideoAttachment video;
    bool hasImage = false;
    bool hasVoice = false;
    bool hasFile = false;
    bool hasVideo = false;
};

Q_DECLARE_METATYPE(RemoteIMMessage)
Q_DECLARE_METATYPE(QList<RemoteIMMessage>)
