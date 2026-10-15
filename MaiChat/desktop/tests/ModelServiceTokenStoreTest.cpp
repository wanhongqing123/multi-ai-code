// 文件作用：验证旧模型服务令牌从 INI 迁到隔离 Keychain 项，并且新保存不落明文。
// 运行边界：临时 INI、随机 service 和 43 字符假令牌；测试结束必删 Keychain 项。
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include "auth/ModelServiceTokenStore.h"

class ModelServiceTokenStoreTest : public QObject {
    Q_OBJECT

private slots:
    void migratesLegacySettingAndNeverPersistsNewToken();
    void failedSecureStorageStillDeletesLegacyPlaintext();
    void failedMigrationUsesValidEnvironmentTokenUntilItIsRemoved();
};

void ModelServiceTokenStoreTest::migratesLegacySettingAndNeverPersistsNewToken() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("model.ini")), QSettings::IniFormat);
    const QString service = QStringLiteral("com.maichat.desktop.model.test.") +
                            QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString account = QStringLiteral("cloud_service_token");
    SecureRefreshTokenStore secureStore(service, account);
    struct Cleanup {
        SecureRefreshTokenStore& store;
        ~Cleanup() { store.remove(); }
    } cleanup{secureStore};
    ModelServiceTokenStore modelStore(service, account);
    const QString legacyToken(43, QLatin1Char('L'));
    const QString newToken(43, QLatin1Char('N'));

    settings.setValue(QStringLiteral("agent/cloudServiceToken"), legacyToken);
    settings.sync();
    QCOMPARE(modelStore.loadAndMigrate(settings).token, legacyToken);
    QVERIFY(!settings.contains(QStringLiteral("agent/cloudServiceToken")));
    QCOMPARE(secureStore.read(), legacyToken);

    QVERIFY(modelStore.save(settings, newToken));
    QCOMPARE(modelStore.readStoredToken(), newToken);
    QVERIFY(!settings.contains(QStringLiteral("agent/cloudServiceToken")));
    QFile iniFile(settings.fileName());
    QVERIFY(iniFile.open(QIODevice::ReadOnly));
    const QByteArray iniBytes = iniFile.readAll();
    QVERIFY(!iniBytes.contains(legacyToken.toUtf8()));
    QVERIFY(!iniBytes.contains(newToken.toUtf8()));
}

void ModelServiceTokenStoreTest::failedSecureStorageStillDeletesLegacyPlaintext() {
    const bool envWasSet = qEnvironmentVariableIsSet("MAICHAT_MEDIA_SERVICE_TOKEN");
    const QByteArray previousEnv = qgetenv("MAICHAT_MEDIA_SERVICE_TOKEN");
    struct RestoreEnv {
        bool wasSet;
        QByteArray previous;
        ~RestoreEnv() {
            if (wasSet) qputenv("MAICHAT_MEDIA_SERVICE_TOKEN", previous);
            else qunsetenv("MAICHAT_MEDIA_SERVICE_TOKEN");
        }
    } restore{envWasSet, previousEnv};
    qunsetenv("MAICHAT_MEDIA_SERVICE_TOKEN");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("failed-migration.ini")),
                       QSettings::IniFormat);
    const QString legacyToken(43, QLatin1Char('F'));
    settings.setValue(QStringLiteral("agent/cloudServiceToken"), legacyToken);
    settings.sync();
    // 空 service 是系统存储适配器明确拒绝的无效配置，模拟安全存储不可用。
    ModelServiceTokenStore unavailable(QString(), QStringLiteral("cloud_service_token"));

    const auto first = unavailable.loadAndMigrate(settings);
    QVERIFY(first.secureStorageFailed);
    QVERIFY(first.token.isEmpty());
    QVERIFY(!settings.contains(QStringLiteral("agent/cloudServiceToken")));
    const auto second = unavailable.loadAndMigrate(settings);
    QVERIFY(second.secureStorageFailed);
    QFile iniFile(settings.fileName());
    QVERIFY(iniFile.open(QIODevice::ReadOnly));
    QVERIFY(!iniFile.readAll().contains(legacyToken.toUtf8()));
    iniFile.close();

    const QString recoveryService = QStringLiteral("com.maichat.desktop.model.test.") +
                                    QUuid::createUuid().toString(QUuid::WithoutBraces);
    SecureRefreshTokenStore recoverySecureStore(
        recoveryService, QStringLiteral("cloud_service_token"));
    struct Cleanup {
        SecureRefreshTokenStore& store;
        ~Cleanup() { store.remove(); }
    } cleanup{recoverySecureStore};
    ModelServiceTokenStore recoveredStore(
        recoveryService, QStringLiteral("cloud_service_token"));
    const QString replacement(43, QLatin1Char('R'));
    QVERIFY(recoveredStore.save(settings, replacement));
    const auto recovered = recoveredStore.loadAndMigrate(settings);
    QVERIFY(!recovered.secureStorageFailed);
    QCOMPARE(recovered.token, replacement);
}

void ModelServiceTokenStoreTest::failedMigrationUsesValidEnvironmentTokenUntilItIsRemoved() {
    const bool envWasSet = qEnvironmentVariableIsSet("MAICHAT_MEDIA_SERVICE_TOKEN");
    const QByteArray previousEnv = qgetenv("MAICHAT_MEDIA_SERVICE_TOKEN");
    struct RestoreEnv {
        bool wasSet;
        QByteArray previous;
        ~RestoreEnv() {
            if (wasSet) qputenv("MAICHAT_MEDIA_SERVICE_TOKEN", previous);
            else qunsetenv("MAICHAT_MEDIA_SERVICE_TOKEN");
        }
    } restore{envWasSet, previousEnv};

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("environment-fallback.ini")),
                       QSettings::IniFormat);
    const QString legacyToken(43, QLatin1Char('L'));
    const QByteArray environment(43, 'E');
    settings.setValue(QStringLiteral("agent/cloudServiceToken"), legacyToken);
    settings.sync();
    qputenv("MAICHAT_MEDIA_SERVICE_TOKEN", environment);
    ModelServiceTokenStore unavailable(QString(), QStringLiteral("cloud_service_token"));

    const auto first = unavailable.loadAndMigrate(settings);
    QCOMPARE(first.token, QString::fromLatin1(environment));
    QVERIFY(!first.secureStorageFailed);
    QVERIFY(!settings.contains(QStringLiteral("agent/cloudServiceToken")));
    QVERIFY(settings.value(QStringLiteral("agent/cloudServiceTokenMigrationFailed")).toBool());
    QCOMPARE(unavailable.loadAndMigrate(settings).token, first.token);

    qputenv("MAICHAT_MEDIA_SERVICE_TOKEN", QByteArray(31, 'S'));
    const auto invalidEnvironment = unavailable.loadAndMigrate(settings);
    QVERIFY(invalidEnvironment.token.isEmpty());
    QVERIFY(invalidEnvironment.secureStorageFailed);
    qunsetenv("MAICHAT_MEDIA_SERVICE_TOKEN");
    const auto missingEnvironment = unavailable.loadAndMigrate(settings);
    QVERIFY(missingEnvironment.token.isEmpty());
    QVERIFY(missingEnvironment.secureStorageFailed);

    QFile iniFile(settings.fileName());
    QVERIFY(iniFile.open(QIODevice::ReadOnly));
    const QByteArray iniBytes = iniFile.readAll();
    QVERIFY(!iniBytes.contains(legacyToken.toUtf8()));
    QVERIFY(!iniBytes.contains(environment));
}

QTEST_GUILESS_MAIN(ModelServiceTokenStoreTest)
#include "ModelServiceTokenStoreTest.moc"
