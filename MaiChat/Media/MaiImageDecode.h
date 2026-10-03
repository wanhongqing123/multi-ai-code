#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Decode the first image frame with FFmpeg into tightly packed RGBA8. A positive
// maximum bounds the output size while preserving aspect ratio; zero keeps the
// source dimension. The caller owns rgba and releases it with maiImageDecodeFree.
// icc_profile, when present, points inside the same rgba allocation and remains
// valid until that single maiImageDecodeFree call. color_primaries and
// color_transfer use the FFmpeg AVColorPrimaries/AVColorTransferCharacteristic
// integer values; zero means unspecified.
// This function does not create a Graphics surface or touch a UI thread.
typedef struct MaiImageDecodeResult {
    unsigned char *rgba;
    int width;
    int height;
    int stride;
    int error_code;
    const unsigned char *icc_profile;
    int icc_size;
    int color_primaries;
    int color_transfer;
} MaiImageDecodeResult;

MaiImageDecodeResult maiImageDecodeFile(const char *path, int max_width, int max_height);
void maiImageDecodeFree(void *pixels);

#ifdef __cplusplus
}
#endif
