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
