#include <cmath>
#include <cstdio>

#include "MaiMobileAgent.h"

static int failures = 0;
#define CHECK(condition)                                                 \
    do {                                                                 \
        if (!(condition)) {                                              \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

int main() {
    const unsigned char pixels[] = {255, 0, 0, 255, 0, 255, 0, 255};
    MaiImageFilterResult gray =
        maiImageFilterRgba(pixels, 2, 1, 8, R"({"operation":"grayscale"})");
    CHECK(gray.error == nullptr);
    CHECK(gray.rgba != nullptr);
    CHECK(gray.width == 2 && gray.height == 1);
    if (gray.rgba != nullptr) {
        CHECK(std::abs(static_cast<int>(gray.rgba[0]) - gray.rgba[1]) <= 2);
        CHECK(std::abs(static_cast<int>(gray.rgba[1]) - gray.rgba[2]) <= 2);
        CHECK(gray.rgba[3] == 255);
    }
    maiImageFilterFree(gray.rgba);
    maiImageFilterFree(gray.error);

    MaiImageFilterResult rotated =
        maiImageFilterRgba(pixels, 2, 1, 8, R"({"operation":"rotate","degrees":90})");
    CHECK(rotated.error == nullptr);
    CHECK(rotated.rgba != nullptr);
    CHECK(rotated.width == 1 && rotated.height == 2);
    maiImageFilterFree(rotated.rgba);
    maiImageFilterFree(rotated.error);

    MaiImageFilterResult invalid =
        maiImageFilterRgba(pixels, 2, 1, 8, R"({"operation":"unknown"})");
    CHECK(invalid.rgba == nullptr);
    CHECK(invalid.error != nullptr);
    maiImageFilterFree(invalid.rgba);
    maiImageFilterFree(invalid.error);

    if (failures == 0) std::printf("desktop FFmpeg filter test passed\n");
    return failures == 0 ? 0 : 1;
}
