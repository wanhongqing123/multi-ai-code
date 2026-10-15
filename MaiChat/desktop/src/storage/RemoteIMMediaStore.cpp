// 文件作用：桌面端本地数据持久化；RemoteIMMediaStore 负责持久化读写与缓存管理，本文件给出其实现。
// 主要入口：imageCachePath、voiceCachePath、ensureDir。
// 实现边界：与 RemoteIMMediaStore 的公开声明配套，调用方通过相应接口进入。
#include "storage/RemoteIMMediaStore.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <utility>

RemoteIMMediaStore::RemoteIMMediaStore(QString rootDir) : rootDir_(std::move(rootDir)) {}

QString RemoteIMMediaStore::imageCachePath(const QString& sourceName) const {
    const QString suffix = QFileInfo(sourceName).suffix().isEmpty()
        ? QStringLiteral("jpg")
        : QFileInfo(sourceName).suffix();
    return QDir(ensureDir("images")).filePath(
        QString("remote-im-image-%1.%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(suffix)
    );
}

QString RemoteIMMediaStore::voiceCachePath() const {
    return QDir(ensureDir("voice")).filePath(
        QString("remote-im-voice-%1.m4a").arg(QDateTime::currentMSecsSinceEpoch())
    );
}

QString RemoteIMMediaStore::ensureDir(const QString& name) const {
    QDir dir(rootDir_);
    dir.mkpath(name);
    return dir.filePath(name);
}
