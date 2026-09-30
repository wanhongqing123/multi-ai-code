#include "MaiGraphicsImageRenderer.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include "MaiGraphics.h"

namespace {

constexpr uint64_t kMaximumPixels = 64000000;

class MaiGraphicsThreadContext {
public:
    ~MaiGraphicsThreadContext() {
        reset();
    }

    bool prepare(const char* module) {
        if (mGraphics && mModule && std::strcmp(mModule, module) == 0) return true;
        reset();
        const size_t length = std::strlen(module) + 1;
        auto* copy = static_cast<char*>(std::malloc(length));
        if (!copy) return false;
        std::memcpy(copy, module, length);
        graphics_t* created = nullptr;
        if (gs_create(&created, module, 0) != GS_SUCCESS || !created) {
            std::free(copy);
            return false;
        }
        mModule = copy;
        mGraphics = created;
        return true;
    }

    graphics_t* graphics() const {
        return mGraphics;
    }

    void reset() {
        if (mGraphics) gs_destroy(mGraphics);
        std::free(mModule);
        mGraphics = nullptr;
        mModule = nullptr;
    }

private:
    graphics_t* mGraphics = nullptr;
    char* mModule = nullptr;
};

thread_local MaiGraphicsThreadContext tGraphicsContext;

AVPixelFormat pixelFormat(gs_color_format format) {
    switch (format) {
        case GS_RGBA: return AV_PIX_FMT_RGBA;
        case GS_BGRA: return AV_PIX_FMT_BGRA;
        case GS_BGRX: return AV_PIX_FMT_BGR0;
        default: return AV_PIX_FMT_NONE;
    }
}

bool convertToRgba(const uint8_t* source, uint32_t sourceWidth, uint32_t sourceHeight,
                   AVPixelFormat sourceFormat, uint8_t* destination, uint32_t width,
                   uint32_t height) {
    SwsContext* scale =
        sws_getContext(static_cast<int>(sourceWidth), static_cast<int>(sourceHeight), sourceFormat,
                       static_cast<int>(width), static_cast<int>(height), AV_PIX_FMT_RGBA,
                       SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!scale) return false;
    const uint8_t* sourcePlanes[] = {source, nullptr, nullptr, nullptr};
    const int sourceStrides[] = {static_cast<int>(sourceWidth * 4), 0, 0, 0};
    uint8_t* destinationPlanes[] = {destination, nullptr, nullptr, nullptr};
    const int destinationStrides[] = {static_cast<int>(width * 4), 0, 0, 0};
    const int scaled =
        sws_scale(scale, sourcePlanes, sourceStrides, 0, static_cast<int>(sourceHeight),
                  destinationPlanes, destinationStrides);
    sws_freeContext(scale);
    return scaled == static_cast<int>(height);
}

bool renderRgba(const uint8_t* rgba, uint32_t width, uint32_t height, const char* backend,
                MaiGraphicsImageResult* result) {
    if (!tGraphicsContext.prepare(backend)) return false;
    graphics_t* graphics = tGraphicsContext.graphics();
    gs_enter_context(graphics);

    gs_texture_t* source = nullptr;
    gs_texture_t* target = nullptr;
    gs_stagesurf_t* stage = nullptr;
    bool mapped = false;
    bool rendered = false;
    uint8_t* output = nullptr;
    do {
        const uint8_t* sourcePlanes[] = {rgba};
        source = gs_texture_create(width, height, GS_RGBA, 1, sourcePlanes, 0);
        target = gs_texture_create(width, height, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
        stage = gs_stagesurface_create(width, height, GS_RGBA);
        if (!source || !target || !stage) break;

        gs_copy_texture(target, source);
        gs_flush();
        gs_stage_texture(stage, target);
        gs_flush();

        uint8_t* mappedPixels = nullptr;
        uint32_t mappedStride = 0;
        mapped = gs_stagesurface_map(stage, &mappedPixels, &mappedStride);
        if (!mapped || !mappedPixels || mappedStride < width * 4) break;

        const size_t stride = static_cast<size_t>(width) * 4;
        output = static_cast<uint8_t*>(std::malloc(stride * height));
        if (!output) break;
        for (uint32_t row = 0; row < height; ++row)
            std::memcpy(output + row * stride, mappedPixels + row * mappedStride, stride);

        result->pixels = output;
        result->width = width;
        result->height = height;
        result->stride = static_cast<uint32_t>(stride);
        rendered = true;
    } while (false);

    if (!rendered) std::free(output);
    if (mapped) gs_stagesurface_unmap(stage);
    if (stage) gs_stagesurface_destroy(stage);
    if (target) gs_texture_destroy(target);
    if (source) gs_texture_destroy(source);
    gs_leave_context();
    if (!rendered) tGraphicsContext.reset();
    return rendered;
}

}  // namespace

extern "C" bool maiGraphicsRenderImageFile(const char* file_path, const char* backend,
                                           uint32_t max_width, uint32_t max_height,
                                           MaiGraphicsImageResult* result) {
    if (!result) return false;
    *result = {};
    if (!file_path || !*file_path || !backend || !*backend || !max_width || !max_height)
        return false;

    gs_color_format format = GS_UNKNOWN;
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    uint8_t* decoded = gs_create_texture_file_data(file_path, &format, &sourceWidth, &sourceHeight);
    if (!decoded) return false;

    bool rendered = false;
    do {
        if (!sourceWidth || !sourceHeight ||
            static_cast<uint64_t>(sourceWidth) * sourceHeight > kMaximumPixels)
            break;
        const AVPixelFormat sourceFormat = pixelFormat(format);
        if (sourceFormat == AV_PIX_FMT_NONE) break;

        uint32_t width = sourceWidth;
        uint32_t height = sourceHeight;
        if (width > max_width || height > max_height) {
            if (static_cast<uint64_t>(width) * max_height >
                static_cast<uint64_t>(height) * max_width) {
                height = std::max<uint32_t>(1, static_cast<uint64_t>(height) * max_width / width);
                width = max_width;
            } else {
                width = std::max<uint32_t>(1, static_cast<uint64_t>(width) * max_height / height);
                height = max_height;
            }
        }
        if (static_cast<uint64_t>(width) * height > kMaximumPixels) break;
        const size_t bytes = static_cast<size_t>(width) * height * 4;
        auto* rgba = static_cast<uint8_t*>(std::malloc(bytes));
        if (!rgba) break;
        if (convertToRgba(decoded, sourceWidth, sourceHeight, sourceFormat, rgba, width, height))
            rendered = renderRgba(rgba, width, height, backend, result);
        std::free(rgba);
    } while (false);

    bfree(decoded);
    return rendered;
}

extern "C" void maiGraphicsImageResultFree(MaiGraphicsImageResult* result) {
    if (!result) return;
    std::free(result->pixels);
    *result = {};
}
