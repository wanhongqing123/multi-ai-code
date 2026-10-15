// 文件作用：桌面端IM 会话、消息与登录；RemoteIMCredentialDefaults 负责 IM 凭据缺省值与配置边界，本文件给出其声明。
// 对外边界：本文件给出 RemoteIMCredentialDefaults 的类型和函数声明；参数、返回值和对象生命周期以此为准。
// 公开入口：secretKey。
#pragma once

#include <QString>

class RemoteIMCredentialDefaults {
public:
    static constexpr int sdkAppId = 1600148979;

    static QString secretKey() {
        return QStringLiteral("aa18d554f5e4a235640745e98145e187977f87770b812b2b4f10ef032bd73861");
    }
};
