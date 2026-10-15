// 文件作用：封装智谱图片媒体能力的请求与结果处理。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "MaiTool.h"

// 工具执行时由宿主提供当前选定的 GLM 凭据。
// 密钥不得进入工具元数据、持久化任务或模型可见输出；回调须能在工作线程安全调用。
using MaiGlmApiKeyProvider = std::function<std::string()>;

// 仅提供 glm-image 异步文生图：delegate 提交并记录任务 ID，continue 查询后下载到
// Agent 工作区。网络调用会阻塞，必须在工具线程执行并响应取消；空 Key 仍可 discover。
// 该接口不接受已有图片或视频，不应由主模型当作图像编辑、视频生成工具。
std::unique_ptr<MaiTool> makeMaiGlmImageTool(MaiGlmApiKeyProvider apiKey,
                                             std::string caBundlePath = {});
