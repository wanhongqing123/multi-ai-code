// 文件作用：桌面端 Agent 工具；MaiImageVision 负责本地图片内容分析与视觉模型调用，本文件给出其声明。
// 对外边界：本文件给出 MaiImageVision 的类型和函数声明；参数、返回值和对象生命周期以此为准。
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 桌面 OpenCV 视觉模型仅在创建期间借用模型字节，句柄随后持有已加载网络。
// 传入打包的 YuNet 2023mar 与 PPHumanSeg 2023mar ONNX 模型；
// 由宿主读取模型字节可保留 Windows 上的 Unicode 路径语义。
// 句柄允许多线程调用，内部串行推理；全部调用结束后才能销毁。
// 失败时 *error 是调用方持有的 UTF-8 字符串，必须用 maiImageVisionFree 释放。
void *maiImageVisionCreate(const unsigned char *faceModel, size_t faceModelSize,
                           const unsigned char *personModel,
                           size_t personModelSize, char **error);
void maiImageVisionDestroy(void *handle);

typedef struct MaiVisionFace {
  float x;
  float y;
  float width;
  float height;
  float landmarks[10]; // 右眼、左眼、鼻尖、右嘴角、左嘴角，依次为 x/y 坐标对。
  float confidence;
} MaiVisionFace;

// RGBA8 输入每行至少 width * 4 字节，坐标单位是原图像素。
// 空人脸列表也是成功结果；成功时 error 为空。
// 人脸数组由调用方持有，须用 maiImageVisionFree 释放。
typedef struct MaiVisionFacesResult {
  MaiVisionFace *faces;
  int count;
  char *error;
} MaiVisionFacesResult;
MaiVisionFacesResult maiImageVisionDetectFacesRgba(void *handle,
                                                   const unsigned char *pixels,
                                                   int width, int height,
                                                   int stride);

// 每个输入像素输出一个灰度字节：0 表示背景，255 表示人物。
// 成功时 mask 由调用方持有且 error 为空；须用 maiImageVisionFree 释放 mask。
// 不修改源像素或原图片文件。
typedef struct MaiVisionMaskResult {
  unsigned char *mask;
  int width;
  int height;
  char *error;
} MaiVisionMaskResult;
MaiVisionMaskResult maiImageVisionSegmentPersonRgba(void *handle,
                                                    const unsigned char *pixels,
                                                    int width, int height,
                                                    int stride);

void maiImageVisionFree(void *pointer);

#ifdef __cplusplus
}
#endif
