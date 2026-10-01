#include "SDL.h"

#include <algorithm>

extern "C" void SDL_MixAudioFormat(Uint8* destination, const Uint8* source,
                                   Uint16 format, Uint32 length, int volume) {
    if (!destination || !source || format != AUDIO_S16SYS) return;
    const auto* input = reinterpret_cast<const Sint16*>(source);
    auto* output = reinterpret_cast<Sint16*>(destination);
    const int gain = std::clamp(volume, 0, SDL_MIX_MAXVOLUME);
    for (Uint32 index = 0; index < length / sizeof(Sint16); ++index) {
        const int mixed = output[index] + (static_cast<int>(input[index]) * gain) /
                                              SDL_MIX_MAXVOLUME;
        output[index] = static_cast<Sint16>(std::clamp(mixed, -32768, 32767));
    }
}
