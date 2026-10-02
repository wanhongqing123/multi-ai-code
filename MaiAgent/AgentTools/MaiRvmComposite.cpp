#include "MaiRvmComposite.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace {

float bounded(float value) {
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}

}  // namespace

void maiRvmCompositeRgb8(const std::uint8_t* source, const float* foreground, const float* alpha,
                         const std::uint8_t* background, std::uint8_t* destination,
                         std::size_t pixelCount) {
    if (source == nullptr || foreground == nullptr || alpha == nullptr || background == nullptr ||
        destination == nullptr)
        return;
    for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
        const float opacity = bounded(alpha[pixel]);
        if (opacity >= 0.999f) {
            for (std::size_t channel = 0; channel < 3; ++channel)
                destination[pixel * 3 + channel] = source[pixel * 3 + channel];
            continue;
        }
        if (opacity <= 0.001f) {
            for (std::size_t channel = 0; channel < 3; ++channel)
                destination[pixel * 3 + channel] = background[pixel * 3 + channel];
            continue;
        }
        const float sourceWeight = bounded((opacity - 0.8f) / 0.2f);
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const float original = source[pixel * 3 + channel] / 255.0f;
            const float edge = bounded(foreground[channel * pixelCount + pixel]);
            const float person = edge * (1.0f - sourceWeight) + original * sourceWeight;
            const float backdrop = background[pixel * 3 + channel] / 255.0f;
            const float mixed = opacity * person + (1.0f - opacity) * backdrop;
            destination[pixel * 3 + channel] =
                static_cast<std::uint8_t>(std::lround(bounded(mixed) * 255.0f));
        }
    }
}
