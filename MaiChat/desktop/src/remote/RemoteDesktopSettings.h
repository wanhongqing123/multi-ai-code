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

    // 无人值守要求必须配好密码。没有凭据时该模式不可用——
    // 这是"空密码 = 任何白名单账号都能自动进"这一危险退化的最后一道闸。
    bool canUseUnattended() const { return secret.isValid(); }

    // 落盘/读取时用的有效模式：配置成无人值守但凭据缺失（例如手工改过配置
    // 文件）时，安全降级为有人值守而不是直接放行。
    RemoteDesktop::HostMode effectiveMode() const {
        if (mode == RemoteDesktop::HostMode::Unattended && !canUseUnattended()) {
            return RemoteDesktop::HostMode::Attended;
        }
        return mode;
    }

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
