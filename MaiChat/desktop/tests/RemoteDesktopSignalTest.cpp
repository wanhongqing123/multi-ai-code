#include <QtTest/QtTest>

#include "remote/RemoteDesktopSignal.h"

using namespace RemoteDesktopSignals;

class RemoteDesktopSignalTest : public QObject {
    Q_OBJECT

private slots:
    void roundTripsEverySignalType();
    void roundTripsOptionalFields();
    void treatsPlainChatTextAsUnknown();
    void survivesMalformedPayloads();
    void rejectsUnknownProtocolVersion();
    void rejectsUnknownSignalType();
    void encodesNothingForUnknownType();
    void keepsSignalOutOfPlainSight();
    void roundTripsNoticeWithCode();
    void rejectsNoticeWithoutCode();
    void decodesSharedCaptureGeometryFixture();
    void ignoresInvalidOptionalGeometryWithoutRejectingAccept();
    void acceptsLegacyAcceptWithoutGeometry();
};

void RemoteDesktopSignalTest::roundTripsEverySignalType() {
    const QVector<Type> types{Type::Invite, Type::Accept, Type::Reject, Type::Stop, Type::Notice};

    // 这个列表是手写的，新增枚举值时极易漏——漏了这条用例照样全绿，
    // 名字却还叫"每一种类型"（加 Notice 时就漏过一次）。
    // 所以反过来问编码器认识几种，两边对不上就直接挂。
    int recognisedByEncoder = 0;
    for (int raw = 1; raw < 32; ++raw) {
        Signal probe;
        probe.type = static_cast<Type>(raw);
        probe.noticeCode = QStringLiteral("probe");
        if (!encodeSignal(probe).isEmpty()) ++recognisedByEncoder;
    }
    QCOMPARE(types.size(), recognisedByEncoder);

    for (Type type : types) {
        Signal source;
        source.type = type;
        source.sessionId = QStringLiteral("s-123");
        source.roomId = QStringLiteral("mc-whq-iphone-a1b2c3d4");
        // Notice 必须带 code 才是合法信令。
        source.noticeCode = QString::fromLatin1(NoticeCodes::kSecureDesktopEntered);

        const QString encoded = encodeSignal(source);
        QVERIFY(!encoded.isEmpty());
        QVERIFY(isSignalText(encoded));

        const Signal decoded = decodeSignal(encoded);
        QCOMPARE(static_cast<int>(decoded.type), static_cast<int>(type));
        QCOMPARE(decoded.sessionId, source.sessionId);
        QCOMPARE(decoded.roomId, source.roomId);
        QCOMPARE(decoded.protocolVersion, kProtocolVersion);
    }
}

void RemoteDesktopSignalTest::roundTripsOptionalFields() {
    Signal invite;
    invite.type = Type::Invite;
    invite.sessionId = QStringLiteral("s-777");
    invite.roomId = QStringLiteral("mc-a-b");
    invite.authProof = QStringLiteral("9f86d081884c7d659a2feaa0c55ad015");

    const Signal decodedInvite = decodeSignal(encodeSignal(invite));
    QCOMPARE(decodedInvite.authProof, invite.authProof);
    QVERIFY(decodedInvite.reason.isEmpty());

    Signal reject;
    reject.type = Type::Reject;
    reject.sessionId = QStringLiteral("s-777");
    reject.reason = QStringLiteral("对方正在共享中");

    const Signal decodedReject = decodeSignal(encodeSignal(reject));
    QCOMPARE(decodedReject.reason, reject.reason);
    QVERIFY(decodedReject.authProof.isEmpty());
}

void RemoteDesktopSignalTest::treatsPlainChatTextAsUnknown() {
    const QStringList plain{
        QStringLiteral("你好"),
        QStringLiteral("[remote-desktop] 看起来像信令但没有前缀"),
        QStringLiteral(""),
        QStringLiteral("{\"v\":1,\"type\":\"invite\"}")};
    for (const QString& text : plain) {
        QVERIFY(!isSignalText(text));
        QCOMPARE(static_cast<int>(decodeSignal(text).type), static_cast<int>(Type::Unknown));
    }
}

void RemoteDesktopSignalTest::survivesMalformedPayloads() {
    // 信令通道直接吃用户可控的 IM 文本，畸形输入必须安全降级而不是崩溃。
    const QStringList malformed{
        signalPrefix(),
        signalPrefix() + QStringLiteral("not json at all"),
        signalPrefix() + QStringLiteral("{"),
        signalPrefix() + QStringLiteral("[1,2,3]"),
        signalPrefix() + QStringLiteral("\"just-a-string\""),
        signalPrefix() + QStringLiteral("{\"v\":1}")};
    for (const QString& text : malformed) {
        const Signal decoded = decodeSignal(text);
        QCOMPARE(static_cast<int>(decoded.type), static_cast<int>(Type::Unknown));
    }
}

void RemoteDesktopSignalTest::rejectsUnknownProtocolVersion() {
    const QString future = signalPrefix()
        + QStringLiteral("{\"v\":99,\"type\":\"invite\",\"sessionId\":\"s\"}");
    QCOMPARE(static_cast<int>(decodeSignal(future).type), static_cast<int>(Type::Unknown));
}

void RemoteDesktopSignalTest::rejectsUnknownSignalType() {
    const QString unknown = signalPrefix()
        + QStringLiteral("{\"v\":1,\"type\":\"shutdown\",\"sessionId\":\"s\"}");
    QCOMPARE(static_cast<int>(decodeSignal(unknown).type), static_cast<int>(Type::Unknown));
}

void RemoteDesktopSignalTest::encodesNothingForUnknownType() {
    Signal signal;
    signal.sessionId = QStringLiteral("s-1");
    QVERIFY(encodeSignal(signal).isEmpty());
}

void RemoteDesktopSignalTest::keepsSignalOutOfPlainSight() {
    Signal signal;
    signal.type = Type::Invite;
    signal.sessionId = QStringLiteral("s-1");
    const QString encoded = encodeSignal(signal);

    // 前缀首字符必须是不可见字符：万一信令被误当普通消息渲染，用户不会看到乱码开头。
    QVERIFY(!encoded.isEmpty());
    QVERIFY(!encoded.at(0).isPrint() || encoded.at(0).category() == QChar::Other_Format);
}

void RemoteDesktopSignalTest::roundTripsNoticeWithCode() {
    Signal notice;
    notice.type = Type::Notice;
    notice.sessionId = QStringLiteral("sess-1");
    notice.noticeCode = QString::fromLatin1(NoticeCodes::kSecureDesktopEntered);

    const Signal decoded = decodeSignal(encodeSignal(notice));
    QCOMPARE(decoded.type, Type::Notice);
    QCOMPARE(decoded.sessionId, QStringLiteral("sess-1"));
    QCOMPARE(decoded.noticeCode, QString::fromLatin1(NoticeCodes::kSecureDesktopEntered));

    // 播报同样不该出现在聊天记录里。
    QVERIFY(isSignalText(encodeSignal(notice)));

    Signal left = notice;
    left.noticeCode = QString::fromLatin1(NoticeCodes::kSecureDesktopLeft);
    QCOMPARE(decodeSignal(encodeSignal(left)).noticeCode,
             QString::fromLatin1(NoticeCodes::kSecureDesktopLeft));
    // 进入与离开必须是两个不同的码，否则控制端分不清该显示还是该收起。
    QVERIFY(QString::fromLatin1(NoticeCodes::kSecureDesktopEntered)
            != QString::fromLatin1(NoticeCodes::kSecureDesktopLeft));
}

void RemoteDesktopSignalTest::rejectsNoticeWithoutCode() {
    // 不带 code 的播报没有任何意义，控制端不知道该显示什么，按不认识丢弃。
    Signal notice;
    notice.type = Type::Notice;
    notice.sessionId = QStringLiteral("sess-1");
    QCOMPARE(decodeSignal(encodeSignal(notice)).type, Type::Unknown);

    // 其它类型不受这条约束影响，别误伤。
    Signal stop;
    stop.type = Type::Stop;
    stop.sessionId = QStringLiteral("sess-1");
    QCOMPARE(decodeSignal(encodeSignal(stop)).type, Type::Stop);
}

void RemoteDesktopSignalTest::decodesSharedCaptureGeometryFixture() {
    // 与 Swift 端共用的逐字段 fixture。字段名与层级一旦漂移，这里会直接失败；
    // v 仍是 1，老客户端只会忽略新增对象。
    const QString fixture = signalPrefix() + QStringLiteral(
        R"({"v":1,"type":"accept","sessionId":"session-geometry-1","roomId":"mc-room-1","captureGeometry":{"sourceWidth":2560,"sourceHeight":1600,"captureX":0,"captureY":0,"captureWidth":2560,"captureHeight":1600,"contentMode":"fit","revision":1}})");

    const Signal decoded = decodeSignal(fixture);
    QCOMPARE(decoded.type, Type::Accept);
    QCOMPARE(decoded.sessionId, QStringLiteral("session-geometry-1"));
    QCOMPARE(decoded.roomId, QStringLiteral("mc-room-1"));
    QVERIFY(decoded.captureGeometry.has_value());
    const auto& geometry = *decoded.captureGeometry;
    QCOMPARE(geometry.sourceSize, QSize(2560, 1600));
    QCOMPARE(geometry.captureRect, QRect(0, 0, 2560, 1600));
    QCOMPARE(geometry.contentMode, RemoteDesktop::CaptureContentMode::Fit);
    QCOMPARE(geometry.revision, quint64(1));

    // C++ 发出去后也必须能无损读回同一组值。
    const Signal roundTripped = decodeSignal(encodeSignal(decoded));
    QVERIFY(roundTripped.captureGeometry.has_value());
    QCOMPARE(roundTripped.captureGeometry->sourceSize, geometry.sourceSize);
    QCOMPARE(roundTripped.captureGeometry->captureRect, geometry.captureRect);
    QCOMPARE(roundTripped.captureGeometry->revision, geometry.revision);

    Signal maximumRevision = decoded;
    maximumRevision.captureGeometry->revision =
        RemoteDesktop::kMaxCaptureGeometryRevision;
    const Signal maximumRevisionRoundTrip = decodeSignal(encodeSignal(maximumRevision));
    QVERIFY(maximumRevisionRoundTrip.captureGeometry.has_value());
    QCOMPARE(maximumRevisionRoundTrip.captureGeometry->revision,
             RemoteDesktop::kMaxCaptureGeometryRevision);
}

void RemoteDesktopSignalTest::ignoresInvalidOptionalGeometryWithoutRejectingAccept() {
    const QString commonPrefix =
        QStringLiteral(R"({"v":1,"type":"accept","sessionId":"s","roomId":"r","captureGeometry":)");
    const QString commonSuffix = QStringLiteral("}");
    const QStringList invalidObjects{
        QStringLiteral("null"),
        QStringLiteral(R"("not-an-object")"),
        // 超大尺寸必须在进入 QRect/浮点计算前拒绝。
        QStringLiteral(R"({"sourceWidth":999999999,"sourceHeight":1600,"captureX":0,"captureY":0,"captureWidth":2560,"captureHeight":1600,"contentMode":"fit","revision":1})"),
        // captureX + captureWidth 越过 source 右边界。
        QStringLiteral(R"({"sourceWidth":2560,"sourceHeight":1600,"captureX":2500,"captureY":0,"captureWidth":100,"captureHeight":1600,"contentMode":"fit","revision":1})"),
        // 非整数不能被 toInt 静默截断。
        QStringLiteral(R"({"sourceWidth":2560.5,"sourceHeight":1600,"captureX":0,"captureY":0,"captureWidth":2560,"captureHeight":1600,"contentMode":"fit","revision":1})"),
        // 未知 mode 只让增强映射失效，不影响 Accept 进房。
        QStringLiteral(R"({"sourceWidth":2560,"sourceHeight":1600,"captureX":0,"captureY":0,"captureWidth":2560,"captureHeight":1600,"contentMode":"crop","revision":1})"),
        // 超过跨语言约定的 INT_MAX 会让 int 编码溢出，必须整体忽略该可选对象。
        QStringLiteral(R"({"sourceWidth":2560,"sourceHeight":1600,"captureX":0,"captureY":0,"captureWidth":2560,"captureHeight":1600,"contentMode":"fit","revision":2147483648})")};

    for (const QString& geometry : invalidObjects) {
        const Signal decoded =
            decodeSignal(signalPrefix() + commonPrefix + geometry + commonSuffix);
        QCOMPARE(decoded.type, Type::Accept);
        QCOMPARE(decoded.sessionId, QStringLiteral("s"));
        QVERIFY2(!decoded.captureGeometry.has_value(), qPrintable(geometry));
    }
}

void RemoteDesktopSignalTest::acceptsLegacyAcceptWithoutGeometry() {
    const Signal decoded = decodeSignal(
        signalPrefix()
        + QStringLiteral(R"({"v":1,"type":"accept","sessionId":"old-s","roomId":"old-r"})"));
    QCOMPARE(decoded.type, Type::Accept);
    QCOMPARE(decoded.sessionId, QStringLiteral("old-s"));
    QVERIFY(!decoded.captureGeometry.has_value());
}

QTEST_MAIN(RemoteDesktopSignalTest)
#include "RemoteDesktopSignalTest.moc"
