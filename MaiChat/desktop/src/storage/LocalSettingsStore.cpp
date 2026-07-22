#include "storage/LocalSettingsStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <utility>

LocalSettingsStore::LocalSettingsStore(QString filePath) : filePath_(std::move(filePath)) {}

LocalIMSettings LocalSettingsStore::load() const {
    QFile file(filePath_);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    return LocalIMSettings{object["userId"].toString()};
}

bool LocalSettingsStore::save(const LocalIMSettings& settings) const {
    QFileInfo info(filePath_);
    QDir().mkpath(info.absolutePath());
    QFile file(filePath_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    QJsonObject object;
    object["userId"] = settings.userId.trimmed();
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    return true;
}
