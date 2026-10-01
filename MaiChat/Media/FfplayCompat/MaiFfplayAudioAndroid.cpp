#include "SDL.h"

#include <aaudio/AAudio.h>
#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "MaiFfplayCompatInternal.h"

namespace {

struct AudioDevice {
    AAudioStream* stream = nullptr;
    SDL_AudioSpec spec{};
    std::thread worker;
    std::mutex mutex;
    std::condition_variable condition;
    bool started = false;
    bool paused = true;
    bool closing = false;
};

std::mutex sDevicesMutex;
std::unordered_map<SDL_AudioDeviceID, std::unique_ptr<AudioDevice>> sDevices;
std::atomic<SDL_AudioDeviceID> sNextId{2};

void playAudio(AudioDevice* device) {
    pthread_setname_np(pthread_self(), "mai-ffplay-audio");
    std::vector<Uint8> buffer(device->spec.size);
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(device->mutex);
            device->condition.wait(lock, [device] { return device->closing || !device->paused; });
            if (device->closing) break;
        }
        device->spec.callback(device->spec.userdata, buffer.data(),
                              static_cast<int>(buffer.size()));
        int32_t remaining = static_cast<int32_t>(device->spec.samples);
        const int32_t bytesPerFrame = device->spec.channels * sizeof(Sint16);
        Uint8* next = buffer.data();
        while (remaining > 0) {
            const aaudio_result_t written = AAudioStream_write(
                device->stream, next, remaining, 200000000);
            if (written < 0) {
                std::lock_guard<std::mutex> lock(device->mutex);
                if (!device->closing && !device->paused)
                    maiFfplaySetError(AAudio_convertResultToText(written));
                break;
            }
            if (written == 0) continue;
            remaining -= written;
            next += written * bytesPerFrame;
        }
    }
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
    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || !builder) {
        maiFfplaySetError("AAudio stream builder failed");
        return 0;
    }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSampleRate(builder, wanted->freq);
    AAudioStreamBuilder_setChannelCount(builder, wanted->channels);
    auto device = std::make_unique<AudioDevice>();
    const aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &device->stream);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK || !device->stream) {
        maiFfplaySetError(AAudio_convertResultToText(result));
        return 0;
    }
    device->spec = *wanted;
    device->spec.freq = AAudioStream_getSampleRate(device->stream);
    device->spec.channels = static_cast<Uint8>(AAudioStream_getChannelCount(device->stream));
    device->spec.silence = 0;
    device->spec.size = static_cast<Uint32>(device->spec.samples) *
                        device->spec.channels * sizeof(Sint16);
    if (obtained) *obtained = device->spec;
    const SDL_AudioDeviceID id = sNextId.fetch_add(1);
    std::lock_guard<std::mutex> lock(sDevicesMutex);
    sDevices.emplace(id, std::move(device));
    return id;
}

extern "C" void SDL_CloseAudioDevice(SDL_AudioDeviceID id) {
    std::unique_ptr<AudioDevice> device;
    {
        std::lock_guard<std::mutex> lock(sDevicesMutex);
        auto found = sDevices.find(id);
        if (found == sDevices.end()) return;
        device = std::move(found->second);
        sDevices.erase(found);
    }
    {
        std::lock_guard<std::mutex> lock(device->mutex);
        device->closing = true;
    }
    device->condition.notify_all();
    AAudioStream_requestStop(device->stream);
    if (device->worker.joinable()) device->worker.join();
    AAudioStream_close(device->stream);
}

extern "C" void SDL_PauseAudioDevice(SDL_AudioDeviceID id, int pause) {
    std::lock_guard<std::mutex> devicesLock(sDevicesMutex);
    auto found = sDevices.find(id);
    if (found == sDevices.end()) return;
    AudioDevice* device = found->second.get();
    {
        std::lock_guard<std::mutex> lock(device->mutex);
        device->paused = pause != 0;
        if (!device->started && !device->paused) {
            device->started = true;
            device->worker = std::thread(playAudio, device);
        }
    }
    if (pause) AAudioStream_requestPause(device->stream);
    else AAudioStream_requestStart(device->stream);
    device->condition.notify_all();
}
