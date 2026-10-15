// 文件作用：将 RVM 人像遮罩与原视频背景合成。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <cstddef>
#include <cstdint>

// 用 RVM 输出的平面 FP32 前景与 alpha 合成一帧 RGB8 图像。
// 源图、背景和目标均为交错 RGB8；本次调用之外不能假定它们共享存储或生命周期。
// 完全不透明的源像素原样复制；仅软边缘使用模型前景消除原背景污染，
// 完全透明的像素来自替换背景。调用方须为每个 RGB8 缓冲提供 3*pixelCount 字节，
// 前景提供 3*pixelCount 个浮点数，alpha 提供 pixelCount 个浮点数。
void maiRvmCompositeRgb8(const std::uint8_t* source, const float* foreground, const float* alpha,
                         const std::uint8_t* background, std::uint8_t* destination,
                         std::size_t pixelCount);
