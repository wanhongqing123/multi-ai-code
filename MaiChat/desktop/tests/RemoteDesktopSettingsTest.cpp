#include <QtTest/QtTest>

#include <QTemporaryDir>

#include "remote/RemoteDesktopSettings.h"

using RemoteDesktop::HostMode;

class RemoteDesktopSettingsTest : public QObject {
    Q_OBJECT

private slots:
    void defaultsToAttendedWithoutPassword();
    void roundTripsThroughDisk();
    void neverWritesPlaintextPassword();
    void fallsBackToAttendedWhenUnattendedHasNoPassword();
    void recoversFromCorruptFileWithSafeDefaults();
    void treatsUnknownModeAsAttended();
    void matchesAllowListExactlyAfterTrimming();
};

void RemoteDesktopSettingsTest::defaultsToAttendedWithoutPassword() {
    const RemoteDesktopSettings settings;
    QCOMPARE(static_cast<int>(settings.mode), static_cast<int>(HostMode::Attended));
    QVERIFY(!settings.canUseUnattended());
    QVERIFY(settings.allowedUserIds.isEmpty());
    QCOMPARE(settings.consecutiveAuthFailures, 0);
}

void RemoteDesktopSettingsTest::roundTripsThroughDisk() {
    QTemporaryDir dir;
    const RemoteDesktopSettingsStore store(dir.filePath(QStringLiteral("remote-desktop.json")));

    RemoteDesktopSettings settings;
    settings.mode = HostMode::Unattended;
    settings.secret = RemoteDesktopAuth::deriveSecret(QStringLiteral("pw-123"),
                                                      RemoteDesktopAuth::generateSalt());
    settings.allowedUserIds =
        QStringList{QStringLiteral("whq-iphone"), QStringLiteral("mac-multi-ai-code")};
    settings.consecutiveAuthFailures = 2;
    QVERIFY(store.save(settings));

    const RemoteDesktopSettings loaded = store.load();
    QCOMPARE(static_cast<int>(loaded.mode), static_cast<int>(HostMode::Unattended));
    QCOMPARE(loaded.secret.salt, settings.secret.salt);
    QCOMPARE(loaded.secret.hash, settings.secret.hash);
    QCOMPARE(loaded.allowedUserIds, settings.allowedUserIds);
    QCOMPARE(loaded.consecutiveAuthFailures, 2);
    QVERIFY(loaded.canUseUnattended());
}

void RemoteDesktopSettingsTest::neverWritesPlaintextPassword() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("remote-desktop.json"));
    const RemoteDesktopSettingsStore store(path);

    const QString password = QStringLiteral("super-secret-passphrase");
    RemoteDesktopSettings settings;
    settings.mode = HostMode::Unattended;
    settings.secret =
        RemoteDesktopAuth::deriveSecret(password, RemoteDesktopAuth::generateSalt());
    QVERIFY(store.save(settings));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray onDisk = file.readAll();
    // 配置文件是明文 JSON，密码绝不能出现在里面。
    QVERIFY(!onDisk.contains(password.toUtf8()));
}

void RemoteDesktopSettingsTest::fallsBackToAttendedWhenUnattendedHasNoPassword() {
    RemoteDesktopSettings settings;
    settings.mode = HostMode::Unattended;  // 手工改配置文件也可能造出这种状态
    QVERIFY(!settings.canUseUnattended());
    // 缺凭据时必须收紧到有人值守，而不是无密码自动放行。
    QCOMPARE(static_cast<int>(settings.effectiveMode()), static_cast<int>(HostMode::Attended));
}

void RemoteDesktopSettingsTest::recoversFromCorruptFileWithSafeDefaults() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("remote-desktop.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{ this is not valid json");
    file.close();

    const RemoteDesktopSettings loaded = RemoteDesktopSettingsStore(path).load();
    // 解析失败不得放宽权限。
    QCOMPARE(static_cast<int>(loaded.mode), static_cast<int>(HostMode::Attended));
    QVERIFY(!loaded.canUseUnattended());
    QVERIFY(loaded.allowedUserIds.isEmpty());
}

void RemoteDesktopSettingsTest::treatsUnknownModeAsAttended() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("remote-desktop.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{\"mode\":\"always-allow-everyone\"}");
    file.close();

    QCOMPARE(static_cast<int>(RemoteDesktopSettingsStore(path).load().mode),
             static_cast<int>(HostMode::Attended));
}

void RemoteDesktopSettingsTest::matchesAllowListExactlyAfterTrimming() {
    RemoteDesktopSettings settings;
    settings.allowedUserIds = QStringList{QStringLiteral("  whq-iphone  ")};

    QVERIFY(settings.isSenderAllowed(QStringLiteral("whq-iphone")));
    QVERIFY(settings.isSenderAllowed(QStringLiteral(" whq-iphone ")));
    // 前缀/子串不算命中，避免 whq-iphone-evil 混进来。
    QVERIFY(!settings.isSenderAllowed(QStringLiteral("whq-iphone-evil")));
    QVERIFY(!settings.isSenderAllowed(QStringLiteral("whq")));
    QVERIFY(!settings.isSenderAllowed(QString()));
}

QTEST_MAIN(RemoteDesktopSettingsTest)
#include "RemoteDesktopSettingsTest.moc"
