#include <stdint.h>
#include <stdio.h>

#include "MaiGraphics.h"
#include "vec4.h"

static bool surface_matches_color(gs_stagesurf_t* surface, uint8_t red, uint8_t green, uint8_t blue,
                                  uint8_t alpha) {
    uint8_t* pixels = NULL;
    uint32_t stride = 0;
    if (!gs_stagesurface_map(surface, &pixels, &stride) || !pixels || stride < 8) return false;

    bool matches = true;
    for (uint32_t row = 0; row < 2; ++row) {
        for (uint32_t column = 0; column < 2; ++column) {
            const uint8_t* pixel = pixels + row * stride + column * 4;
            if (pixel[0] != red || pixel[1] != green || pixel[2] != blue || pixel[3] != alpha) {
                fprintf(stderr, "pixel %u,%u: expected %u,%u,%u,%u; got %u,%u,%u,%u\n", row, column,
                        red, green, blue, alpha, pixel[0], pixel[1], pixel[2], pixel[3]);
                matches = false;
            }
        }
    }
    gs_stagesurface_unmap(surface);
    return matches;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;

    graphics_t* graphics = NULL;
    const int create_result = gs_create(&graphics, argv[1], 0);
    if (create_result == GS_ERROR_NOT_SUPPORTED) return 77;
    if (create_result != GS_SUCCESS || !graphics) {
        fprintf(stderr, "Metal device creation failed: %d\n", create_result);
        return 1;
    }

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

        passed = surface_matches_color(surface, 255, 0, 0, 255);
        gs_set_render_target(NULL, NULL);

        uint8_t image[16];
        for (size_t pixel = 0; pixel < 4; ++pixel) {
            image[pixel * 4] = 0;
            image[pixel * 4 + 1] = 255;
            image[pixel * 4 + 2] = 0;
            image[pixel * 4 + 3] = 255;
        }
        const uint8_t* image_data[] = {image};
        gs_texture_t* uploaded = gs_texture_create(2, 2, GS_RGBA, 1, image_data, 0);
        if (!uploaded) {
            passed = false;
        } else {
            gs_copy_texture(texture, uploaded);
            gs_flush();
            gs_stage_texture(surface, texture);
            gs_flush();
            passed = surface_matches_color(surface, 0, 255, 0, 255) && passed;
            gs_texture_destroy(uploaded);
        }
    }

    if (surface) gs_stagesurface_destroy(surface);
    if (texture) gs_texture_destroy(texture);
    gs_leave_context();
    gs_destroy(graphics);

    if (!passed) fputs("Metal render target readback or texture upload failed\n", stderr);
    return passed ? 0 : 1;
}
