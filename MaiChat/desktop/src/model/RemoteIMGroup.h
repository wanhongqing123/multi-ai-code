#pragma once

#include <QList>
#include <QString>

struct RemoteIMGroup {
    QString groupId;
    QString name;
    int memberCount = 0;
    bool pendingActivation = false;
    // -1 means this source did not provide a server unread count.
    int unreadCount = -1;
};

struct RemoteIMGroupMessage {
    QString id;
    QString groupId;
    QString senderId;
    QString text;
    qint64 createdAtMillis = 0;
    qint64 sequence = 0;
    bool fromSelf = false;
    bool pending = false;
    bool failed = false;
};

struct RemoteIMGroupHistoryPage {
    QList<RemoteIMGroupMessage> messages;
    qint64 nextSequence = 0;
    bool hasMore = false;
};

Q_DECLARE_METATYPE(RemoteIMGroup)
Q_DECLARE_METATYPE(QList<RemoteIMGroup>)
Q_DECLARE_METATYPE(RemoteIMGroupMessage)
Q_DECLARE_METATYPE(QList<RemoteIMGroupMessage>)
