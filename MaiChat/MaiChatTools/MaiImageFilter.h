// 文件作用：声明把图片滤镜参数交给 FFmpeg 滤镜图执行，并将输出像素与错误信息交还移动端所需的接口和数据结构。
// 调用边界：平台实现与上层调用方通过本文件约定参数、所有权和释放方式。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// FFmpeg-backed, in-memory RGBA image filter. Pixels are row-major RGBA8; each output row has
// width * 4 bytes. The JSON selects one validated image operation, never an arbitrary filter graph.
// On success rgba is owned by the caller and error is null. On failure rgba is null and error is
// an owned UTF-8 string. Release either pointer with maiImageFilterFree.
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
