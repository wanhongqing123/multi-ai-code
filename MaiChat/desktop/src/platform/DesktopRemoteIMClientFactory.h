// 文件作用：桌面端操作系统能力适配；DesktopRemoteIMClientFactory 负责对象创建和配置装配，本文件给出其声明。
// 对外边界：本文件给出 DesktopRemoteIMClientFactory 的类型和函数声明；参数、返回值和对象生命周期以此为准。
// 直接依赖：im/RemoteIMClient.h；实现与这些接口协作。
#pragma once

#include <memory>

#include "im/RemoteIMClient.h"

std::unique_ptr<RemoteIMClient> createDesktopRemoteIMClient();

