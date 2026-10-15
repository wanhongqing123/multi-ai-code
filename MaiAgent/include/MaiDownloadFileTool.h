// 文件作用：下载远程文件到受控工作区并报告传输结果。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <memory>
#include <string>

#include "MaiTool.h"

// 将 HTTP(S) 字节下载为 Agent 工作区下的新文件。
// 先流式写临时文件、传输中限制字节数，收到完整成功响应后才交付。
// 调用运行在 Agent 阻塞工具线程，不占用 UI 线程；错误以稳定代码的 JSON 返回，
// 下载内容绝不作为程序执行。
// 传入 caBundlePath 时，下载与任务查询使用同一 CA 信任根。网络失败会返回连接阶段、
// curl 错误码、HTTP 状态和重定向后主机名；带签名 URL 的查询参数绝不回灌给模型。
std::unique_ptr<MaiTool> makeMaiDownloadFileTool(std::string caBundlePath = {});
