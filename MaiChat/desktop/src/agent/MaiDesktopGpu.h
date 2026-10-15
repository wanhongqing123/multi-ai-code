// 文件作用：桌面端创作中心和 Agent 会话；MaiDesktopGpu 负责桌面 GPU 设备与渲染资源管理，本文件给出其声明。
// 对外边界：本文件给出 MaiDesktopGpu 的类型和函数声明；参数、返回值和对象生命周期以此为准。
// 直接依赖：MaiSystemResourcesTool.h；实现与这些接口协作。
#pragma once

#include <optional>

#include "MaiSystemResourcesTool.h"

// 返回当前桌面平台公开的 GPU 计数器；不可取得的指标保持缺席。
std::optional<MaiGpuResources> maiDesktopGpuResources();
