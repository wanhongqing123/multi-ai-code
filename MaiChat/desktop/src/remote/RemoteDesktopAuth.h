#pragma once

#include <QByteArray>
#include <QString>

// 无人值守模式的访问密码校验。
//
// 为什么需要它：无人值守是自动放行，只认 IM 账号身份是不够的——手机丢了，
// 别人拿着已登录的 MaiChat 就能直接看你电脑。所以再加一层密码。
//
// 三条不变量：
//  1. 被控端本地只存 PBKDF2 哈希，不存明文；
//  2. 明文密码永不进入信令（传的是 HMAC proof）；
//  3. proof 绑定 sessionId/roomId/发起人，每次会话随机 → 抓包重放无效。
namespace RemoteDesktopAuth {

// PBKDF2-HMAC-SHA256 迭代次数。取 10 万次：本地校验一次约几十毫秒，
// 用户无感，但离线爆破成本被显著抬高。
constexpr int kPbkdf2Iterations = 100000;

// 连续校验失败达到此次数后，被控端自动降级为「有人值守」，防在线爆破。
constexpr int kMaxConsecutiveFailures = 5;

struct StoredSecret {
    QByteArray salt;  // 每台设备随机生成一次
    QByteArray hash;  // PBKDF2(password, salt)
    bool isValid() const { return !salt.isEmpty() && !hash.isEmpty(); }
};

// 生成随机 salt（设置密码时调用一次）。
QByteArray generateSalt();

// PBKDF2-HMAC-SHA256（RFC 8018），输出固定 32 字节。
// 暴露出来是为了能用标准测试向量验证实现正确性——自洽的往返测试无法
// 发现「实现算错但前后一致」这类问题。
QByteArray pbkdf2Sha256(const QByteArray& password, const QByteArray& salt, int iterations);

// 由明文密码派生存储用的哈希。空密码返回无效 StoredSecret：
// 调用方据此拒绝「无人值守 + 空密码」这种危险组合。
StoredSecret deriveSecret(const QString& password, const QByteArray& salt);

// 控制端：用明文密码和本次会话参数生成 proof。
// proof = HMAC-SHA256(PBKDF2(password, salt), sessionId|roomId|fromUserId)
// 控制端需要知道被控端的 salt——salt 不是机密，随 invite 前的握手或
// 首次配对时交换即可；一期由用户在控制端输入密码时本地保存配对信息。
QString makeAuthProof(const StoredSecret& secret,
                      const QString& sessionId,
                      const QString& roomId,
                      const QString& fromUserId);

// 被控端：用本地存储的哈希重算并比对。任一参数不符即失败（防重放/防篡改）。
// 使用定长时间比较，避免通过响应时间侧信道逐字节试探。
bool verifyAuthProof(const StoredSecret& secret,
                     const QString& proof,
                     const QString& sessionId,
                     const QString& roomId,
                     const QString& fromUserId);

// 失败计数是否已达到需要降级为「有人值守」的阈值。
bool shouldDowngradeToAttended(int consecutiveFailures);

}  // namespace RemoteDesktopAuth
