extern "C" {
#include "ffplay_renderer.h"
#include <libavutil/error.h>
}

// ffplay's renderer selection is handled by MaiChat's Graphics view. The
// original Vulkan renderer is not instantiated inside an application view.
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
