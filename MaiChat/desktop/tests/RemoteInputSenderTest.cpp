#include <QtTest/QtTest>

#include "remote/RemoteInputSender.h"

using namespace RemoteInput;

namespace {

QVector<Packet> decodeAll(const QVector<RemoteInputSender::OutgoingPacket>& outgoing,
                          Channel channel) {
    QVector<Packet> packets;
    for (const auto& item : outgoing) {
        if (item.channel != channel) continue;
        Packet packet;
        if (decodePacket(item.payload, &packet)) packets.append(packet);
    }
    return packets;
}

int countEvents(const QVector<Packet>& packets) {
    int total = 0;
    for (const auto& packet : packets) total += packet.events.size();
    return total;
}

}  // namespace

class RemoteInputSenderTest : public QObject {
    Q_OBJECT

private slots:
    void computesLetterboxRectForBothOrientations();
    void mapsWidgetCoordinatesIgnoringLetterbox();
    void rejectsPointsOnLetterboxButClampsWhenAsked();
    void composesEncodedAndSurfaceFitThenMapsCaptureIntoSource();
    void keepsLegacyMappingWhenCaptureGeometryIsUnknown();
    void mapsSameSourcePointWhetherEncoderFollowsSourceAspect();
    void batchesMovePathIntoOnePacketKeepingEndpoint();
    void respectsByteBudgetNotJustMessageCount();
    void batchesReliableEventsIntoOnePacket();
    void sumsWheelDeltasWithinOneTick();
    void prioritisesKeysOverMovesWhenBudgetIsTight();
    void staysWithinTrtcMessageBudget();
    void neverExceedsBudgetInAnySlidingWindow();
    void dropsOversizedEventInsteadOfBlockingQueue();
    void queuesNothingWithoutActiveSession();
};

void RemoteInputSenderTest::computesLetterboxRectForBothOrientations() {
    // 画面比控件更宽 → 上下留黑边，宽度顶满。
    const QRectF wide = fitContentRect(QSizeF(1000, 1000), QSizeF(1920, 1080));
    QCOMPARE(wide.width(), 1000.0);
    QCOMPARE(wide.height(), 1000.0 * 1080.0 / 1920.0);
    QCOMPARE(wide.x(), 0.0);
    QVERIFY(wide.y() > 0.0);

    // 画面比控件更高 → 左右留黑边，高度顶满。
    const QRectF tall = fitContentRect(QSizeF(1000, 500), QSizeF(1000, 1000));
    QCOMPARE(tall.height(), 500.0);
    QCOMPARE(tall.width(), 500.0);
    QCOMPARE(tall.y(), 0.0);
    QCOMPARE(tall.x(), 250.0);

    // 比例一致时不该留黑边。
    const QRectF exact = fitContentRect(QSizeF(1920, 1080), QSizeF(1920, 1080));
    QCOMPARE(exact, QRectF(0, 0, 1920, 1080));

    // 尺寸非法时返回空矩形，调用方据此判定"还不能换算"。
    QVERIFY(fitContentRect(QSizeF(0, 0), QSizeF(1920, 1080)).isEmpty());
    QVERIFY(fitContentRect(QSizeF(100, 100), QSizeF(0, 0)).isEmpty());
}

void RemoteInputSenderTest::mapsWidgetCoordinatesIgnoringLetterbox() {
    RemoteInputSender sender;
    sender.beginSession(QStringLiteral("s1"));
    // 控件 1000x1000、画面 16:9 → 上下各留 218.75 黑边。
    sender.setContentRect(fitContentRect(QSizeF(1000, 1000), QSizeF(1920, 1080)));

    double x = 0.0;
    double y = 0.0;
    // 内容区左上角 → (0,0)，不是控件左上角。黑边不减掉的话这里会整体偏移。
    QVERIFY(sender.mapToNormalized(QPointF(0, sender.contentRect().y()), &x, &y));
    QCOMPARE(x, 0.0);
    QCOMPARE(y, 0.0);

    QVERIFY(sender.mapToNormalized(QPointF(500, 500), &x, &y));
    QCOMPARE(x, 0.5);
    QVERIFY(qAbs(y - 0.5) < 1e-9);
}

void RemoteInputSenderTest::rejectsPointsOnLetterboxButClampsWhenAsked() {
    RemoteInputSender sender;
    sender.beginSession(QStringLiteral("s1"));
    sender.setContentRect(fitContentRect(QSizeF(1000, 1000), QSizeF(1920, 1080)));

    double x = 0.0;
    double y = 0.0;
    // 黑边不对应远端屏幕的任何位置，硬映射会变成点到屏幕边缘，很意外。
    QVERIFY(!sender.mapToNormalized(QPointF(500, 5), &x, &y));

    // 但拖拽途中滑出画面区要继续跟随，钳到边界而不是原地不动。
    QVERIFY(sender.mapToNormalizedClamped(QPointF(500, 5), &x, &y));
    QCOMPARE(y, 0.0);
    QVERIFY(sender.mapToNormalizedClamped(QPointF(500, 995), &x, &y));
    QCOMPARE(y, 1.0);

    // 还没拿到画面尺寸时不能瞎换算。
    RemoteInputSender fresh;
    fresh.beginSession(QStringLiteral("s1"));
    QVERIFY(!fresh.mapToNormalized(QPointF(1, 1), &x, &y));
    QVERIFY(!fresh.mapToNormalizedClamped(QPointF(1, 1), &x, &y));
}

void RemoteInputSenderTest::composesEncodedAndSurfaceFitThenMapsCaptureIntoSource() {
    RemoteDesktop::CaptureGeometry geometry;
    geometry.sourceSize = QSize(3000, 2000);
    geometry.captureRect = QRect(300, 200, 2400, 1600);  // source 中间的 3:2 区域
    geometry.contentMode = RemoteDesktop::CaptureContentMode::Fit;
    geometry.revision = 7;
    QVERIFY(geometry.isValid());

    // 编码帧 16:9，capture 3:2：第一级在编码帧左右各补 150px。
    // surface 是正方形：第二级又在 surface 上下补边。
    const CaptureCoordinateMapping mapping = calculateCaptureCoordinateMapping(
        QSizeF(1000, 1000), QSizeF(1920, 1080), geometry);
    QVERIFY(mapping.usesCaptureGeometry);
    QVERIFY(qAbs(mapping.encodedContentRect.x() - 150.0) < 1e-9);
    QVERIFY(qAbs(mapping.encodedContentRect.width() - 1620.0) < 1e-9);
    QVERIFY(qAbs(mapping.surfaceVideoRect.y() - 218.75) < 1e-9);
    QVERIFY(mapping.surfaceContentRect.x() > mapping.surfaceVideoRect.x());
    QVERIFY(mapping.surfaceContentRect.width() < mapping.surfaceVideoRect.width());

    RemoteInputSender sender;
    sender.beginSession(QStringLiteral("s1"));
    sender.setCaptureGeometry(geometry);
    sender.setContentRect(mapping.surfaceContentRect);
    QCOMPARE(sender.normalizedTargetRect(), QRectF(0.1, 0.1, 0.8, 0.8));

    double x = 0.0;
    double y = 0.0;
    // capture 左边缘不是 source 左边缘：最终输入必须落在 source 的 10%。
    QVERIFY(sender.mapToNormalized(
        QPointF(mapping.surfaceContentRect.left(), mapping.surfaceContentRect.center().y()),
        &x, &y));
    QVERIFY(qAbs(x - 0.1) < 1e-9);
    QVERIFY(qAbs(y - 0.5) < 1e-9);

    // 帧内黑边上的点不产生点击；拖拽出界时则钳到 capture/source 的边界，
    // 不能误钳到整个 source 的 0。
    QVERIFY(!sender.mapToNormalized(
        QPointF(mapping.surfaceVideoRect.left() + 1.0, mapping.surfaceVideoRect.center().y()),
        &x, &y));
    QVERIFY(sender.mapToNormalizedClamped(
        QPointF(mapping.surfaceVideoRect.left() + 1.0, mapping.surfaceVideoRect.center().y()),
        &x, &y));
    QVERIFY(qAbs(x - 0.1) < 1e-9);
}

void RemoteInputSenderTest::keepsLegacyMappingWhenCaptureGeometryIsUnknown() {
    const CaptureCoordinateMapping mapping = calculateCaptureCoordinateMapping(
        QSizeF(1000, 1000), QSizeF(1920, 1080), std::nullopt);
    QVERIFY(!mapping.usesCaptureGeometry);
    QCOMPARE(mapping.surfaceContentRect, mapping.surfaceVideoRect);
    QCOMPARE(mapping.encodedContentRect, QRectF(0, 0, 1920, 1080));

    RemoteInputSender sender;
    sender.setCaptureGeometry(std::nullopt);
    sender.setContentRect(mapping.surfaceContentRect);
    double x = 0.0;
    double y = 0.0;
    QVERIFY(sender.mapToNormalized(mapping.surfaceContentRect.center(), &x, &y));
    QCOMPARE(x, 0.5);
    QCOMPARE(y, 0.5);
}

void RemoteInputSenderTest::mapsSameSourcePointWhetherEncoderFollowsSourceAspect() {
    RemoteDesktop::CaptureGeometry geometry;
    geometry.sourceSize = QSize(2560, 1600);
    geometry.captureRect = QRect(0, 0, 2560, 1600);
    geometry.contentMode = RemoteDesktop::CaptureContentMode::Fit;
    geometry.revision = 1;

    const QSizeF surface(1920, 1080);
    // 旧 SDK 未采纳 source-aspect：固定 1920x1080 编码帧内部左右各补 96px。
    const auto fixedCanvas = calculateCaptureCoordinateMapping(
        surface, QSizeF(1920, 1080), geometry);
    QVERIFY(qAbs(fixedCanvas.encodedContentRect.x() - 96.0) < 1e-9);
    QVERIFY(qAbs(fixedCanvas.encodedContentRect.width() - 1728.0) < 1e-9);

    // 新策略生效：实际编码 1728x1080，active rect 是整帧，黑边只在 Qt surface。
    const auto sourceAspect = calculateCaptureCoordinateMapping(
        surface, QSizeF(1728, 1080), geometry);
    QCOMPARE(sourceAspect.encodedContentRect, QRectF(0, 0, 1728, 1080));
    QVERIFY(qAbs(fixedCanvas.surfaceContentRect.x()
                 - sourceAspect.surfaceContentRect.x()) < 1e-9);
    QVERIFY(qAbs(fixedCanvas.surfaceContentRect.width()
                 - sourceAspect.surfaceContentRect.width()) < 1e-9);

    const QPointF sameVisiblePoint(
        fixedCanvas.surfaceContentRect.x() + fixedCanvas.surfaceContentRect.width() * 0.25,
        fixedCanvas.surfaceContentRect.y() + fixedCanvas.surfaceContentRect.height() * 0.75);
    RemoteInputSender fixedSender;
    fixedSender.setCaptureGeometry(geometry);
    fixedSender.setContentRect(fixedCanvas.surfaceContentRect);
    RemoteInputSender sourceAspectSender;
    sourceAspectSender.setCaptureGeometry(geometry);
    sourceAspectSender.setContentRect(sourceAspect.surfaceContentRect);

    double fixedX = 0.0;
    double fixedY = 0.0;
    double sourceAspectX = 0.0;
    double sourceAspectY = 0.0;
    QVERIFY(fixedSender.mapToNormalized(sameVisiblePoint, &fixedX, &fixedY));
    QVERIFY(sourceAspectSender.mapToNormalized(
        sameVisiblePoint, &sourceAspectX, &sourceAspectY));
    QVERIFY(qAbs(fixedX - 0.25) < 1e-9);
    QVERIFY(qAbs(fixedY - 0.75) < 1e-9);
    QVERIFY(qAbs(fixedX - sourceAspectX) < 1e-9);
    QVERIFY(qAbs(fixedY - sourceAspectY) < 1e-9);
}

void RemoteInputSenderTest::batchesMovePathIntoOnePacketKeepingEndpoint() {
    RemoteInputSender sender;
    sender.beginSession(QStringLiteral("s1"));

    for (int i = 0; i < 50; ++i) sender.queueMouseMove(i / 100.0, i / 100.0);

    const auto outgoing = sender.flush(1000);
    const auto moves = decodeAll(outgoing, Channel::Unreliable);
    // 50 次移动合成**一个包**，但里面带的是整段轨迹而不是一个点：
    // 限的是包数不是事件数，中间点几乎白送。
    QCOMPARE(moves.size(), 1);
    QVERIFY(moves[0].events.size() > 1);
    QVERIFY(moves[0].events.size() <= RemoteInputSender::kMaxMovePointsPerPacket);
    // 轨迹必须保序，否则被控端会画出乱线。
    for (int i = 1; i < moves[0].events.size(); ++i) {
        QVERIFY(moves[0].events[i].x >= moves[0].events[i - 1].x);
    }
    // 抽稀可以丢中间点，但**终点必须在**——终点才是光标最终该停的位置。
    QCOMPARE(moves[0].events.last().x, 0.49);

    // 发完就没了，下一 tick 不该重发。
    QVERIFY(decodeAll(sender.flush(1100), Channel::Unreliable).isEmpty());
}

void RemoteInputSenderTest::respectsByteBudgetNotJustMessageCount() {
    RemoteInputSender sender;
    sender.beginSession(QStringLiteral("s1"));

    // 条数和字节数是两个独立的闸门，SDK 任一超了都拒发。光数包数不够：
    // 28 个接近 1KB 的包只有 28 条，却是 28KB，早就爆了 16KB/秒。
    int bytes = 0;
    for (qint64 now = 0; now < 1000; now += 5) {
        sender.queueText(QString(700, QLatin1Char('x')));
        for (const auto& item : sender.flush(now)) bytes += item.payload.size();
    }
    QVERIFY2(bytes <= kMaxBytesPerSecond,
             qPrintable(QStringLiteral("一秒发了 %1 字节，超过 %2")
                            .arg(bytes)
                            .arg(kMaxBytesPerSecond)));
}

void RemoteInputSenderTest::batchesReliableEventsIntoOnePacket() {
    RemoteInputSender sender;
    sender.beginSession(QStringLiteral("s1"));

    for (int i = 0; i < 20; ++i) {
        sender.queueKey(0x41 + i, true);
        sender.queueKey(0x41 + i, false);
    }

    const auto outgoing = sender.flush(1000);
    const auto reliable = decodeAll(outgoing, Channel::Reliable);
    // 40 条按键事件合批：1KB 装得下几十条，合批把配额省下来给移动。
    QCOMPARE(countEvents(reliable), 40);
    QVERIFY(reliable.size() <= 2);
    // 序号必须连号，否则被控端会当成跳号而全抬。
    for (int i = 1; i < reliable.size(); ++i) {
        QCOMPARE(reliable[i].sequence, reliable[i - 1].sequence + 1);
    }
}

void RemoteInputSenderTest::sumsWheelDeltasWithinOneTick() {
    RemoteInputSender sender;
    sender.beginSession(QStringLiteral("s1"));

    // 一次快速滚动会连出几十个事件，按增量求和后只占一个包。
    for (int i = 0; i < 30; ++i) sender.queueWheel(120, 0.5, 0.5);

    const auto reliable = decodeAll(sender.flush(1000), Channel::Reliable);
    QCOMPARE(countEvents(reliable), 1);
    QCOMPARE(reliable[0].events[0].type, EventType::MouseWheel);
    QCOMPARE(reliable[0].events[0].wheelDelta, 30 * 120);
}

void RemoteInputSenderTest::prioritisesKeysOverMovesWhenBudgetIsTight() {
    RemoteInputSender sender;
    sender.beginSession(QStringLiteral("s1"));

    // 先用移动把窗口名额占满。
    qint64 now = 1000;
    for (int i = 0; i < 40; ++i) {
        sender.queueMouseMove(0.5, 0.5);
        sender.flush(now);
        now += RemoteInputSender::kMoveIntervalMs;
    }

    // 名额吃紧时同时排队按键与移动：按键必须出得去，移动可以牺牲。
    // 移动丢了下一包就纠正回来，按键丢了会留下悬空状态。
    sender.queueMouseMove(0.9, 0.9);
    sender.queueKey(0x11, true);
    const auto outgoing = sender.flush(now);
    QCOMPARE(countEvents(decodeAll(outgoing, Channel::Reliable)), 1);
}

void RemoteInputSenderTest::staysWithinTrtcMessageBudget() {
    RemoteInputSender sender;
    sender.beginSession(QStringLiteral("s1"));

    // 模拟一秒钟疯狂操作：每 10ms 一次移动 + 每 100ms 一次点击。
    int packets = 0;
    int bytes = 0;
    for (qint64 now = 1000; now < 2000; now += 10) {
        sender.queueMouseMove((now % 100) / 100.0, 0.5);
        if (now % 100 == 0) {
            sender.queueMouseButton(MouseButton::Left, true, 0.5, 0.5);
            sender.queueMouseButton(MouseButton::Left, false, 0.5, 0.5);
        }
        for (const auto& item : sender.flush(now)) {
            ++packets;
            bytes += item.payload.size();
            // 单包超 1KB 会被中间路由丢弃。
            QVERIFY(item.payload.size() <= kMaxPacketBytes);
        }
    }

    // 30 条/秒是**整个客户端**的总配额，两个 cmdID 与 SEI 共享，
    // 不是每通道各 30 条。超了 SDK 会直接丢包。
    QVERIFY2(packets <= kMaxMessagesPerSecond,
             qPrintable(QStringLiteral("一秒发了 %1 个包，超过 %2 的总配额")
                            .arg(packets)
                            .arg(kMaxMessagesPerSecond)));
    QVERIFY(bytes <= kMaxBytesPerSecond);
}

void RemoteInputSenderTest::neverExceedsBudgetInAnySlidingWindow() {
    RemoteInputSender sender;
    sender.beginSession(QStringLiteral("s1"));

    // 逐毫秒喂满 5 秒，把发送时刻全记下来。
    QVector<qint64> sent;
    int deliveredEvents = 0;
    for (qint64 now = 0; now < 5000; ++now) {
        sender.queueMouseMove(0.5, 0.5);
        sender.queueKey(0x41, true);
        for (const auto& item : sender.flush(now)) {
            sent.append(now);
            Packet packet;
            if (decodePacket(item.payload, &packet)) deliveredEvents += packet.events.size();
        }
    }

    // 关键是**任意**一秒窗口都不能超，不是"从零点开始的那一秒"。
    // 令牌桶栽在这里：桶容量本身就是突发额度，满桶起步时
    // "桶容量 + 一秒补充量"会一起挤进同一个窗口。
    int worst = 0;
    for (int start = 0; start < sent.size(); ++start) {
        int count = 0;
        for (int i = start; i < sent.size() && sent[i] - sent[start] < 1000; ++i) ++count;
        worst = qMax(worst, count);
    }
    QVERIFY2(worst <= kMaxMessagesPerSecond,
             qPrintable(QStringLiteral("最坏的一秒窗口发了 %1 个包，超过 %2")
                            .arg(worst)
                            .arg(kMaxMessagesPerSecond)));

    // 同时别限过头。这里要量的是**送达的事件数**而不是包数：合批本来就会
    // 让包数变少、吞吐变高，拿包数当吞吐指标会把优化误判成退化
    // （改成轨迹合批后就在这条上栽过一次）。
    //
    // 上面是每毫秒一个按键的极端压测（1000 次/秒），已经超过通道的物理上限
    // ——1KB/包装约 30 条、28 包/秒，天花板约 840 条/秒。这里只要求没被限成
    // 涓流；真实输入速率远在天花板之下，由下面那段验证。
    QVERIFY2(deliveredEvents >= 2000,
             qPrintable(QStringLiteral("5 秒只送出 %1 个事件，限得太狠").arg(deliveredEvents)));

    // 真实速率：按键 20 次/秒（打字很快了）+ 鼠标 125Hz。这个量级必须一条不丢。
    RemoteInputSender realistic;
    realistic.beginSession(QStringLiteral("s2"));
    int queuedKeys = 0;
    int deliveredKeys = 0;
    for (qint64 now = 0; now < 5000; ++now) {
        if (now % 8 == 0) realistic.queueMouseMove(0.5, 0.5);
        if (now % 50 == 0) {
            realistic.queueKey(0x41, true);
            ++queuedKeys;
        }
        for (const auto& item : realistic.flush(now)) {
            Packet packet;
            if (!decodePacket(item.payload, &packet)) continue;
            for (const auto& event : packet.events) {
                if (event.type == EventType::Key) ++deliveredKeys;
            }
        }
    }
    QCOMPARE(deliveredKeys, queuedKeys);
}

void RemoteInputSenderTest::dropsOversizedEventInsteadOfBlockingQueue() {
    RemoteInputSender sender;
    sender.beginSession(QStringLiteral("s1"));

    // 单条就撑破 1KB 的文本永远塞不进包里。必须丢掉它继续，
    // 否则整条可靠队列会被它堵死，后面的按键一条都发不出去。
    sender.queueText(QString(5000, QLatin1Char('a')));
    sender.queueKey(0x11, true);

    const auto reliable = decodeAll(sender.flush(1000), Channel::Reliable);
    QCOMPARE(countEvents(reliable), 1);
    QCOMPARE(reliable[0].events[0].type, EventType::Key);
}

void RemoteInputSenderTest::queuesNothingWithoutActiveSession() {
    RemoteInputSender sender;
    // 没会话就不该攒任何事件，更不该发出去。
    sender.queueMouseMove(0.5, 0.5);
    sender.queueKey(0x41, true);
    QVERIFY(sender.flush(1000).isEmpty());

    sender.beginSession(QStringLiteral("s1"));
    sender.queueKey(0x41, true);
    sender.endSession();
    // 结束会话要把攒着没发的清空，不能等下一场会话再吐出去。
    QVERIFY(sender.flush(2000).isEmpty());
}

QTEST_MAIN(RemoteInputSenderTest)
#include "RemoteInputSenderTest.moc"
