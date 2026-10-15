// 文件作用：声明 Agent 使用的图形处理能力和资源接口。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

// MaiAgent 对外导出 OBS 的 gs_* 图形接口。使用设备资源前须链接 maiagent_graphics，
// 并通过 gs_create 创建平台后端。每次 gs_* 资源调用都要求当前线程先调用
// gs_enter_context，再以 gs_leave_context 配对退出。
// gs_destroy 后全部资源句柄失效；平台后端必须由宿主提供，仅包含此头文件不会自动创建。
#include "graphics.h"
