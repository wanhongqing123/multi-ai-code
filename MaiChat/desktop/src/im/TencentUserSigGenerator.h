// 文件作用：桌面端IM 会话、消息与登录；TencentUserSigGenerator 负责腾讯 IM UserSig 的签名构造与相关参数处理，本文件给出其声明。
// 对外边界：本文件给出 TencentUserSigGenerator 的类型和函数声明；参数、返回值和对象生命周期以此为准。
#pragma once

#include <QString>

class TencentUserSigGenerator {
public:
    static QString generate(int sdkAppId,
                            const QString& userId,
                            const QString& secretKey,
                            int expireSeconds = 604800,
                            int currentTimeSeconds = -1);
};
