#pragma once

#include <QMetaType>
#include <QList>
#include <QString>

struct RemoteIMContact {
    QString userId;
    QString displayName;
    QString avatarUrl;
};

Q_DECLARE_METATYPE(RemoteIMContact)
Q_DECLARE_METATYPE(QList<RemoteIMContact>)
