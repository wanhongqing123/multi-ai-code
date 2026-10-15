// 文件作用：在权限边界内执行 shell 命令并收集结果。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <memory>

#include "MaiTool.h"

// shell：跑一条命令。
//
// **平台不一定支持**（iOS 的沙箱不允许 exec）。注册之前先问
// maiIsProcessExecutionSupported()——摆一个永远失败的工具比不摆更糟。
std::unique_ptr<MaiTool> makeMaiShellTool();
