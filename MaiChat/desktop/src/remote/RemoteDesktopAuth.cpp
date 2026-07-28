#include "remote/RemoteDesktopAuth.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>

namespace RemoteDesktopAuth {
namespace {

constexpr int kSaltBytes = 16;
constexpr int kHashBytes = 32;  // SHA-256 输出长度

QByteArray hmacSha256(const QByteArray& key, const QByteArray& message) {
    QMessageAuthenticationCode code(QCryptographicHash::Sha256);
    code.setKey(key);
    code.addData(message);
    return code.result();
}

// 定长时间比较：普通 == 会在首个不同字节处提前返回，攻击者可据响应时间
// 逐字节试探 proof。
bool constantTimeEquals(const QByteArray& a, const QByteArray& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

// proof 绑定的上下文。sessionId 每次会话随机，因此旧 proof 无法重放到新会话。
QByteArray proofMessage(const QString& sessionId, const QString& roomId, const QString& fromUserId) {
    return (sessionId + QLatin1Char('|') + roomId + QLatin1Char('|') + fromUserId).toUtf8();
}

}  // namespace

QByteArray generateSalt() {
    QByteArray salt(kSaltBytes, 0);
    QRandomGenerator::system()->generate(salt.begin(), salt.end());
    return salt;
}

// PBKDF2-HMAC-SHA256（RFC 8018）。Qt 5.15 没有内置实现，这里按标准写一遍；
// 输出长度固定 32 字节，因此只需一个 block（dkLen <= hLen）。
QByteArray pbkdf2Sha256(const QByteArray& password, const QByteArray& salt, int iterations) {
    if (iterations < 1) return QByteArray();

    QByteArray blockIndex;
    blockIndex.append(char(0)).append(char(0)).append(char(0)).append(char(1));

    QByteArray u = hmacSha256(password, salt + blockIndex);
    QByteArray result = u;
    for (int i = 1; i < iterations; ++i) {
        u = hmacSha256(password, u);
        for (int b = 0; b < kHashBytes; ++b) {
            result[b] = static_cast<char>(result[b] ^ u[b]);
        }
    }
    return result;
}

StoredSecret deriveSecret(const QString& password, const QByteArray& salt) {
    StoredSecret secret;
    // 空密码不产生有效凭据：调用方据此禁用「无人值守」选项。
    if (password.isEmpty() || salt.isEmpty()) return secret;
    secret.salt = salt;
    secret.hash = pbkdf2Sha256(password.toUtf8(), salt, kPbkdf2Iterations);
    return secret;
}

QString makeAuthProof(const StoredSecret& secret,
                      const QString& sessionId,
                      const QString& roomId,
                      const QString& fromUserId) {
    if (!secret.isValid()) return QString();
    return QString::fromLatin1(
        hmacSha256(secret.hash, proofMessage(sessionId, roomId, fromUserId)).toHex());
}

bool verifyAuthProof(const StoredSecret& secret,
                     const QString& proof,
                     const QString& sessionId,
                     const QString& roomId,
                     const QString& fromUserId) {
    // 无凭据或空 proof 一律失败：不允许「没设密码就自动放行」。
    if (!secret.isValid() || proof.isEmpty()) return false;
    const QByteArray expected =
        hmacSha256(secret.hash, proofMessage(sessionId, roomId, fromUserId)).toHex();
    return constantTimeEquals(expected, proof.toLatin1());
}

bool shouldDowngradeToAttended(int consecutiveFailures) {
    return consecutiveFailures >= kMaxConsecutiveFailures;
}

}  // namespace RemoteDesktopAuth
