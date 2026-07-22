#pragma once

#include <QString>

class RemoteIMCredentialDefaults {
public:
    static constexpr int sdkAppId = 1600148979;

    static QString secretKey() {
        return QStringLiteral("aa18d554f5e4a235640745e98145e187977f87770b812b2b4f10ef032bd73861");
    }
};
