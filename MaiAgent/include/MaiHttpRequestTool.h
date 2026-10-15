// 文件作用：执行受控 HTTP 请求并整理状态与错误响应。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <memory>
#include <string>

#include "MaiTool.h"

// 发送一次有界 HTTP(S) 请求，返回状态、选定响应头、耗时及有界正文。
// 它是模型工具，不是 HTTP 服务端，也不替代 curl_fetch。
// 方法、请求头和正文可能传出工作区数据，因此每次调用都须明确审批。
// CA 证书包路径由宿主提供，只用于 TLS 校验；留空使用 libcurl 的平台默认值。
// 工具实例可由多个工作线程安全共用。
std::unique_ptr<MaiTool> makeMaiHttpRequestTool(std::string caBundlePath = {});
