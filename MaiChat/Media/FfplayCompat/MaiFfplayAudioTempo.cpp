#include "MaiFfplayAudioTempo.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace {

constexpr int kInputFrames = 2048;
constexpr int kMaximumInputBatches = 64;
std::atomic<uint64_t> sSeekGeneration{0};

} // namespace

extern "C" void maiFfplayAudioTempoNotifySeek(void) {
  sSeekGeneration.fetch_add(1, std::memory_order_relaxed);
}

MaiFfplayAudioTempo::MaiFfplayAudioTempo(const SDL_AudioSpec &spec,
                                         int speedPercent)
    : mInputCallback(spec.callback), mInputUserData(spec.userdata),
      mInput(kInputFrames * spec.channels), mChannels(spec.channels),
      mSeekGeneration(sSeekGeneration.load(std::memory_order_relaxed)) {
  static_assert(sizeof(soundtouch::SAMPLETYPE) == sizeof(Sint16));
  mProcessor.setSampleRate(spec.freq);
  mProcessor.setChannels(spec.channels);
  mProcessor.setTempo(speedPercent / 100.0);
}

void MaiFfplayAudioTempo::fill(Uint8 *output, int length) {
  if (!output || length <= 0 || mChannels < 1)
    return;
  const uint64_t generation = sSeekGeneration.load(std::memory_order_relaxed);
  if (mSeekGeneration != generation) {
    mProcessor.clear();
    mSeekGeneration = generation;
  }
  const int frameBytes = mChannels * static_cast<int>(sizeof(Sint16));
  const int outputFrames = length / frameBytes;
  auto *samples = reinterpret_cast<Sint16 *>(output);
  int produced = 0;
  for (int batches = 0;
       produced < outputFrames && batches < kMaximumInputBatches; ++batches) {
    const uint available = mProcessor.numSamples();
    if (available > 0) {
      const uint received = mProcessor.receiveSamples(
          samples + produced * mChannels,
          std::min<uint>(available, outputFrames - produced));
      produced += static_cast<int>(received);
      continue;
    }
    mInputCallback(mInputUserData, reinterpret_cast<Uint8 *>(mInput.data()),
                   static_cast<int>(mInput.size() * sizeof(Sint16)));
    mProcessor.putSamples(mInput.data(), kInputFrames);
  }
  if (produced * frameBytes < length)
    std::memset(output + produced * frameBytes, 0,
                length - produced * frameBytes);
}
