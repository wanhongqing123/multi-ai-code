// 文件作用：把 ffplay 音频回调接到 Apple 音频输出，管理音频会话和播放设备。
// 调用边界：由 MaiChat 媒体或 Agent 宿主调用；平台资源在停止或失败时须正确释放。
#include "SDL.h"

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "MaiFfplayCompatInternal.h"
#include "MaiFfplayAudioTempo.h"
#include "MaiFfplayEntry.h"

namespace {

struct AudioDevice {
    AudioQueueRef queue = nullptr;
    SDL_AudioSpec spec{};
    AudioQueueBufferRef buffers[3]{};
    std::unique_ptr<MaiFfplayAudioTempo> tempo;
    std::atomic<bool> closing{false};
    bool started = false;
};

std::mutex sAudioMutex;
std::unordered_map<SDL_AudioDeviceID, std::unique_ptr<AudioDevice>> sAudioDevices;
std::atomic<SDL_AudioDeviceID> sNextAudioId{2};
std::atomic<uint64_t> sNonSilentBuffers{0};

void fillBuffer(AudioDevice* device, AudioQueueBufferRef buffer) {
    if (device->closing) return;
    buffer->mAudioDataByteSize = device->spec.size;
    std::memset(buffer->mAudioData, device->spec.silence, device->spec.size);
    if (device->tempo)
        device->tempo->fill(static_cast<Uint8*>(buffer->mAudioData),
                            static_cast<int>(device->spec.size));
    else
        device->spec.callback(device->spec.userdata,
                              static_cast<Uint8*>(buffer->mAudioData),
                              static_cast<int>(device->spec.size));
    if (!device->closing &&
        AudioQueueEnqueueBuffer(device->queue, buffer, 0, nullptr) == noErr) {
        const auto* samples = static_cast<const Sint16*>(buffer->mAudioData);
        const size_t count = buffer->mAudioDataByteSize / sizeof(Sint16);
        if (std::any_of(samples, samples + count, [](Sint16 sample) { return sample != 0; }))
            sNonSilentBuffers.fetch_add(1, std::memory_order_relaxed);
    }
}

void audioCallback(void* userData, AudioQueueRef, AudioQueueBufferRef buffer) {
    fillBuffer(static_cast<AudioDevice*>(userData), buffer);
}

AudioDevice* findDevice(SDL_AudioDeviceID id) {
    auto found = sAudioDevices.find(id);
    return found == sAudioDevices.end() ? nullptr : found->second.get();
}

}  // namespace

extern "C" __attribute__((visibility("default"))) uint64_t
maiFfplayAppleNonSilentBufferCount(void) {
    return sNonSilentBuffers.load(std::memory_order_relaxed);
}

extern "C" SDL_AudioDeviceID SDL_OpenAudioDevice(const char*, int capture,
                                                  const SDL_AudioSpec* wanted,
                                                  SDL_AudioSpec* obtained, int) {
    if (capture || !wanted || !wanted->callback || wanted->format != AUDIO_S16SYS ||
        wanted->freq < 1 || wanted->channels < 1 || wanted->samples < 1) {
        maiFfplaySetError("unsupported ffplay PCM audio format");
        return 0;
    }
    auto device = std::make_unique<AudioDevice>();
    device->spec = *wanted;
    device->spec.silence = 0;
    device->spec.size = static_cast<Uint32>(wanted->samples) * wanted->channels * sizeof(Sint16);
    MaiFfplayPlaybackStatus playback{};
    if (maiFfplayGetPlaybackStatus(&playback) && playback.speed_percent != 100)
        device->tempo = std::make_unique<MaiFfplayAudioTempo>(device->spec,
                                                               playback.speed_percent);

    AudioStreamBasicDescription format{};
    format.mSampleRate = wanted->freq;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    format.mBytesPerPacket = wanted->channels * sizeof(Sint16);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = format.mBytesPerPacket;
    format.mChannelsPerFrame = wanted->channels;
    format.mBitsPerChannel = 16;
    if (AudioQueueNewOutput(&format, audioCallback, device.get(), nullptr, nullptr, 0,
                            &device->queue) != noErr) {
        maiFfplaySetError("CoreAudio could not open the output device");
        return 0;
    }
    for (auto& buffer : device->buffers) {
        if (AudioQueueAllocateBuffer(device->queue, device->spec.size, &buffer) != noErr) {
            AudioQueueDispose(device->queue, true);
            maiFfplaySetError("CoreAudio could not allocate an output buffer");
            return 0;
        }
    }
    if (obtained) *obtained = device->spec;
    const SDL_AudioDeviceID id = sNextAudioId.fetch_add(1);
    std::lock_guard<std::mutex> lock(sAudioMutex);
    sAudioDevices.emplace(id, std::move(device));
    return id;
}

extern "C" void SDL_CloseAudioDevice(SDL_AudioDeviceID id) {
    std::unique_ptr<AudioDevice> device;
    {
        std::lock_guard<std::mutex> lock(sAudioMutex);
        auto found = sAudioDevices.find(id);
        if (found == sAudioDevices.end()) return;
        device = std::move(found->second);
        sAudioDevices.erase(found);
    }
    device->closing = true;
    AudioQueueStop(device->queue, true);
    AudioQueueDispose(device->queue, true);
}

extern "C" void SDL_PauseAudioDevice(SDL_AudioDeviceID id, int pause) {
    std::lock_guard<std::mutex> lock(sAudioMutex);
    AudioDevice* device = findDevice(id);
    if (!device || device->closing) return;
    if (pause) {
        if (device->started) AudioQueuePause(device->queue);
        return;
    }
    if (!device->started) {
        device->started = true;
        // ffplay 在解复用线程打开音频设备。若此时调用解码回调，
        // 会等待同一线程尚未入队的音频包。先用静音启动 CoreAudio，
        // 待解复用线程恢复后再运行真实回调。
        for (AudioQueueBufferRef buffer : device->buffers) {
            buffer->mAudioDataByteSize = device->spec.size;
            std::memset(buffer->mAudioData, device->spec.silence,
                        device->spec.size);
            AudioQueueEnqueueBuffer(device->queue, buffer, 0, nullptr);
        }
    }
    if (AudioQueueStart(device->queue, nullptr) != noErr)
        maiFfplaySetError("CoreAudio could not start playback");
}
