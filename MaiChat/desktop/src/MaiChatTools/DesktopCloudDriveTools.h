#pragma once

#include <functional>
#include <memory>

#include <QString>

#include "auth/DesktopVideoCircleAccess.h"

class MaiToolRegistry;
class QObject;
struct MaiCloudVod;

// The Desktop adapter supplies account credentials, private task storage, and
// player presentation. Every HTTP/multipart/download byte still uses the same
// shared MaiCloudDriveClient and MaiCurlDownload libraries as the normal UI.
void registerDesktopCloudDriveTools(MaiToolRegistry& registry, std::function<DesktopVideoCircleAccess()> accessProvider,
                                    QObject* presenter,
                                    std::function<bool(const QString&, bool isImage,
                                                       std::shared_ptr<MaiCloudVod>)> openMedia);
