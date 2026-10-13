#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "MaiFfplayAudioTempo.h"

namespace {

constexpr int kSampleRate = 44100;
constexpr int kFramesPerBuffer = 1024;
constexpr double kToneFrequency = 440.0;
constexpr double kPi = 3.14159265358979323846;

struct ToneInput {
  uint64_t consumedFrames = 0;
};

void fillTone(void *user, Uint8 *destination, int length) {
  auto *input = static_cast<ToneInput *>(user);
  auto *samples = reinterpret_cast<Sint16 *>(destination);
  const int frames = length / (2 * static_cast<int>(sizeof(Sint16)));
  for (int index = 0; index < frames; ++index) {
    const double phase =
        2.0 * kPi * kToneFrequency * input->consumedFrames / kSampleRate;
    const Sint16 sample = static_cast<Sint16>(std::sin(phase) * 12000);
    samples[index * 2] = sample;
    samples[index * 2 + 1] = sample;
    ++input->consumedFrames;
  }
}

bool verifyTempo(int speedPercent, double minimumInputRatio,
                 double maximumInputRatio) {
  ToneInput input;
  SDL_AudioSpec spec{};
  spec.freq = kSampleRate;
  spec.channels = 2;
  spec.samples = kFramesPerBuffer;
  spec.size = kFramesPerBuffer * spec.channels * sizeof(Sint16);
  spec.callback = fillTone;
  spec.userdata = &input;
  MaiFfplayAudioTempo tempo(spec, speedPercent);
  std::vector<Sint16> output(80 * kFramesPerBuffer * spec.channels);
  for (int buffer = 0; buffer < 80; ++buffer) {
    tempo.fill(reinterpret_cast<Uint8 *>(
                   output.data() + buffer * kFramesPerBuffer * spec.channels),
               spec.size);
  }
  const double inputRatio = input.consumedFrames / (80.0 * kFramesPerBuffer);
  if (inputRatio < minimumInputRatio || inputRatio > maximumInputRatio) {
    std::fprintf(stderr, "tempo %d%% consumed %.2fx input\n", speedPercent,
                 inputRatio);
    return false;
  }
  // 中间稳定区间统计正向过零次数，验证改变时长时仍保持 440 Hz 音高。
  int crossings = 0;
  int nonSilent = 0;
  const int first = 20 * kFramesPerBuffer;
  const int last = 55 * kFramesPerBuffer;
  for (int frame = first + 1; frame < last; ++frame) {
    const Sint16 previous = output[(frame - 1) * spec.channels];
    const Sint16 current = output[frame * spec.channels];
    if (previous <= 0 && current > 0)
      ++crossings;
    if (current != 0)
      ++nonSilent;
  }
  const double frequency =
      crossings * static_cast<double>(kSampleRate) / (last - first);
  if (nonSilent < (last - first) * 0.9 || frequency < 400 || frequency > 480) {
    std::fprintf(stderr, "tempo %d%% pitch %.1f Hz, non-silent frames %d\n",
                 speedPercent, frequency, nonSilent);
    return false;
  }
  return true;
}

} // namespace

int main() {
  return verifyTempo(50, 0.45, 0.85) && verifyTempo(200, 1.8, 2.4) ? 0 : 1;
}
