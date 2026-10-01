#include "MaiFfplayEntry.h"

#include <setjmp.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#define MAI_FFPLAY_THREAD_LOCAL __declspec(thread)
#define MAI_FFPLAY_NORETURN __declspec(noreturn)
#else
#include <stdatomic.h>
#define MAI_FFPLAY_THREAD_LOCAL _Thread_local
#define MAI_FFPLAY_NORETURN _Noreturn
#endif

#include "SDL.h"
#include "MaiFfplayCompatInternal.h"

#if defined(_WIN32)
static volatile LONG sRunning;
static volatile LONG sPlaybackActive;
static volatile LONG64 sPositionUs;
static volatile LONG64 sDurationUs;
static volatile LONG sPaused;
#define MAI_STATUS_STORE64(field, value) InterlockedExchange64(&(field), (value))
#define MAI_STATUS_LOAD64(field) InterlockedCompareExchange64(&(field), 0, 0)
#define MAI_STATUS_STORE(field, value) InterlockedExchange(&(field), (value))
#define MAI_STATUS_LOAD(field) InterlockedCompareExchange(&(field), 0, 0)
#else
static atomic_flag sRunning = ATOMIC_FLAG_INIT;
static atomic_int sPlaybackActive;
static atomic_int_fast64_t sPositionUs;
static atomic_int_fast64_t sDurationUs;
static atomic_int sPaused;
#define MAI_STATUS_STORE64(field, value) atomic_store(&(field), (value))
#define MAI_STATUS_LOAD64(field) atomic_load(&(field))
#define MAI_STATUS_STORE(field, value) atomic_store(&(field), (value))
#define MAI_STATUS_LOAD(field) atomic_load(&(field))
#endif
static MAI_FFPLAY_THREAD_LOCAL jmp_buf sExitPoint;
static MAI_FFPLAY_THREAD_LOCAL int sExitReady;
static MAI_FFPLAY_THREAD_LOCAL int sDidCleanUp;
static void (*sDiagnosticSink)(void* user_data, int level, const char* line);
static void* sDiagnosticUserData;

struct VideoState;
static int maiFfplayPeepEventsWithStatus(struct VideoState* state,
                                        SDL_Event* events, int count, int action,
                                        Uint32 min_type, Uint32 max_type);

static void (*maiFfplaySignal(int signal_number, void (*handler)(int)))(int)
{
    (void)signal_number;
    (void)handler;
    return SIG_DFL;
}

static void maiFfplayQuit(void)
{
    sDidCleanUp = 1;
    SDL_Quit();
}

static MAI_FFPLAY_NORETURN void maiFfplayExit(int status)
{
    if (sExitReady) longjmp(sExitPoint, status + 1);
    abort();
}

#define main maiFfplayCliMain
#define exit maiFfplayExit
#define signal maiFfplaySignal
#define SDL_Quit maiFfplayQuit
// ffplay.c has one SDL_PeepEvents call, inside its event loop. Sample its
// existing master clock there without changing the upstream playback logic.
#define SDL_PeepEvents(events, count, action, min_type, max_type) \
    maiFfplayPeepEventsWithStatus(is, events, count, action, min_type, max_type)
#include "../ffplay.c"
#undef SDL_PeepEvents
#undef SDL_Quit
#undef signal
#undef exit
#undef main

static int maiFfplayPeepEventsWithStatus(VideoState* state,
                                        SDL_Event* events, int count, int action,
                                        Uint32 min_type, Uint32 max_type)
{
    if (state && state->ic) {
        const int64_t length = state->ic->duration;
        const double clock = get_master_clock(state);
        const double start = state->ic->start_time == AV_NOPTS_VALUE ? 0.0 :
                             state->ic->start_time / (double)AV_TIME_BASE;
        if (length > 0 && isfinite(clock)) {
            const double relative = fmax(0.0, clock - start);
            MAI_STATUS_STORE64(sPositionUs,
                relative >= length / (double)AV_TIME_BASE ? length :
                (int64_t)(relative * AV_TIME_BASE));
        }
        MAI_STATUS_STORE64(sDurationUs, length > 0 ? length : 0);
        MAI_STATUS_STORE(sPaused, state->paused ? 1 : 0);
    }
    return SDL_PeepEvents(events, count, action, min_type, max_type);
}

int maiFfplayGetPlaybackStatus(MaiFfplayPlaybackStatus* status)
{
    if (!status) return 0;
    status->running = MAI_STATUS_LOAD(sPlaybackActive);
    status->position_us = MAI_STATUS_LOAD64(sPositionUs);
    status->duration_us = MAI_STATUS_LOAD64(sDurationUs);
    status->paused = MAI_STATUS_LOAD(sPaused);
    return status->running;
}

static void maiFfplayLogCallback(void* context, int level,
                                 const char* format, va_list arguments)
{
    if (sDiagnosticSink) {
        char line[1024];
        int print_prefix = 1;
        va_list copied;
        va_copy(copied, arguments);
        av_log_format_line2(context, level, format, copied,
                            line, sizeof(line), &print_prefix);
        va_end(copied);
        sDiagnosticSink(sDiagnosticUserData, level, line);
    }
    av_log_default_callback(context, level, format, arguments);
}

void maiFfplaySetDiagnosticSink(void (*sink)(void*, int, const char*), void* user_data)
{
    sDiagnosticUserData = user_data;
    sDiagnosticSink = sink;
    av_log_set_callback(sink ? maiFfplayLogCallback : av_log_default_callback);
}

static void resetFfplayOptions(void)
{
    file_iformat = NULL;
    input_filename = NULL;
    window_title = NULL;
    default_width = 640;
    default_height = 480;
    screen_width = screen_height = 0;
    screen_left = SDL_WINDOWPOS_CENTERED;
    screen_top = SDL_WINDOWPOS_CENTERED;
    audio_disable = video_disable = subtitle_disable = 0;
    for (int index = 0; index < AVMEDIA_TYPE_NB; ++index)
        wanted_stream_spec[index] = NULL;
    seek_by_bytes = -1;
    seek_interval = 10;
    display_disable = borderless = alwaysontop = 0;
    startup_volume = 100;
    show_status = -1;
    av_sync_type = AV_SYNC_AUDIO_MASTER;
    start_time = duration = AV_NOPTS_VALUE;
    fast = genpts = lowres = 0;
    decoder_reorder_pts = -1;
    autoexit = exit_on_keydown = exit_on_mousedown = 0;
    loop = 1;
    framedrop = infinite_buffer = -1;
    show_mode = SHOW_MODE_NONE;
    audio_codec_name = subtitle_codec_name = video_codec_name = NULL;
    rdftspeed = 0.02;
    cursor_last_shown = cursor_hidden = 0;
    vfilters_list = NULL;
    nb_vfilters = 0;
    afilters = NULL;
    autorotate = find_stream_info = 1;
    filter_nbthreads = enable_vulkan = 0;
    vulkan_params = video_background = NULL;
    hwaccel = NULL;
    is_full_screen = 0;
    audio_callback_time = 0;
    window = NULL;
    renderer = NULL;
    renderer_info = (SDL_RendererInfo){0};
    audio_dev = 0;
    vk_renderer = NULL;
}

int maiFfplayRun(const MaiFfplayHost* host, int argc, char** argv)
{
    if (!host || !host->graphics_view_id || argc < 2 || !argv) return -1;
#if defined(_WIN32)
    if (InterlockedCompareExchange(&sRunning, 1, 0) != 0) return -1;
#else
    if (atomic_flag_test_and_set(&sRunning)) return -1;
#endif
    resetFfplayOptions();
    MAI_STATUS_STORE64(sPositionUs, 0);
    MAI_STATUS_STORE64(sDurationUs, 0);
    MAI_STATUS_STORE(sPaused, 0);
    MAI_STATUS_STORE(sPlaybackActive, 1);
    maiFfplayBindHost(host);
    sExitReady = 1;
    sDidCleanUp = 0;
    const int jumped = setjmp(sExitPoint);
    int result = jumped ? jumped - 1 : maiFfplayCliMain(argc, argv);
    sExitReady = 0;
    if (!sDidCleanUp) {
        uninit_opts();
        av_freep(&input_filename);
        avformat_network_deinit();
        SDL_Quit();
    }
    maiFfplayBindHost(NULL);
    MAI_STATUS_STORE(sPlaybackActive, 0);
#if defined(_WIN32)
    InterlockedExchange(&sRunning, 0);
#else
    atomic_flag_clear(&sRunning);
#endif
    return result;
}

void maiFfplayRequestQuit(void)
{
    SDL_Event event = {0};
    event.type = SDL_USEREVENT + 2;
    SDL_PushEvent(&event);
}

int maiFfplaySendCommand(const char* command)
{
    if (!command) return -1;
    if (!strcmp(command, "close")) {
        SDL_Event event = {0};
        event.type = SDL_USEREVENT + 2;
        return SDL_PushEvent(&event) > 0 ? 1 : 0;
    }
    struct { const char* command; int key; } keys[] = {
        {"pause", SDLK_p}, {"step", SDLK_s},
        {"seek_forward", SDLK_RIGHT}, {"seek_backward", SDLK_LEFT},
        {"seek_minute_forward", SDLK_UP}, {"seek_minute_backward", SDLK_DOWN},
        {"next_audio", SDLK_a}, {"next_video", SDLK_v},
        {"next_subtitle", SDLK_t}, {"next_program", SDLK_c},
        {"next_filter", SDLK_w}, {"mute", SDLK_m},
        {"volume_up", SDLK_0}, {"volume_down", SDLK_9},
        {"fullscreen", SDLK_f},
    };
    for (size_t index = 0; index < sizeof(keys) / sizeof(keys[0]); ++index) {
        if (strcmp(command, keys[index].command)) continue;
        SDL_Event event = {0};
        event.type = SDL_KEYDOWN;
        event.key.keysym.sym = keys[index].key;
        return SDL_PushEvent(&event) > 0 ? 1 : 0;
    }
    return -1;
}

int maiFfplaySeekPercent(double fraction)
{
    if (!(fraction >= 0.0 && fraction <= 1.0)) return -1;
    const int width = maiFfplayWindowWidth();
    if (width < 1) return 0;
    SDL_Event event = {0};
    event.type = SDL_MOUSEBUTTONDOWN;
    event.button.button = SDL_BUTTON_RIGHT;
    event.button.x = (int)(fraction * width);
    return SDL_PushEvent(&event) > 0 ? 1 : 0;
}
