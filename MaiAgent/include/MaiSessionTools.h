// 文件作用：声明通过现有 Agent 会话门面管理会话的模型工具。
// 对外契约：工具只管理会话记录，不删除工作目录文件；删除和工作目录切换必须逐次审批。
#pragma once

#include <memory>

#include "MaiTool.h"

enum class MaiSessionCreateMode {
    InheritWorkspace,
    DefaultOrExplicitDirectory,
};

// 桌面宿主可覆盖内置的新建工具：显式路径用用户指定的目录，省略时用宿主默认目录。
std::unique_ptr<MaiTool> makeMaiCreateSessionTool(MaiSessionCreateMode mode,
                                                  std::string defaultDirectory = {});

// 注册会话列表、新建、删除和当前根会话改名工具；改名只接收 title，不接收目标 ID。
// 它复用普通 MaiUpdateSession 的持久化/事件，只要求普通写审批，不额外逐次审批。
// 仅在宿主按会话路径执行全部文件能力时，才注册
// 工作目录切换工具。调用端仍可直接通过 MaiAgent::submit 执行相同的会话操作。
void registerMaiSessionTools(MaiToolRegistry& registry, bool includeWorkspaceSwitch = true);
