#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Experimental Agent Graphics Subsystem C interface. Resources are opaque and owned by the
// graphics device that created them. Every function below must be called from the physical
// thread that called ags_create; wrong-thread calls fail without touching graphics state.
// Hosts should marshal calls through MaiGraphicsTaskRunner. No function retains caller buffers.
// The software backend is an offscreen reference renderer; GPU backends and presentation are
// separate future implementations of the same interface.
typedef struct ags_graphics ags_graphics_t;
typedef struct ags_texture ags_texture_t;

typedef enum ags_backend {
    AGS_BACKEND_SOFTWARE = 0,
    AGS_BACKEND_METAL = 1,
    AGS_BACKEND_D3D11 = 2,
    AGS_BACKEND_OPENGL_ES = 3,
} ags_backend;

typedef enum ags_result {
    AGS_SUCCESS = 0,
    AGS_ERROR_INVALID_ARGUMENT = -1,
    AGS_ERROR_WRONG_THREAD = -2,
    AGS_ERROR_INVALID_STATE = -3,
    AGS_ERROR_NOT_SUPPORTED = -4,
    AGS_ERROR_OUT_OF_MEMORY = -5,
} ags_result;

typedef enum ags_color_format { AGS_COLOR_RGBA8 = 1 } ags_color_format;

typedef struct ags_color {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t alpha;
} ags_color;

typedef struct ags_rect {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} ags_rect;

// Creates a device and transparent offscreen target. Only AGS_BACKEND_SOFTWARE is currently
// implemented; other backend choices return NOT_SUPPORTED. Dimensions are limited to 16384
// per edge and 64 million pixels. On failure *graphics is null.
ags_result ags_create(ags_graphics_t** graphics, ags_backend backend, uint32_t width,
                      uint32_t height);

// Returns INVALID_STATE while textures owned by the device remain alive or a frame is active.
// On success the handle is invalidated; callers must not use it afterward.
ags_result ags_destroy(ags_graphics_t* graphics);
ags_result ags_resize(ags_graphics_t* graphics, uint32_t width, uint32_t height);

// Texture dimensions have the same limits as the target. Destroy textures before ags_destroy.
ags_result ags_texture_create(ags_graphics_t* graphics, uint32_t width, uint32_t height,
                              ags_color_format format, ags_texture_t** texture);
ags_result ags_texture_destroy(ags_texture_t* texture);

// Copies RGBA8 rows into the texture. pixels must remain readable for height rows, each with
// at least width*4 bytes. stride is the source byte distance between row starts.
ags_result ags_texture_set_image(ags_texture_t* texture, const uint8_t* pixels, size_t stride);

// Frame operations must be balanced. Draw uses nearest-neighbor sampling and straight-alpha
// source-over blending. opacity is finite and in [0,1]. A texture must belong to this device.
ags_result ags_begin_frame(ags_graphics_t* graphics);
ags_result ags_clear(ags_graphics_t* graphics, ags_color color);
ags_result ags_draw_sprite(ags_graphics_t* graphics, const ags_texture_t* texture,
                           ags_rect destination, float opacity);
ags_result ags_end_frame(ags_graphics_t* graphics);

// Copies the completed target into RGBA8 output rows. out_bytes must cover stride*height bytes;
// stride must be at least width*4. Readback is unavailable while a frame is active.
ags_result ags_readback(ags_graphics_t* graphics, uint8_t* pixels, size_t stride, size_t out_bytes);

#ifdef __cplusplus
}
#endif
