#include <QtTest/QtTest>

#include "remote/RemoteDesktopAuth.h"

using namespace RemoteDesktopAuth;

class RemoteDesktopAuthTest : public QObject {
    Q_OBJECT

private slots:
    void matchesStandardPbkdf2Vectors();
    void acceptsMatchingProof();
    void rejectsTamperedSessionId();
    void rejectsTamperedRoomIdOrSender();
    void rejectsEmptyProof();
    void rejectsWrongPassword();
    void refusesEmptyPassword();
    void keepsPlaintextOutOfStorageAndProof();
    void derivesDistinctHashesPerSalt();
    void generatesRandomSalts();
    void downgradesAfterConsecutiveFailures();
};

namespace {

const QString kPassword = QStringLiteral("hunter2-很强的密码");
const QString kSessionId = QStringLiteral("s-abc123");
const QString kRoomId = QStringLiteral("mc-whq-iphone-a1b2c3d4");
const QString kFromUser = QStringLiteral("whq-iphone");

StoredSecret makeSecret(const QString& password = kPassword) {
    static const QByteArray salt = generateSalt();
    return deriveSecret(password, salt);
}

}  // namespace

void RemoteDesktopAuthTest::matchesStandardPbkdf2Vectors() {
    // PBKDF2-HMAC-SHA256 公开测试向量（P="password", S="salt", dkLen=32）。
    // 没有这组断言，往返测试即使在实现算错的情况下也会全绿。
    QCOMPARE(pbkdf2Sha256("password", "salt", 1).toHex(),
             QByteArray("120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"));
    QCOMPARE(pbkdf2Sha256("password", "salt", 2).toHex(),
             QByteArray("ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"));
    QCOMPARE(pbkdf2Sha256("password", "salt", 4096).toHex(),
             QByteArray("c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"));
    // 非法迭代次数不产生可用密钥。
    QVERIFY(pbkdf2Sha256("password", "salt", 0).isEmpty());
}

void RemoteDesktopAuthTest::acceptsMatchingProof() {
    const StoredSecret secret = makeSecret();
    const QString proof = makeAuthProof(secret, kSessionId, kRoomId, kFromUser);

    QVERIFY(!proof.isEmpty());
    QVERIFY(verifyAuthProof(secret, proof, kSessionId, kRoomId, kFromUser));
}

void RemoteDesktopAuthTest::rejectsTamperedSessionId() {
    // 防重放的核心：旧 proof 换到新 sessionId 上必须失效。
    const StoredSecret secret = makeSecret();
    const QString proof = makeAuthProof(secret, kSessionId, kRoomId, kFromUser);

    QVERIFY(!verifyAuthProof(secret, proof, QStringLiteral("s-other"), kRoomId, kFromUser));
}

void RemoteDesktopAuthTest::rejectsTamperedRoomIdOrSender() {
    const StoredSecret secret = makeSecret();
    const QString proof = makeAuthProof(secret, kSessionId, kRoomId, kFromUser);

    QVERIFY(!verifyAuthProof(secret, proof, kSessionId, QStringLiteral("mc-evil"), kFromUser));
    QVERIFY(!verifyAuthProof(secret, proof, kSessionId, kRoomId, QStringLiteral("attacker")));
}

void RemoteDesktopAuthTest::rejectsEmptyProof() {
    // 「没带 proof 就放行」是无人值守最危险的退化路径，必须显式挡住。
    const StoredSecret secret = makeSecret();
    QVERIFY(!verifyAuthProof(secret, QString(), kSessionId, kRoomId, kFromUser));
    QVERIFY(!verifyAuthProof(secret, QStringLiteral(""), kSessionId, kRoomId, kFromUser));
}

void RemoteDesktopAuthTest::rejectsWrongPassword() {
    const QByteArray salt = generateSalt();
    const StoredSecret stored = deriveSecret(kPassword, salt);
    const StoredSecret attacker = deriveSecret(QStringLiteral("wrong-password"), salt);

    const QString proof = makeAuthProof(attacker, kSessionId, kRoomId, kFromUser);
    QVERIFY(!verifyAuthProof(stored, proof, kSessionId, kRoomId, kFromUser));
}

void RemoteDesktopAuthTest::refusesEmptyPassword() {
    const StoredSecret secret = deriveSecret(QString(), generateSalt());
    QVERIFY(!secret.isValid());
    // 无效凭据下 proof 生成与校验都必须失败，杜绝「空密码 = 任何人可进」。
    QVERIFY(makeAuthProof(secret, kSessionId, kRoomId, kFromUser).isEmpty());
    QVERIFY(!verifyAuthProof(secret, QStringLiteral("anything"), kSessionId, kRoomId, kFromUser));
}

void RemoteDesktopAuthTest::keepsPlaintextOutOfStorageAndProof() {
    const StoredSecret secret = makeSecret();
    const QString proof = makeAuthProof(secret, kSessionId, kRoomId, kFromUser);
    const QByteArray plaintext = kPassword.toUtf8();

    QVERIFY(!secret.hash.contains(plaintext));
    QVERIFY(!secret.salt.contains(plaintext));
    QVERIFY(!proof.toUtf8().contains(plaintext));
    // proof 是 HMAC-SHA256 的 hex 表示，长度固定 64。
    QCOMPARE(proof.size(), 64);
}

void RemoteDesktopAuthTest::derivesDistinctHashesPerSalt() {
    const StoredSecret a = deriveSecret(kPassword, generateSalt());
    const StoredSecret b = deriveSecret(kPassword, generateSalt());
    QVERIFY(a.isValid() && b.isValid());
    // 同密码不同 salt → 不同哈希，避免彩虹表跨设备复用。
    QVERIFY(a.hash != b.hash);
}

void RemoteDesktopAuthTest::generatesRandomSalts() {
    QSet<QByteArray> salts;
    for (int i = 0; i < 16; ++i) salts.insert(generateSalt());
    QCOMPARE(salts.size(), 16);
}

void RemoteDesktopAuthTest::downgradesAfterConsecutiveFailures() {
    QVERIFY(!shouldDowngradeToAttended(0));
    QVERIFY(!shouldDowngradeToAttended(kMaxConsecutiveFailures - 1));
    QVERIFY(shouldDowngradeToAttended(kMaxConsecutiveFailures));
    QVERIFY(shouldDowngradeToAttended(kMaxConsecutiveFailures + 3));
}

QTEST_MAIN(RemoteDesktopAuthTest)
#include "RemoteDesktopAuthTest.moc"
