#pragma once

#include <atomic>
#include <string>

#include "MaiTool.h"

// Agent 工作线程调用；HTML 已由 MaiPdfTool 限定为工作区内的自包含文档。
MaiToolResult renderDesktopPdf(const std::string &htmlUtf8,
                               const std::string &absoluteOutputPath,
                               const std::atomic<bool> *cancel);
