#pragma once

#include "im/TimSdkRemoteIMClient.h"

class MacRemoteIMClient final : public TimSdkRemoteIMClient {
public:
    explicit MacRemoteIMClient(QObject* parent = nullptr);
};
