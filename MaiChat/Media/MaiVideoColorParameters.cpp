// 文件作用：描述视频色域与转换矩阵参数，供解码和渲染链路使用。
// 调用边界：由 MaiChat 媒体或 Agent 宿主调用；平台资源在停止或失败时须正确释放。
#include "MaiVideoColorParameters.h"

// 色彩系数及有限／全范围归一化遵循 OBS 的 libobs/media-io/video-matrices.c。
// 着色器使用与 format_conversion.effect 相同的 color_vec0/1/2 和 color_range_min/max 参数。
MaiVideoColorParameters maiVideoColorParameters(MaiVideoColorSpace space,
                                                MaiVideoColorRange range) {
    float kb = 0.0722f;
    float kr = 0.2126f;
    if (space == MAI_VIDEO_COLOR_BT601) {
        kb = 0.114f;
        kr = 0.299f;
    } else if (space == MAI_VIDEO_COLOR_BT2020) {
        kb = 0.0593f;
        kr = 0.2627f;
    }

    const bool full = range == MAI_VIDEO_RANGE_FULL;
    const float blackY = full ? 0.0f : 16.0f;
    const float blackChroma = 128.0f;
    const float yScale = 255.0f / (full ? 255.0f : 235.0f - 16.0f);
    const float chromaScale = 255.0f / ((full ? 255.0f : 240.0f - 16.0f) / 2.0f);
    const float kg = 1.0f - kb - kr;

    MaiVideoColorParameters result = {};
    result.vectors[0][0] = yScale;
    result.vectors[0][2] = chromaScale * (1.0f - kr);
    result.vectors[1][0] = yScale;
    result.vectors[1][1] = chromaScale * (kb - 1.0f) * kb / kg;
    result.vectors[1][2] = chromaScale * (kr - 1.0f) * kr / kg;
    result.vectors[2][0] = yScale;
    result.vectors[2][1] = chromaScale * (1.0f - kb);
    for (int row = 0; row < 3; ++row) {
        result.vectors[row][3] =
            -(result.vectors[row][0] * blackY +
              (result.vectors[row][1] + result.vectors[row][2]) * blackChroma) /
            255.0f;
        result.rangeMin[row] = full ? 0.0f : 16.0f / 255.0f;
        result.rangeMax[row] = full ? 1.0f : (row == 0 ? 235.0f : 240.0f) / 255.0f;
    }
    return result;
}
