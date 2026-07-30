#include <QtTest/QtTest>

#include "ui/RemoteInputCapture.h"

using namespace RemoteInput;

namespace {

QVector<Packet> drain(RemoteInputSender& sender, qint64 nowMs) {
    QVector<Packet> packets;
    for (const auto& item : sender.flush(nowMs)) {
        Packet packet;
        if (decodePacket(item.payload, &packet)) packets.append(packet);
    }
    return packets;
}

QVector<Event> allEvents(const QVector<Packet>& packets) {
    QVector<Event> events;
    for (const auto& packet : packets) events += packet.events;
    return events;
}

bool hasType(const QVector<Event>& events, EventType type) {
    for (const auto& event : events) {
        if (event.type == type) return true;
    }
    return false;
}

}  // namespace

class RemoteInputCaptureTest : public QObject {
    Q_OBJECT

private slots:
    void capturesNothingUntilEnabled();
    void mapsClickThroughLetterbox();
    void ignoresClicksOnLetterboxButKeepsDragging();
    void alwaysSendsReleaseEvenOutsideContent();
    void releasesEverythingWhenDisabledOrFocusLost();
    void recomputesContentRectOnResize();
    void emergencyHotkeyStopsControlAndIsNotForwarded();
    void sendsCanonicalKeyCodesWhenNativeCodeIsZero();
};

void RemoteInputCaptureTest::sendsCanonicalKeyCodesWhenNativeCodeIsZero() {
    RemoteInputSender sender;
    sender.beginSession(QStringLiteral("s1"));
    QWidget surface;
    surface.resize(800, 450);

    RemoteInputCapture capture(sender);
    capture.attachTo(&surface);
    capture.setRemoteVideoSize(QSize(1920, 1080));
    capture.setEnabled(true);
    drain(sender, 1000);

    // macOS 的 A 键原生 CGKeyCode 就是 0，旧实现会把它误判为“没有键码”。
    QKeyEvent press(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, 0, 0, 0,
                    QStringLiteral("a"));
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier, 0, 0, 0,
                      QStringLiteral("a"));
    QCoreApplication::sendEvent(&surface, &press);
    QCoreApplication::sendEvent(&surface, &release);

    const auto events = allEvents(drain(sender, 1100));
    int keyEventCount = 0;
    for (const auto& event : events) {
        if (event.type != EventType::Key) continue;
        QCOMPARE(event.keyCode, 0x41u);
        ++keyEventCount;
    }
    QCOMPARE(keyEventCount, 2);
}

void RemoteInputCaptureTest::emergencyHotkeyStopsControlAndIsNotForwarded() {
    RemoteInputSender sender;
    sender.beginSession(QStringLiteral("s1"));
    QWidget surface;
    surface.resize(800, 450);

    RemoteInputCapture capture(sender);
    capture.attachTo(&surface);
    capture.setRemoteVideoSize(QSize(1920, 1080));
    capture.setEnabled(true);
    drain(sender, 1000);

    QSignalSpy releaseSpy(&capture, &RemoteInputCapture::releaseControlRequested);
    QTest::keyClick(&surface, Qt::Key_Q,
                    Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
    QCOMPARE(releaseSpy.count(), 1);

    // Q 必须被本地吞掉；macOS 会先产生修饰键的独立按下事件，所以识别到
    // 急停后还要补 ReleaseAll，确保远端不会留下按住的 Ctrl/Alt/Shift。
    const auto events = allEvents(drain(sender, 1100));
    for (const auto& event : events) {
        QVERIFY2(event.type != EventType::Key || event.keyCode != 0x51,
                 "急停热键 Q 被转发给了远端");
    }
    QVERIFY2(hasType(events, EventType::ReleaseAll), "急停后没有释放远端已按下的修饰键");
}

void RemoteInputCaptureTest::capturesNothingUntilEnabled() {
    RemoteInputSender sender;
    sender.beginSession(QStringLiteral("s1"));
    QWidget surface;
    surface.resize(800, 450);

    RemoteInputCapture capture(sender);
    capture.attachTo(&surface);
    capture.setRemoteVideoSize(QSize(1920, 1080));

    // 没开控制时，画面上的鼠标动作一律不该变成远程输入。
    QTest::mouseMove(&surface, QPoint(400, 225));
    QTest::mouseClick(&surface, Qt::LeftButton, Qt::NoModifier, QPoint(400, 225));
    QVERIFY(drain(sender, 1000).isEmpty());
}

void RemoteInputCaptureTest::mapsClickThroughLetterbox() {
    RemoteInputSender sender;
    sender.beginSession(QStringLiteral("s1"));
    QWidget surface;
    // 控件 800x600、画面 16:9 → 上下留黑边，内容区高 450。
    surface.resize(800, 600);

    RemoteInputCapture capture(sender);
    capture.attachTo(&surface);
    capture.setRemoteVideoSize(QSize(1920, 1080));
    capture.setEnabled(true);

    QTest::mouseClick(&surface, Qt::LeftButton, Qt::NoModifier, QPoint(400, 300));
    const auto events = allEvents(drain(sender, 1000));
    QVERIFY(!events.isEmpty());

    // 控件正中 → 画面正中。黑边没减掉的话 y 会明显偏离 0.5。
    bool sawPress = false;
    for (const auto& event : events) {
        if (event.type != EventType::MouseButton || !event.pressed) continue;
        sawPress = true;
        QVERIFY(qAbs(event.x - 0.5) < 0.01);
        QVERIFY2(qAbs(event.y - 0.5) < 0.01,
                 qPrintable(QStringLiteral("y 算成了 %1，黑边没减干净").arg(event.y)));
    }
    QVERIFY(sawPress);
}

void RemoteInputCaptureTest::ignoresClicksOnLetterboxButKeepsDragging() {
    RemoteInputSender sender;
    sender.beginSession(QStringLiteral("s1"));
    QWidget surface;
    surface.resize(800, 600);

    RemoteInputCapture capture(sender);
    capture.attachTo(&surface);
    capture.setRemoteVideoSize(QSize(1920, 1080));
    capture.setEnabled(true);

    // 上方黑边（内容区从 y=75 才开始）。点这里不对应远端任何位置，
    // 硬映射会变成点到屏幕最上沿，很意外。
    QTest::mouseClick(&surface, Qt::LeftButton, Qt::NoModifier, QPoint(400, 10));
    QVERIFY(!hasType(allEvents(drain(sender, 1000)), EventType::MouseButton));

    // 但从画面内按下后拖到黑边上，必须继续跟随而不是原地不动。
    // 这里直接投事件而不用 QTest::mouseMove：后者依赖真实光标与可见窗口，
    // 在无头测试里不产生 MouseMove，会把"没实现"和"没触发"混为一谈。
    QTest::mousePress(&surface, Qt::LeftButton, Qt::NoModifier, QPoint(400, 300));
    drain(sender, 1100);
    QMouseEvent move(QEvent::MouseMove, QPointF(400, 10), Qt::NoButton, Qt::LeftButton,
                     Qt::NoModifier);
    QCoreApplication::sendEvent(&surface, &move);
    const auto moves = allEvents(drain(sender, 1200));
    QVERIFY(hasType(moves, EventType::MouseMove));
}

void RemoteInputCaptureTest::alwaysSendsReleaseEvenOutsideContent() {
    RemoteInputSender sender;
    sender.beginSession(QStringLiteral("s1"));
    QWidget surface;
    surface.resize(800, 600);

    RemoteInputCapture capture(sender);
    capture.attachTo(&surface);
    capture.setRemoteVideoSize(QSize(1920, 1080));
    capture.setEnabled(true);

    QTest::mousePress(&surface, Qt::LeftButton, Qt::NoModifier, QPoint(400, 300));
    drain(sender, 1000);

    // 在黑边上松手：抬起**必须**发出去。漏掉抬起会让远端一直按着，
    // 那正是"悬空按键"，人不在电脑旁没法自己解。
    QTest::mouseRelease(&surface, Qt::LeftButton, Qt::NoModifier, QPoint(400, 10));
    const auto events = allEvents(drain(sender, 1100));
    bool sawRelease = false;
    for (const auto& event : events) {
        if (event.type == EventType::MouseButton && !event.pressed) sawRelease = true;
    }
    QVERIFY2(sawRelease, "在黑边上松手时漏掉了抬起事件");
}

void RemoteInputCaptureTest::releasesEverythingWhenDisabledOrFocusLost() {
    RemoteInputSender sender;
    sender.beginSession(QStringLiteral("s1"));
    QWidget surface;
    surface.resize(800, 450);

    RemoteInputCapture capture(sender);
    capture.attachTo(&surface);
    capture.setRemoteVideoSize(QSize(1920, 1080));
    capture.setEnabled(true);
    drain(sender, 1000);

    // 关掉控制：本地松手对端是看不见的，必须主动让它全部放开。
    capture.setEnabled(false);
    QVERIFY(hasType(allEvents(drain(sender, 1100)), EventType::ReleaseAll));

    capture.setEnabled(true);
    drain(sender, 1200);
    // 焦点跑了同理：之后的抬起事件本地根本收不到。
    QFocusEvent focusOut(QEvent::FocusOut);
    QCoreApplication::sendEvent(&surface, &focusOut);
    QVERIFY(hasType(allEvents(drain(sender, 1300)), EventType::ReleaseAll));
}

void RemoteInputCaptureTest::recomputesContentRectOnResize() {
    RemoteInputSender sender;
    sender.beginSession(QStringLiteral("s1"));
    QWidget surface;
    surface.resize(800, 600);

    RemoteInputCapture capture(sender);
    capture.attachTo(&surface);
    capture.setRemoteVideoSize(QSize(1920, 1080));
    capture.setEnabled(true);

    const QRectF before = sender.contentRect();
    surface.resize(1200, 600);
    QCoreApplication::sendPostedEvents(&surface, QEvent::Resize);
    QResizeEvent resize(QSize(1200, 600), QSize(800, 600));
    QCoreApplication::sendEvent(&surface, &resize);

    // 控件尺寸变了黑边就变了，不重算的话坐标会整体偏移。
    QVERIFY(sender.contentRect() != before);
    // 变宽后画面高度顶满、左右留黑边：宽度应是 600*16/9，而不是铺满 1200。
    const QRectF after = sender.contentRect();
    QCOMPARE(after.height(), 600.0);
    QVERIFY(qAbs(after.width() - 600.0 * 16.0 / 9.0) < 0.5);
    QVERIFY(after.x() > 0.0);
}

QTEST_MAIN(RemoteInputCaptureTest)
#include "RemoteInputCaptureTest.moc"
