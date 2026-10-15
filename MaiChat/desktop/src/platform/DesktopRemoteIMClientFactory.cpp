// 文件作用：桌面端操作系统能力适配；DesktopRemoteIMClientFactory 负责对象创建和配置装配，本文件给出其实现。
// 直接依赖：im/FakeRemoteIMClient.h、platform/MacRemoteIMClient.h、platform/WindowsRemoteIMClient.h、platform/UnsupportedRemoteIMClient.h；实现与这些接口协作。
#include "platform/DesktopRemoteIMClientFactory.h"

#include <QtGlobal>

#if defined(MAICHAT_USE_FAKE_CLIENT)
#include "im/FakeRemoteIMClient.h"
#elif defined(Q_OS_MACOS)
#include "platform/MacRemoteIMClient.h"
#elif defined(Q_OS_WIN)
#include "platform/WindowsRemoteIMClient.h"
#else
#include "platform/UnsupportedRemoteIMClient.h"
#endif

std::unique_ptr<RemoteIMClient> createDesktopRemoteIMClient() {
#if defined(MAICHAT_USE_FAKE_CLIENT)
    return std::make_unique<FakeRemoteIMClient>();
#elif defined(Q_OS_MACOS)
    return std::make_unique<MacRemoteIMClient>();
#elif defined(Q_OS_WIN)
    return std::make_unique<WindowsRemoteIMClient>();
#else
    return std::make_unique<UnsupportedRemoteIMClient>();
#endif
}
