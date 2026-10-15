// 文件作用：管理桌面创作中心共享服务令牌，并把旧 QSettings 明文迁到系统安全存储。
// 类型关系：ModelServiceTokenStore 复用 SecureRefreshTokenStore 的系统存储能力。
// 对外边界：迁移失败会清除旧明文；有效环境令牌可继续使用，否则显式报告失败。
#pragma once

#include <QSettings>
#include <QString>

#include "auth/SecureRefreshTokenStore.h"

class ModelServiceTokenStore {
public:
    struct LoadResult {
        QString token;
        bool secureStorageFailed = false;
    };

    explicit ModelServiceTokenStore(
        QString service = QStringLiteral("com.maichat.desktop.model"),
        QString account = QStringLiteral("cloud_service_token"));
    static ModelServiceTokenStore forCurrentApplication();

    LoadResult loadAndMigrate(QSettings& settings) const;
    bool save(QSettings& settings, const QString& token) const;
    QString readStoredToken() const;

private:
    SecureRefreshTokenStore secureStore_;
};
