#include "SDL.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

#include "MaiFfplayCompatInternal.h"
#include "MaiFfplayHost.h"

struct SDL_Window {
    int width = 0;
    int height = 0;
    MaiFfplayHost host{};
};

struct SDL_Texture {
    Uint32 format = SDL_PIXELFORMAT_UNKNOWN;
    int width = 0;
    int height = 0;
    SDL_BlendMode blend = SDL_BLENDMODE_NONE;
    std::vector<Uint8> pixels;
    std::vector<Uint8> chromaU;
    std::vector<Uint8> chromaV;
};

struct SDL_Renderer {
    SDL_Window* window = nullptr;
    SDL_Texture* video = nullptr;
    SDL_Texture* overlay = nullptr;
    SDL_Texture* visualization = nullptr;
    SDL_Rect videoRect{};
    SDL_Rect overlayRect{};
    Uint8 drawColor[4]{};
    std::vector<Uint8> canvas;
};

namespace {

std::mutex sHostMutex;
MaiFfplayHost sHost{};
std::atomic<int> sYuvConversion{SDL_YUV_CONVERSION_AUTOMATIC};
std::atomic<int> sWindowWidth{0};

bool isPacked(Uint32 format) { return format != SDL_PIXELFORMAT_IYUV; }

void fillCanvas(SDL_Renderer* renderer, const SDL_Rect& area) {
    if (!renderer || !renderer->window) return;
    const int width = renderer->window->width;
    const int height = renderer->window->height;
    if (width < 1 || height < 1 || static_cast<uint64_t>(width) * height > 64000000) return;
    if (renderer->canvas.empty())
        renderer->canvas.resize(static_cast<size_t>(width) * height * 4);
    const int left = std::clamp(area.x, 0, width);
    const int top = std::clamp(area.y, 0, height);
    const int right = std::clamp(area.x + area.w, 0, width);
    const int bottom = std::clamp(area.y + area.h, 0, height);
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            Uint8* pixel = renderer->canvas.data() + (static_cast<size_t>(y) * width + x) * 4;
            std::memcpy(pixel, renderer->drawColor, 4);
        }
    }
}

MaiVideoPixelFormat videoFormat(Uint32 format) {
    switch (format) {
    case SDL_PIXELFORMAT_IYUV: return MAI_VIDEO_PIXEL_I420;
    case SDL_PIXELFORMAT_ARGB8888: return MAI_VIDEO_PIXEL_BGRA;
    default: return MAI_VIDEO_PIXEL_RGBA;
    }
}

std::vector<Uint8> subtitleRgba(const SDL_Texture* texture) {
    std::vector<Uint8> rgba;
    if (!texture || texture->pixels.empty()) return rgba;
    rgba.resize(texture->pixels.size());
    for (size_t index = 0; index + 3 < rgba.size(); index += 4) {
        if (texture->format == SDL_PIXELFORMAT_ARGB8888) {
            rgba[index] = texture->pixels[index + 2];
            rgba[index + 1] = texture->pixels[index + 1];
            rgba[index + 2] = texture->pixels[index];
        } else {
            rgba[index] = texture->pixels[index];
            rgba[index + 1] = texture->pixels[index + 1];
            rgba[index + 2] = texture->pixels[index + 2];
        }
        rgba[index + 3] = texture->pixels[index + 3];
    }
    return rgba;
}

}  // namespace

extern "C" void maiFfplayBindHost(const MaiFfplayHost* host) {
    std::lock_guard<std::mutex> lock(sHostMutex);
    sHost = host ? *host : MaiFfplayHost{};
}

extern "C" int maiFfplayWindowWidth(void) { return sWindowWidth; }

extern "C" SDL_Window* SDL_CreateWindow(const char* title, int, int, int width,
                                         int height, Uint32) {
    MaiFfplayHost host;
    {
        std::lock_guard<std::mutex> lock(sHostMutex);
        host = sHost;
    }
    if (!host.graphics_view_id) {
        maiFfplaySetError("MaiChat has not bound a Graphics video view");
        return nullptr;
    }
    SDL_Window* window = new (std::nothrow) SDL_Window;
    if (!window) return nullptr;
    window->width = width;
    window->height = height;
    sWindowWidth = width;
    window->host = host;
    if (host.set_title) host.set_title(host.user_data, title);
    return window;
}

extern "C" void SDL_DestroyWindow(SDL_Window* window) { delete window; }

extern "C" void SDL_SetWindowTitle(SDL_Window* window, const char* title) {
    if (window && window->host.set_title) window->host.set_title(window->host.user_data, title);
}

extern "C" void SDL_SetWindowSize(SDL_Window* window, int width, int height) {
    if (!window || width < 1 || height < 1) return;
    window->width = width;
    window->height = height;
    sWindowWidth = width;
    if (window->host.set_size)
        window->host.set_size(window->host.user_data, width, height);
}

extern "C" void SDL_SetWindowPosition(SDL_Window* window, int x, int y) {
    if (window && window->host.set_position)
        window->host.set_position(window->host.user_data, x, y);
}

extern "C" int SDL_SetWindowFullscreen(SDL_Window* window, Uint32 flags) {
    if (!window) return -1;
    if (window->host.set_fullscreen)
        window->host.set_fullscreen(window->host.user_data, flags != 0);
    return 0;
}

extern "C" void SDL_ShowWindow(SDL_Window* window) {
    if (window && window->host.show_window)
        window->host.show_window(window->host.user_data);
}

extern "C" int SDL_ShowCursor(int) { return 0; }

extern "C" SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, int, Uint32) {
    if (!window) return nullptr;
    auto* renderer = new (std::nothrow) SDL_Renderer;
    if (renderer) renderer->window = window;
    return renderer;
}

extern "C" void SDL_DestroyRenderer(SDL_Renderer* renderer) { delete renderer; }

extern "C" int SDL_GetRendererInfo(SDL_Renderer* renderer, SDL_RendererInfo* info) {
    if (!renderer || !info) return -1;
    *info = {};
    info->name = "MaiGraphics";
    info->num_texture_formats = 3;
    info->texture_formats[0] = SDL_PIXELFORMAT_IYUV;
    info->texture_formats[1] = SDL_PIXELFORMAT_ABGR8888;
    info->texture_formats[2] = SDL_PIXELFORMAT_ARGB8888;
    info->max_texture_width = 16384;
    info->max_texture_height = 16384;
    return 0;
}

extern "C" int SDL_SetRenderDrawColor(SDL_Renderer* renderer, Uint8 r, Uint8 g,
                                      Uint8 b, Uint8 a) {
    if (!renderer) return -1;
    renderer->drawColor[0] = r;
    renderer->drawColor[1] = g;
    renderer->drawColor[2] = b;
    renderer->drawColor[3] = a;
    return 0;
}

extern "C" int SDL_RenderClear(SDL_Renderer* renderer) {
    if (!renderer) return -1;
    renderer->video = nullptr;
    renderer->overlay = nullptr;
    renderer->visualization = nullptr;
    renderer->canvas.clear();
    return 0;
}

extern "C" int SDL_RenderFillRect(SDL_Renderer* renderer, const SDL_Rect* rectangle) {
    if (!renderer || !renderer->window) return -1;
    const SDL_Rect full{0, 0, renderer->window->width, renderer->window->height};
    fillCanvas(renderer, rectangle ? *rectangle : full);
    return 0;
}

extern "C" int SDL_RenderCopy(SDL_Renderer* renderer, SDL_Texture* texture,
                              const SDL_Rect*, const SDL_Rect* target) {
    if (!renderer || !texture) return -1;
    if (texture->blend == SDL_BLENDMODE_BLEND) {
        renderer->overlay = texture;
        renderer->overlayRect = target ? *target : SDL_Rect{0, 0, texture->width, texture->height};
    } else {
        renderer->visualization = texture;
    }
    return 0;
}

extern "C" int SDL_RenderCopyEx(SDL_Renderer* renderer, SDL_Texture* texture,
                                const SDL_Rect*, const SDL_Rect* target,
                                double, const void*, int) {
    if (!renderer || !texture) return -1;
    renderer->video = texture;
    renderer->videoRect = target ? *target : SDL_Rect{0, 0, texture->width, texture->height};
    return 0;
}

extern "C" void SDL_RenderPresent(SDL_Renderer* renderer) {
    if (!renderer || !renderer->window) return;
    if (renderer->video) {
        const SDL_Texture* texture = renderer->video;
        MaiVideoFrame frame{};
        frame.width = texture->width;
        frame.height = texture->height;
        frame.format = videoFormat(texture->format);
        frame.color_space = sYuvConversion == SDL_YUV_CONVERSION_BT601
                                ? MAI_VIDEO_COLOR_BT601 : MAI_VIDEO_COLOR_BT709;
        frame.color_range = sYuvConversion == SDL_YUV_CONVERSION_JPEG
                                ? MAI_VIDEO_RANGE_FULL : MAI_VIDEO_RANGE_LIMITED;
        frame.data[0] = texture->pixels.data();
        frame.linesize[0] = texture->width * (isPacked(texture->format) ? 4 : 1);
        if (!isPacked(texture->format)) {
            frame.data[1] = texture->chromaU.data();
            frame.data[2] = texture->chromaV.data();
            frame.linesize[1] = frame.linesize[2] = (texture->width + 1) / 2;
        }
        std::vector<Uint8> rgba = subtitleRgba(renderer->overlay);
        MaiVideoSubtitle subtitle{};
        if (renderer->overlay && renderer->overlay->width == texture->width &&
            renderer->overlay->height == texture->height) {
            subtitle.rgba = rgba.data();
            subtitle.width = texture->width;
            subtitle.height = texture->height;
            subtitle.stride = texture->width * 4;
        }
        if (!renderer->window->host.present_video ||
            !renderer->window->host.present_video(renderer->window->host.user_data,
                                                  &frame,
                                                  subtitle.rgba ? &subtitle : nullptr))
            maiFfplaySetError("Graphics rejected the ffplay video frame");
        return;
    }
    if (renderer->visualization && !renderer->visualization->pixels.empty()) {
        SDL_Texture* texture = renderer->visualization;
        if (renderer->window->host.present_rgba)
            renderer->window->host.present_rgba(renderer->window->host.user_data,
                                                texture->pixels.data(), texture->width,
                                                texture->height, texture->width * 4);
    } else if (!renderer->canvas.empty()) {
        if (renderer->window->host.present_rgba)
            renderer->window->host.present_rgba(renderer->window->host.user_data,
                                                renderer->canvas.data(), renderer->window->width,
                                                renderer->window->height,
                                                renderer->window->width * 4);
    }
}

extern "C" SDL_Texture* SDL_CreateTexture(SDL_Renderer*, Uint32 format, int,
                                           int width, int height) {
    if (width < 1 || height < 1 || static_cast<uint64_t>(width) * height > 64000000)
        return nullptr;
    auto* texture = new (std::nothrow) SDL_Texture;
    if (!texture) return nullptr;
    texture->format = format;
    texture->width = width;
    texture->height = height;
    texture->pixels.resize(static_cast<size_t>(width) * height *
                           (isPacked(format) ? 4 : 1));
    if (!isPacked(format)) {
        texture->chromaU.resize(static_cast<size_t>((width + 1) / 2) * ((height + 1) / 2));
        texture->chromaV.resize(texture->chromaU.size());
    }
    return texture;
}

extern "C" void SDL_DestroyTexture(SDL_Texture* texture) { delete texture; }

extern "C" int SDL_QueryTexture(SDL_Texture* texture, Uint32* format, int* access,
                                int* width, int* height) {
    if (!texture) return -1;
    if (format) *format = texture->format;
    if (access) *access = SDL_TEXTUREACCESS_STREAMING;
    if (width) *width = texture->width;
    if (height) *height = texture->height;
    return 0;
}

extern "C" int SDL_SetTextureBlendMode(SDL_Texture* texture, SDL_BlendMode blend) {
    if (!texture) return -1;
    texture->blend = blend;
    return 0;
}

extern "C" int SDL_GetTextureBlendMode(SDL_Texture* texture, SDL_BlendMode* blend) {
    if (!texture || !blend) return -1;
    *blend = texture->blend;
    return 0;
}

extern "C" int SDL_LockTexture(SDL_Texture* texture, const SDL_Rect* rectangle,
                               void** pixels, int* pitch) {
    if (!texture || !pixels || !pitch || !isPacked(texture->format)) return -1;
    const int x = rectangle ? rectangle->x : 0;
    const int y = rectangle ? rectangle->y : 0;
    if (x < 0 || y < 0 || x >= texture->width || y >= texture->height) return -1;
    *pitch = texture->width * 4;
    *pixels = texture->pixels.data() + (static_cast<size_t>(y) * texture->width + x) * 4;
    return 0;
}

extern "C" void SDL_UnlockTexture(SDL_Texture*) {}

extern "C" int SDL_UpdateTexture(SDL_Texture* texture, const SDL_Rect* rectangle,
                                 const void* pixels, int pitch) {
    if (!texture || !pixels || !isPacked(texture->format)) return -1;
    const SDL_Rect area = rectangle ? *rectangle : SDL_Rect{0, 0, texture->width, texture->height};
    if (area.x < 0 || area.y < 0 || area.w < 1 || area.h < 1 ||
        area.x + area.w > texture->width || area.y + area.h > texture->height ||
        pitch < area.w * 4) return -1;
    for (int y = 0; y < area.h; ++y)
        std::memcpy(texture->pixels.data() +
                        (static_cast<size_t>(area.y + y) * texture->width + area.x) * 4,
                    static_cast<const Uint8*>(pixels) + static_cast<size_t>(y) * pitch,
                    static_cast<size_t>(area.w) * 4);
    return 0;
}

extern "C" int SDL_UpdateYUVTexture(SDL_Texture* texture, const SDL_Rect*,
                                    const Uint8* y, int yPitch, const Uint8* u, int uPitch,
                                    const Uint8* v, int vPitch) {
    if (!texture || texture->format != SDL_PIXELFORMAT_IYUV ||
        !y || !u || !v || yPitch < texture->width ||
        uPitch < (texture->width + 1) / 2 || vPitch < (texture->width + 1) / 2)
        return -1;
    for (int row = 0; row < texture->height; ++row)
        std::memcpy(texture->pixels.data() + static_cast<size_t>(row) * texture->width,
                    y + static_cast<size_t>(row) * yPitch, texture->width);
    const int chromaWidth = (texture->width + 1) / 2;
    const int chromaHeight = (texture->height + 1) / 2;
    for (int row = 0; row < chromaHeight; ++row) {
        std::memcpy(texture->chromaU.data() + static_cast<size_t>(row) * chromaWidth,
                    u + static_cast<size_t>(row) * uPitch, chromaWidth);
        std::memcpy(texture->chromaV.data() + static_cast<size_t>(row) * chromaWidth,
                    v + static_cast<size_t>(row) * vPitch, chromaWidth);
    }
    return 0;
}

extern "C" const char* SDL_GetPixelFormatName(Uint32 format) {
    switch (format) {
    case SDL_PIXELFORMAT_IYUV: return "IYUV";
    case SDL_PIXELFORMAT_ARGB8888: return "ARGB8888";
    case SDL_PIXELFORMAT_ABGR8888: return "ABGR8888";
    default: return "unknown";
    }
}

extern "C" void SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_MODE mode) {
    sYuvConversion = mode;
}
