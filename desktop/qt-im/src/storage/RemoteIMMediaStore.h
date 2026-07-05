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
