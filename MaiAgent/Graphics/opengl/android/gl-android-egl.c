#include "../gl-subsystem.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android/native_window.h>

struct gl_windowinfo {
    ANativeWindow* window;
    EGLSurface surface;
};

struct gl_platform {
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    EGLSurface pbuffer;
};

struct gl_platform* gl_platform_create(gs_device_t* device, uint32_t adapter) {
    UNUSED_PARAMETER(device);
    UNUSED_PARAMETER(adapter);

    struct gl_platform* platform = bzalloc(sizeof(*platform));
    platform->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (platform->display == EGL_NO_DISPLAY || !eglInitialize(platform->display, NULL, NULL))
        goto fail;
    if (!eglBindAPI(EGL_OPENGL_ES_API)) goto fail;

    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE,
        EGL_PBUFFER_BIT | EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE,
        EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE,
        8,
        EGL_GREEN_SIZE,
        8,
        EGL_BLUE_SIZE,
        8,
        EGL_ALPHA_SIZE,
        8,
        EGL_NONE,
    };
    EGLint count = 0;
    if (!eglChooseConfig(platform->display, config_attributes, &platform->config, 1, &count) ||
        !count)
        goto fail;

    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    platform->context =
        eglCreateContext(platform->display, platform->config, EGL_NO_CONTEXT, context_attributes);
    if (platform->context == EGL_NO_CONTEXT) goto fail;

    const EGLint surface_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    platform->pbuffer =
        eglCreatePbufferSurface(platform->display, platform->config, surface_attributes);
    if (platform->pbuffer == EGL_NO_SURFACE ||
        !eglMakeCurrent(platform->display, platform->pbuffer, platform->pbuffer, platform->context))
        goto fail;
    return platform;

fail:
    blog(LOG_ERROR, "Android EGL context creation failed: 0x%x", eglGetError());
    gl_platform_destroy(platform);
    return NULL;
}

void gl_platform_destroy(struct gl_platform* platform) {
    if (!platform) return;
    if (platform->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(platform->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (platform->pbuffer != EGL_NO_SURFACE)
            eglDestroySurface(platform->display, platform->pbuffer);
        if (platform->context != EGL_NO_CONTEXT)
            eglDestroyContext(platform->display, platform->context);
        eglTerminate(platform->display);
    }
    bfree(platform);
}

struct gl_windowinfo* gl_windowinfo_create(const struct gs_init_data* info) {
    if (!info || !info->window.native_window) return NULL;
    struct gl_windowinfo* window_info = bzalloc(sizeof(*window_info));
    window_info->window = info->window.native_window;
    ANativeWindow_acquire(window_info->window);
    return window_info;
}

void gl_windowinfo_destroy(struct gl_windowinfo* window_info) {
    if (!window_info) return;
    ANativeWindow_release(window_info->window);
    bfree(window_info);
}

bool gl_platform_init_swapchain(struct gs_swap_chain* swap) {
    struct gl_platform* platform = swap->device->plat;
    swap->wi->surface =
        eglCreateWindowSurface(platform->display, platform->config, swap->wi->window, NULL);
    return swap->wi->surface != EGL_NO_SURFACE;
}

void gl_platform_cleanup_swapchain(struct gs_swap_chain* swap) {
    struct gl_platform* platform = swap->device->plat;
    if (swap->wi->surface != EGL_NO_SURFACE) {
        eglDestroySurface(platform->display, swap->wi->surface);
        swap->wi->surface = EGL_NO_SURFACE;
    }
}

void device_enter_context(gs_device_t* device) {
    struct gl_platform* platform = device->plat;
    EGLSurface surface = device->cur_swap ? device->cur_swap->wi->surface : platform->pbuffer;
    eglMakeCurrent(platform->display, surface, surface, platform->context);
}

void device_leave_context(gs_device_t* device) {
    glFlush();
    eglMakeCurrent(device->plat->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    device->cur_vertex_buffer = NULL;
    device->cur_index_buffer = NULL;
    device->cur_render_target = NULL;
    device->cur_zstencil_buffer = NULL;
    device->cur_swap = NULL;
    device->cur_fbo = NULL;
}

void gl_clear_context(gs_device_t* device) {
    eglMakeCurrent(device->plat->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void gl_update(gs_device_t* device) {
    UNUSED_PARAMETER(device);
}

void* device_get_device_obj(gs_device_t* device) {
    return device->plat->context;
}

void device_load_swapchain(gs_device_t* device, gs_swapchain_t* swap) {
    device->cur_swap = swap;
    struct gl_platform* platform = device->plat;
    EGLSurface surface = swap ? swap->wi->surface : platform->pbuffer;
    eglMakeCurrent(platform->display, surface, surface, platform->context);
    device_set_render_target(device, NULL, NULL);
}

bool device_is_present_ready(gs_device_t* device) {
    return device->cur_swap && device->cur_swap->wi->surface != EGL_NO_SURFACE;
}

void device_present(gs_device_t* device) {
    if (!device_is_present_ready(device)) return;
    glFlush();
    eglSwapBuffers(device->plat->display, device->cur_swap->wi->surface);
}

bool device_enum_adapters(gs_device_t* device,
                          bool (*callback)(void* param, const char* name, uint32_t id),
                          void* param) {
    UNUSED_PARAMETER(device);
    return callback ? callback(param, "Android GLES", 0) : false;
}

bool device_is_monitor_hdr(gs_device_t* device, void* monitor) {
    UNUSED_PARAMETER(device);
    UNUSED_PARAMETER(monitor);
    return false;
}

void gl_getclientsize(const struct gs_swap_chain* swap, uint32_t* width, uint32_t* height) {
    EGLint result_width = 0;
    EGLint result_height = 0;
    eglQuerySurface(swap->device->plat->display, swap->wi->surface, EGL_WIDTH, &result_width);
    eglQuerySurface(swap->device->plat->display, swap->wi->surface, EGL_HEIGHT, &result_height);
    *width = result_width > 0 ? (uint32_t)result_width : 0;
    *height = result_height > 0 ? (uint32_t)result_height : 0;
}
