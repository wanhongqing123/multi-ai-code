#pragma once

#include <functional>

#include <QString>

#include "auth/DesktopVideoCircleAccess.h"

class MaiToolRegistry;
class QObject;

// The Desktop adapter supplies account credentials, private task storage, and
// player presentation. Every HTTP/multipart/download byte still uses the same
// shared MaiCloudDriveClient and MaiCurlDownload libraries as the normal UI.
void registerDesktopCloudDriveTools(MaiToolRegistry& registry, std::function<DesktopVideoCircleAccess()> accessProvider,
                                    QObject* presenter,
                                    std::function<bool(const QString&, bool isImage)> openDownloadedMedia);
