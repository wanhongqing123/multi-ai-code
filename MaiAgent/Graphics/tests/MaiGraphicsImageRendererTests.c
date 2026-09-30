#include <stdio.h>

#include "MaiGraphicsImageRenderer.h"

int main(int argc, char** argv) {
    if (argc != 3) return 2;

    for (int attempt = 0; attempt < 2; ++attempt) {
        MaiGraphicsImageResult image = {0};
        if (!maiGraphicsRenderImageFile(argv[2], argv[1], 1, 1, &image)) {
            fputs("FFmpeg decode or Graphics render failed\n", stderr);
            return 1;
        }
        const bool valid = image.width == 1 && image.height == 1 && image.stride >= 4 &&
                           image.pixels && image.pixels[0] == 255 && image.pixels[1] == 0 &&
                           image.pixels[2] == 0 && image.pixels[3] == 255;
        maiGraphicsImageResultFree(&image);
        if (!valid) {
            fputs("Rendered RGBA pixel differs from the source image\n", stderr);
            return 1;
        }
    }
    return 0;
}
