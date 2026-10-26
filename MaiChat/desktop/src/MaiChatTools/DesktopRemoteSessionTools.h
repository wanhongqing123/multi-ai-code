// Agent entry points for managing Sessions on another signed-in MaiChat device.
#pragma once

#include <functional>

class DesktopSessionSyncCoordinator;
class MaiToolRegistry;
class QObject;

void registerDesktopRemoteSessionTools(
    MaiToolRegistry& registry, QObject* dispatcher,
    std::function<DesktopSessionSyncCoordinator*()> coordinatorProvider);
