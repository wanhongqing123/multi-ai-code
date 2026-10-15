// 文件作用：声明 Agent 使用的图形处理能力和资源接口。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

// OBS's gs_* interface is the Graphics API exported by MaiAgent. Link the
// maiagent_graphics target and create a backend with gs_create before using
// device resources. All gs_* resource calls require gs_enter_context on the
// calling thread and must be balanced with gs_leave_context. Resource handles
// become invalid after gs_destroy. A platform backend must be available and
// passed to gs_create; the header alone does not provide one.
#include "graphics.h"
