// 文件作用：把视频水印移除流程封装为单一 Agent 工具。
// 对外类型：MaiWatermarkForegroundEdge、MaiWatermarkRepairRegion、MaiTemporalWatermarkOptions、
// MaiTemporalWatermarkResult；线程、失败和生命周期以类型附近注释为准。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "MaiTool.h"

enum class MaiWatermarkForegroundEdge {
    None,
    Left,
    Right,
};

// 每个区域使用原始解码帧坐标；endSeconds 为 -1 时一直处理到片尾。
// 有无遮挡参考帧时自动借背景；没有时由本地模型补画，不向 Agent 暴露算法模式。
// beforeSeconds 为 -1 表示没有可用参考帧；afterSeconds 为 -1 表示只借前帧。
// 时间窗和参考时刻必须由调用方对原片核实，不能把视频任务 ID 当时间戳。
struct MaiWatermarkRepairRegion {
    MaiWatermarkForegroundEdge foregroundEdge = MaiWatermarkForegroundEdge::None;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    double startSeconds = 0;
    double endSeconds = -1;
    // 非负时以解码帧号精确裁剪，优先于秒数；endFrame 不包含。
    int startFrame = -1;
    int endFrame = -1;
    double beforeSeconds = -1;
    double afterSeconds = -1;
};

struct MaiTemporalWatermarkOptions {
    std::string modelPath;
    std::string runtimePath;
    const void* ortApiBase = nullptr;
    std::vector<MaiWatermarkRepairRegion> regions;
};

struct MaiTemporalWatermarkResult {
    std::string outputPath;
    std::string error;
    int frames = 0;
    int audioStreamsCopied = 0;
    int modelCalls = 0;
    int foregroundFrames = 0;
    // 只在靠近画面底部、能可靠分出亮色字样的无参考帧区域启用局部遮罩。
    int localizedTextRegions = 0;
    double durationSeconds = 0;
    // 耗时均为毫秒；阶段时间是包含关系，modelInferenceMs 包含在扫描或出片阶段中。
    double modelInitializationMs = 0;
    double referenceScanMs = 0;
    double modelInferenceMs = 0;
    double repairAndEncodeMs = 0;
    double totalMs = 0;
    // 本次会话实际请求的后端名；当前正式 LaMa 路径为 "cpu"。
    std::string inferenceBackend;
};

// 宿主在 Agent 工作线程调用共享 C++ 视频处理器；输入只读，输出必须是新文件。
// 失败或取消时处理器删掉未完成输出。模型路径和 ORT 入口来自宿主，绝不从
// Agent 工具参数获取，避免模型指定任意二进制或访问工作区外文件。
using MaiTemporalWatermarkProcessor = MaiTemporalWatermarkResult (*)(
    const std::string& inputPath, const std::string& outputPath,
    const MaiTemporalWatermarkOptions& options, const MaiToolContext& context);

// 注册为唯一的 media_remove_watermark Agent 工具；没有本地模型时不注册。
std::unique_ptr<MaiTool> makeMaiTemporalWatermarkTool(MaiTemporalWatermarkProcessor processor,
                                                      std::string modelPath,
                                                      std::string runtimePath = {},
                                                      const void* apiBase = nullptr);
