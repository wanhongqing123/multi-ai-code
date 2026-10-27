#include "network/DesktopServiceRoute.h"

#include <QMutex>
#include <QMutexLocker>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>

namespace {

QMutex& routeMutex() {
    static QMutex mutex;
    return mutex;
}

DesktopServiceProxyRoute& configuredRoute() {
    static DesktopServiceProxyRoute route;
    return route;
}

DesktopServiceProxyRoute& selectedRoute() {
    static DesktopServiceProxyRoute route;
    return route;
}

bool sameRoute(const DesktopServiceProxyRoute& left, const DesktopServiceProxyRoute& right) {
    return left.mode == right.mode && left.proxyUrl == right.proxyUrl;
}

void remember(const DesktopServiceProxyRoute& configured,
              const DesktopServiceProxyRoute& selected) {
    QMutexLocker lock(&routeMutex());
    configuredRoute() = configured;
    selectedRoute() = selected;
}

}  // namespace

DesktopServiceProxyRoute DesktopServiceRoute::configured(const QUrl& url) {
    const auto proxies = QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(url));
    if (proxies.isEmpty() || proxies.first().type() == QNetworkProxy::NoProxy ||
        proxies.first().type() == QNetworkProxy::DefaultProxy) {
        return {DesktopServiceProxyMode::Direct, {}};
    }
    const QNetworkProxy proxy = proxies.first();
    if (proxy.type() != QNetworkProxy::HttpProxy || proxy.hostName().isEmpty() ||
        proxy.port() == 0) {
        return {DesktopServiceProxyMode::Unsupported, {}};
    }
    QUrl address;
    address.setScheme(QStringLiteral("http"));
    address.setHost(proxy.hostName());
    address.setPort(proxy.port());
    address.setPath(QStringLiteral("/"));
    return {DesktopServiceProxyMode::HttpProxy, address.toString(QUrl::FullyEncoded)};
}

DesktopServiceProxyRoute DesktopServiceRoute::choose(
    const DesktopServiceProxyRoute& configured, const Probe& probe) {
    if (configured.mode != DesktopServiceProxyMode::HttpProxy) return configured;
    {
        QMutexLocker lock(&routeMutex());
        if (sameRoute(configuredRoute(), configured) &&
            selectedRoute().mode != DesktopServiceProxyMode::Unavailable) {
            return selectedRoute();
        }
    }
    if (probe(configured)) {
        remember(configured, configured);
        return configured;
    }
    const DesktopServiceProxyRoute direct{DesktopServiceProxyMode::Direct, {}};
    if (probe(direct)) {
        remember(configured, direct);
        return direct;
    }
    // A slow but required proxy might still complete the real request. Do not
    // select unverified direct access when neither short probe succeeded.
    return configured;
}

DesktopServiceProxyRoute DesktopServiceRoute::current(
    const DesktopServiceProxyRoute& configured) {
    QMutexLocker lock(&routeMutex());
    if (sameRoute(configuredRoute(), configured) &&
        selectedRoute().mode != DesktopServiceProxyMode::Unavailable) {
        return selectedRoute();
    }
    return configured;
}

void DesktopServiceRoute::invalidate(const DesktopServiceProxyRoute& route) {
    QMutexLocker lock(&routeMutex());
    if (sameRoute(selectedRoute(), route)) {
        selectedRoute() = {};
    }
}
