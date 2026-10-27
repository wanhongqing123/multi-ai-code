// Select one verified route for MaiChat's own account and Session services.
// A route is chosen before side-effecting requests and is shared with WSS.
#pragma once

#include <QString>
#include <QUrl>

#include <functional>

enum class DesktopServiceProxyMode {
    Direct,
    HttpProxy,
    Unsupported,
    Unavailable,
};

struct DesktopServiceProxyRoute {
    DesktopServiceProxyMode mode = DesktopServiceProxyMode::Unavailable;
    QString proxyUrl;
};

class DesktopServiceRoute final {
public:
    using Probe = std::function<bool(const DesktopServiceProxyRoute&)>;

    static DesktopServiceProxyRoute configured(const QUrl& url);
    static DesktopServiceProxyRoute choose(const DesktopServiceProxyRoute& configured,
                                           const Probe& probe);
    static DesktopServiceProxyRoute current(const DesktopServiceProxyRoute& configured);
    static void invalidate(const DesktopServiceProxyRoute& route);
};
