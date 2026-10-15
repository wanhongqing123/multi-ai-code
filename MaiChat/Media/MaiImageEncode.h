// 文件作用：声明把内存中的图像像素编码为可保存或发送的图片文件所需的接口和数据结构。
// 调用边界：平台实现与上层调用方通过本文件约定参数、所有权和释放方式。
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MaiImageEncodeResult {
    unsigned char *bytes;
    size_t size;
    int error_code;
} MaiImageEncodeResult;

// 把 RGBA8 图像编码为 PNG；有 ICC 配置时复制进 PNG iCCP 数据块。
// 色彩原色与传递函数使用 FFmpeg 对应整数枚举值。
// 返回字节由调用方使用 maiImageEncodeFree 释放。
MaiImageEncodeResult maiImageEncodePngRgba(const unsigned char *rgba, int width, int height,
                                           int stride, const unsigned char *icc_profile,
                                           size_t icc_size, int color_primaries,
                                           int color_transfer);
void maiImageEncodeFree(void *bytes);

#ifdef __cplusplus
}
#endif
