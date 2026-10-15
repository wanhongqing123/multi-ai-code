// 文件作用：桌面端本地数据持久化；RemoteIMMediaStore 负责持久化读写与缓存管理，本文件给出其声明。
// 对外边界：本文件给出 RemoteIMMediaStore 的类型和函数声明；参数、返回值和对象生命周期以此为准。
#pragma once

#include <QString>

class RemoteIMMediaStore {
public:
    explicit RemoteIMMediaStore(QString rootDir);

    QString imageCachePath(const QString& sourceName) const;
    QString voiceCachePath() const;

private:
    QString ensureDir(const QString& name) const;

    QString rootDir_;
};
