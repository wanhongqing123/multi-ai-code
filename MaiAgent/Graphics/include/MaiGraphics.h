#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Experimental Agent Graphics Subsystem C interface. Resources are opaque and owned by the
// graphics device that created them. Every function below must be called from the physical
// thread that called ag_create; wrong-thread calls fail without touching graphics state.
// Hosts should marshal calls through MaiGraphicsTaskRunner. No function retains caller buffers.
// The software backend is an offscreen reference renderer; GPU backends and presentation are
// separate future implementations of the same interface.
typedef struct ag_graphics ag_graphics_t;
typedef struct ag_texture ag_texture_t;

typedef enum ag_backend {
    AG_BACKEND_SOFTWARE = 0,
    AG_BACKEND_METAL = 1,
    AG_BACKEND_D3D11 = 2,
    AG_BACKEND_OPENGL_ES = 3,
} ag_backend;

typedef enum ag_result {
    AG_SUCCESS = 0,
    AG_ERROR_INVALID_ARGUMENT = -1,
    AG_ERROR_WRONG_THREAD = -2,
    AG_ERROR_INVALID_STATE = -3,
    AG_ERROR_NOT_SUPPORTED = -4,
    AG_ERROR_OUT_OF_MEMORY = -5,
} ag_result;

typedef enum ag_color_format { AG_COLOR_RGBA8 = 1 } ag_color_format;

typedef struct ag_color {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t alpha;
} ag_color;

typedef struct ag_rect {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} ag_rect;

// Creates a device and transparent offscreen target. Only AG_BACKEND_SOFTWARE is currently
// implemented; other backend choices return NOT_SUPPORTED. Dimensions are limited to 16384
// per edge and 64 million pixels. On failure *graphics is null.
ag_result ag_create(ag_graphics_t** graphics, ag_backend backend, uint32_t width, uint32_t height);

// Returns INVALID_STATE while textures owned by the device remain alive or a frame is active.
// On success the handle is invalidated; callers must not use it afterward.
ag_result ag_destroy(ag_graphics_t* graphics);
ag_result ag_resize(ag_graphics_t* graphics, uint32_t width, uint32_t height);

// Texture dimensions have the same limits as the target. Destroy textures before ag_destroy.
ag_result ag_texture_create(ag_graphics_t* graphics, uint32_t width, uint32_t height,
                            ag_color_format format, ag_texture_t** texture);
ag_result ag_texture_destroy(ag_texture_t* texture);

// Copies RGBA8 rows into the texture. pixels must remain readable for height rows, each with
// at least width*4 bytes. stride is the source byte distance between row starts.
ag_result ag_texture_set_image(ag_texture_t* texture, const uint8_t* pixels, size_t stride);

// Frame operations must be balanced. Draw uses nearest-neighbor sampling and straight-alpha
// source-over blending. opacity is finite and in [0,1]. A texture must belong to this device.
ag_result ag_begin_frame(ag_graphics_t* graphics);
ag_result ag_clear(ag_graphics_t* graphics, ag_color color);
ag_result ag_draw_sprite(ag_graphics_t* graphics, const ag_texture_t* texture, ag_rect destination,
                         float opacity);
ag_result ag_end_frame(ag_graphics_t* graphics);

// Copies the completed target into RGBA8 output rows. out_bytes must cover stride*height bytes;
// stride must be at least width*4. Readback is unavailable while a frame is active.
ag_result ag_readback(ag_graphics_t* graphics, uint8_t* pixels, size_t stride, size_t out_bytes);

#ifdef __cplusplus
}
#endif
