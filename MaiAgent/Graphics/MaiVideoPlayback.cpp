#include "MaiVideoPlayback.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include "MaiGraphicsPresenter.h"

struct MaiVideoPlayback {
    using Clock = std::chrono::steady_clock;

    uint64_t viewId;
    std::string path;
    MaiVideoPlaybackCallback callback;
    void* userData;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable condition;
    bool stopped = false;
    bool paused = true;
    bool stepPending = false;
    bool seekPending = false;
    bool loop = false;
    bool clockReset = true;
    bool ended = false;
    double speed = 1.0;
    int64_t seekPositionMs = 0;
    std::atomic<int64_t> positionMs{0};
    std::atomic<int64_t> durationMs{0};

    MaiVideoPlayback(uint64_t view, std::string file, MaiVideoPlaybackCallback notify, void* user)
        : viewId(view), path(std::move(file)), callback(notify), userData(user) {}

    void notify(MaiVideoPlaybackEvent event) {
        if (callback) callback(this, event, userData);
    }

    static int interrupted(void* opaque) {
        auto* playback = static_cast<MaiVideoPlayback*>(opaque);
        std::lock_guard<std::mutex> lock(playback->mutex);
        return playback->stopped ? 1 : 0;
    }

    bool waitForFrame(int64_t ptsMs, Clock::time_point* baseWall, int64_t* basePts) {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            if (stopped || seekPending) return false;
            if (paused && !stepPending) {
                condition.wait(lock);
                continue;
            }
            if (clockReset) {
                *baseWall = Clock::now();
                *basePts = ptsMs;
                clockReset = false;
            }
            if (stepPending) {
                stepPending = false;
                return true;
            }
            const auto elapsed = std::chrono::duration<double, std::milli>(
                static_cast<double>(ptsMs - *basePts) / speed);
            const auto target = *baseWall + std::chrono::duration_cast<Clock::duration>(elapsed);
            if (Clock::now() >= target) return true;
            condition.wait_until(lock, target);
        }
    }

    void run() {
#if defined(_WIN32)
        SetThreadDescription(GetCurrentThread(), L"mai-video");
#elif defined(__APPLE__)
        pthread_setname_np("mai-video");
#else
        pthread_setname_np(pthread_self(), "mai-video");
#endif

        AVFormatContext* rawFormat = avformat_alloc_context();
        if (!rawFormat) {
            notify(MAI_VIDEO_ERROR);
            return;
        }
        rawFormat->interrupt_callback = {interrupted, this};
        if (avformat_open_input(&rawFormat, path.c_str(), nullptr, nullptr) < 0) {
            if (rawFormat) avformat_free_context(rawFormat);
            notify(MAI_VIDEO_ERROR);
            return;
        }
        auto closeFormat = [](AVFormatContext* value) { avformat_close_input(&value); };
        std::unique_ptr<AVFormatContext, decltype(closeFormat)> format(rawFormat, closeFormat);
        if (avformat_find_stream_info(format.get(), nullptr) < 0) {
            notify(MAI_VIDEO_ERROR);
            return;
        }
        const int streamIndex =
            av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (streamIndex < 0) {
            notify(MAI_VIDEO_ERROR);
            return;
        }
        AVStream* stream = format->streams[streamIndex];
        const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder) {
            notify(MAI_VIDEO_ERROR);
            return;
        }
        auto freeCodec = [](AVCodecContext* value) { avcodec_free_context(&value); };
        std::unique_ptr<AVCodecContext, decltype(freeCodec)> codec(avcodec_alloc_context3(decoder),
                                                                   freeCodec);
        if (!codec || avcodec_parameters_to_context(codec.get(), stream->codecpar) < 0 ||
            avcodec_open2(codec.get(), decoder, nullptr) < 0) {
            notify(MAI_VIDEO_ERROR);
            return;
        }
        auto freePacket = [](AVPacket* value) { av_packet_free(&value); };
        auto freeFrame = [](AVFrame* value) { av_frame_free(&value); };
        std::unique_ptr<AVPacket, decltype(freePacket)> packet(av_packet_alloc(), freePacket);
        std::unique_ptr<AVFrame, decltype(freeFrame)> frame(av_frame_alloc(), freeFrame);
        if (!packet || !frame) {
            notify(MAI_VIDEO_ERROR);
            return;
        }
        if (format->duration > 0)
            durationMs = av_rescale_q(format->duration, AV_TIME_BASE_Q, {1, 1000});
        else if (stream->duration > 0)
            durationMs = av_rescale_q(stream->duration, stream->time_base, {1, 1000});
        notify(MAI_VIDEO_READY);

        SwsContext* scaler = nullptr;
        Clock::time_point baseWall = Clock::now();
        int64_t basePts = 0;
        int64_t lastPts = 0;
        const AVRational milliseconds = {1, 1000};
        const int64_t frameDuration =
            stream->avg_frame_rate.num > 0
                ? av_rescale_q(1, av_inv_q(stream->avg_frame_rate), milliseconds)
                : 33;

        bool failed = false;
        auto presentFrame = [&]() -> bool {
            int64_t pts =
                frame->best_effort_timestamp != AV_NOPTS_VALUE
                    ? av_rescale_q(frame->best_effort_timestamp, stream->time_base, milliseconds)
                    : lastPts + std::max<int64_t>(1, frameDuration);
            if (!waitForFrame(pts, &baseWall, &basePts)) return false;
            const uint64_t pixels = static_cast<uint64_t>(frame->width) * frame->height;
            if (!frame->width || !frame->height || pixels > 64000000) {
                failed = true;
                return false;
            }
            scaler = sws_getCachedContext(scaler, frame->width, frame->height,
                                          static_cast<AVPixelFormat>(frame->format), frame->width,
                                          frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr,
                                          nullptr, nullptr);
            if (!scaler) {
                failed = true;
                return false;
            }
            std::vector<uint8_t> rgba(static_cast<size_t>(pixels) * 4);
            uint8_t* output[] = {rgba.data(), nullptr, nullptr, nullptr};
            const int strides[] = {frame->width * 4, 0, 0, 0};
            if (sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, output,
                          strides) != frame->height) {
                failed = true;
                return false;
            }
            if (!maiGraphicsPresenterShowFrame(viewId, rgba.data(), frame->width, frame->height,
                                               strides[0], false)) {
                failed = true;
                return false;
            }
            lastPts = pts;
            positionMs = pts;
            return true;
        };

        for (;;) {
            int64_t pendingSeek = -1;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock,
                               [this] { return stopped || seekPending || !paused || stepPending; });
                if (stopped) break;
                if (seekPending) {
                    pendingSeek = seekPositionMs;
                    seekPending = false;
                    clockReset = true;
                    ended = false;
                    if (paused) stepPending = true;
                }
            }
            if (pendingSeek >= 0) {
                const int64_t target = av_rescale_q(pendingSeek, milliseconds, stream->time_base);
                if (av_seek_frame(format.get(), streamIndex, target, AVSEEK_FLAG_BACKWARD) < 0) {
                    failed = true;
                    break;
                }
                avcodec_flush_buffers(codec.get());
                lastPts = pendingSeek;
                positionMs = pendingSeek;
            }

            const int readResult = av_read_frame(format.get(), packet.get());
            if (readResult < 0) {
                if (readResult != AVERROR_EOF) {
                    failed = true;
                    break;
                }
                if (avcodec_send_packet(codec.get(), nullptr) < 0) {
                    failed = true;
                    break;
                }
                int receiveResult = 0;
                while ((receiveResult = avcodec_receive_frame(codec.get(), frame.get())) == 0) {
                    const bool presented = presentFrame();
                    av_frame_unref(frame.get());
                    if (!presented) break;
                }
                if (receiveResult < 0 && receiveResult != AVERROR(EAGAIN) &&
                    receiveResult != AVERROR_EOF)
                    failed = true;
                if (failed) break;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stopped) break;
                    if (seekPending) continue;
                }
                notify(MAI_VIDEO_ENDED);
                std::lock_guard<std::mutex> lock(mutex);
                ended = true;
                paused = true;
                if (loop) {
                    seekPositionMs = 0;
                    seekPending = true;
                    paused = false;
                }
                continue;
            }
            if (packet->stream_index != streamIndex) {
                av_packet_unref(packet.get());
                continue;
            }
            const int sendResult = avcodec_send_packet(codec.get(), packet.get());
            av_packet_unref(packet.get());
            if (sendResult < 0) {
                failed = true;
                break;
            }
            int receiveResult = 0;
            while ((receiveResult = avcodec_receive_frame(codec.get(), frame.get())) == 0) {
                const bool presented = presentFrame();
                av_frame_unref(frame.get());
                if (!presented) break;
            }
            if (receiveResult < 0 && receiveResult != AVERROR(EAGAIN) &&
                receiveResult != AVERROR_EOF)
                failed = true;
            if (failed) break;
        }
        if (scaler) sws_freeContext(scaler);
        if (failed) notify(MAI_VIDEO_ERROR);
    }
};

extern "C" MaiVideoPlayback* maiVideoPlaybackCreate(uint64_t view_id, const char* file_path,
                                                    MaiVideoPlaybackCallback callback,
                                                    void* user_data) {
    if (!view_id || !file_path || !*file_path) return nullptr;
    MaiVideoPlayback* playback = nullptr;
    try {
        playback = new MaiVideoPlayback(view_id, file_path, callback, user_data);
        playback->worker = std::thread([playback] {
            try {
                playback->run();
            } catch (const std::exception&) {
                playback->notify(MAI_VIDEO_ERROR);
            }
        });
        return playback;
    } catch (const std::exception&) {
        delete playback;
        return nullptr;
    }
}

extern "C" void maiVideoPlaybackDestroy(MaiVideoPlayback* playback) {
    if (!playback) return;
    {
        std::lock_guard<std::mutex> lock(playback->mutex);
        playback->stopped = true;
    }
    playback->condition.notify_all();
    if (playback->worker.joinable()) playback->worker.join();
    delete playback;
}

extern "C" bool maiVideoPlaybackPlay(MaiVideoPlayback* playback) {
    if (!playback) return false;
    {
        std::lock_guard<std::mutex> lock(playback->mutex);
        if (playback->stopped) return false;
        if (playback->ended) {
            playback->seekPending = true;
            playback->seekPositionMs = 0;
        }
        playback->paused = false;
        playback->clockReset = true;
    }
    playback->condition.notify_all();
    playback->notify(MAI_VIDEO_PLAYING);
    return true;
}

extern "C" bool maiVideoPlaybackPause(MaiVideoPlayback* playback) {
    if (!playback) return false;
    {
        std::lock_guard<std::mutex> lock(playback->mutex);
        if (playback->stopped) return false;
        playback->paused = true;
    }
    playback->condition.notify_all();
    playback->notify(MAI_VIDEO_PAUSED);
    return true;
}

extern "C" bool maiVideoPlaybackSeek(MaiVideoPlayback* playback, int64_t position_ms) {
    if (!playback || position_ms < 0) return false;
    {
        std::lock_guard<std::mutex> lock(playback->mutex);
        if (playback->stopped) return false;
        playback->seekPositionMs = position_ms;
        playback->seekPending = true;
    }
    playback->condition.notify_all();
    return true;
}

extern "C" bool maiVideoPlaybackStep(MaiVideoPlayback* playback) {
    if (!playback) return false;
    {
        std::lock_guard<std::mutex> lock(playback->mutex);
        if (playback->stopped) return false;
        playback->paused = true;
        playback->stepPending = true;
    }
    playback->condition.notify_all();
    return true;
}

extern "C" bool maiVideoPlaybackSetSpeed(MaiVideoPlayback* playback, double speed) {
    if (!playback || speed < 0.25 || speed > 4.0) return false;
    {
        std::lock_guard<std::mutex> lock(playback->mutex);
        if (playback->stopped) return false;
        playback->speed = speed;
        playback->clockReset = true;
    }
    playback->condition.notify_all();
    return true;
}

extern "C" void maiVideoPlaybackSetLoop(MaiVideoPlayback* playback, bool loop) {
    if (!playback) return;
    std::lock_guard<std::mutex> lock(playback->mutex);
    playback->loop = loop;
}

extern "C" int64_t maiVideoPlaybackPosition(const MaiVideoPlayback* playback) {
    return playback ? playback->positionMs.load() : -1;
}

extern "C" int64_t maiVideoPlaybackDuration(const MaiVideoPlayback* playback) {
    return playback ? playback->durationMs.load() : -1;
}
