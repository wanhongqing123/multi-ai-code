#include <QtTest/QtTest>

#include "remote/RemoteInputInjector.h"

using namespace RemoteInput;

namespace {

// 记录型 Fake：只把落到系统的操作按顺序记下来，测试断言这串操作。
class RecordingSink final : public IRemoteInputSink {
public:
    QStringList calls;

    void moveTo(double x, double y) override {
        calls << QStringLiteral("move %1,%2").arg(x).arg(y);
    }
    void mouseButton(MouseButton button, bool pressed, double x, double y) override {
        Q_UNUSED(x);
        Q_UNUSED(y);
        calls << QStringLiteral("btn %1 %2")
                     .arg(static_cast<int>(button))
                     .arg(pressed ? QStringLiteral("down") : QStringLiteral("up"));
    }
    void wheel(int delta, double x, double y) override {
        Q_UNUSED(x);
        Q_UNUSED(y);
        calls << QStringLiteral("wheel %1").arg(delta);
    }
    void key(quint32 keyCode, bool pressed) override {
        calls << QStringLiteral("key %1 %2")
                     .arg(keyCode)
                     .arg(pressed ? QStringLiteral("down") : QStringLiteral("up"));
    }
    void text(const QString& value) override { calls << QStringLiteral("text %1").arg(value); }
};

constexpr quint32 kVkControl = 0x11;
constexpr quint32 kVkLeftWin = 0x5B;
constexpr quint32 kVkL = 0x4C;
constexpr quint32 kVkC = 0x43;

Packet makePacket(const QString& sessionId, quint32 sequence, const QVector<Event>& events) {
    Packet packet;
    packet.sessionId = sessionId;
    packet.sequence = sequence;
    packet.events = events;
    return packet;
}

Event keyEvent(quint32 keyCode, bool pressed) {
    Event event;
    event.type = EventType::Key;
    event.keyCode = keyCode;
    event.pressed = pressed;
    return event;
}

Event buttonEvent(MouseButton button, bool pressed) {
    Event event;
    event.type = EventType::MouseButton;
    event.button = button;
    event.pressed = pressed;
    return event;
}

Event moveEvent(double x, double y) {
    Event event;
    event.type = EventType::MouseMove;
    event.x = x;
    event.y = y;
    return event;
}

}  // namespace

class RemoteInputInjectorTest : public QObject {
    Q_OBJECT

private slots:
    void rejectsInputWhenNoSessionActive();
    void rejectsInputFromAnotherSession();
    void appliesEventsAndTracksHeldState();
    void releasesEverythingWhenSessionEnds();
    void releasesEverythingOnReliableSequenceGap();
    void dropsStaleUnreliablePacketsButNeverReleasesOnGap();
    void watchdogReleasesAfterSilence();
    void blocksWinPlusLButKeepsOtherCombinations();
    void releaseAllEventClearsHeldState();
    void mapsNormalizedCoordinatesOntoVirtualDesktop();
    void reportsInjectionGeometryForDiagnostics();
    void remembersLastInjectedMoveForDiagnostics();
};

void RemoteInputInjectorTest::reportsInjectionGeometryForDiagnostics() {
    const QString description = describeInjectionGeometry();
    QVERIFY2(!description.isEmpty(), "注入端几何摘要为空，被控端日志会缺一半信息");
#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
    // 排查坐标偏移时，被控端的主屏尺寸是必须能和控制端 contentRect 对起来的
    // 那个数；缺了它日志就只能看出"有偏差"，看不出偏多少。
    QVERIFY2(description.contains(QLatin1String("primary=")),
             qPrintable(QStringLiteral("几何摘要里没有主屏尺寸：%1").arg(description)));
    QVERIFY2(description.contains(QLatin1String("aspect=")),
             qPrintable(QStringLiteral("几何摘要里没有宽高比：%1").arg(description)));
#endif
}

void RemoteInputInjectorTest::remembersLastInjectedMoveForDiagnostics() {
    RemoteInputInjector injector(std::make_unique<RecordingSink>());
    QVERIFY2(!injector.hasLastMove(), "还没注入过就报告有坐标样本");

    injector.beginSession(QStringLiteral("s1"));
    Event move;
    move.type = EventType::MouseMove;
    move.x = 0.25;
    move.y = 0.75;
    QVERIFY(injector.handlePacket(makePacket(QStringLiteral("s1"), 1, {move}),
                                  Channel::Unreliable, 1000));

    QVERIFY(injector.hasLastMove());
    QVERIFY(qAbs(injector.lastMoveX() - 0.25) < 1e-9);
    QVERIFY(qAbs(injector.lastMoveY() - 0.75) < 1e-9);
}

void RemoteInputInjectorTest::mapsNormalizedCoordinatesOntoVirtualDesktop() {
    // 单屏 1920x1080：主屏即整个虚拟桌面。
    VirtualDesktopRect screen{0, 0, 1920, 1080};
    int x = 0;
    int y = 0;

    normalizedToVirtualDesktop(0.0, 0.0, screen, screen, &x, &y);
    QCOMPARE(x, 0);
    QCOMPARE(y, 0);

    // 右下角必须能到 65535，否则最右/最下那一列永远点不到
    // （拿 width 而不是 width-1 当分母就会差这一列）。
    normalizedToVirtualDesktop(1.0, 1.0, screen, screen, &x, &y);
    QCOMPARE(x, 65535);
    QCOMPARE(y, 65535);

    // 容差按"一个像素折合多少个 65535 单位"来算才有意义。用 width-1 作分母
    // 会带来固定半像素的偏移，那是这个约定本身的代价（换来最后一列可达），
    // 不是 bug——所以容差写死成 20 这种数字只会误报。
    const int unitsPerPixelX = 65535 / screen.width;
    const int unitsPerPixelY = 65535 / screen.height;
    normalizedToVirtualDesktop(0.5, 0.5, screen, screen, &x, &y);
    QVERIFY(qAbs(x - 32767) <= unitsPerPixelX);
    QVERIFY(qAbs(y - 32767) <= unitsPerPixelY);

    // 双屏：主屏在右、副屏在左，虚拟桌面从 -1920 起。我们只采主屏，
    // 所以主屏的中点应落在整个虚拟桌面的 3/4 处，而不是正中间。
    const VirtualDesktopRect primary{0, 0, 1920, 1080};
    const VirtualDesktopRect virtualDesktop{-1920, 0, 3840, 1080};
    normalizedToVirtualDesktop(0.5, 0.5, primary, virtualDesktop, &x, &y);
    QVERIFY2(qAbs(x - static_cast<int>(65535 * 0.75)) <= 40,
             qPrintable(QStringLiteral("双屏下主屏中点算到了 %1").arg(x)));

    // 主屏左上角落在虚拟桌面的正中间（副屏占了左半边）。
    normalizedToVirtualDesktop(0.0, 0.0, primary, virtualDesktop, &x, &y);
    QVERIFY(qAbs(x - 32767) <= 40);

    // 越界输入必须钳住，不能算出负数或超过 65535 的坐标。
    normalizedToVirtualDesktop(-5.0, 9.0, screen, screen, &x, &y);
    QCOMPARE(x, 0);
    QCOMPARE(y, 65535);

    // 虚拟桌面尺寸非法时不能除零。
    normalizedToVirtualDesktop(0.5, 0.5, screen, VirtualDesktopRect{0, 0, 0, 0}, &x, &y);
    QCOMPARE(x, 0);
    QCOMPARE(y, 0);
}

void RemoteInputInjectorTest::rejectsInputWhenNoSessionActive() {
    auto sink = std::make_unique<RecordingSink>();
    auto* recorder = sink.get();
    RemoteInputInjector injector(std::move(sink));

    QVERIFY(!injector.isSessionActive());
    // 没有会话就收到输入包，必须一律拒绝——否则等于没授权也能操作电脑。
    QVERIFY(!injector.handlePacket(makePacket(QStringLiteral("s1"), 1, {moveEvent(0.5, 0.5)}),
                                   Channel::Unreliable, 1000));
    QVERIFY(recorder->calls.isEmpty());
}

void RemoteInputInjectorTest::rejectsInputFromAnotherSession() {
    auto sink = std::make_unique<RecordingSink>();
    auto* recorder = sink.get();
    RemoteInputInjector injector(std::move(sink));
    injector.beginSession(QStringLiteral("s1"));

    // 上一场会话的残留包不该操作这一场的电脑。
    QVERIFY(!injector.handlePacket(makePacket(QStringLiteral("s0"), 1, {moveEvent(0.5, 0.5)}),
                                   Channel::Unreliable, 1000));
    QVERIFY(recorder->calls.isEmpty());

    QVERIFY(injector.handlePacket(makePacket(QStringLiteral("s1"), 1, {moveEvent(0.5, 0.5)}),
                                  Channel::Unreliable, 1000));
    QCOMPARE(recorder->calls.size(), 1);
}

void RemoteInputInjectorTest::appliesEventsAndTracksHeldState() {
    auto sink = std::make_unique<RecordingSink>();
    auto* recorder = sink.get();
    RemoteInputInjector injector(std::move(sink));
    injector.beginSession(QStringLiteral("s1"));

    Event wheel;
    wheel.type = EventType::MouseWheel;
    wheel.wheelDelta = 120;
    Event text;
    text.type = EventType::Text;
    text.text = QStringLiteral("你好");

    QVERIFY(injector.handlePacket(
        makePacket(QStringLiteral("s1"), 1,
                   {moveEvent(0.25, 0.5), buttonEvent(MouseButton::Left, true),
                    keyEvent(kVkControl, true), wheel, text}),
        Channel::Reliable, 1000));

    QCOMPARE(recorder->calls,
             QStringList({QStringLiteral("move 0.25,0.5"), QStringLiteral("btn 0 down"),
                          QStringLiteral("key 17 down"), QStringLiteral("wheel 120"),
                          QStringLiteral("text 你好")}));
    QCOMPARE(injector.heldKeys(), QVector<quint32>({kVkControl}));
    QCOMPARE(injector.heldButtons(), QVector<MouseButton>({MouseButton::Left}));

    // 抬起后不能还记着。
    QVERIFY(injector.handlePacket(makePacket(QStringLiteral("s1"), 2,
                                             {keyEvent(kVkControl, false),
                                              buttonEvent(MouseButton::Left, false)}),
                                  Channel::Reliable, 1100));
    QVERIFY(injector.heldKeys().isEmpty());
    QVERIFY(injector.heldButtons().isEmpty());
    QVERIFY(!injector.hasAnythingHeld());
}

void RemoteInputInjectorTest::releasesEverythingWhenSessionEnds() {
    auto sink = std::make_unique<RecordingSink>();
    auto* recorder = sink.get();
    RemoteInputInjector injector(std::move(sink));
    injector.beginSession(QStringLiteral("s1"));
    injector.handlePacket(makePacket(QStringLiteral("s1"), 1,
                                     {keyEvent(kVkControl, true),
                                      buttonEvent(MouseButton::Left, true)}),
                          Channel::Reliable, 1000);
    recorder->calls.clear();

    // Ctrl 按着的时候会话断了，被控机会一直是 Ctrl 按住状态，点什么都变成
    // Ctrl+点击，而人不在电脑旁没法自己解。结束会话必须把按住的全抬了。
    injector.endSession();
    QVERIFY(recorder->calls.contains(QStringLiteral("key 17 up")));
    QVERIFY(recorder->calls.contains(QStringLiteral("btn 0 up")));
    QVERIFY(!injector.hasAnythingHeld());
    QVERIFY(!injector.isSessionActive());
}

void RemoteInputInjectorTest::releasesEverythingOnReliableSequenceGap() {
    auto sink = std::make_unique<RecordingSink>();
    auto* recorder = sink.get();
    RemoteInputInjector injector(std::move(sink));
    injector.beginSession(QStringLiteral("s1"));
    injector.handlePacket(makePacket(QStringLiteral("s1"), 1, {keyEvent(kVkControl, true)}),
                          Channel::Reliable, 1000);
    recorder->calls.clear();

    // 可靠有序通道本不该跳号。跳了说明中间的抬起包多半丢了，先全抬再继续，
    // 否则 Ctrl 会一直悬空。
    QVERIFY(injector.handlePacket(makePacket(QStringLiteral("s1"), 9, {keyEvent(kVkC, true)}),
                                  Channel::Reliable, 1200));
    QCOMPARE(recorder->calls,
             QStringList({QStringLiteral("key 17 up"), QStringLiteral("key 67 down")}));
    QCOMPARE(injector.heldKeys(), QVector<quint32>({kVkC}));

    // 连号不触发全抬。
    recorder->calls.clear();
    QVERIFY(injector.handlePacket(makePacket(QStringLiteral("s1"), 10, {keyEvent(kVkC, false)}),
                                  Channel::Reliable, 1300));
    QCOMPARE(recorder->calls, QStringList({QStringLiteral("key 67 up")}));
}

void RemoteInputInjectorTest::dropsStaleUnreliablePacketsButNeverReleasesOnGap() {
    auto sink = std::make_unique<RecordingSink>();
    auto* recorder = sink.get();
    RemoteInputInjector injector(std::move(sink));
    injector.beginSession(QStringLiteral("s1"));
    injector.handlePacket(makePacket(QStringLiteral("s1"), 1, {keyEvent(kVkControl, true)}),
                          Channel::Reliable, 1000);
    recorder->calls.clear();

    QVERIFY(injector.handlePacket(makePacket(QStringLiteral("s1"), 10, {moveEvent(0.5, 0.5)}),
                                  Channel::Unreliable, 1100));
    // 乱序到的旧位置要丢掉，否则光标会往回跳。
    QVERIFY(!injector.handlePacket(makePacket(QStringLiteral("s1"), 4, {moveEvent(0.1, 0.1)}),
                                   Channel::Unreliable, 1150));
    QVERIFY(injector.handlePacket(makePacket(QStringLiteral("s1"), 11, {moveEvent(0.6, 0.6)}),
                                  Channel::Unreliable, 1200));

    QCOMPARE(recorder->calls,
             QStringList({QStringLiteral("move 0.5,0.5"), QStringLiteral("move 0.6,0.6")}));
    // 不可靠通道丢包是设计的一部分，跳号绝不能触发全抬——否则正常移动
    // 每丢一包就把用户按住的键放开一次，拖拽根本没法用。
    QCOMPARE(injector.heldKeys(), QVector<quint32>({kVkControl}));
}

void RemoteInputInjectorTest::watchdogReleasesAfterSilence() {
    auto sink = std::make_unique<RecordingSink>();
    auto* recorder = sink.get();
    RemoteInputInjector injector(std::move(sink));
    injector.beginSession(QStringLiteral("s1"));
    injector.handlePacket(makePacket(QStringLiteral("s1"), 1, {keyEvent(kVkControl, true)}),
                          Channel::Reliable, 1000);
    recorder->calls.clear();

    // 没到超时不动手。
    injector.tickWatchdog(1000 + RemoteInputInjector::kSilenceTimeoutMs - 1);
    QVERIFY(recorder->calls.isEmpty());
    QVERIFY(injector.hasAnythingHeld());

    // 链路静默到超时——控制端可能已经崩了，抬起包永远不会来了。
    injector.tickWatchdog(1000 + RemoteInputInjector::kSilenceTimeoutMs);
    QCOMPARE(recorder->calls, QStringList({QStringLiteral("key 17 up")}));
    QVERIFY(!injector.hasAnythingHeld());

    // 已经抬干净了就不该重复触发。
    recorder->calls.clear();
    injector.tickWatchdog(99999);
    QVERIFY(recorder->calls.isEmpty());
}

void RemoteInputInjectorTest::blocksWinPlusLButKeepsOtherCombinations() {
    auto sink = std::make_unique<RecordingSink>();
    auto* recorder = sink.get();
    RemoteInputInjector injector(std::move(sink));
    injector.beginSession(QStringLiteral("s1"));

    // Win+L 注得进去，但按下去被控机立刻进安全桌面，远程直接失联且再也解不开。
    injector.handlePacket(makePacket(QStringLiteral("s1"), 1, {keyEvent(kVkLeftWin, true)}),
                          Channel::Reliable, 1000);
    recorder->calls.clear();
    injector.handlePacket(makePacket(QStringLiteral("s1"), 2, {keyEvent(kVkL, true)}),
                          Channel::Reliable, 1100);
    QVERIFY(recorder->calls.isEmpty());
    // 被拦的键不能计入按住集合，否则会话结束时会抬一个从没按下的键。
    QCOMPARE(injector.heldKeys(), QVector<quint32>({kVkLeftWin}));

    // 单独按 L 正常。
    injector.handlePacket(makePacket(QStringLiteral("s1"), 3, {keyEvent(kVkLeftWin, false)}),
                          Channel::Reliable, 1200);
    recorder->calls.clear();
    injector.handlePacket(makePacket(QStringLiteral("s1"), 4, {keyEvent(kVkL, true)}),
                          Channel::Reliable, 1300);
    QCOMPARE(recorder->calls, QStringList({QStringLiteral("key 76 down")}));

    // Ctrl+L 之类的普通组合不受影响，别拦过头。
    QVERIFY(!isBlockedKeyCombination(kVkL, QSet<quint32>({kVkControl})));
    QVERIFY(isBlockedKeyCombination(kVkL, QSet<quint32>({kVkLeftWin})));
    QVERIFY(!isBlockedKeyCombination(kVkC, QSet<quint32>({kVkLeftWin})));
}

void RemoteInputInjectorTest::releaseAllEventClearsHeldState() {
    auto sink = std::make_unique<RecordingSink>();
    auto* recorder = sink.get();
    RemoteInputInjector injector(std::move(sink));
    injector.beginSession(QStringLiteral("s1"));
    injector.handlePacket(makePacket(QStringLiteral("s1"), 1,
                                     {keyEvent(kVkControl, true),
                                      buttonEvent(MouseButton::Right, true)}),
                          Channel::Reliable, 1000);
    recorder->calls.clear();

    Event releaseAll;
    releaseAll.type = EventType::ReleaseAll;
    // 控制端主动要求全抬（比如它那边窗口失焦了）。
    injector.handlePacket(makePacket(QStringLiteral("s1"), 2, {releaseAll}),
                          Channel::Reliable, 1100);
    QVERIFY(recorder->calls.contains(QStringLiteral("key 17 up")));
    QVERIFY(recorder->calls.contains(QStringLiteral("btn 1 up")));
    QVERIFY(!injector.hasAnythingHeld());
}

QTEST_MAIN(RemoteInputInjectorTest)
#include "RemoteInputInjectorTest.moc"
