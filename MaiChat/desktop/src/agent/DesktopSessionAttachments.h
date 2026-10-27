#pragma once

#include "auth/DesktopVideoCircleAccess.h"

#include <QJsonObject>
#include <QString>

struct DesktopSessionRoom {
    QString slot;
    QString login;
    QString session;
    QString key() const { return slot + QLatin1Char(':') + session; }
};

struct DesktopSessionAttachmentResult {
    QJsonObject metadata;
    QString path;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

// Synchronous adapter; callers run upload/download on a worker thread.
class DesktopSessionAttachments final {
public:
    static DesktopSessionAttachmentResult upload(const DesktopVideoCircleAccess& access,
        const DesktopSessionRoom& room, const QString& source);
    static DesktopSessionAttachmentResult download(const DesktopVideoCircleAccess& access,
        const DesktopSessionRoom& room, const QString& attachmentId);
    static DesktopSessionAttachmentResult cleanupRoom(const DesktopVideoCircleAccess& access,
        const DesktopSessionRoom& room);
};
