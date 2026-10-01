#include "MaiFfplayEntry.h"

#include <setjmp.h>
#include <signal.h>
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
#else
static atomic_flag sRunning = ATOMIC_FLAG_INIT;
#endif
static MAI_FFPLAY_THREAD_LOCAL jmp_buf sExitPoint;
static MAI_FFPLAY_THREAD_LOCAL int sExitReady;
static MAI_FFPLAY_THREAD_LOCAL int sDidCleanUp;

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
#include "../ffplay.c"
#undef SDL_Quit
#undef signal
#undef exit
#undef main

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
