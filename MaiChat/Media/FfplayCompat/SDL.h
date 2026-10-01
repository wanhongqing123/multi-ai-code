#pragma once

// Compatibility declarations for FFmpeg's unmodified ffplay.c. This is not
// SDL: implementations below route events to MaiChat commands, video to
// Graphics, and audio to native platform devices.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef int16_t Sint16;
typedef struct SDL_mutex SDL_mutex;
typedef struct SDL_cond SDL_cond;
typedef struct SDL_Thread SDL_Thread;
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;
typedef Uint32 SDL_AudioDeviceID;
typedef int SDL_BlendMode;
typedef int SDL_YUV_CONVERSION_MODE;

typedef struct SDL_Rect { int x, y, w, h; } SDL_Rect;
typedef struct SDL_RendererInfo {
    const char* name;
    Uint32 flags;
    Uint32 num_texture_formats;
    Uint32 texture_formats[16];
    int max_texture_width;
    int max_texture_height;
} SDL_RendererInfo;
typedef struct SDL_AudioSpec {
    int freq;
    Uint16 format;
    Uint8 channels;
    Uint8 silence;
    Uint16 samples;
    Uint16 padding;
    Uint32 size;
    void (*callback)(void* userdata, Uint8* stream, int length);
    void* userdata;
} SDL_AudioSpec;
typedef struct SDL_Keysym { int sym; } SDL_Keysym;
typedef union SDL_Event {
    Uint32 type;
    struct { Uint32 type; SDL_Keysym keysym; } key;
    struct { Uint32 type; Uint8 button; int x, y; } button;
    struct { Uint32 type; Uint32 state; int x, y; } motion;
    struct { Uint32 type; Uint8 event; int data1, data2; } window;
    struct { Uint32 type; int code; void* data1; void* data2; } user;
} SDL_Event;

#define SDL_VERSION_ATLEAST(major, minor, patch) 1
#define SDL_INIT_TIMER 0x1u
#define SDL_INIT_AUDIO 0x10u
#define SDL_INIT_VIDEO 0x20u
#define SDL_INIT_EVENTS 0x4000u
#define SDL_AUDIO_ALLOW_FREQUENCY_CHANGE 0x1u
#define SDL_AUDIO_ALLOW_CHANNELS_CHANGE 0x4u
#define AUDIO_S16SYS 0x8010u
#define SDL_MIX_MAXVOLUME 128
#define SDL_BLENDMODE_NONE 0
#define SDL_BLENDMODE_BLEND 1
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_RENDERER_ACCELERATED 0x2u
#define SDL_RENDERER_PRESENTVSYNC 0x4u
#define SDL_FLIP_VERTICAL 0x2u
#define SDL_YUV_CONVERSION_AUTOMATIC 0
#define SDL_YUV_CONVERSION_JPEG 1
#define SDL_YUV_CONVERSION_BT601 2
#define SDL_YUV_CONVERSION_BT709 3
#define SDL_WINDOW_HIDDEN 0x1u
#define SDL_WINDOW_RESIZABLE 0x2u
#define SDL_WINDOW_BORDERLESS 0x4u
#define SDL_WINDOW_ALWAYS_ON_TOP 0x8u
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x10u
#define SDL_WINDOW_VULKAN 0x20u
#define SDL_WINDOWPOS_CENTERED 0
#define SDL_WINDOWPOS_UNDEFINED 0
#define SDL_QUIT 0x100u
#define SDL_WINDOWEVENT 0x200u
#define SDL_WINDOWEVENT_EXPOSED 1
#define SDL_WINDOWEVENT_SIZE_CHANGED 2
#define SDL_KEYDOWN 0x300u
#define SDL_MOUSEMOTION 0x400u
#define SDL_MOUSEBUTTONDOWN 0x401u
#define SDL_SYSWMEVENT 0x500u
#define SDL_USEREVENT 0x8000u
#define SDL_FIRSTEVENT 0u
#define SDL_LASTEVENT 0xffffu
#define SDL_GETEVENT 2
#define SDL_IGNORE 0
#define SDL_BUTTON_LEFT 1
#define SDL_BUTTON_RIGHT 3
#define SDL_BUTTON_RMASK 4u
#define SDLK_ESCAPE 27
#define SDLK_SPACE 32
#define SDLK_0 '0'
#define SDLK_9 '9'
#define SDLK_a 'a'
#define SDLK_c 'c'
#define SDLK_f 'f'
#define SDLK_m 'm'
#define SDLK_p 'p'
#define SDLK_q 'q'
#define SDLK_s 's'
#define SDLK_t 't'
#define SDLK_v 'v'
#define SDLK_w 'w'
#define SDLK_LEFT 0x1001
#define SDLK_RIGHT 0x1002
#define SDLK_UP 0x1003
#define SDLK_DOWN 0x1004
#define SDLK_PAGEUP 0x1005
#define SDLK_PAGEDOWN 0x1006
#define SDLK_KP_DIVIDE 0x1007
#define SDLK_KP_MULTIPLY 0x1008
#define SDL_HINT_RENDER_SCALE_QUALITY "render_scale_quality"
#define SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR "video_x11_compositor"

#define SDL_PIXELFORMAT_UNKNOWN 0u
#define SDL_PIXELFORMAT_RGB332 1u
#define SDL_PIXELFORMAT_RGB444 2u
#define SDL_PIXELFORMAT_RGB555 3u
#define SDL_PIXELFORMAT_BGR555 4u
#define SDL_PIXELFORMAT_RGB565 5u
#define SDL_PIXELFORMAT_BGR565 6u
#define SDL_PIXELFORMAT_RGB24 7u
#define SDL_PIXELFORMAT_BGR24 8u
#define SDL_PIXELFORMAT_RGB888 9u
#define SDL_PIXELFORMAT_BGR888 10u
#define SDL_PIXELFORMAT_RGBX8888 11u
#define SDL_PIXELFORMAT_BGRX8888 12u
#define SDL_PIXELFORMAT_ARGB8888 13u
#define SDL_PIXELFORMAT_ABGR8888 14u
#define SDL_PIXELFORMAT_RGBA8888 15u
#define SDL_PIXELFORMAT_BGRA8888 16u
#define SDL_PIXELFORMAT_IYUV 17u
#define SDL_PIXELFORMAT_YUY2 18u
#define SDL_PIXELFORMAT_UYVY 19u

int SDL_Init(Uint32 flags);
void SDL_Quit(void);
const char* SDL_GetError(void);
void SDL_Delay(Uint32 milliseconds);
const char* SDL_getenv(const char* name);
int SDL_setenv(const char* name, const char* value, int overwrite);
int SDL_SetHint(const char* name, const char* value);
SDL_mutex* SDL_CreateMutex(void);
void SDL_DestroyMutex(SDL_mutex* mutex);
int SDL_LockMutex(SDL_mutex* mutex);
int SDL_UnlockMutex(SDL_mutex* mutex);
SDL_cond* SDL_CreateCond(void);
void SDL_DestroyCond(SDL_cond* condition);
int SDL_CondSignal(SDL_cond* condition);
int SDL_CondWait(SDL_cond* condition, SDL_mutex* mutex);
int SDL_CondWaitTimeout(SDL_cond* condition, SDL_mutex* mutex, Uint32 milliseconds);
SDL_Thread* SDL_CreateThread(int (*function)(void*), const char* name, void* data);
void SDL_WaitThread(SDL_Thread* thread, int* status);
Uint8 SDL_EventState(Uint32 type, int state);
void SDL_PumpEvents(void);
int SDL_PeepEvents(SDL_Event* events, int count, int action, Uint32 min_type, Uint32 max_type);
int SDL_PushEvent(SDL_Event* event);
SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int width, int height, Uint32 flags);
void SDL_DestroyWindow(SDL_Window* window);
void SDL_SetWindowTitle(SDL_Window* window, const char* title);
void SDL_SetWindowSize(SDL_Window* window, int width, int height);
void SDL_SetWindowPosition(SDL_Window* window, int x, int y);
int SDL_SetWindowFullscreen(SDL_Window* window, Uint32 flags);
void SDL_ShowWindow(SDL_Window* window);
int SDL_ShowCursor(int toggle);
SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, int index, Uint32 flags);
void SDL_DestroyRenderer(SDL_Renderer* renderer);
int SDL_GetRendererInfo(SDL_Renderer* renderer, SDL_RendererInfo* info);
int SDL_SetRenderDrawColor(SDL_Renderer* renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
int SDL_RenderClear(SDL_Renderer* renderer);
int SDL_RenderFillRect(SDL_Renderer* renderer, const SDL_Rect* rectangle);
int SDL_RenderCopy(SDL_Renderer* renderer, SDL_Texture* texture,
                   const SDL_Rect* source, const SDL_Rect* target);
int SDL_RenderCopyEx(SDL_Renderer* renderer, SDL_Texture* texture,
                     const SDL_Rect* source, const SDL_Rect* target,
                     double angle, const void* center, int flip);
void SDL_RenderPresent(SDL_Renderer* renderer);
SDL_Texture* SDL_CreateTexture(SDL_Renderer* renderer, Uint32 format,
                                int access, int width, int height);
void SDL_DestroyTexture(SDL_Texture* texture);
int SDL_QueryTexture(SDL_Texture* texture, Uint32* format, int* access,
                     int* width, int* height);
int SDL_SetTextureBlendMode(SDL_Texture* texture, SDL_BlendMode blend);
int SDL_GetTextureBlendMode(SDL_Texture* texture, SDL_BlendMode* blend);
int SDL_LockTexture(SDL_Texture* texture, const SDL_Rect* rect, void** pixels, int* pitch);
void SDL_UnlockTexture(SDL_Texture* texture);
int SDL_UpdateTexture(SDL_Texture* texture, const SDL_Rect* rect,
                      const void* pixels, int pitch);
int SDL_UpdateYUVTexture(SDL_Texture* texture, const SDL_Rect* rect,
                         const Uint8* y, int y_pitch, const Uint8* u, int u_pitch,
                         const Uint8* v, int v_pitch);
const char* SDL_GetPixelFormatName(Uint32 format);
void SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_MODE mode);
SDL_AudioDeviceID SDL_OpenAudioDevice(const char* device, int capture,
                                       const SDL_AudioSpec* wanted, SDL_AudioSpec* obtained,
                                       int allowed_changes);
void SDL_CloseAudioDevice(SDL_AudioDeviceID device);
void SDL_PauseAudioDevice(SDL_AudioDeviceID device, int pause);
void SDL_MixAudioFormat(Uint8* destination, const Uint8* source,
                        Uint16 format, Uint32 length, int volume);

#ifdef __cplusplus
}
#endif
