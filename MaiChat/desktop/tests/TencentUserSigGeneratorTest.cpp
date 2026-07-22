#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <zlib.h>

#include "im/TencentUserSigGenerator.h"

namespace {

QByteArray fromTencentBase64Url(QString userSig) {
    userSig.replace(QStringLiteral("*"), QStringLiteral("+"));
    userSig.replace(QStringLiteral("-"), QStringLiteral("/"));
    userSig.replace(QStringLiteral("_"), QStringLiteral("="));
    return QByteArray::fromBase64(userSig.toUtf8());
}

QByteArray inflateZlib(const QByteArray& input) {
    QByteArray output;
    output.resize(4096);
    uLongf outputSize = static_cast<uLongf>(output.size());
    const int result = uncompress(reinterpret_cast<Bytef*>(output.data()),
                                  &outputSize,
                                  reinterpret_cast<const Bytef*>(input.constData()),
                                  static_cast<uLong>(input.size()));
    if (result != Z_OK) return {};
    output.resize(static_cast<int>(outputSize));
    return output;
}

}  // namespace

class TencentUserSigGeneratorTest : public QObject {
    Q_OBJECT

private slots:
    void generatesDeterministicTencentUserSigPayload();
    void generatedUserSigUsesTencentZlibWrapper();
    void rejectsInvalidInputs();
};

void TencentUserSigGeneratorTest::generatesDeterministicTencentUserSigPayload() {
    const QString userSig = TencentUserSigGenerator::generate(
        1400000000,
        QStringLiteral("ios-master"),
        QStringLiteral("test-secret"),
        604800,
        1700000000
    );

    const QJsonObject payload = QJsonDocument::fromJson(inflateZlib(fromTencentBase64Url(userSig))).object();
    QCOMPARE(payload.value(QStringLiteral("TLS.ver")).toString(), QStringLiteral("2.0"));
    QCOMPARE(payload.value(QStringLiteral("TLS.identifier")).toString(), QStringLiteral("ios-master"));
    QCOMPARE(payload.value(QStringLiteral("TLS.sdkappid")).toInt(), 1400000000);
    QCOMPARE(payload.value(QStringLiteral("TLS.expire")).toInt(), 604800);
    QCOMPARE(payload.value(QStringLiteral("TLS.time")).toInt(), 1700000000);
    QCOMPARE(payload.value(QStringLiteral("TLS.sig")).toString(), QStringLiteral("57RbWolBbRKyFp7zn2OutFLug34qrpKe0Eb8yaH7GP0="));
}

void TencentUserSigGeneratorTest::generatedUserSigUsesTencentZlibWrapper() {
    const QString userSig = TencentUserSigGenerator::generate(
        1400000000,
        QStringLiteral("ios-master"),
        QStringLiteral("test-secret"),
        604800,
        1700000000
    );

    const QByteArray compressed = fromTencentBase64Url(userSig);
    QVERIFY(compressed.size() > 6);
    QCOMPARE(static_cast<unsigned char>(compressed.at(0)), static_cast<unsigned char>(0x78));
    const quint16 header = (static_cast<quint8>(compressed.at(0)) << 8) | static_cast<quint8>(compressed.at(1));
    QCOMPARE(header % 31, 0);
    QVERIFY(!inflateZlib(compressed).isEmpty());
}

void TencentUserSigGeneratorTest::rejectsInvalidInputs() {
    QVERIFY(TencentUserSigGenerator::generate(0, QStringLiteral("ios-master"), QStringLiteral("test-secret")).isEmpty());
    QVERIFY(TencentUserSigGenerator::generate(1400000000, QStringLiteral("   "), QStringLiteral("test-secret")).isEmpty());
    QVERIFY(TencentUserSigGenerator::generate(1400000000, QStringLiteral("ios-master"), QStringLiteral("   ")).isEmpty());
}

QTEST_MAIN(TencentUserSigGeneratorTest)
#include "TencentUserSigGeneratorTest.moc"
