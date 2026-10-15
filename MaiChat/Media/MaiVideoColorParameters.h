// 文件作用：声明描述视频色域与转换矩阵参数，供解码和渲染链路使用所需的接口和数据结构。
// 调用边界：平台实现与上层调用方通过本文件约定参数、所有权和释放方式。
#pragma once

#include "MaiGraphicsPresenter.h"

struct MaiVideoColorParameters {
    float vectors[3][4];
    float rangeMin[3];
    float rangeMax[3];
};

MaiVideoColorParameters maiVideoColorParameters(MaiVideoColorSpace space, MaiVideoColorRange range);
