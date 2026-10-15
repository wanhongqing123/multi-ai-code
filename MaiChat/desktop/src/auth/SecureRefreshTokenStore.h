// 文件作用：在 macOS Keychain 或 Windows Credential Manager 中保存桌面端的小型敏感字符串。
// 类型关系：SecureRefreshTokenStore 默认保存会话刷新令牌；模型服务令牌用独立 service/account 复用它。
// 对外边界：只写系统安全存储，不写 QSettings；访问令牌、IM UserSig 和扫码 verifier 不持久化。
// 直接依赖：Security.framework 或 WinCred；其它平台不提供持久化能力。
#pragma once

#include <QString>

// 默认项是刷新令牌，其他调用方必须传入独立 service/account。
class SecureRefreshTokenStore {
public:
    explicit SecureRefreshTokenStore(
        QString service = QStringLiteral("com.maichat.desktop.auth"),
        QString account = QStringLiteral("refresh_token"));
    QString read() const;
    bool write(const QString& token) const;
    bool remove() const;

private:
    QString service_;
    QString account_;
};
