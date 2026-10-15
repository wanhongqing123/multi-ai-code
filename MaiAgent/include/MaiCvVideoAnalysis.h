// 文件作用：分析视频帧和视频时序中的视觉信息。
// 对外类型：MaiToolContext、MaiCvVideoAnalysisKind、MaiCvVideoAnalysisOptions、MaiCvTimeSegment；线程、失败和生命周期以类型附近注释为准。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MaiToolContext;

enum class MaiCvVideoAnalysisKind { Scene, Motion };

struct MaiCvVideoAnalysisOptions {
    MaiCvVideoAnalysisKind kind = MaiCvVideoAnalysisKind::Scene;
    double sampleIntervalSeconds = 0.5;
    double threshold = 0.45;
    int pixelThreshold = 20;
    double minimumSegmentSeconds = 0.75;
};

struct MaiCvTimeSegment {
    double startSeconds = 0;
    double endSeconds = 0;
};

struct MaiCvVideoAnalysisResult {
    std::vector<MaiCvTimeSegment> segments;
    double durationSeconds = 0;
    std::uint64_t sampledFrames = 0;
    std::uint64_t decodedFrames = 0;
    std::string error;
};

// 流式解码一个本地视频，以 OpenCV 分析采样帧。调用前由宿主解析并校验路径；
// 本函数不创建或修改文件，耗时可能接近完整解码，必须放在工作线程执行。
// 取消时返回错误且不返回片段；时间均以媒体起点为零，单位为秒。
// sampleIntervalSeconds 范围 0.1–5，threshold 严格位于 0–1，
// pixelThreshold 范围 1–255，minimumSegmentSeconds 范围 0–30。
// 无效输入或解码失败返回非空 error；运动分析没有符合阈值的动作时，空片段列表仍是成功。
MaiCvVideoAnalysisResult analyzeMaiCvVideo(const std::string& path,
                                           const MaiCvVideoAnalysisOptions& options,
                                           const MaiToolContext& context);
