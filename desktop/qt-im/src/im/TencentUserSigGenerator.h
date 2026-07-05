#pragma once

#include <QString>

class TencentUserSigGenerator {
public:
    static QString generate(int sdkAppId,
                            const QString& userId,
                            const QString& secretKey,
                            int expireSeconds = 604800,
                            int currentTimeSeconds = -1);
};
