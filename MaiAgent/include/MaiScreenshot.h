#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "MaiError.h"

// 平台原生屏幕捕获的结果。像素在平台实现内编码成完整 PNG 文件，调用方不接触 D3D、DXGI
// 或任何界面框架。成功时 pngBytes 非空、width/height 是编码后图片的像素尺寸；失败时三者清空。
struct MaiScreenshot {
    // 完整 PNG 文件字节，不是裸像素。
    std::vector<std::uint8_t> pngBytes;
    // PNG 的最终像素尺寸；失败时都是 0。
    int width = 0;
    int height = 0;
};

enum class MaiScreenshotTarget {
    DisplayUnderPointer,  // 鼠标指针所在的显示器。
    WindowByTitle,        // 一个位于当前交互桌面的可见顶层窗口。
};

struct MaiScreenshotRequest {
    MaiScreenshotTarget target = MaiScreenshotTarget::DisplayUnderPointer;
    // WindowByTitle 时必填，UTF-8。平台实现先找标题完全相同的可见顶层窗口；没有完全匹配时，
    // 仅在子串匹配唯一时使用它，避免模型含糊指定后截错窗口。
    std::string windowTitle;
};

// 当前平台是否提供原生屏幕捕获。调用方只有在返回 true 时才应把 screenshot 工具暴露给模型；
// 暴露一个永远失败的工具会诱使模型反复重试。
bool maiIsScreenshotSupported();

// 按 request 截取鼠标所在显示器或指定窗口，并编码为 PNG。
//
// 这是阻塞调用，只能在 agent 的工作线程调用。Windows 实现使用 Windows Graphics Capture
// 从 D3D11 纹理读取显示器或窗口画面；不依赖 Qt、窗口系统控件或外部进程。失败时 screenshot
// 会保持为空，并返回可供模型纠正的错误。
MaiError maiCaptureScreenshot(const MaiScreenshotRequest& request, MaiScreenshot& screenshot);
