#include "remote/RemoteDesktopSettings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

using RemoteDesktop::HostMode;

QString modeToString(HostMode mode) {
    switch (mode) {
        case HostMode::Disabled: return QStringLiteral("disabled");
        case HostMode::Attended: return QStringLiteral("attended");
        case HostMode::Unattended: return QStringLiteral("unattended");
    }
    return QStringLiteral("attended");
}

// 未知取值一律落到「有人值守」：配置文件被写坏时应当更保守，而不是更开放。
HostMode modeFromString(const QString& value) {
    if (value == QStringLiteral("disabled")) return HostMode::Disabled;
    if (value == QStringLiteral("unattended")) return HostMode::Unattended;
    return HostMode::Attended;
}

}  // namespace

bool RemoteDesktopSettings::isSenderAllowed(const QString& userId) const {
    const QString clean = userId.trimmed();
    if (clean.isEmpty()) return false;
    for (const QString& allowed : allowedUserIds) {
        if (allowed.trimmed() == clean) return true;
    }
    return false;
}

RemoteDesktopSettingsStore::RemoteDesktopSettingsStore(QString filePath)
    : filePath_(std::move(filePath)) {}

RemoteDesktopSettings RemoteDesktopSettingsStore::load() const {
    RemoteDesktopSettings settings;

    QFile file(filePath_);
    if (!file.open(QIODevice::ReadOnly)) return settings;

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return settings;

    const QJsonObject object = document.object();
    settings.mode = modeFromString(object.value(QStringLiteral("mode")).toString());
    settings.consecutiveAuthFailures =
        qMax(0, object.value(QStringLiteral("consecutiveAuthFailures")).toInt(0));

    settings.secret.salt =
        QByteArray::fromBase64(object.value(QStringLiteral("salt")).toString().toLatin1());
    settings.secret.hash =
        QByteArray::fromBase64(object.value(QStringLiteral("passwordHash")).toString().toLatin1());

    const QJsonArray allowed = object.value(QStringLiteral("allowedUserIds")).toArray();
    for (const QJsonValue& value : allowed) {
        const QString userId = value.toString().trimmed();
        if (!userId.isEmpty()) settings.allowedUserIds.append(userId);
    }
    return settings;
}

bool RemoteDesktopSettingsStore::save(const RemoteDesktopSettings& settings) const {
    QDir().mkpath(QFileInfo(filePath_).absolutePath());

    QJsonObject object;
    object.insert(QStringLiteral("mode"), modeToString(settings.mode));
    object.insert(QStringLiteral("consecutiveAuthFailures"), settings.consecutiveAuthFailures);
    // 只写哈希与 salt；明文密码从不落盘。
    object.insert(QStringLiteral("salt"),
                  QString::fromLatin1(settings.secret.salt.toBase64()));
    object.insert(QStringLiteral("passwordHash"),
                  QString::fromLatin1(settings.secret.hash.toBase64()));

    QJsonArray allowed;
    for (const QString& userId : settings.allowedUserIds) {
        const QString clean = userId.trimmed();
        if (!clean.isEmpty()) allowed.append(clean);
    }
    object.insert(QStringLiteral("allowedUserIds"), allowed);

    QFile file(filePath_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(QJsonDocument(object).toJson(QJsonDocument::Indented)) >= 0;
}
