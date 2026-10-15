// 文件作用：桌面端本地数据持久化；LocalSettingsStore 负责持久化读写与缓存管理，本文件给出其实现。
// 主要入口：load、save。
// 实现边界：与 LocalSettingsStore 的公开声明配套，调用方通过相应接口进入。
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
