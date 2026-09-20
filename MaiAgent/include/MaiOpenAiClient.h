#pragma once

#include <memory>

#include "MaiModelClient.h"

// 按配置造一个模型客户端。
//
// 现在只有 OpenAI 的 chat-completions 这一种实现（GLM、DeepSeek、Kimi 这些都兼容
// 那套线格式），所以工厂就落在这个文件里。将来真加了 Responses 那一路，
// 这里改成按 MaiModelConfig::wire 分发，调用方不用动。
std::unique_ptr<MaiModelClient> makeMaiModelClient(MaiModelConfig config);
