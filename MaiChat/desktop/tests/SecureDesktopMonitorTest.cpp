#include <QtTest/QtTest>

#include "remote/SecureDesktopMonitor.h"

using namespace RemoteDesktop;

namespace {

class FakeProbe final : public ISecureDesktopProbe {
public:
    bool active = false;
    int calls = 0;

    bool isSecureDesktopActive() override {
        ++calls;
        return active;
    }
};

}  // namespace

class SecureDesktopMonitorTest : public QObject {
    Q_OBJECT

private slots:
    void startsInactiveAndStaysQuiet();
    void reportsEnterOnlyAfterDebounce();
    void reportsLeaveWhenBackToNormal();
    void ignoresMomentaryFlicker();
    void reportsEachTransitionOnlyOnce();
    void realProbeReportsNormalDesktopAsNotSecure();
};

void SecureDesktopMonitorTest::realProbeReportsNormalDesktopAsNotSecure() {
    // 前面几条测的都是判定逻辑（用 Fake）；这条测**真实探测**本身。
    // 测试跑在普通桌面上，必须报 false。一旦这里误报，用户会在完全正常的
    // 情况下反复收到"对方电脑弹出了系统授权框"——比不做提示还糟。
    //
    // 这也顺带验证了"只有 ERROR_ACCESS_DENIED 才算安全桌面"这条判断：
    // 若把 OpenInputDesktop 的任意失败都当成安全桌面，这条就会挂。
    auto probe = createSecureDesktopProbe();
    QVERIFY(probe != nullptr);
    QVERIFY(!probe->isSecureDesktopActive());

    // 连续探测不该出现随机翻转。
    for (int i = 0; i < 20; ++i) QVERIFY(!probe->isSecureDesktopActive());
}

void SecureDesktopMonitorTest::startsInactiveAndStaysQuiet() {
    auto probe = std::make_unique<FakeProbe>();
    SecureDesktopMonitor monitor(std::move(probe));

    // 一切正常时不该报任何事件，否则提示条会一直闪。
    for (qint64 now = 0; now < 5000; now += 200) {
        QCOMPARE(monitor.poll(now), SecureDesktopMonitor::Change::None);
    }
    QVERIFY(!monitor.isActive());
}

void SecureDesktopMonitorTest::reportsEnterOnlyAfterDebounce() {
    auto probe = std::make_unique<FakeProbe>();
    auto* fake = probe.get();
    SecureDesktopMonitor monitor(std::move(probe));

    fake->active = true;
    // 刚观察到还不算数：桌面切换有过渡期，探测也可能瞬时抖动。
    QCOMPARE(monitor.poll(1000), SecureDesktopMonitor::Change::None);
    QCOMPARE(monitor.poll(1000 + SecureDesktopMonitor::kDebounceMs - 1),
             SecureDesktopMonitor::Change::None);
    QVERIFY(!monitor.isActive());

    // 稳定够久才认账。
    QCOMPARE(monitor.poll(1000 + SecureDesktopMonitor::kDebounceMs),
             SecureDesktopMonitor::Change::Entered);
    QVERIFY(monitor.isActive());
}

void SecureDesktopMonitorTest::reportsLeaveWhenBackToNormal() {
    auto probe = std::make_unique<FakeProbe>();
    auto* fake = probe.get();
    SecureDesktopMonitor monitor(std::move(probe));

    fake->active = true;
    monitor.poll(0);
    QCOMPARE(monitor.poll(SecureDesktopMonitor::kDebounceMs),
             SecureDesktopMonitor::Change::Entered);

    // 用户按掉 UAC 后要收起提示，否则会一直挂着"对方正在授权"。
    fake->active = false;
    monitor.poll(2000);
    QCOMPARE(monitor.poll(2000 + SecureDesktopMonitor::kDebounceMs),
             SecureDesktopMonitor::Change::Left);
    QVERIFY(!monitor.isActive());
}

void SecureDesktopMonitorTest::ignoresMomentaryFlicker() {
    auto probe = std::make_unique<FakeProbe>();
    auto* fake = probe.get();
    SecureDesktopMonitor monitor(std::move(probe));

    // 一闪而过的探测抖动不该产生提示。
    fake->active = true;
    QCOMPARE(monitor.poll(1000), SecureDesktopMonitor::Change::None);
    fake->active = false;
    QCOMPARE(monitor.poll(1100), SecureDesktopMonitor::Change::None);
    QVERIFY(!monitor.isActive());

    // 抖动之后又持续为真，仍要从头计时，不能把之前那次的时间算进去。
    fake->active = true;
    QCOMPARE(monitor.poll(1200), SecureDesktopMonitor::Change::None);
    QCOMPARE(monitor.poll(1200 + SecureDesktopMonitor::kDebounceMs - 1),
             SecureDesktopMonitor::Change::None);
    QCOMPARE(monitor.poll(1200 + SecureDesktopMonitor::kDebounceMs),
             SecureDesktopMonitor::Change::Entered);
}

void SecureDesktopMonitorTest::reportsEachTransitionOnlyOnce() {
    auto probe = std::make_unique<FakeProbe>();
    auto* fake = probe.get();
    SecureDesktopMonitor monitor(std::move(probe));

    fake->active = true;
    monitor.poll(0);
    QCOMPARE(monitor.poll(SecureDesktopMonitor::kDebounceMs),
             SecureDesktopMonitor::Change::Entered);

    // 持续处于安全桌面时不能反复上报，否则会给控制端刷屏。
    for (qint64 now = 2000; now < 20000; now += 500) {
        QCOMPARE(monitor.poll(now), SecureDesktopMonitor::Change::None);
    }
    QVERIFY(monitor.isActive());
}

QTEST_MAIN(SecureDesktopMonitorTest)
#include "SecureDesktopMonitorTest.moc"
