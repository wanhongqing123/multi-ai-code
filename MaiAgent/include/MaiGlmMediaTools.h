#pragma once

#include <functional>
#include <memory>
#include <string>

#include "MaiTool.h"

// The host supplies its currently selected GLM credential when a tool runs. The key never enters
// tool metadata, persisted tasks, or model-visible output. The callback must be worker-safe.
using MaiGlmApiKeyProvider = std::function<std::string()>;

// 仅提供 glm-image 异步文生图：delegate 提交并记录任务 ID，continue 查询后下载到
// Agent 工作区。网络调用会阻塞，必须在工具线程执行并响应取消；空 Key 仍可 discover。
// 该接口不接受已有图片或视频，不应由主模型当作图像编辑、视频生成工具。
std::unique_ptr<MaiTool> makeMaiGlmImageTool(MaiGlmApiKeyProvider apiKey,
                                             std::string caBundlePath = {});
