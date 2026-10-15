// 文件作用：声明通过 SoundTouch 调整音频节奏，使倍速播放保持原音调并在跳转后清空旧样本所需的接口和数据结构。
// 调用边界：平台实现与上层调用方通过本文件约定参数、所有权和释放方式。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Seek 时丢弃 SoundTouch 中尚未输出的旧位置音频。
void maiFfplayAudioTempoNotifySeek(void);

#ifdef __cplusplus
}

#include <SoundTouch.h>

#include <vector>

#include "SDL.h"

// FFplay 仍负责解码 PCM；SoundTouch 只改变样本输出速度，不改变音高。
// 每个音频设备独占一个实例，由该设备的输出线程调用 fill。
class MaiFfplayAudioTempo {
public:
  MaiFfplayAudioTempo(const SDL_AudioSpec &spec, int speedPercent);

  void fill(Uint8 *output, int length);

private:
  soundtouch::SoundTouch mProcessor;
  void (*mInputCallback)(void *, Uint8 *, int);
  void *mInputUserData;
  std::vector<Sint16> mInput;
  int mChannels;
  uint64_t mSeekGeneration;
};
#endif
