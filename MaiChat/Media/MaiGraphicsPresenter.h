// 文件作用：声明接收解码画面并把图像交给平台图形层显示所需的接口和数据结构。
// 调用边界：平台实现与上层调用方通过本文件约定参数、所有权和释放方式。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 整个进程共用一个图形线程和一个图片解码线程；必须先启动呈现器，再挂载视图。
// 桌面端和 Android 的 backend 是模块路径，iOS 使用 "builtin:metal"。
// effect_directory 包含打包的 default.effect 及其相对引用文件。
// 重新配置前必须先卸载全部视图，再调用 maiGraphicsPresenterStop。
// 回调在图形线程执行；宿主更新界面时必须自行切回 UI 线程。绘制失败保留上一帧。
typedef void (*MaiGraphicsPresentCallback)(uint64_t view_id, bool success, void* user_data);
typedef void (*MaiGraphicsNativeViewCallback)(void* native_view);
bool maiGraphicsPresenterStart(const char* backend, const char* effect_directory,
                               MaiGraphicsPresentCallback callback, void* user_data);

// native_view 在 Apple 平台是 UI 线程安装的 CAMetalLayer，Android 是 ANativeWindow，
// Windows 是 HWND。入队前调用 retain_view，最后一个图形任务结束后调用 release_view。
// 平台句柄需要引用计数时必须同时传两个回调；宿主保证句柄活到 detach 完成时可同时传 NULL。
// 挂载和绘制均为异步操作；非零 ID 只代表请求已受理，最终结果以回调为准。
uint64_t maiGraphicsPresenterAttach(void* native_view, uint32_t width, uint32_t height,
                                    MaiGraphicsNativeViewCallback retain_view,
                                    MaiGraphicsNativeViewCallback release_view);
bool maiGraphicsPresenterShowImage(uint64_t view_id, const char* file_path, bool fill_view);
typedef enum MaiVideoPixelFormat {
    MAI_VIDEO_PIXEL_RGBA,
    MAI_VIDEO_PIXEL_BGRA,
    MAI_VIDEO_PIXEL_NV12,
    MAI_VIDEO_PIXEL_I420,
    MAI_VIDEO_PIXEL_P010,
    MAI_VIDEO_PIXEL_I010
} MaiVideoPixelFormat;

typedef enum MaiVideoColorSpace {
    MAI_VIDEO_COLOR_BT601,
    MAI_VIDEO_COLOR_BT709,
    MAI_VIDEO_COLOR_BT2020
} MaiVideoColorSpace;

typedef enum MaiVideoColorRange {
    MAI_VIDEO_RANGE_LIMITED,
    MAI_VIDEO_RANGE_FULL
} MaiVideoColorRange;

typedef enum MaiVideoTransfer {
    MAI_VIDEO_TRANSFER_SDR,
    MAI_VIDEO_TRANSFER_HLG,
    MAI_VIDEO_TRANSFER_PQ
} MaiVideoTransfer;

// 输入平面只借用到调用返回；呈现器先复制，再跨线程交给图形任务。
// RGBA/BGRA 使用第 0 平面，NV12/P010 使用 0–1，I420/I010 使用 0–2。
// P010 的 10 位样本位于 16 位字的高位，I010 位于低位；允许负行跨度。
// 色彩元数据只对 YUV 帧生效。
typedef struct MaiVideoFrame {
    const uint8_t* data[3];
    int32_t linesize[3];
    uint32_t width;
    uint32_t height;
    MaiVideoPixelFormat format;
    MaiVideoColorSpace color_space;
    MaiVideoColorRange color_range;
    MaiVideoTransfer color_transfer;
} MaiVideoFrame;

// 字幕画布是与解码视频同坐标系的非预乘 RGBA；返回前会复制画布。
// 没有可见字幕时传 NULL；宽高必须与视频帧一致。
typedef struct MaiVideoSubtitle {
    const uint8_t* rgba;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
} MaiVideoSubtitle;

bool maiGraphicsPresenterShowVideoFrame(uint64_t view_id, const MaiVideoFrame* frame,
                                        bool fill_view);
bool maiGraphicsPresenterShowVideoFrameWithSubtitle(uint64_t view_id,
                                                    const MaiVideoFrame* frame,
                                                    const MaiVideoSubtitle* subtitle,
                                                    bool fill_view);
// 把一帧解码后的 RGBA8 复制进图形队列；调用返回后可复用输入像素。
// stride 至少为 width * 4。帧调度与音画同步由媒体会话负责；新帧会覆盖尚未绘制的旧帧。
bool maiGraphicsPresenterShowFrame(uint64_t view_id, const uint8_t* pixels, uint32_t width,
                                   uint32_t height, uint32_t stride, bool fill_view);
void maiGraphicsPresenterResize(uint64_t view_id, uint32_t width, uint32_t height);
void maiGraphicsPresenterDetach(uint64_t view_id);

// 必须在应用 UI 事件循环仍可处理视图任务时停止呈现器。
// 停止会等待图形和解码线程退出，并使所有视图 ID 失效。
void maiGraphicsPresenterStop(void);

#ifdef __cplusplus
}
#endif
