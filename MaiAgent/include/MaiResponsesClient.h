#pragma once

#include <memory>

#include "MaiModelClient.h"

// OpenAI 兼容的 Responses 线协议实现。配置由调用方提供；
// 返回中立的 MaiModelClient 接口，供应商格式不泄漏到上层。
std::unique_ptr<MaiModelClient> makeMaiResponsesClient(MaiModelConfig config);
