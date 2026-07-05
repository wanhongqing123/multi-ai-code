#pragma once

#include <memory>

#include "im/RemoteIMClient.h"

std::unique_ptr<RemoteIMClient> createDesktopRemoteIMClient();

