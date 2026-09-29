#include "MaiGraphics.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <thread>
#include <vector>

struct ags_graphics {
    std::thread::id owner;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> target;
    size_t textureCount = 0;
    bool frameActive = false;
};

struct ags_texture {
    ags_graphics_t* owner = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;
};

namespace {

constexpr uint32_t kMaximumEdge = 16384;
constexpr uint64_t kMaximumPixels = 64000000;

bool validSize(uint32_t width, uint32_t height) {
    return width > 0 && height > 0 && width <= kMaximumEdge && height <= kMaximumEdge &&
           static_cast<uint64_t>(width) * height <= kMaximumPixels;
}

size_t pixelBytes(uint32_t width, uint32_t height) {
    return static_cast<size_t>(width) * height * 4;
}

bool ownsCurrentThread(const ags_graphics_t* graphics) {
    return graphics->owner == std::this_thread::get_id();
}

uint8_t channel(double value) {
    return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 255.0)));
}

}  // namespace

extern "C" ags_result ags_create(ags_graphics_t** graphics, ags_backend backend, uint32_t width,
                                 uint32_t height) {
    if (!graphics) return AGS_ERROR_INVALID_ARGUMENT;
    *graphics = nullptr;
    if (backend != AGS_BACKEND_SOFTWARE) return AGS_ERROR_NOT_SUPPORTED;
    if (!validSize(width, height)) return AGS_ERROR_INVALID_ARGUMENT;
    try {
        auto* created = new ags_graphics_t;
        try {
            created->target.resize(pixelBytes(width, height));
        } catch (...) {
            delete created;
            throw;
        }
        created->owner = std::this_thread::get_id();
        created->width = width;
        created->height = height;
        *graphics = created;
        return AGS_SUCCESS;
    } catch (const std::bad_alloc&) {
        return AGS_ERROR_OUT_OF_MEMORY;
    }
}

extern "C" ags_result ags_destroy(ags_graphics_t* graphics) {
    if (!graphics) return AGS_ERROR_INVALID_ARGUMENT;
    if (!ownsCurrentThread(graphics)) return AGS_ERROR_WRONG_THREAD;
    if (graphics->textureCount || graphics->frameActive) return AGS_ERROR_INVALID_STATE;
    delete graphics;
    return AGS_SUCCESS;
}

extern "C" ags_result ags_resize(ags_graphics_t* graphics, uint32_t width, uint32_t height) {
    if (!graphics || !validSize(width, height)) return AGS_ERROR_INVALID_ARGUMENT;
    if (!ownsCurrentThread(graphics)) return AGS_ERROR_WRONG_THREAD;
    if (graphics->frameActive) return AGS_ERROR_INVALID_STATE;
    try {
        std::vector<uint8_t> resized(pixelBytes(width, height));
        graphics->target.swap(resized);
        graphics->width = width;
        graphics->height = height;
        return AGS_SUCCESS;
    } catch (const std::bad_alloc&) {
        return AGS_ERROR_OUT_OF_MEMORY;
    }
}

extern "C" ags_result ags_texture_create(ags_graphics_t* graphics, uint32_t width, uint32_t height,
                                         ags_color_format format, ags_texture_t** texture) {
    if (!texture) return AGS_ERROR_INVALID_ARGUMENT;
    *texture = nullptr;
    if (!graphics || !validSize(width, height)) return AGS_ERROR_INVALID_ARGUMENT;
    if (!ownsCurrentThread(graphics)) return AGS_ERROR_WRONG_THREAD;
    if (format != AGS_COLOR_RGBA8) return AGS_ERROR_NOT_SUPPORTED;
    try {
        auto* created = new ags_texture_t;
        try {
            created->pixels.resize(pixelBytes(width, height));
        } catch (...) {
            delete created;
            throw;
        }
        created->owner = graphics;
        created->width = width;
        created->height = height;
        ++graphics->textureCount;
        *texture = created;
        return AGS_SUCCESS;
    } catch (const std::bad_alloc&) {
        return AGS_ERROR_OUT_OF_MEMORY;
    }
}

extern "C" ags_result ags_texture_destroy(ags_texture_t* texture) {
    if (!texture) return AGS_ERROR_INVALID_ARGUMENT;
    if (!ownsCurrentThread(texture->owner)) return AGS_ERROR_WRONG_THREAD;
    --texture->owner->textureCount;
    delete texture;
    return AGS_SUCCESS;
}

extern "C" ags_result ags_texture_set_image(ags_texture_t* texture, const uint8_t* pixels,
                                            size_t stride) {
    if (!texture || !pixels || stride < static_cast<size_t>(texture->width) * 4 ||
        stride > std::numeric_limits<size_t>::max() / texture->height)
        return AGS_ERROR_INVALID_ARGUMENT;
    if (!ownsCurrentThread(texture->owner)) return AGS_ERROR_WRONG_THREAD;
    for (uint32_t row = 0; row < texture->height; ++row) {
        std::memcpy(texture->pixels.data() + static_cast<size_t>(row) * texture->width * 4,
                    pixels + static_cast<size_t>(row) * stride,
                    static_cast<size_t>(texture->width) * 4);
    }
    return AGS_SUCCESS;
}

extern "C" ags_result ags_begin_frame(ags_graphics_t* graphics) {
    if (!graphics) return AGS_ERROR_INVALID_ARGUMENT;
    if (!ownsCurrentThread(graphics)) return AGS_ERROR_WRONG_THREAD;
    if (graphics->frameActive) return AGS_ERROR_INVALID_STATE;
    graphics->frameActive = true;
    return AGS_SUCCESS;
}

extern "C" ags_result ags_clear(ags_graphics_t* graphics, ags_color color) {
    if (!graphics) return AGS_ERROR_INVALID_ARGUMENT;
    if (!ownsCurrentThread(graphics)) return AGS_ERROR_WRONG_THREAD;
    if (!graphics->frameActive) return AGS_ERROR_INVALID_STATE;
    for (size_t index = 0; index < graphics->target.size(); index += 4) {
        graphics->target[index] = color.red;
        graphics->target[index + 1] = color.green;
        graphics->target[index + 2] = color.blue;
        graphics->target[index + 3] = color.alpha;
    }
    return AGS_SUCCESS;
}

extern "C" ags_result ags_draw_sprite(ags_graphics_t* graphics, const ags_texture_t* texture,
                                      ags_rect destination, float opacity) {
    if (!graphics || !texture || !destination.width || !destination.height ||
        !std::isfinite(opacity) || opacity < 0 || opacity > 1)
        return AGS_ERROR_INVALID_ARGUMENT;
    if (!ownsCurrentThread(graphics)) return AGS_ERROR_WRONG_THREAD;
    if (!graphics->frameActive || texture->owner != graphics) return AGS_ERROR_INVALID_STATE;
    const int64_t left = std::max<int64_t>(0, destination.x);
    const int64_t top = std::max<int64_t>(0, destination.y);
    const int64_t right =
        std::min<int64_t>(graphics->width, static_cast<int64_t>(destination.x) + destination.width);
    const int64_t bottom = std::min<int64_t>(
        graphics->height, static_cast<int64_t>(destination.y) + destination.height);
    for (int64_t y = top; y < bottom; ++y) {
        const uint32_t sourceY = static_cast<uint32_t>(static_cast<uint64_t>(y - destination.y) *
                                                       texture->height / destination.height);
        for (int64_t x = left; x < right; ++x) {
            const uint32_t sourceX = static_cast<uint32_t>(
                static_cast<uint64_t>(x - destination.x) * texture->width / destination.width);
            const uint8_t* source = texture->pixels.data() +
                                    (static_cast<size_t>(sourceY) * texture->width + sourceX) * 4;
            uint8_t* target =
                graphics->target.data() + (static_cast<size_t>(y) * graphics->width + x) * 4;
            const double sourceAlpha = (source[3] / 255.0) * opacity;
            const double targetAlpha = target[3] / 255.0;
            const double outputAlpha = sourceAlpha + targetAlpha * (1.0 - sourceAlpha);
            for (int channelIndex = 0; channelIndex < 3; ++channelIndex) {
                target[channelIndex] =
                    outputAlpha == 0
                        ? 0
                        : channel((source[channelIndex] * sourceAlpha +
                                   target[channelIndex] * targetAlpha * (1.0 - sourceAlpha)) /
                                  outputAlpha);
            }
            target[3] = channel(outputAlpha * 255.0);
        }
    }
    return AGS_SUCCESS;
}

extern "C" ags_result ags_end_frame(ags_graphics_t* graphics) {
    if (!graphics) return AGS_ERROR_INVALID_ARGUMENT;
    if (!ownsCurrentThread(graphics)) return AGS_ERROR_WRONG_THREAD;
    if (!graphics->frameActive) return AGS_ERROR_INVALID_STATE;
    graphics->frameActive = false;
    return AGS_SUCCESS;
}

extern "C" ags_result ags_readback(ags_graphics_t* graphics, uint8_t* pixels, size_t stride,
                                   size_t out_bytes) {
    if (!graphics || !pixels || stride < static_cast<size_t>(graphics->width) * 4 ||
        stride > std::numeric_limits<size_t>::max() / graphics->height ||
        out_bytes < stride * graphics->height)
        return AGS_ERROR_INVALID_ARGUMENT;
    if (!ownsCurrentThread(graphics)) return AGS_ERROR_WRONG_THREAD;
    if (graphics->frameActive) return AGS_ERROR_INVALID_STATE;
    for (uint32_t row = 0; row < graphics->height; ++row) {
        std::memcpy(pixels + static_cast<size_t>(row) * stride,
                    graphics->target.data() + static_cast<size_t>(row) * graphics->width * 4,
                    static_cast<size_t>(graphics->width) * 4);
    }
    return AGS_SUCCESS;
}
