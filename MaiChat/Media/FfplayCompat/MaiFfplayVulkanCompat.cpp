// 文件作用：为未启用 Vulkan 的 ffplay 构建提供兼容入口。
// 调用边界：由 MaiChat 媒体或 Agent 宿主调用；平台资源在停止或失败时须正确释放。
extern "C" {
#include "ffplay_renderer.h"
#include <libavutil/error.h>
}

// ffplay.c keeps its Vulkan-specific option surface unchanged. MaiChat's
// video output uses the Graphics backend selected by each platform instead.
extern "C" VkRenderer* vk_get_renderer(void) { return nullptr; }
extern "C" int vk_renderer_create(VkRenderer*, SDL_Window*, AVDictionary*) {
    return AVERROR(ENOSYS);
}
extern "C" int vk_renderer_get_hw_dev(VkRenderer*, AVBufferRef**) {
    return AVERROR(ENOSYS);
}
extern "C" int vk_renderer_display(VkRenderer*, AVFrame*, RenderParams*) {
    return AVERROR(ENOSYS);
}
extern "C" int vk_renderer_resize(VkRenderer*, int, int) { return AVERROR(ENOSYS); }
extern "C" void vk_renderer_destroy(VkRenderer*) {}
