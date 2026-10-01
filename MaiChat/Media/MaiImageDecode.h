#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Decode the first image frame with FFmpeg into tightly packed RGBA8. A positive
// maximum bounds the output size while preserving aspect ratio; zero keeps the
// source dimension. The caller owns rgba and releases it with maiImageDecodeFree.
// This function does not create a Graphics surface or touch a UI thread.
typedef struct MaiImageDecodeResult {
    unsigned char* rgba;
    int width;
    int height;
    int stride;
    int error_code;
} MaiImageDecodeResult;

MaiImageDecodeResult maiImageDecodeFile(const char* path, int max_width, int max_height);
void maiImageDecodeFree(void* pixels);

#ifdef __cplusplus
}
#endif
