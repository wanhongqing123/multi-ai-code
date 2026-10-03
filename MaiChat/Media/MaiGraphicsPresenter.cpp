#include "MaiGraphicsPresenter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include "MaiGraphics.h"
#include "MaiVideoColorParameters.h"
#include "vec4.h"

namespace {

constexpr uint64_t kMaximumPixels = 64000000;

class MaiGraphicsTaskRunner {
public:
    explicit MaiGraphicsTaskRunner(const char* name) : mName(name), mThread([this] { run(); }) {}

    ~MaiGraphicsTaskRunner() {
        stop();
    }

    bool post(std::function<void()> task) {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mStopping) return false;
        mTasks.push_back(std::move(task));
        mCondition.notify_one();
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mStopping = true;
        }
        mCondition.notify_one();
        if (mThread.joinable()) mThread.join();
    }

private:
    void run() {
#if defined(_WIN32)
        SetThreadDescription(GetCurrentThread(), std::strcmp(mName, "mai-graphics") == 0
                                                     ? L"mai-graphics"
                                                     : L"mai-img-decode");
#elif defined(__APPLE__)
        pthread_setname_np(mName);
#else
        pthread_setname_np(pthread_self(), mName);
#endif
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mMutex);
                mCondition.wait(lock, [this] { return mStopping || !mTasks.empty(); });
                if (mTasks.empty()) return;
                task = std::move(mTasks.front());
                mTasks.pop_front();
            }
            task();
        }
    }

    const char* mName;
    std::mutex mMutex;
    std::condition_variable mCondition;
    std::deque<std::function<void()>> mTasks;
    bool mStopping = false;
    std::thread mThread;
};

struct MaiDecodedImage;
struct MaiDecodedVideoFrame;

struct MaiGraphicsViewState {
    ~MaiGraphicsViewState() {
        if (releaseView && nativeView) releaseView(nativeView);
    }
    uint64_t id = 0;
    void* nativeView = nullptr;
    MaiGraphicsNativeViewCallback releaseView = nullptr;
    std::atomic<uint32_t> width{0};
    std::atomic<uint32_t> height{0};
    std::atomic<uint64_t> generation{0};
    std::atomic<bool> fillView{false};
    std::atomic<bool> active{true};
    gs_swapchain_t* swapchain = nullptr;
    gs_texture_t* texture = nullptr;
    uint32_t textureWidth = 0;
    uint32_t textureHeight = 0;
    std::shared_ptr<MaiDecodedImage> currentImage;
    std::shared_ptr<MaiDecodedVideoFrame> currentVideo;
    std::array<gs_texture_t*, 3> videoTextures{};
    gs_texture_t* subtitleTexture = nullptr;
    uint32_t subtitleWidth = 0;
    uint32_t subtitleHeight = 0;
    gs_texrender_t* convertedVideo = nullptr;
    gs_color_format convertedVideoFormat = GS_UNKNOWN;
    MaiVideoPixelFormat videoFormat = MAI_VIDEO_PIXEL_RGBA;
    uint32_t videoWidth = 0;
    uint32_t videoHeight = 0;
    bool highBitFailureLogged = false;
    uint64_t highBitRendered = 0;
};

struct MaiDecodedImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;
};

struct MaiDecodedVideoFrame {
    uint32_t width = 0;
    uint32_t height = 0;
    MaiVideoPixelFormat format = MAI_VIDEO_PIXEL_RGBA;
    MaiVideoColorSpace colorSpace = MAI_VIDEO_COLOR_BT709;
    MaiVideoColorRange colorRange = MAI_VIDEO_RANGE_LIMITED;
    MaiVideoTransfer colorTransfer = MAI_VIDEO_TRANSFER_SDR;
    std::array<std::vector<uint8_t>, 3> planes;
    std::array<uint32_t, 3> strides{};
    std::vector<uint8_t> subtitlePixels;
};

uint32_t planeCount(MaiVideoPixelFormat format) {
    return format == MAI_VIDEO_PIXEL_I420 || format == MAI_VIDEO_PIXEL_I010 ? 3
         : format == MAI_VIDEO_PIXEL_NV12 || format == MAI_VIDEO_PIXEL_P010 ? 2 : 1;
}

uint32_t planeWidth(uint32_t width, uint32_t index) {
    return index == 0 ? width : (width + 1) / 2;
}

uint32_t planeHeight(uint32_t height, uint32_t index) {
    return index == 0 ? height : (height + 1) / 2;
}

uint32_t planeBytes(MaiVideoPixelFormat format, uint32_t width, uint32_t index) {
    if (index == 0 && (format == MAI_VIDEO_PIXEL_RGBA || format == MAI_VIDEO_PIXEL_BGRA))
        return width * 4;
    if (format == MAI_VIDEO_PIXEL_P010)
        return planeWidth(width, index) * (index == 0 ? 2 : 4);
    if (format == MAI_VIDEO_PIXEL_I010)
        return planeWidth(width, index) * 2;
    return format == MAI_VIDEO_PIXEL_NV12 && index == 1 ? planeWidth(width, index) * 2
                                                        : planeWidth(width, index);
}

AVPixelFormat pixelFormat(gs_color_format format) {
    switch (format) {
        case GS_RGBA: return AV_PIX_FMT_RGBA;
        case GS_BGRA: return AV_PIX_FMT_BGRA;
        case GS_BGRX: return AV_PIX_FMT_BGR0;
        default: return AV_PIX_FMT_NONE;
    }
}

bool decodeImage(const char* path, uint32_t maxWidth, uint32_t maxHeight, bool fillView,
                 MaiDecodedImage* image) {
    gs_color_format format = GS_UNKNOWN;
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    uint8_t* decoded = gs_create_texture_file_data(path, &format, &sourceWidth, &sourceHeight);
    if (!decoded) return false;

    bool success = false;
    do {
        if (!sourceWidth || !sourceHeight ||
            static_cast<uint64_t>(sourceWidth) * sourceHeight > kMaximumPixels)
            break;
        const AVPixelFormat inputFormat = pixelFormat(format);
        if (inputFormat == AV_PIX_FMT_NONE) break;

        uint32_t width = sourceWidth;
        uint32_t height = sourceHeight;
        if (fillView && (width > maxWidth || height > maxHeight)) {
            const double scale = std::min(1.0, std::max(static_cast<double>(maxWidth) / width,
                                                        static_cast<double>(maxHeight) / height));
            width = std::max<uint32_t>(1, static_cast<uint32_t>(width * scale));
            height = std::max<uint32_t>(1, static_cast<uint32_t>(height * scale));
        } else if (width > maxWidth || height > maxHeight) {
            if (static_cast<uint64_t>(width) * maxHeight >
                static_cast<uint64_t>(height) * maxWidth) {
                height = std::max<uint32_t>(1, static_cast<uint64_t>(height) * maxWidth / width);
                width = maxWidth;
            } else {
                width = std::max<uint32_t>(1, static_cast<uint64_t>(width) * maxHeight / height);
                height = maxHeight;
            }
        }
        if (static_cast<uint64_t>(width) * height > kMaximumPixels) break;

        SwsContext* scale =
            sws_getContext(static_cast<int>(sourceWidth), static_cast<int>(sourceHeight),
                           inputFormat, static_cast<int>(width), static_cast<int>(height),
                           AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scale) break;
        image->pixels.resize(static_cast<size_t>(width) * height * 4);
        const uint8_t* sourcePlanes[] = {decoded, nullptr, nullptr, nullptr};
        const int sourceStrides[] = {static_cast<int>(sourceWidth * 4), 0, 0, 0};
        uint8_t* outputPlanes[] = {image->pixels.data(), nullptr, nullptr, nullptr};
        const int outputStrides[] = {static_cast<int>(width * 4), 0, 0, 0};
        const int rows = sws_scale(scale, sourcePlanes, sourceStrides, 0,
                                   static_cast<int>(sourceHeight), outputPlanes, outputStrides);
        sws_freeContext(scale);
        if (rows != static_cast<int>(height)) break;
        image->width = width;
        image->height = height;
        success = true;
    } while (false);

    bfree(decoded);
    return success;
}

class MaiGraphicsPresenter {
public:
    MaiGraphicsPresenter(std::string backend, std::string effectDirectory,
                         MaiGraphicsPresentCallback callback, void* userData)
        : mBackend(std::move(backend)),
          mEffectFile(effectDirectory + "/default.effect"),
          mConversionEffectFile(effectDirectory + "/video_conversion.effect"),
          mHdrConversionEffectFile(std::move(effectDirectory) + "/format_conversion.effect"),
          mCallback(callback),
          mUserData(userData),
          mGraphicsRunner("mai-graphics"),
          mDecodeRunner("mai-img-decode") {}

    ~MaiGraphicsPresenter() {
        {
            std::lock_guard<std::mutex> lock(mViewsMutex);
            for (auto& item : mViews) item.second->active = false;
            mViews.clear();
        }
        mDecodeRunner.stop();
        if (!mGraphics) {
            mGraphicsRunner.stop();
            return;
        }
        mGraphicsRunner.post([this] {
            gs_enter_context(mGraphics);
            for (auto& view : mGraphicsViews) {
                if (view->texture) gs_texture_destroy(view->texture);
                for (gs_texture_t* texture : view->videoTextures)
                    if (texture) gs_texture_destroy(texture);
                if (view->subtitleTexture) gs_texture_destroy(view->subtitleTexture);
                if (view->convertedVideo) gs_texrender_destroy(view->convertedVideo);
                if (view->swapchain) gs_swapchain_destroy(view->swapchain);
            }
            mGraphicsViews.clear();
            if (mConversionEffect) gs_effect_destroy(mConversionEffect);
            if (mHdrConversionEffect) gs_effect_destroy(mHdrConversionEffect);
            if (mEffect) {
                gs_effect_destroy(mEffect);
                mEffect = nullptr;
            }
            gs_leave_context();
            gs_destroy(mGraphics);
            mGraphics = nullptr;
        });
        mGraphicsRunner.stop();
    }

    bool start() {
        auto ready = std::make_shared<std::promise<bool>>();
        auto future = ready->get_future();
        if (!mGraphicsRunner.post([this, ready] {
                const bool created =
                    gs_create(&mGraphics, mBackend.c_str(), 0) == GS_SUCCESS && mGraphics;
                ready->set_value(created);
            }))
            return false;
        return future.get();
    }

    uint64_t attach(void* nativeView, uint32_t width, uint32_t height,
                    MaiGraphicsNativeViewCallback retainView,
                    MaiGraphicsNativeViewCallback releaseView) {
        if (!nativeView || !width || !height || (retainView == nullptr) != (releaseView == nullptr))
            return 0;
        auto view = std::make_shared<MaiGraphicsViewState>();
        if (retainView) retainView(nativeView);
        view->id = mNextViewId++;
        view->nativeView = nativeView;
        view->releaseView = releaseView;
        view->width = width;
        view->height = height;
        {
            std::lock_guard<std::mutex> lock(mViewsMutex);
            mViews.emplace(view->id, view);
        }
        const bool queued = mGraphicsRunner.post([this, view] {
            if (!view->active) return;
            gs_enter_context(mGraphics);
            gs_init_data data = {};
#if defined(__APPLE__)
            data.window.view = (id)view->nativeView;
#elif defined(__ANDROID__)
            data.window.native_window = view->nativeView;
#elif defined(_WIN32)
            data.window.hwnd = view->nativeView;
#endif
            data.cx = view->width;
            data.cy = view->height;
            data.format = GS_RGBA;
            data.zsformat = GS_ZS_NONE;
            view->swapchain = gs_swapchain_create(&data);
            if (view->swapchain) mGraphicsViews.push_back(view);
            gs_leave_context();
            if (!view->swapchain) notify(view->id, false);
        });
        if (!queued) {
            view->active = false;
            std::lock_guard<std::mutex> lock(mViewsMutex);
            mViews.erase(view->id);
            return 0;
        }
        return view->id;
    }

    bool showImage(uint64_t id, const char* path, bool fillView) {
        if (!path || !*path) return false;
        std::shared_ptr<MaiGraphicsViewState> view = findView(id);
        if (!view) return false;
        const uint64_t generation = ++view->generation;
        std::string copiedPath(path);
        return mDecodeRunner.post([this, view, generation, fillView,
                                   copiedPath = std::move(copiedPath)] {
            if (!view->active || view->generation != generation) return;
            auto image = std::make_shared<MaiDecodedImage>();
            const bool decoded =
                decodeImage(copiedPath.c_str(), view->width, view->height, fillView, image.get());
            mGraphicsRunner.post([this, view, generation, fillView, image, decoded] {
                if (!view->active || view->generation != generation) return;
                view->fillView = fillView;
                const bool presented = decoded && renderImage(view.get(), *image);
                if (presented) {
                    view->currentImage = image;
                    view->currentVideo.reset();
                }
                notify(view->id, presented);
            });
        });
    }

    bool showFrame(uint64_t id, const uint8_t* pixels, uint32_t width, uint32_t height,
                   uint32_t stride, bool fillView) {
        if (!pixels || !width || !height ||
            static_cast<uint64_t>(width) * height > kMaximumPixels ||
            static_cast<uint64_t>(stride) < static_cast<uint64_t>(width) * 4)
            return false;
        std::shared_ptr<MaiGraphicsViewState> view = findView(id);
        if (!view) return false;
        auto image = std::make_shared<MaiDecodedImage>();
        image->width = width;
        image->height = height;
        image->pixels.resize(static_cast<size_t>(width) * height * 4);
        for (uint32_t row = 0; row < height; ++row) {
            std::memcpy(image->pixels.data() + static_cast<size_t>(row) * width * 4,
                        pixels + static_cast<size_t>(row) * stride, static_cast<size_t>(width) * 4);
        }
        const uint64_t generation = ++view->generation;
        return mGraphicsRunner.post([this, view, image, generation, fillView] {
            if (!view->active || view->generation != generation) return;
            view->fillView = fillView;
            const bool presented = renderImage(view.get(), *image);
            if (presented) {
                view->currentImage = image;
                view->currentVideo.reset();
            }
            notify(view->id, presented);
        });
    }

    bool showVideoFrame(uint64_t id, const MaiVideoFrame* source,
                        const MaiVideoSubtitle* subtitle, bool fillView) {
        if (!source || !source->width || !source->height || source->width > 16384 ||
            source->height > 16384 ||
            static_cast<uint64_t>(source->width) * source->height > kMaximumPixels)
            return false;
        const MaiVideoPixelFormat format = source->format;
        if (format != MAI_VIDEO_PIXEL_RGBA && format != MAI_VIDEO_PIXEL_BGRA &&
            format != MAI_VIDEO_PIXEL_NV12 && format != MAI_VIDEO_PIXEL_I420 &&
            format != MAI_VIDEO_PIXEL_P010 && format != MAI_VIDEO_PIXEL_I010)
            return false;
        std::shared_ptr<MaiGraphicsViewState> view = findView(id);
        if (!view) return false;

        auto frame = std::make_shared<MaiDecodedVideoFrame>();
        frame->width = source->width;
        frame->height = source->height;
        frame->format = format;
        frame->colorSpace = source->color_space;
        frame->colorRange = source->color_range;
        frame->colorTransfer = source->color_transfer;
        for (uint32_t index = 0; index < planeCount(format); ++index) {
            const uint32_t rowBytes = planeBytes(format, source->width, index);
            const uint32_t rows = planeHeight(source->height, index);
            const int64_t lineSize = source->linesize[index];
            if (!source->data[index] || lineSize == 0 ||
                (lineSize < 0 ? -lineSize : lineSize) < rowBytes)
                return false;
            frame->strides[index] = rowBytes;
            frame->planes[index].resize(static_cast<size_t>(rowBytes) * rows);
            for (uint32_t row = 0; row < rows; ++row) {
                const uint8_t* sourceRow =
                    source->data[index] + static_cast<std::ptrdiff_t>(row) * lineSize;
                std::memcpy(frame->planes[index].data() + static_cast<size_t>(row) * rowBytes,
                            sourceRow, rowBytes);
            }
        }
        if (subtitle) {
            if (!subtitle->rgba || subtitle->width != source->width ||
                subtitle->height != source->height ||
                subtitle->stride < source->width * 4)
                return false;
            frame->subtitlePixels.resize(static_cast<size_t>(source->width) * source->height * 4);
            for (uint32_t row = 0; row < source->height; ++row)
                std::memcpy(frame->subtitlePixels.data() +
                                static_cast<size_t>(row) * source->width * 4,
                            subtitle->rgba + static_cast<size_t>(row) * subtitle->stride,
                            static_cast<size_t>(source->width) * 4);
        }

        const uint64_t generation = ++view->generation;
        return mGraphicsRunner.post([this, view, frame, generation, fillView] {
            if (!view->active || view->generation != generation) return;
            view->fillView = fillView;
            const bool presented = renderVideo(view.get(), *frame);
            if (presented) {
                view->currentVideo = frame;
                view->currentImage.reset();
            }
            notify(view->id, presented);
        });
    }

    void resize(uint64_t id, uint32_t width, uint32_t height) {
        std::shared_ptr<MaiGraphicsViewState> view = findView(id);
        if (!view || !width || !height) return;
        view->width = width;
        view->height = height;
        mGraphicsRunner.post([this, view, width, height] {
            if (!view->active || !view->swapchain) return;
            gs_enter_context(mGraphics);
            gs_load_swapchain(view->swapchain);
            gs_resize(width, height);
            gs_load_swapchain(nullptr);
            gs_leave_context();
            if (view->currentVideo)
                renderVideo(view.get(), *view->currentVideo);
            else if (view->currentImage)
                renderImage(view.get(), *view->currentImage);
        });
    }

    void detach(uint64_t id) {
        std::shared_ptr<MaiGraphicsViewState> view;
        {
            std::lock_guard<std::mutex> lock(mViewsMutex);
            auto found = mViews.find(id);
            if (found == mViews.end()) return;
            view = found->second;
            view->active = false;
            mViews.erase(found);
        }
        mGraphicsRunner.post([this, view] {
            gs_enter_context(mGraphics);
            if (view->texture) {
                gs_texture_destroy(view->texture);
                view->texture = nullptr;
            }
            for (gs_texture_t*& texture : view->videoTextures) {
                if (texture) gs_texture_destroy(texture);
                texture = nullptr;
            }
            if (view->subtitleTexture) {
                gs_texture_destroy(view->subtitleTexture);
                view->subtitleTexture = nullptr;
            }
            if (view->convertedVideo) {
                gs_texrender_destroy(view->convertedVideo);
                view->convertedVideo = nullptr;
            }
            if (view->swapchain) {
                gs_load_swapchain(nullptr);
                gs_swapchain_destroy(view->swapchain);
                view->swapchain = nullptr;
            }
            mGraphicsViews.erase(std::remove(mGraphicsViews.begin(), mGraphicsViews.end(), view),
                                 mGraphicsViews.end());
            gs_leave_context();
        });
    }

private:
    std::shared_ptr<MaiGraphicsViewState> findView(uint64_t id) {
        std::lock_guard<std::mutex> lock(mViewsMutex);
        auto found = mViews.find(id);
        return found == mViews.end() ? nullptr : found->second;
    }

    void notify(uint64_t id, bool success) {
        if (mCallback) mCallback(id, success, mUserData);
    }

    bool ensureDrawEffect() {
        if (!mEffectAttempted) {
            mEffectAttempted = true;
            char* errors = nullptr;
            mEffect = gs_effect_create_from_file(mEffectFile.c_str(), &errors);
            if (errors) bfree(errors);
            if (mEffect) {
                mImageParameter = gs_effect_get_param_by_name(mEffect, "image");
                mDrawTechnique = gs_effect_get_technique(mEffect, "Draw");
                mDrawTonemapTechnique = gs_effect_get_technique(mEffect, "DrawTonemap");
            }
        }
        return mImageParameter && mDrawTechnique;
    }

    bool ensureConversionEffect() {
        if (!mConversionEffectAttempted) {
            mConversionEffectAttempted = true;
            char* errors = nullptr;
            mConversionEffect = gs_effect_create_from_file(mConversionEffectFile.c_str(), &errors);
            if (errors) bfree(errors);
        }
        return mConversionEffect != nullptr;
    }

    bool ensureHdrConversionEffect() {
        if (!mHdrConversionEffectAttempted) {
            mHdrConversionEffectAttempted = true;
            char* errors = nullptr;
            mHdrConversionEffect = gs_effect_create_from_file(mHdrConversionEffectFile.c_str(),
                                                              &errors);
            if (errors) std::fprintf(stderr, "[MaiVideoHDR] shader: %s\n", errors);
            if (errors) bfree(errors);
        }
        return mHdrConversionEffect != nullptr;
    }

    bool drawTexture(MaiGraphicsViewState* view, gs_texture_t* texture, uint32_t sourceWidth,
                     uint32_t sourceHeight, gs_texture_t* subtitle = nullptr,
                     bool hdr = false) {
        if (!view->swapchain || !texture || !ensureDrawEffect()) return false;
        gs_technique_t* drawTechnique = hdr ? mDrawTonemapTechnique : mDrawTechnique;
        if (!drawTechnique) return false;
        const uint32_t width = view->width;
        const uint32_t height = view->height;
        gs_load_swapchain(view->swapchain);
        gs_begin_scene();
        gs_set_viewport(0, 0, static_cast<int>(width), static_cast<int>(height));
        gs_ortho(0.0f, static_cast<float>(width), 0.0f, static_cast<float>(height), -100.0f,
                 100.0f);
        struct vec4 black;
        vec4_set(&black, 0.0f, 0.0f, 0.0f, 1.0f);
        gs_clear(GS_CLEAR_COLOR, &black, 1.0f, 0);
        gs_set_cull_mode(GS_NEITHER);
        gs_enable_blending(false);
        if (hdr) gs_effect_set_texture(mImageParameter, texture);
        else gs_effect_set_texture_srgb(mImageParameter, texture);
        const float widthScale = static_cast<float>(width) / sourceWidth;
        const float heightScale = static_cast<float>(height) / sourceHeight;
        const float scale =
            view->fillView ? std::max(widthScale, heightScale) : std::min(widthScale, heightScale);
        const float drawWidth = scale * sourceWidth;
        const float drawHeight = scale * sourceHeight;
        gs_matrix_push();
        gs_matrix_translate3f((width - drawWidth) / 2.0f, (height - drawHeight) / 2.0f, 0.0f);
        const size_t passes = gs_technique_begin(drawTechnique);
        bool presented = passes > 0;
        for (size_t pass = 0; pass < passes; ++pass) {
            if (!gs_technique_begin_pass(drawTechnique, pass)) {
                presented = false;
                break;
            }
            gs_draw_sprite(texture, 0, std::max<uint32_t>(1, drawWidth),
                           std::max<uint32_t>(1, drawHeight));
            gs_technique_end_pass(drawTechnique);
        }
        gs_technique_end(drawTechnique);
        if (presented && subtitle) {
            gs_blend_state_push();
            gs_enable_blending(true);
            gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
            gs_effect_set_texture_srgb(mImageParameter, subtitle);
            const size_t subtitlePasses = gs_technique_begin(mDrawTechnique);
            presented = subtitlePasses > 0;
            for (size_t pass = 0; pass < subtitlePasses; ++pass) {
                if (!gs_technique_begin_pass(mDrawTechnique, pass)) {
                    presented = false;
                    break;
                }
                gs_draw_sprite(subtitle, 0, std::max<uint32_t>(1, drawWidth),
                               std::max<uint32_t>(1, drawHeight));
                gs_technique_end_pass(mDrawTechnique);
            }
            gs_technique_end(mDrawTechnique);
            gs_blend_state_pop();
        }
        gs_matrix_pop();
        gs_end_scene();
        if (presented && gs_is_present_ready())
            gs_present();
        else
            presented = false;
        gs_load_swapchain(nullptr);
        return presented;
    }

    bool renderImage(MaiGraphicsViewState* view, const MaiDecodedImage& image) {
        if (!view->swapchain || !image.width || !image.height) return false;
        gs_enter_context(mGraphics);
        if (!view->texture || view->textureWidth != image.width ||
            view->textureHeight != image.height) {
            if (view->texture) gs_texture_destroy(view->texture);
            view->texture =
                gs_texture_create(image.width, image.height, GS_RGBA, 1, nullptr, GS_DYNAMIC);
            view->textureWidth = image.width;
            view->textureHeight = image.height;
        }
        if (view->texture)
            gs_texture_set_image(view->texture, image.pixels.data(), image.width * 4, false);
        const bool presented = drawTexture(view, view->texture, image.width, image.height);
        gs_leave_context();
        return presented;
    }

    void destroyVideoTextures(MaiGraphicsViewState* view) {
        for (gs_texture_t*& texture : view->videoTextures) {
            if (texture) gs_texture_destroy(texture);
            texture = nullptr;
        }
        view->videoWidth = view->videoHeight = 0;
    }

    bool uploadVideoPlanes(MaiGraphicsViewState* view, const MaiDecodedVideoFrame& frame) {
        if (view->videoWidth != frame.width || view->videoHeight != frame.height ||
            view->videoFormat != frame.format || !view->videoTextures[0]) {
            destroyVideoTextures(view);
            for (uint32_t index = 0; index < planeCount(frame.format); ++index) {
                const gs_color_format textureFormat =
                    frame.format == MAI_VIDEO_PIXEL_RGBA                 ? GS_RGBA
                    : frame.format == MAI_VIDEO_PIXEL_BGRA               ? GS_BGRA
                    : frame.format == MAI_VIDEO_PIXEL_P010 && index == 1 ? GS_RG16
                    : frame.format == MAI_VIDEO_PIXEL_P010 ||
                              frame.format == MAI_VIDEO_PIXEL_I010       ? GS_R16
                    : frame.format == MAI_VIDEO_PIXEL_NV12 && index == 1 ? GS_R8G8
                                                                         : GS_R8;
                view->videoTextures[index] = gs_texture_create(
                    planeWidth(frame.width, index), planeHeight(frame.height, index), textureFormat,
                    1, nullptr, GS_DYNAMIC);
                if (!view->videoTextures[index]) {
                    destroyVideoTextures(view);
                    return false;
                }
            }
            view->videoWidth = frame.width;
            view->videoHeight = frame.height;
            view->videoFormat = frame.format;
        }
        for (uint32_t index = 0; index < planeCount(frame.format); ++index)
            gs_texture_set_image(view->videoTextures[index], frame.planes[index].data(),
                                 frame.strides[index], false);
        return true;
    }

    gs_texture_t* convertVideo(MaiGraphicsViewState* view, const MaiDecodedVideoFrame& frame) {
        if (frame.format == MAI_VIDEO_PIXEL_RGBA || frame.format == MAI_VIDEO_PIXEL_BGRA)
            return view->videoTextures[0];
        const bool highBit = frame.format == MAI_VIDEO_PIXEL_P010 ||
                             frame.format == MAI_VIDEO_PIXEL_I010;
        if (highBit ? !ensureHdrConversionEffect() : !ensureConversionEffect()) return nullptr;
        gs_effect_t* effect = highBit ? mHdrConversionEffect : mConversionEffect;
        const char* techniqueName = nullptr;
        if (highBit) {
            const bool planar = frame.format == MAI_VIDEO_PIXEL_I010;
            techniqueName = frame.colorTransfer == MAI_VIDEO_TRANSFER_HLG
                                ? (planar ? "I010_HLG_2020_709_Reverse"
                                          : "P010_HLG_2020_709_Reverse")
                            : frame.colorTransfer == MAI_VIDEO_TRANSFER_PQ
                                ? (planar ? "I010_PQ_2020_709_Reverse"
                                          : "P010_PQ_2020_709_Reverse")
                                : (planar ? "I010_SRGB_Reverse" : "P010_SRGB_Reverse");
        } else {
            techniqueName = frame.format == MAI_VIDEO_PIXEL_NV12
                                ? "NV12_Reverse" : "I420_Reverse";
        }
        gs_technique_t* technique = gs_effect_get_technique(effect, techniqueName);
        gs_eparam_t* image = gs_effect_get_param_by_name(effect, "image");
        gs_eparam_t* image1 = gs_effect_get_param_by_name(effect, "image1");
        gs_eparam_t* image2 = gs_effect_get_param_by_name(effect, "image2");
        if (!technique || !image || !image1 ||
            ((frame.format == MAI_VIDEO_PIXEL_I420 ||
              frame.format == MAI_VIDEO_PIXEL_I010) && !image2))
            return nullptr;
        const gs_color_format outputFormat =
            highBit ? GS_RGBA16F :
#if defined(__ANDROID__)
                GS_RGBA_UNORM;
#else
                GS_RGBA;
#endif
        if (!view->convertedVideo || view->convertedVideoFormat != outputFormat) {
            if (view->convertedVideo) gs_texrender_destroy(view->convertedVideo);
            view->convertedVideo = gs_texrender_create(
                outputFormat, GS_ZS_NONE);
            view->convertedVideoFormat = outputFormat;
        }
        if (!view->convertedVideo) return nullptr;
        gs_texrender_reset(view->convertedVideo);
        if (!gs_texrender_begin(view->convertedVideo, frame.width, frame.height)) return nullptr;

        const MaiVideoColorParameters color =
            maiVideoColorParameters(frame.colorSpace, frame.colorRange);
        const char* colorNames[] = {"color_vec0", "color_vec1", "color_vec2"};
        for (int row = 0; row < 3; ++row) {
            struct vec4 value;
            vec4_set(&value, color.vectors[row][0], color.vectors[row][1], color.vectors[row][2],
                     color.vectors[row][3]);
            gs_effect_set_vec4(gs_effect_get_param_by_name(effect, colorNames[row]),
                               &value);
        }
        gs_effect_set_val(gs_effect_get_param_by_name(effect, "color_range_min"),
                          color.rangeMin, sizeof(color.rangeMin));
        gs_effect_set_val(gs_effect_get_param_by_name(effect, "color_range_max"),
                          color.rangeMax, sizeof(color.rangeMax));
        gs_effect_set_float(gs_effect_get_param_by_name(effect, "width"),
                            static_cast<float>(frame.width));
        gs_effect_set_float(gs_effect_get_param_by_name(effect, "height"),
                            static_cast<float>(frame.height));
        gs_effect_set_float(gs_effect_get_param_by_name(effect, "width_d2"),
                            frame.width * 0.5f);
        gs_effect_set_float(gs_effect_get_param_by_name(effect, "height_d2"),
                            frame.height * 0.5f);
        gs_effect_set_float(gs_effect_get_param_by_name(effect, "width_x2_i"),
                            0.5f / frame.width);
        gs_effect_set_float(gs_effect_get_param_by_name(effect, "height_x2_i"),
                            0.5f / frame.height);
        if (highBit) {
            const float maximumNits = frame.colorTransfer == MAI_VIDEO_TRANSFER_HLG
                                          ? 1000.0f : 10000.0f;
            gs_effect_set_float(gs_effect_get_param_by_name(effect,
                                    "maximum_over_sdr_white_nits"), maximumNits / 300.0f);
            gs_effect_set_float(gs_effect_get_param_by_name(effect, "hlg_exponent"), 0.2f);
            gs_effect_set_float(gs_effect_get_param_by_name(effect, "hdr_lw"), 1000.0f);
            gs_effect_set_float(gs_effect_get_param_by_name(effect, "hdr_lmax"), 1000.0f);
        }
        gs_effect_set_texture(image, view->videoTextures[0]);
        gs_effect_set_texture(image1, view->videoTextures[1]);
        if (frame.format == MAI_VIDEO_PIXEL_I420 || frame.format == MAI_VIDEO_PIXEL_I010)
            gs_effect_set_texture(image2, view->videoTextures[2]);
        const bool previousSrgb = gs_framebuffer_srgb_enabled();
        gs_enable_framebuffer_srgb(highBit);
        gs_enable_blending(false);
        const size_t passes = gs_technique_begin(technique);
        bool converted = passes > 0;
        for (size_t pass = 0; pass < passes; ++pass) {
            if (!gs_technique_begin_pass(technique, pass)) {
                converted = false;
                break;
            }
            gs_draw(GS_TRIS, 0, 3);
            gs_technique_end_pass(technique);
        }
        gs_technique_end(technique);
        gs_enable_blending(true);
        gs_enable_framebuffer_srgb(previousSrgb);
        gs_texrender_end(view->convertedVideo);
        return converted ? gs_texrender_get_texture(view->convertedVideo) : nullptr;
    }

    bool renderVideo(MaiGraphicsViewState* view, const MaiDecodedVideoFrame& frame) {
        if (!view->swapchain) return false;
        gs_enter_context(mGraphics);
        const bool uploaded = uploadVideoPlanes(view, frame);
        gs_texture_t* texture = uploaded ? convertVideo(view, frame) : nullptr;
        if (!frame.subtitlePixels.empty() &&
            (!view->subtitleTexture || view->subtitleWidth != frame.width ||
             view->subtitleHeight != frame.height)) {
            if (view->subtitleTexture) gs_texture_destroy(view->subtitleTexture);
            view->subtitleTexture = gs_texture_create(frame.width, frame.height, GS_RGBA, 1,
                                                      nullptr, GS_DYNAMIC);
            view->subtitleWidth = frame.width;
            view->subtitleHeight = frame.height;
        }
        if (!frame.subtitlePixels.empty() && view->subtitleTexture)
            gs_texture_set_image(view->subtitleTexture, frame.subtitlePixels.data(),
                                 frame.width * 4, false);
        const bool presented = drawTexture(view, texture, frame.width, frame.height,
                                           frame.subtitlePixels.empty() ? nullptr
                                                                       : view->subtitleTexture,
                                           frame.colorTransfer != MAI_VIDEO_TRANSFER_SDR);
        if (!presented && frame.colorTransfer != MAI_VIDEO_TRANSFER_SDR &&
            !view->highBitFailureLogged) {
            view->highBitFailureLogged = true;
            std::fprintf(stderr, "[MaiVideoHDR] render failed format=%d transfer=%d\n",
                         static_cast<int>(frame.format),
                         static_cast<int>(frame.colorTransfer));
        }
#if !defined(NDEBUG)
        if (presented && frame.colorTransfer != MAI_VIDEO_TRANSFER_SDR &&
            ++view->highBitRendered % 100 == 0)
            std::fprintf(stderr, "[MaiVideoHDR] rendered=%llu\n",
                         static_cast<unsigned long long>(view->highBitRendered));
#endif
        gs_leave_context();
        return presented;
    }

    std::string mBackend;
    std::string mEffectFile;
    std::string mConversionEffectFile;
    std::string mHdrConversionEffectFile;
    gs_effect_t* mEffect = nullptr;
    gs_effect_t* mConversionEffect = nullptr;
    gs_effect_t* mHdrConversionEffect = nullptr;
    gs_eparam_t* mImageParameter = nullptr;
    gs_technique_t* mDrawTechnique = nullptr;
    gs_technique_t* mDrawTonemapTechnique = nullptr;
    bool mEffectAttempted = false;
    bool mConversionEffectAttempted = false;
    bool mHdrConversionEffectAttempted = false;
    MaiGraphicsPresentCallback mCallback;
    void* mUserData;
    MaiGraphicsTaskRunner mGraphicsRunner;
    MaiGraphicsTaskRunner mDecodeRunner;
    graphics_t* mGraphics = nullptr;
    std::mutex mViewsMutex;
    std::unordered_map<uint64_t, std::shared_ptr<MaiGraphicsViewState>> mViews;
    std::vector<std::shared_ptr<MaiGraphicsViewState>> mGraphicsViews;
    std::atomic<uint64_t> mNextViewId{1};
};

std::mutex sPresenterMutex;
std::shared_ptr<MaiGraphicsPresenter> sPresenter;

std::shared_ptr<MaiGraphicsPresenter> presenter() {
    std::lock_guard<std::mutex> lock(sPresenterMutex);
    return sPresenter;
}

}  // namespace

extern "C" bool maiGraphicsPresenterStart(const char* backend, const char* effect_directory,
                                          MaiGraphicsPresentCallback callback, void* user_data) {
    if (!backend || !*backend || !effect_directory || !*effect_directory) return false;
    try {
        std::lock_guard<std::mutex> lock(sPresenterMutex);
        if (sPresenter) return false;
        auto created =
            std::make_shared<MaiGraphicsPresenter>(backend, effect_directory, callback, user_data);
        if (!created->start()) return false;
        sPresenter = std::move(created);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

extern "C" uint64_t maiGraphicsPresenterAttach(void* native_view, uint32_t width, uint32_t height,
                                               MaiGraphicsNativeViewCallback retain_view,
                                               MaiGraphicsNativeViewCallback release_view) {
    try {
        auto current = presenter();
        return current ? current->attach(native_view, width, height, retain_view, release_view) : 0;
    } catch (const std::exception&) {
        return 0;
    }
}

extern "C" bool maiGraphicsPresenterShowImage(uint64_t view_id, const char* file_path,
                                              bool fill_view) {
    try {
        auto current = presenter();
        return current && current->showImage(view_id, file_path, fill_view);
    } catch (const std::exception&) {
        return false;
    }
}

extern "C" bool maiGraphicsPresenterShowFrame(uint64_t view_id, const uint8_t* pixels,
                                              uint32_t width, uint32_t height, uint32_t stride,
                                              bool fill_view) {
    try {
        auto current = presenter();
        return current && current->showFrame(view_id, pixels, width, height, stride, fill_view);
    } catch (const std::exception&) {
        return false;
    }
}

extern "C" bool maiGraphicsPresenterShowVideoFrame(uint64_t view_id, const MaiVideoFrame* frame,
                                                    bool fill_view) {
    return maiGraphicsPresenterShowVideoFrameWithSubtitle(view_id, frame, nullptr, fill_view);
}

extern "C" bool maiGraphicsPresenterShowVideoFrameWithSubtitle(
    uint64_t view_id, const MaiVideoFrame* frame, const MaiVideoSubtitle* subtitle,
    bool fill_view) {
    try {
        auto current = presenter();
        return current && current->showVideoFrame(view_id, frame, subtitle, fill_view);
    } catch (const std::exception&) {
        return false;
    }
}

extern "C" void maiGraphicsPresenterResize(uint64_t view_id, uint32_t width, uint32_t height) {
    auto current = presenter();
    if (current) current->resize(view_id, width, height);
}

extern "C" void maiGraphicsPresenterDetach(uint64_t view_id) {
    auto current = presenter();
    if (current) current->detach(view_id);
}

extern "C" void maiGraphicsPresenterStop(void) {
    std::shared_ptr<MaiGraphicsPresenter> old;
    {
        std::lock_guard<std::mutex> lock(sPresenterMutex);
        old = std::move(sPresenter);
    }
}
