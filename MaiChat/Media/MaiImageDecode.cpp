#include "MaiImageDecode.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace {

constexpr long long kMaximumPixels = 64'000'000;

MaiImageDecodeResult failed(int error) {
    return {nullptr, 0, 0, 0, error};
}

}  // namespace

extern "C" MaiImageDecodeResult maiImageDecodeFile(const char* path, int max_width,
                                                   int max_height) {
    if (!path || !*path || max_width < 0 || max_height < 0) return failed(AVERROR(EINVAL));

    AVFormatContext* rawFormat = nullptr;
    int status = avformat_open_input(&rawFormat, path, nullptr, nullptr);
    if (status < 0) return failed(status);
    auto closeFormat = [](AVFormatContext* value) { avformat_close_input(&value); };
    std::unique_ptr<AVFormatContext, decltype(closeFormat)> format(rawFormat, closeFormat);

    status = avformat_find_stream_info(format.get(), nullptr);
    if (status < 0) return failed(status);
    const AVCodec* decoder = nullptr;
    const int stream = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (stream < 0 || !decoder) return failed(stream < 0 ? stream : AVERROR_DECODER_NOT_FOUND);

    auto freeCodec = [](AVCodecContext* value) { avcodec_free_context(&value); };
    std::unique_ptr<AVCodecContext, decltype(freeCodec)> codec(avcodec_alloc_context3(decoder),
                                                               freeCodec);
    if (!codec) return failed(AVERROR(ENOMEM));
    status = avcodec_parameters_to_context(codec.get(), format->streams[stream]->codecpar);
    if (status < 0) return failed(status);
    status = avcodec_open2(codec.get(), decoder, nullptr);
    if (status < 0) return failed(status);

    auto freePacket = [](AVPacket* value) { av_packet_free(&value); };
    auto freeFrame = [](AVFrame* value) { av_frame_free(&value); };
    std::unique_ptr<AVPacket, decltype(freePacket)> packet(av_packet_alloc(), freePacket);
    std::unique_ptr<AVFrame, decltype(freeFrame)> frame(av_frame_alloc(), freeFrame);
    if (!packet || !frame) return failed(AVERROR(ENOMEM));

    bool decoded = false;
    while ((status = av_read_frame(format.get(), packet.get())) >= 0) {
        if (packet->stream_index != stream) {
            av_packet_unref(packet.get());
            continue;
        }
        status = avcodec_send_packet(codec.get(), packet.get());
        av_packet_unref(packet.get());
        if (status < 0) return failed(status);
        status = avcodec_receive_frame(codec.get(), frame.get());
        if (status == AVERROR(EAGAIN)) continue;
        if (status < 0) return failed(status);
        decoded = true;
        break;
    }
    if (!decoded) {
        status = avcodec_send_packet(codec.get(), nullptr);
        if (status < 0) return failed(status);
        status = avcodec_receive_frame(codec.get(), frame.get());
        if (status < 0) return failed(status);
    }

    const int sourceWidth = frame->width;
    const int sourceHeight = frame->height;
    if (sourceWidth < 1 || sourceHeight < 1 ||
        static_cast<long long>(sourceWidth) * sourceHeight > kMaximumPixels)
        return failed(AVERROR(EINVAL));

    double ratio = 1.0;
    if (max_width > 0) ratio = std::min(ratio, static_cast<double>(max_width) / sourceWidth);
    if (max_height > 0) ratio = std::min(ratio, static_cast<double>(max_height) / sourceHeight);
    const int width = std::max(1, static_cast<int>(std::floor(sourceWidth * ratio)));
    const int height = std::max(1, static_cast<int>(std::floor(sourceHeight * ratio)));
    if (static_cast<long long>(width) * height > kMaximumPixels ||
        width > std::numeric_limits<int>::max() / 4)
        return failed(AVERROR(EINVAL));

    auto freeScale = [](SwsContext* value) { sws_freeContext(value); };
    std::unique_ptr<SwsContext, decltype(freeScale)> scale(
        sws_getContext(sourceWidth, sourceHeight, static_cast<AVPixelFormat>(frame->format), width,
                       height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr),
        freeScale);
    if (!scale) return failed(AVERROR(ENOSYS));

    const int stride = width * 4;
    auto pixels = std::unique_ptr<unsigned char, decltype(&std::free)>(
        static_cast<unsigned char*>(std::malloc(static_cast<size_t>(stride) * height)), &std::free);
    if (!pixels) return failed(AVERROR(ENOMEM));
    unsigned char* output[] = {pixels.get(), nullptr, nullptr, nullptr};
    const int outputLines[] = {stride, 0, 0, 0};
    status =
        sws_scale(scale.get(), frame->data, frame->linesize, 0, sourceHeight, output, outputLines);
    if (status != height) return failed(status < 0 ? status : AVERROR_INVALIDDATA);
    return {pixels.release(), width, height, stride, 0};
}

extern "C" void maiImageDecodeFree(void* pixels) {
    std::free(pixels);
}
