// 文件作用：桌面端 Agent 工具；DesktopPdfRenderer 负责内容渲染和资源生命周期，本文件给出其声明。
// 对外边界：本文件给出 DesktopPdfRenderer 的类型和函数声明；参数、返回值和对象生命周期以此为准。
// 直接依赖：MaiTool.h；实现与这些接口协作。
#pragma once

#include <atomic>
#include <string>

#include "MaiTool.h"

// Agent 工作线程调用；HTML 已由 MaiPdfTool 限定为工作区内的自包含文档。
MaiToolResult renderDesktopPdf(const std::string &htmlUtf8,
                               const std::string &absoluteOutputPath,
                               const std::atomic<bool> *cancel);
