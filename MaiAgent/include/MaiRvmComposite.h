#pragma once

#include <cstddef>
#include <cstdint>

// Composites one RGB8 frame using RVM's planar FP32 foreground and alpha outputs. Source,
// background, and destination are interleaved RGB8 and may share neither storage nor lifetime
// assumptions beyond this call. Fully opaque source pixels are copied unchanged. Only soft edge
// pixels use the model's foreground estimate to remove original-background spill. Fully clear
// pixels come from the replacement background. The caller must supply 3*pixelCount bytes for each
// RGB8 buffer, 3*pixelCount floats for foreground, and pixelCount floats for alpha.
void maiRvmCompositeRgb8(const std::uint8_t* source, const float* foreground, const float* alpha,
                         const std::uint8_t* background, std::uint8_t* destination,
                         std::size_t pixelCount);
