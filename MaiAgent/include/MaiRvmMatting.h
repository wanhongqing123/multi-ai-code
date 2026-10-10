#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "MaiError.h"

struct MaiRvmMattingResult {
    std::vector<float> foregroundRgbPlanar;
    std::vector<float> alpha;
};

// 一个 RVM MobileNetV3 抠像会话处理一条视频流。帧必须在同一工作线程按显示顺序
// 依次送入；四份递归张量跨帧保留，只在开始处理新视频时调用 reset() 清空。
// 调用方在 matte()/infer() 期间持有 RGB32F 平面缓冲 [1,3,H,W]，像素先归一化到
// [0,1]。输出透明度形状为 [H,W]，不借用输入内存；尺寸或运行库无效时返回错误，
// 不修改源视频帧。
// runtimePath 只能指向宿主打包的运行库；留空时仅查找进程已链接的 OrtGetApiBase，
// 不搜索系统任意路径。头文件来自仓库内的 ONNX Runtime 源码，实际运行库仍由
// 各平台宿主打包并通过 runtimePath 或 apiBase 提供。
class MaiRvmMattingSession {
public:
    static MaiResult<std::unique_ptr<MaiRvmMattingSession>> open(
        const std::string& modelPath, const std::string& runtimePath = {},
        const void* apiBase = nullptr);
    ~MaiRvmMattingSession();

    MaiRvmMattingSession(const MaiRvmMattingSession&) = delete;
    MaiRvmMattingSession& operator=(const MaiRvmMattingSession&) = delete;

    MaiResult<std::vector<float>> matte(float* rgbPlanar, int width, int height,
                                        float downsampleRatio = 0.25f);
    // 同时返回 RVM 预测的人物前景 RGB 和透明度，用于处理边缘污染。
    // 完全不透明的区域可直接取解码源帧颜色，减少模型造成的肤色/衣服偏差。
    MaiResult<MaiRvmMattingResult> infer(float* rgbPlanar, int width, int height,
                                         float downsampleRatio = 0.25f);
    MaiError reset();
    std::string runtimeVersion() const;

private:
    MaiRvmMattingSession() = default;
    class Impl;
    std::unique_ptr<Impl> mImpl;
};
