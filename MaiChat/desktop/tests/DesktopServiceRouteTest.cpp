#include <QTest>

#include "network/DesktopServiceRoute.h"

class DesktopServiceRouteTest final : public QObject {
    Q_OBJECT

private slots:
    void healthyProxyRemainsSelected();
    void failedProxyUsesVerifiedDirectRouteForSocket();
    void unverifiedDirectDoesNotReplaceConfiguredProxy();
    void invalidatedRouteIsReprobedOnNextRequest();
};

void DesktopServiceRouteTest::healthyProxyRemainsSelected() {
    const DesktopServiceProxyRoute configured{
        DesktopServiceProxyMode::HttpProxy, QStringLiteral("http://127.0.0.1:32001/")};
    int probes = 0;
    const auto selected = DesktopServiceRoute::choose(configured,
        [&](const DesktopServiceProxyRoute& route) {
            ++probes;
            return route.mode == DesktopServiceProxyMode::HttpProxy;
        });
    QCOMPARE(probes, 1);
    QCOMPARE(selected.mode, DesktopServiceProxyMode::HttpProxy);
    QCOMPARE(DesktopServiceRoute::current(configured).proxyUrl, configured.proxyUrl);
}

void DesktopServiceRouteTest::failedProxyUsesVerifiedDirectRouteForSocket() {
    const DesktopServiceProxyRoute configured{
        DesktopServiceProxyMode::HttpProxy, QStringLiteral("http://127.0.0.1:32002/")};
    int probes = 0;
    const auto selected = DesktopServiceRoute::choose(configured,
        [&](const DesktopServiceProxyRoute& route) {
            ++probes;
            return route.mode == DesktopServiceProxyMode::Direct;
        });
    QCOMPARE(probes, 2);
    QCOMPARE(selected.mode, DesktopServiceProxyMode::Direct);
    QCOMPARE(DesktopServiceRoute::current(configured).mode, DesktopServiceProxyMode::Direct);
}

void DesktopServiceRouteTest::unverifiedDirectDoesNotReplaceConfiguredProxy() {
    const DesktopServiceProxyRoute configured{
        DesktopServiceProxyMode::HttpProxy, QStringLiteral("http://127.0.0.1:32003/")};
    int probes = 0;
    const auto selected = DesktopServiceRoute::choose(configured,
        [&](const DesktopServiceProxyRoute&) {
            ++probes;
            return false;
        });
    QCOMPARE(probes, 2);
    QCOMPARE(selected.mode, DesktopServiceProxyMode::HttpProxy);
    QCOMPARE(selected.proxyUrl, configured.proxyUrl);
    QCOMPARE(DesktopServiceRoute::current(configured).mode, DesktopServiceProxyMode::HttpProxy);
}

void DesktopServiceRouteTest::invalidatedRouteIsReprobedOnNextRequest() {
    const DesktopServiceProxyRoute configured{
        DesktopServiceProxyMode::HttpProxy, QStringLiteral("http://127.0.0.1:32004/")};
    const auto first = DesktopServiceRoute::choose(configured,
        [](const DesktopServiceProxyRoute& route) {
            return route.mode == DesktopServiceProxyMode::HttpProxy;
        });
    QCOMPARE(first.mode, DesktopServiceProxyMode::HttpProxy);
    DesktopServiceRoute::invalidate(first);
    int probes = 0;
    const auto second = DesktopServiceRoute::choose(configured,
        [&](const DesktopServiceProxyRoute& route) {
            ++probes;
            return route.mode == DesktopServiceProxyMode::Direct;
        });
    QCOMPARE(probes, 2);
    QCOMPARE(second.mode, DesktopServiceProxyMode::Direct);
}

QTEST_GUILESS_MAIN(DesktopServiceRouteTest)
#include "DesktopServiceRouteTest.moc"
