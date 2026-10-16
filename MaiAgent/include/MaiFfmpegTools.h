// 文件作用：封装 FFmpeg 媒体探测、转码和剪辑工具。
// 对外类型：MaiFfmpegEngine；线程、失败和生命周期以类型附近注释为准。
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "MaiTool.h"
#include "MaiViewImageTool.h"

// 宿主持有并链接内嵌 FFmpeg 引擎。每个回调都在调用方进程的 Agent 工具线程运行；
// 必须返回退出状态，不得直接终止 App。可选日志和错误回调在串行工具调用期间触发。
// setLogSink(nullptr, nullptr) 返回后，日志接收方不得继续引用 opaque 指针；
// 日志行可能来自 FFmpeg 自己的工作线程。
struct MaiFfmpegEngine {
    int (*runFfmpeg)(int argc, char** argv) = nullptr;
    void (*setFfmpegCancelCheck)(int (*check)(void*), void* opaque) = nullptr;
    int (*runFfprobe)(int argc, char** argv) = nullptr;
    void (*setFfprobeCancelCheck)(int (*check)(void*), void* opaque) = nullptr;
    void (*setLogSink)(void (*sink)(void* opaque, const char* line), void* opaque) = nullptr;
    int (*describeError)(int code, char* buffer, std::size_t size) = nullptr;
};

// 本地视频探测结果使用 FFprobe 的容器/视频流实测值，不接受模型或用户估算。
// durationSeconds 是总时长；framesPerSecond 优先取平均帧率，缺失时取声明帧率。
struct MaiVideoMetadata {
    double durationSeconds = 0;
    double framesPerSecond = 0;
};

// 在 Agent 工具线程读取一段本地视频的时长和帧率。回调只读文件，支持 context 取消；
// 路径越界、无视频流、FFprobe 失败或关键元数据缺失时返回明确错误。
using MaiVideoMetadataProvider =
    std::function<MaiResult<MaiVideoMetadata>(const std::string&, const MaiToolContext&)>;

// 对应引擎回调为空时不得注册工具。FFmpeg 原始命令代码使用进程全局状态，
// 所以 ffmpeg 与 ffprobe 工具必须串行调用；取消信号在进程内送达活动引擎。
std::unique_ptr<MaiTool> makeMaiFfmpegTool(MaiFfmpegEngine engine);
std::unique_ptr<MaiTool> makeMaiFfprobeTool(MaiFfmpegEngine engine);

// 把宿主提供的内嵌 FFprobe 引擎封装为强类型元数据回调。runFfprobe 为空时返回空回调；
// 返回的回调不拥有引擎函数，宿主必须保证底层代码在调用结束前仍已加载。
MaiVideoMetadataProvider makeMaiFfprobeVideoMetadataProvider(MaiFfmpegEngine engine);

// 生成边长均不超过 1280 像素的 JPEG 模型观察图，不修改原图片。
// 预览文件留在 Agent 工作区供后续轮次使用；引擎调用与普通 FFmpeg 工具串行。
MaiImagePreviewCallback makeMaiFfmpegImagePreview(MaiFfmpegEngine engine);
MaiModelImagePreparer makeMaiFfmpegModelImagePreparer(MaiFfmpegEngine engine);
