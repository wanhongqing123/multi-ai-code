// 文件作用：封装 MiniMax 图片及视频生成任务。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "MaiCreativeMediaSupport.h"

// The host supplies its MiniMax API key on demand from secure storage. The callback must be
// worker-safe. An empty key leaves discovery available but disables paid submission.
using MaiMiniMaxApiKeyProvider = std::function<std::string()>;

// 视频使用 H3/H3 Max V2 异步接口。图片使用 image-01 或 image-01-live 的同步接口；
// 无参考图是文生图，一张人物参考图走 character 主体图生图，不提供任意局部重绘。
// 本地图只作为私有 OSS 上传源，供应商收到 HTTPS URL；输出 URL 立刻下载到工作区。
// 两类生成都可能扣费，必须通过工具逐次授权并响应取消；空 Key 只允许 discover。
std::unique_ptr<MaiTool> makeMaiMiniMaxVideoTool(MaiMiniMaxApiKeyProvider apiKey,
                                                 std::string caBundlePath = {},
                                                 MaiCreativeMediaUploadProvider uploadMedia = {});
std::unique_ptr<MaiTool> makeMaiMiniMaxImageTool(MaiMiniMaxApiKeyProvider apiKey,
                                                 std::string caBundlePath = {},
                                                 MaiCreativeMediaUploadProvider uploadMedia = {});
