#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include "remote/RemoteDesktopAuth.h"
#include "remote/RemoteDesktopSession.h"

// 远程桌面的本地配置：被控模式、访问密码凭据、允许的发起方、失败计数。
//
// 密码只以 PBKDF2 哈希 + salt 形式落盘，任何时候都不写明文。
struct RemoteDesktopSettings {
    RemoteDesktop::HostMode mode = RemoteDesktop::HostMode::Attended;
    RemoteDesktopAuth::StoredSecret secret;
    QStringList allowedUserIds;
    // 无人值守连续校验失败次数；达到阈值后模式被降级，此计数随之清零。
    int consecutiveAuthFailures = 0;

    // 是否设了访问密码。密码是可选加固，不是无人值守的前提：
    // 白名单本身即授权，密码只是想再加一道锁的人才设。
    bool hasPassword() const { return secret.isValid(); }

    // 无人值守不再要求密码，模式即所选模式。
    RemoteDesktop::HostMode effectiveMode() const { return mode; }

    bool isSenderAllowed(const QString& userId) const;
};

class RemoteDesktopSettingsStore {
public:
    explicit RemoteDesktopSettingsStore(QString filePath);

    // 文件不存在或损坏时返回默认设置（有人值守、无密码、空白名单），
    // 绝不因为解析失败而放宽权限。
    RemoteDesktopSettings load() const;
    bool save(const RemoteDesktopSettings& settings) const;

private:
    QString filePath_;
};
