// 文件作用：桌面端本地数据持久化；LocalSettingsStore 负责持久化读写与缓存管理，本文件给出其声明。
// 对外边界：本文件给出 LocalSettingsStore 的类型和函数声明；参数、返回值和对象生命周期以此为准。
#pragma once

#include <QString>

struct LocalIMSettings {
    QString userId;
};

class LocalSettingsStore {
public:
    explicit LocalSettingsStore(QString filePath);

    LocalIMSettings load() const;
    bool save(const LocalIMSettings& settings) const;

private:
    QString filePath_;
};
