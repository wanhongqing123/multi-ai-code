// 文件作用：上传工作区文件并返回可引用的远程地址。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <memory>
#include <string>

#include "MaiTool.h"

// 将有界工作区文件作为 HTTP(S) PUT 或 POST 的原始正文流式上传，不修改原文件。
// 每次调用都会把本地字节发往远端，必须取得用户审批。
// multipart 表单和供应商凭据不属于此工具的职责。
std::unique_ptr<MaiTool> makeMaiUploadFileTool(std::string caBundlePath = {});
