#include <array>
#include <cstdint>
#include <cstdio>

#include "MaiRvmComposite.h"

int main() {
    constexpr std::array<std::uint8_t, 9> source = {10, 20, 30, 200, 200, 200, 100, 120, 140};
    constexpr std::array<std::uint8_t, 9> background = {255, 0, 0, 0, 255, 0, 0, 0, 255};
    constexpr std::array<float, 9> foreground = {0.8f, 1.0f, 1.0f, 0.7f, 0.0f,
                                                 0.0f, 0.6f, 0.0f, 0.0f};
    constexpr std::array<float, 3> alpha = {1.0f, 0.0f, 0.5f};
    std::array<std::uint8_t, 9> output{};
    maiRvmCompositeRgb8(source.data(), foreground.data(), alpha.data(), background.data(),
                        output.data(), alpha.size());
    if (output[0] != 10 || output[1] != 20 || output[2] != 30 || output[3] != 0 ||
        output[4] != 255 || output[5] != 0 || output[6] < 120 || output[8] < 100) {
        std::printf("RVM compositing changed opaque source or transparent background\n");
        return 1;
    }
    return 0;
}
