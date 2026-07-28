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
};

void RemoteDesktopSignalTest::roundTripsEverySignalType() {
    const QVector<Type> types{Type::Invite, Type::Accept, Type::Reject, Type::Stop};
    for (Type type : types) {
        Signal source;
        source.type = type;
        source.sessionId = QStringLiteral("s-123");
        source.roomId = QStringLiteral("mc-whq-iphone-a1b2c3d4");

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

QTEST_MAIN(RemoteDesktopSignalTest)
#include "RemoteDesktopSignalTest.moc"
