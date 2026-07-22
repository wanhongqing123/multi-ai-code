#include "platform/DesktopRemoteIMClientFactory.h"

#include <QtGlobal>

#if defined(MULTI_AI_IM_USE_FAKE_CLIENT)
#include "im/FakeRemoteIMClient.h"
#elif defined(Q_OS_MACOS)
#include "platform/MacRemoteIMClient.h"
#elif defined(Q_OS_WIN)
#include "platform/WindowsRemoteIMClient.h"
#else
#include "platform/UnsupportedRemoteIMClient.h"
#endif

std::unique_ptr<RemoteIMClient> createDesktopRemoteIMClient() {
#if defined(MULTI_AI_IM_USE_FAKE_CLIENT)
    return std::make_unique<FakeRemoteIMClient>();
#elif defined(Q_OS_MACOS)
    return std::make_unique<MacRemoteIMClient>();
#elif defined(Q_OS_WIN)
    return std::make_unique<WindowsRemoteIMClient>();
#else
    return std::make_unique<UnsupportedRemoteIMClient>();
#endif
}
