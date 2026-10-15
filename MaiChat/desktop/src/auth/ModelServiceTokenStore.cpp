// 文件作用：执行模型服务令牌的安全迁移、读取和保存；旧 INI 项只在迁移时读取。
// 主要入口：loadAndMigrate 和 save；迁移失败时可用有效环境令牌继续工作。
// 实现边界：只处理令牌；服务 URL、模型选择仍由调用方保存在 QSettings。
#include "auth/ModelServiceTokenStore.h"

#include <QCoreApplication>
#include <utility>

namespace {

const QString kLegacySetting = QStringLiteral("agent/cloudServiceToken");
const QString kMigrationFailureSetting =
    QStringLiteral("agent/cloudServiceTokenMigrationFailed");

QString environmentToken() {
    const QString token = qEnvironmentVariable("MAICHAT_MEDIA_SERVICE_TOKEN");
    return token.size() >= 32 ? token : QString();
}

}  // namespace

ModelServiceTokenStore::ModelServiceTokenStore(QString service, QString account)
    : secureStore_(std::move(service), std::move(account)) {}

ModelServiceTokenStore ModelServiceTokenStore::forCurrentApplication() {
    const QString organization = QCoreApplication::organizationName();
    const QString application = QCoreApplication::applicationName();
    if (organization == QStringLiteral("MaiChat") &&
        application == QStringLiteral("Desktop IM"))
        return ModelServiceTokenStore();
    // 自动化使用独立 Keychain/CredMan 项，避免测试碰正式桌面账号或模型令牌。
    return ModelServiceTokenStore(
        QStringLiteral("com.maichat.desktop.model.test.") + organization +
            QLatin1Char('.') + application,
        QStringLiteral("cloud_service_token"));
}

ModelServiceTokenStore::LoadResult
ModelServiceTokenStore::loadAndMigrate(QSettings& settings) const {
    if (settings.contains(kLegacySetting)) {
        const QString legacy = settings.value(kLegacySetting).toString();
        const bool stored = legacy.isEmpty() ||
                            (secureStore_.write(legacy) && secureStore_.read() == legacy);
        // 即使系统安全存储失败，也立即清除旧 INI 明文；无环境令牌时要求用户重填。
        settings.remove(kLegacySetting);
        if (stored) settings.remove(kMigrationFailureSetting);
        else settings.setValue(kMigrationFailureSetting, true);
        settings.sync();
        if (settings.status() != QSettings::NoError || settings.contains(kLegacySetting))
            return {{}, true};
        if (!stored) {
            const QString environment = environmentToken();
            return {environment, environment.isEmpty()};
        }
    }
    if (settings.value(kMigrationFailureSetting, false).toBool()) {
        const QString environment = environmentToken();
        return {environment, environment.isEmpty()};
    }
    const QString stored = secureStore_.read();
    const QString environment = environmentToken();
    return {stored.isEmpty() ? environment : stored, false};
}

bool ModelServiceTokenStore::save(QSettings& settings, const QString& token) const {
    if (token.isEmpty()) {
        settings.remove(kLegacySetting);
        settings.setValue(kMigrationFailureSetting, true);
        settings.sync();
        return false;
    }
    const QString previous = secureStore_.read();
    const bool stored = secureStore_.write(token) && secureStore_.read() == token;
    settings.remove(kLegacySetting);
    if (stored) settings.remove(kMigrationFailureSetting);
    else if (previous.isEmpty()) settings.setValue(kMigrationFailureSetting, true);
    settings.sync();
    if (stored && settings.status() == QSettings::NoError &&
        !settings.contains(kLegacySetting))
        return true;
    if (previous.isEmpty()) secureStore_.remove();
    else secureStore_.write(previous);
    return false;
}

QString ModelServiceTokenStore::readStoredToken() const {
    return secureStore_.read();
}
