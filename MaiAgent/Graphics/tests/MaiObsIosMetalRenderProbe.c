#include <stdint.h>
#include <stdatomic.h>
#include <time.h>

#include "MaiGraphics.h"
#include "MaiGraphicsPresenter.h"
#include "MaiVideoPlayback.h"
#include "vec4.h"

static atomic_int present_result;
static atomic_int presented_frames;
static atomic_int video_ready;

static void on_present(uint64_t view_id, bool success, void* user_data) {
    (void)view_id;
    (void)user_data;
    atomic_store(&present_result, success ? 1 : -1);
    if (success) atomic_fetch_add(&presented_frames, 1);
}

static void on_video(MaiVideoPlayback* playback, MaiVideoPlaybackEvent event,
                     void* user_data) {
    (void)playback;
    (void)user_data;
    if (event == MAI_VIDEO_READY) atomic_store(&video_ready, 1);
    if (event == MAI_VIDEO_ERROR) atomic_store(&video_ready, -1);
}

int maiObsIosMetalPresentProbe(const char* image_path, const char* effect_directory,
                               const char* video_path, void* layer) {
    if (!maiGraphicsPresenterStart("builtin:metal", effect_directory, on_present, NULL))
        return 4;
    const uint64_t view_id = maiGraphicsPresenterAttach(layer, 64, 64, NULL, NULL);
    const bool accepted = view_id && maiGraphicsPresenterShowImage(view_id, image_path, false);
    const struct timespec delay = {0, 20000000};
    for (int attempt = 0; accepted && attempt < 400 && atomic_load(&present_result) == 0;
         ++attempt)
        nanosleep(&delay, NULL);
    bool passed = accepted && atomic_load(&present_result) == 1;
    if (passed) {
        const uint8_t green[] = {0, 255, 0, 255, 0, 255, 0, 255,
                                 0, 255, 0, 255, 0, 255, 0, 255};
        atomic_store(&present_result, 0);
        passed = maiGraphicsPresenterShowFrame(view_id, green, 2, 2, 8, false);
        for (int attempt = 0; passed && attempt < 400 && atomic_load(&present_result) == 0;
             ++attempt)
            nanosleep(&delay, NULL);
        passed = passed && atomic_load(&present_result) == 1;
    }
    if (passed) {
        MaiVideoPlayback* video = maiVideoPlaybackCreate(view_id, video_path, on_video, NULL);
        passed = video != NULL;
        for (int attempt = 0; passed && attempt < 400 && atomic_load(&video_ready) == 0;
             ++attempt)
            nanosleep(&delay, NULL);
        passed = passed && atomic_load(&video_ready) == 1;
        const int initial = atomic_load(&presented_frames);
        if (passed) passed = maiVideoPlaybackPlay(video);
        for (int attempt = 0; passed && attempt < 400 &&
                              atomic_load(&presented_frames) < initial + 2; ++attempt)
            nanosleep(&delay, NULL);
        passed = passed && atomic_load(&presented_frames) >= initial + 2;
        maiVideoPlaybackDestroy(video);
    }
    if (view_id) maiGraphicsPresenterDetach(view_id);
    maiGraphicsPresenterStop();
    return passed ? 0 : 5;
}

int maiObsIosMetalRenderProbe(const char* image_path) {
    graphics_t* graphics = NULL;
    if (gs_create(&graphics, "builtin:metal", 0) != GS_SUCCESS || !graphics) return 1;

    gs_enter_context(graphics);
    gs_texture_t* texture = gs_texture_create(2, 2, GS_RGBA, 1, NULL, GS_RENDER_TARGET);
    gs_stagesurf_t* surface = gs_stagesurface_create(2, 2, GS_RGBA);
    bool passed = gs_get_device_type() == GS_DEVICE_METAL && texture && surface;

    if (passed) {
        struct vec4 red;
        vec4_set(&red, 1.0f, 0.0f, 0.0f, 1.0f);
        gs_set_render_target(texture, NULL);
        gs_clear(GS_CLEAR_COLOR, &red, 1.0f, 0);
        gs_stage_texture(surface, texture);
        gs_flush();

        uint8_t* pixels = NULL;
        uint32_t stride = 0;
        passed = gs_stagesurface_map(surface, &pixels, &stride) && pixels && stride >= 8;
        if (passed) {
            for (uint32_t row = 0; row < 2; ++row) {
                for (uint32_t column = 0; column < 2; ++column) {
                    const uint8_t* pixel = pixels + row * stride + column * 4;
                    if (pixel[0] != 255 || pixel[1] != 0 || pixel[2] != 0 || pixel[3] != 255)
                        passed = false;
                }
            }
            gs_stagesurface_unmap(surface);
        }
        gs_set_render_target(NULL, NULL);
    }

    if (surface) gs_stagesurface_destroy(surface);
    if (texture) gs_texture_destroy(texture);
    gs_leave_context();
    gs_destroy(graphics);
    (void)image_path;
    return passed ? 0 : 2;
}
