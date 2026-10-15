// 文件作用：声明把图片滤镜参数交给 FFmpeg 滤镜图执行，并将输出像素与错误信息交还移动端所需的接口和数据结构。
// 调用边界：平台实现与上层调用方通过本文件约定参数、所有权和释放方式。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 基于 FFmpeg 的内存 RGBA 图片滤镜。像素按行排列为 RGBA8，每个输出行占 width * 4 字节。
// JSON 只能选择经过校验的图片操作，不能传入任意滤镜图。
// 成功时 rgba 由调用方持有且 error 为空；失败时 rgba 为空，error 是宿主持有的 UTF-8 字符串。
// 两种返回指针都须用 maiImageFilterFree 释放。
typedef struct MaiImageFilterResult {
    unsigned char* rgba;
    int width;
    int height;
    char* error;
} MaiImageFilterResult;
MaiImageFilterResult maiImageFilterRgba(const unsigned char* pixels, int width, int height,
                                       int stride, const char* operationJson);
void maiImageFilterFree(void* pointer);

#ifdef __cplusplus
}
#endif
