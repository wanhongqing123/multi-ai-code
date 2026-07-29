#include <QtTest/QtTest>

#include "remote/RemoteInputProtocol.h"

using namespace RemoteInput;

class RemoteInputProtocolTest : public QObject {
    Q_OBJECT

private slots:
    void roundTripsEveryEventType();
    void clampsCoordinatesOnEncodeAndDecode();
    void rejectsMalformedPayloads();
    void rejectsMismatchedProtocolVersion();
    void skipsUnknownEventsInsteadOfDroppingPacket();
    void rejectsOutOfRangeKeyCode();
    void reliableAndUnreliableUseDistinctCmdIds();
    void detectsPacketsTooBigForOneSend();
};

void RemoteInputProtocolTest::roundTripsEveryEventType() {
    Packet packet;
    packet.sessionId = QStringLiteral("sess-1");
    packet.sequence = 4242;

    Event move;
    move.type = EventType::MouseMove;
    move.x = 0.25;
    move.y = 0.75;

    Event button;
    button.type = EventType::MouseButton;
    button.x = 0.5;
    button.y = 0.5;
    button.button = MouseButton::Right;
    button.pressed = true;

    Event wheel;
    wheel.type = EventType::MouseWheel;
    wheel.x = 0.1;
    wheel.y = 0.2;
    wheel.wheelDelta = -240;

    Event key;
    key.type = EventType::Key;
    key.keyCode = 0x11;  // VK_CONTROL
    key.pressed = true;

    Event text;
    text.type = EventType::Text;
    // 非 ASCII 必须原样还原：中文 IME 上屏走的就是这条路。
    text.text = QStringLiteral("你好，世界");

    Event releaseAll;
    releaseAll.type = EventType::ReleaseAll;

    packet.events = {move, button, wheel, key, text, releaseAll};

    Packet decoded;
    QVERIFY(decodePacket(encodePacket(packet), &decoded));
    QCOMPARE(decoded.protocolVersion, kProtocolVersion);
    QCOMPARE(decoded.sessionId, QStringLiteral("sess-1"));
    QCOMPARE(decoded.sequence, 4242u);
    QCOMPARE(decoded.events.size(), 6);

    QCOMPARE(decoded.events[0].type, EventType::MouseMove);
    QCOMPARE(decoded.events[0].x, 0.25);
    QCOMPARE(decoded.events[0].y, 0.75);

    QCOMPARE(decoded.events[1].type, EventType::MouseButton);
    QCOMPARE(decoded.events[1].button, MouseButton::Right);
    QVERIFY(decoded.events[1].pressed);

    QCOMPARE(decoded.events[2].type, EventType::MouseWheel);
    QCOMPARE(decoded.events[2].wheelDelta, -240);

    QCOMPARE(decoded.events[3].type, EventType::Key);
    QCOMPARE(decoded.events[3].keyCode, 0x11u);
    QVERIFY(decoded.events[3].pressed);

    QCOMPARE(decoded.events[4].type, EventType::Text);
    QCOMPARE(decoded.events[4].text, QStringLiteral("你好，世界"));

    QCOMPARE(decoded.events[5].type, EventType::ReleaseAll);
}

void RemoteInputProtocolTest::clampsCoordinatesOnEncodeAndDecode() {
    Packet packet;
    packet.sessionId = QStringLiteral("sess-1");

    Event tooSmall;
    tooSmall.type = EventType::MouseMove;
    tooSmall.x = -3.5;
    tooSmall.y = -0.001;

    Event tooBig;
    tooBig.type = EventType::MouseMove;
    tooBig.x = 42.0;
    tooBig.y = 1.0001;

    Event notANumber;
    notANumber.type = EventType::MouseMove;
    notANumber.x = std::numeric_limits<double>::quiet_NaN();
    notANumber.y = std::numeric_limits<double>::quiet_NaN();

    packet.events = {tooSmall, tooBig, notANumber};

    Packet decoded;
    QVERIFY(decodePacket(encodePacket(packet), &decoded));
    QCOMPARE(decoded.events.size(), 3);
    // 越界坐标必须钳住而不是照单全收：坏包不该把光标甩到屏幕外。
    QCOMPARE(decoded.events[0].x, 0.0);
    QCOMPARE(decoded.events[0].y, 0.0);
    QCOMPARE(decoded.events[1].x, 1.0);
    QCOMPARE(decoded.events[1].y, 1.0);
    // NaN 比不出大小，两个越界分支都不命中，必须显式挡掉，否则一路传到注入层。
    QCOMPARE(decoded.events[2].x, 0.0);
    QCOMPARE(decoded.events[2].y, 0.0);
}

void RemoteInputProtocolTest::rejectsMalformedPayloads() {
    Packet decoded;
    // 这个函数直接吃网络字节，畸形输入一律丢弃且不得崩。
    QVERIFY(!decodePacket(QByteArray(), &decoded));
    QVERIFY(!decodePacket(QByteArray("not json at all"), &decoded));
    QVERIFY(!decodePacket(QByteArray("{\"v\":1,"), &decoded));
    QVERIFY(!decodePacket(QByteArray("[1,2,3]"), &decoded));
    QVERIFY(!decodePacket(QByteArray("{\"v\":1,\"n\":0}"), &decoded));          // 缺 events
    QVERIFY(!decodePacket(QByteArray("{\"v\":1,\"n\":0,\"e\":{}}"), &decoded)); // events 不是数组
    QVERIFY(!decodePacket(QByteArray("{\"v\":1,\"e\":[]}"), &decoded));         // 缺序号
    QVERIFY(!decodePacket(QByteArray("{\"v\":1,\"n\":-5,\"e\":[]}"), &decoded));
    QVERIFY(!decodePacket(encodePacket(Packet()), nullptr));

    // 失败时不得写坏调用方的对象。
    Packet keepMe;
    keepMe.sessionId = QStringLiteral("original");
    QVERIFY(!decodePacket(QByteArray("garbage"), &keepMe));
    QCOMPARE(keepMe.sessionId, QStringLiteral("original"));
}

void RemoteInputProtocolTest::rejectsMismatchedProtocolVersion() {
    Packet decoded;
    // 版本对不上宁可整包不动，也不要用错误的语义去操作别人的电脑。
    QVERIFY(!decodePacket(QByteArray("{\"v\":99,\"n\":0,\"e\":[]}"), &decoded));
    QVERIFY(!decodePacket(QByteArray("{\"v\":\"1\",\"n\":0,\"e\":[]}"), &decoded));
    QVERIFY(!decodePacket(QByteArray("{\"n\":0,\"e\":[]}"), &decoded));
}

void RemoteInputProtocolTest::skipsUnknownEventsInsteadOfDroppingPacket() {
    // 未知事件类型多半来自新版本控制端；老版被控端应当跳过它、继续执行认得的，
    // 而不是整包丢弃——否则一个新事件就让整条输入流卡死。
    const QByteArray payload =
        "{\"v\":1,\"s\":\"sess\",\"n\":7,\"e\":["
        "{\"t\":\"m\",\"x\":0.5,\"y\":0.5},"
        "{\"t\":\"zzz\"},"
        "\"not-an-object\","
        "{\"t\":\"x\",\"s\":\"\"},"
        "{\"t\":\"k\",\"k\":65,\"d\":true}]}";

    Packet decoded;
    QVERIFY(decodePacket(payload, &decoded));
    QCOMPARE(decoded.sequence, 7u);
    // 未知 tag、非对象、空文本都被跳过，剩下 move 与 key 两条。
    QCOMPARE(decoded.events.size(), 2);
    QCOMPARE(decoded.events[0].type, EventType::MouseMove);
    QCOMPARE(decoded.events[1].type, EventType::Key);
    QCOMPARE(decoded.events[1].keyCode, 65u);
}

void RemoteInputProtocolTest::rejectsOutOfRangeKeyCode() {
    // 越界键码拿去 SendInput 会点到完全无关的键，按坏事件跳过。
    Packet decoded;
    QVERIFY(decodePacket(QByteArray("{\"v\":1,\"n\":1,\"e\":["
                                    "{\"t\":\"k\",\"k\":70000,\"d\":true},"
                                    "{\"t\":\"k\",\"k\":-1,\"d\":true}]}"),
                         &decoded));
    QCOMPARE(decoded.events.size(), 0);
}

void RemoteInputProtocolTest::reliableAndUnreliableUseDistinctCmdIds() {
    // 同一 cmdID 内 reliable/ordered 必须前后一致（TRTC 硬约束），
    // 两类事件混用同一个 ID 会被拒发，所以必须是两个不同的 ID。
    QVERIFY(kCmdIdUnreliable != kCmdIdReliable);
    // TRTC 只接受 1..10。
    QVERIFY(kCmdIdUnreliable >= 1 && kCmdIdUnreliable <= 10);
    QVERIFY(kCmdIdReliable >= 1 && kCmdIdReliable <= 10);
}

void RemoteInputProtocolTest::detectsPacketsTooBigForOneSend() {
    Packet small;
    small.sessionId = QStringLiteral("sess-1");
    Event move;
    move.type = EventType::MouseMove;
    move.x = 0.5;
    move.y = 0.5;
    small.events = {move};
    QVERIFY(fitsInOnePacket(small));

    // 一条超长文本就能顶破 1KB，发送方必须先拆分再发，不能指望 SDK 帮忙。
    Packet big;
    big.sessionId = QStringLiteral("sess-1");
    Event text;
    text.type = EventType::Text;
    text.text = QString(2000, QLatin1Char('a'));
    big.events = {text};
    QVERIFY(!fitsInOnePacket(big));
    QVERIFY(encodePacket(big).size() > kMaxPacketBytes);
}

QTEST_MAIN(RemoteInputProtocolTest)
#include "RemoteInputProtocolTest.moc"
