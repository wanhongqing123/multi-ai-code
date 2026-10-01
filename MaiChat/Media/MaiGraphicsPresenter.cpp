#include "MaiGraphicsPresenter.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
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
};

struct MaiDecodedImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;
};

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
          mEffectFile(std::move(effectDirectory) + "/default.effect"),
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
                if (view->swapchain) gs_swapchain_destroy(view->swapchain);
            }
            mGraphicsViews.clear();
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
                if (presented) view->currentImage = image;
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
            if (presented) view->currentImage = image;
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
            if (view->currentImage) renderImage(view.get(), *view->currentImage);
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

    bool renderImage(MaiGraphicsViewState* view, const MaiDecodedImage& image) {
        if (!view->swapchain || !image.width || !image.height) return false;
        gs_enter_context(mGraphics);
        if (!mEffectAttempted) {
            mEffectAttempted = true;
            char* errors = nullptr;
            mEffect = gs_effect_create_from_file(mEffectFile.c_str(), &errors);
            if (errors) bfree(errors);
            if (mEffect) {
                mImageParameter = gs_effect_get_param_by_name(mEffect, "image");
                mDrawTechnique = gs_effect_get_technique(mEffect, "Draw");
            }
        }
        gs_eparam_t* parameter = mImageParameter;
        gs_technique_t* technique = mDrawTechnique;
        if (parameter && technique &&
            (!view->texture || view->textureWidth != image.width ||
             view->textureHeight != image.height)) {
            if (view->texture) gs_texture_destroy(view->texture);
            const uint8_t* planes[] = {image.pixels.data()};
            view->texture =
                gs_texture_create(image.width, image.height, GS_RGBA, 1, planes, GS_DYNAMIC);
            view->textureWidth = image.width;
            view->textureHeight = image.height;
        } else if (parameter && technique && view->texture) {
            gs_texture_set_image(view->texture, image.pixels.data(), image.width * 4, false);
        }
        gs_texture_t* texture = parameter && technique ? view->texture : nullptr;
        bool presented = false;
        if (texture) {
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
            gs_effect_set_texture_srgb(parameter, texture);
            const float widthScale = static_cast<float>(width) / image.width;
            const float heightScale = static_cast<float>(height) / image.height;
            const float scale = view->fillView ? std::max(widthScale, heightScale)
                                               : std::min(widthScale, heightScale);
            const float drawWidth = scale * image.width;
            const float drawHeight = scale * image.height;
            gs_matrix_push();
            gs_matrix_translate3f((width - drawWidth) / 2.0f, (height - drawHeight) / 2.0f, 0.0f);
            const size_t passes = gs_technique_begin(technique);
            presented = passes > 0;
            for (size_t pass = 0; pass < passes; ++pass) {
                if (!gs_technique_begin_pass(technique, pass)) {
                    presented = false;
                    break;
                }
                gs_draw_sprite(texture, 0, std::max<uint32_t>(1, drawWidth),
                               std::max<uint32_t>(1, drawHeight));
                gs_technique_end_pass(technique);
            }
            gs_technique_end(technique);
            gs_matrix_pop();
            gs_end_scene();
            if (presented && gs_is_present_ready())
                gs_present();
            else
                presented = false;
            gs_load_swapchain(nullptr);
        }
        gs_leave_context();
        return presented;
    }

    std::string mBackend;
    std::string mEffectFile;
    gs_effect_t* mEffect = nullptr;
    gs_eparam_t* mImageParameter = nullptr;
    gs_technique_t* mDrawTechnique = nullptr;
    bool mEffectAttempted = false;
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
