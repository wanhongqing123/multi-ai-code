#include "SDL.h"

#include <windows.h>
#include <mmsystem.h>

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
    HWAVEOUT output = nullptr;
    HANDLE bufferDone = nullptr;
    SDL_AudioSpec spec{};
    std::vector<Uint8> buffers[3];
    WAVEHDR headers[3]{};
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
    SetThreadDescription(GetCurrentThread(), L"mai-ffplay-audio");
    int next = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(device->mutex);
            device->condition.wait(lock, [device] { return device->closing || !device->paused; });
            if (device->closing) break;
        }
        WAVEHDR& header = device->headers[next];
        if (header.dwFlags & WHDR_INQUEUE) {
            WaitForSingleObject(device->bufferDone, 100);
            continue;
        }
        device->spec.callback(device->spec.userdata,
                              device->buffers[next].data(),
                              static_cast<int>(device->spec.size));
        header.dwBufferLength = device->spec.size;
        const MMRESULT written = waveOutWrite(device->output, &header, sizeof(header));
        if (written != MMSYSERR_NOERROR) {
            maiFfplaySetError("Windows audio output rejected a PCM buffer");
            break;
        }
        next = (next + 1) % 3;
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
    auto device = std::make_unique<AudioDevice>();
    device->spec = *wanted;
    device->spec.silence = 0;
    device->spec.size = static_cast<Uint32>(wanted->samples) *
                        wanted->channels * sizeof(Sint16);
    device->bufferDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!device->bufferDone) {
        maiFfplaySetError("Windows audio completion event could not be created");
        return 0;
    }
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = wanted->channels;
    format.nSamplesPerSec = wanted->freq;
    format.wBitsPerSample = 16;
    format.nBlockAlign = wanted->channels * sizeof(Sint16);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    const MMRESULT opened = waveOutOpen(
        &device->output, WAVE_MAPPER, &format,
        reinterpret_cast<DWORD_PTR>(device->bufferDone), 0, CALLBACK_EVENT);
    if (opened != MMSYSERR_NOERROR) {
        CloseHandle(device->bufferDone);
        maiFfplaySetError("Windows audio output device could not be opened");
        return 0;
    }
    for (int index = 0; index < 3; ++index) {
        device->buffers[index].resize(device->spec.size);
        device->headers[index].lpData = reinterpret_cast<LPSTR>(device->buffers[index].data());
        device->headers[index].dwBufferLength = device->spec.size;
        if (waveOutPrepareHeader(device->output, &device->headers[index],
                                 sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            for (int prepared = 0; prepared < index; ++prepared)
                waveOutUnprepareHeader(device->output, &device->headers[prepared],
                                       sizeof(WAVEHDR));
            waveOutClose(device->output);
            CloseHandle(device->bufferDone);
            maiFfplaySetError("Windows audio buffer preparation failed");
            return 0;
        }
    }
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
    waveOutReset(device->output);
    SetEvent(device->bufferDone);
    if (device->worker.joinable()) device->worker.join();
    for (auto& header : device->headers)
        waveOutUnprepareHeader(device->output, &header, sizeof(WAVEHDR));
    waveOutClose(device->output);
    CloseHandle(device->bufferDone);
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
    if (pause) waveOutPause(device->output);
    else waveOutRestart(device->output);
    device->condition.notify_all();
}
