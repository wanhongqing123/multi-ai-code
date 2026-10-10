#pragma once

#include <memory>
#include <string>

#include "MaiTool.h"

// Downloads HTTP(S) bytes into a new file below the Agent workspace. The tool streams to a
// temporary file, enforces a byte limit during transfer, and publishes the file only after a
// complete successful response. Calls run on the Agent's blocking tool worker, not a UI thread.
// Errors are returned as JSON text with a stable code; no downloaded content is executed.
// 传入 caBundlePath 时，下载与任务查询使用同一 CA 信任根。网络失败会返回连接阶段、
// curl 错误码、HTTP 状态和重定向后主机名；带签名 URL 的查询参数绝不回灌给模型。
std::unique_ptr<MaiTool> makeMaiDownloadFileTool(std::string caBundlePath = {});
