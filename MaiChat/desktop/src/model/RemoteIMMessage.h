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

enum class RemoteIMApprovalAction {
    ApproveOnce,
    ApprovePrefix,
    Reject
};

inline QString remoteIMApprovalActionWireName(RemoteIMApprovalAction action) {
    switch (action) {
        case RemoteIMApprovalAction::ApproveOnce: return QStringLiteral("approve-once");
        case RemoteIMApprovalAction::ApprovePrefix: return QStringLiteral("approve-prefix");
        case RemoteIMApprovalAction::Reject: return QStringLiteral("reject");
    }
    return QString();
}

inline QString remoteIMApprovalActionTitle(RemoteIMApprovalAction action) {
    switch (action) {
        case RemoteIMApprovalAction::ApproveOnce: return QStringLiteral("同意本次");
        case RemoteIMApprovalAction::ApprovePrefix: return QStringLiteral("同意并记住");
        case RemoteIMApprovalAction::Reject: return QStringLiteral("拒绝");
    }
    return QString();
}

inline bool remoteIMApprovalActionFromWireName(const QString& value,
                                               RemoteIMApprovalAction* action) {
    if (value == QStringLiteral("approve-once")) {
        if (action) *action = RemoteIMApprovalAction::ApproveOnce;
        return true;
    }
    if (value == QStringLiteral("approve-prefix")) {
        if (action) *action = RemoteIMApprovalAction::ApprovePrefix;
        return true;
    }
    if (value == QStringLiteral("reject")) {
        if (action) *action = RemoteIMApprovalAction::Reject;
        return true;
    }
    return false;
}

inline bool isValidRemoteIMApprovalToken(const QString& token) {
    constexpr auto prefix = "approval-";
    if (!token.startsWith(QLatin1String(prefix)) || token.size() <= 9 || token.size() > 200) {
        return false;
    }
    for (const QChar character : token) {
        const ushort code = character.unicode();
        const bool asciiAlphaNumeric = (code >= 'a' && code <= 'z')
            || (code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9');
        if (!asciiAlphaNumeric && code != '-' && code != '_') return false;
    }
    return true;
}

struct RemoteIMApprovalRequest {
    QString token;
    QList<RemoteIMApprovalAction> actions;

    bool isValid() const {
        if (!isValidRemoteIMApprovalToken(token) || actions.size() < 2 || actions.size() > 3) {
            return false;
        }
        bool hasApproveOnce = false;
        bool hasApprovePrefix = false;
        bool hasReject = false;
        for (RemoteIMApprovalAction action : actions) {
            bool* seen = action == RemoteIMApprovalAction::ApproveOnce
                ? &hasApproveOnce
                : action == RemoteIMApprovalAction::ApprovePrefix ? &hasApprovePrefix : &hasReject;
            if (*seen) return false;
            *seen = true;
        }
        return hasApproveOnce && hasReject;
    }

    bool allows(RemoteIMApprovalAction action) const { return actions.contains(action); }
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
    RemoteIMApprovalRequest approvalRequest;
    qint64 createdAtMillis = QDateTime::currentMSecsSinceEpoch();
    RemoteIMImageAttachment image;
    RemoteIMVoiceAttachment voice;
    RemoteIMFileAttachment file;
    RemoteIMVideoAttachment video;
    bool hasImage = false;
    bool hasVoice = false;
    bool hasFile = false;
    bool hasVideo = false;
    bool hasApprovalRequest = false;
};

Q_DECLARE_METATYPE(RemoteIMMessage)
Q_DECLARE_METATYPE(QList<RemoteIMMessage>)
