#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// RGBA8 image returned by the shared FFmpeg -> OBS Graphics rendering path.
// pixels belongs to the caller and must be released with
// maiGraphicsImageResultFree. stride is at least width * 4. The result is
// cleared on failure; the function is synchronous and should run off the UI
// thread. A graphics device is reused on that physical thread and destroyed
// when the thread exits. Calls from distinct threads use distinct devices.
typedef struct MaiGraphicsImageResult {
    uint8_t* pixels;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
} MaiGraphicsImageResult;

// backend is a full module path on macOS/Android and "builtin:metal" on iOS.
// max_width and max_height must both be positive; the aspect ratio is kept.
// The image is never enlarged. Returns false for unsupported formats,
// decoding errors, GPU errors, or allocation failures.
bool maiGraphicsRenderImageFile(const char* file_path, const char* backend, uint32_t max_width,
                                uint32_t max_height, MaiGraphicsImageResult* result);
void maiGraphicsImageResultFree(MaiGraphicsImageResult* result);

#ifdef __cplusplus
}
#endif
