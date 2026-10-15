// 文件作用：声明把图片文件解码为供移动端或桌面端绘制的像素缓冲所需的接口和数据结构。
// 调用边界：平台实现与上层调用方通过本文件约定参数、所有权和释放方式。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 用 FFmpeg 解码图片第一帧，输出紧密排列的 RGBA8。
// 正数上限在保持宽高比的同时约束输出尺寸；零保留源尺寸。
// rgba 由调用方持有，并用 maiImageDecodeFree 释放。
// icc_profile 非空时指向同一块 rgba 内存，只在该次释放前有效。
// color_primaries 与 color_transfer 使用 FFmpeg 对应枚举的整数值；零表示未指定。
// 此函数不创建图形表面，也不触碰 UI 线程。
typedef struct MaiImageDecodeResult {
    unsigned char *rgba;
    int width;
    int height;
    int stride;
    int error_code;
    const unsigned char *icc_profile;
    int icc_size;
    int color_primaries;
    int color_transfer;
} MaiImageDecodeResult;

MaiImageDecodeResult maiImageDecodeFile(const char *path, int max_width, int max_height);
void maiImageDecodeFree(void *pixels);

#ifdef __cplusplus
}
#endif
