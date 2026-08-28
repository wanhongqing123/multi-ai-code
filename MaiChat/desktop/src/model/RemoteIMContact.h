#pragma once

#include <QMetaType>
#include <QList>
#include <QString>

struct RemoteIMContact {
    QString userId;
    QString displayName;
    QString avatarUrl;
    // 所属分组名。空串 = 未分组（不是「分组名恰好是空字符串」——空名建不出来）。
    // 分组是纯本地概念：联系人本身就不跨端同步，分组自然也各端各存。
    QString groupName;
};

Q_DECLARE_METATYPE(RemoteIMContact)
Q_DECLARE_METATYPE(QList<RemoteIMContact>)
