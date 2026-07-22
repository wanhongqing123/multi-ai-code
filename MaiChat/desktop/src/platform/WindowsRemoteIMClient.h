#pragma once

#include "im/TimSdkRemoteIMClient.h"

class WindowsRemoteIMClient final : public TimSdkRemoteIMClient {
public:
    explicit WindowsRemoteIMClient(QObject* parent = nullptr);
};
