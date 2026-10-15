// 文件作用：实现 OpenAI Responses 协议的请求和流式解析。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <memory>

#include "MaiModelClient.h"

// OpenAI 兼容的 Responses 线协议实现。配置由调用方提供；
// 返回中立的 MaiModelClient 接口，供应商格式不泄漏到上层。
std::unique_ptr<MaiModelClient> makeMaiResponsesClient(MaiModelConfig config);
