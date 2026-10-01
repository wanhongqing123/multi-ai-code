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

namespace {

struct AudioDevice {
    AudioQueueRef queue = nullptr;
    SDL_AudioSpec spec{};
    AudioQueueBufferRef buffers[3]{};
    std::atomic<bool> closing{false};
    bool started = false;
};

std::mutex sAudioMutex;
std::unordered_map<SDL_AudioDeviceID, std::unique_ptr<AudioDevice>> sAudioDevices;
std::atomic<SDL_AudioDeviceID> sNextAudioId{2};

void fillBuffer(AudioDevice* device, AudioQueueBufferRef buffer) {
    if (device->closing) return;
    buffer->mAudioDataByteSize = device->spec.size;
    std::memset(buffer->mAudioData, device->spec.silence, device->spec.size);
    device->spec.callback(device->spec.userdata,
                          static_cast<Uint8*>(buffer->mAudioData),
                          static_cast<int>(device->spec.size));
    if (!device->closing) AudioQueueEnqueueBuffer(device->queue, buffer, 0, nullptr);
}

void audioCallback(void* userData, AudioQueueRef, AudioQueueBufferRef buffer) {
    fillBuffer(static_cast<AudioDevice*>(userData), buffer);
}

AudioDevice* findDevice(SDL_AudioDeviceID id) {
    auto found = sAudioDevices.find(id);
    return found == sAudioDevices.end() ? nullptr : found->second.get();
}

}  // namespace

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
        // ffplay opens the audio device from its demux thread. Calling its
        // decode callback here would wait for packets that this same thread
        // has not queued yet. Prime CoreAudio with silence; the real callback
        // runs only after the demux thread resumes.
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
