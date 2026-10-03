#include "MaiImageEncode.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {

MaiImageEncodeResult failed(int error) { return {nullptr, 0, error}; }

} // namespace

extern "C" MaiImageEncodeResult maiImageEncodePngRgba(const unsigned char *rgba, int width,
                                                      int height, int stride,
                                                      const unsigned char *icc_profile,
                                                      size_t icc_size, int color_primaries,
                                                      int color_transfer) {
    if (!rgba || width <= 0 || height <= 0 || width > 8192 || height > 8192 ||
        static_cast<long long>(width) * height > 24'000'000 ||
        stride < static_cast<long long>(width) * 4 || icc_size > 1024 * 1024 ||
        (icc_size && !icc_profile))
        return failed(AVERROR(EINVAL));
    const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!encoder)
        return failed(AVERROR_ENCODER_NOT_FOUND);
    auto freeCodec = [](AVCodecContext *value) { avcodec_free_context(&value); };
    auto freeFrame = [](AVFrame *value) { av_frame_free(&value); };
    auto freePacket = [](AVPacket *value) { av_packet_free(&value); };
    std::unique_ptr<AVCodecContext, decltype(freeCodec)> codec(avcodec_alloc_context3(encoder),
                                                               freeCodec);
    std::unique_ptr<AVFrame, decltype(freeFrame)> frame(av_frame_alloc(), freeFrame);
    std::unique_ptr<AVPacket, decltype(freePacket)> packet(av_packet_alloc(), freePacket);
    if (!codec || !frame || !packet)
        return failed(AVERROR(ENOMEM));
    codec->width = width;
    codec->height = height;
    codec->pix_fmt = AV_PIX_FMT_RGBA;
    codec->time_base = {1, 1};
    codec->color_primaries = static_cast<AVColorPrimaries>(color_primaries);
    codec->color_trc = static_cast<AVColorTransferCharacteristic>(color_transfer);
    codec->colorspace = AVCOL_SPC_RGB;
    codec->color_range = AVCOL_RANGE_JPEG;
    int result = avcodec_open2(codec.get(), encoder, nullptr);
    if (result < 0)
        return failed(result);
    frame->width = width;
    frame->height = height;
    frame->format = AV_PIX_FMT_RGBA;
    frame->data[0] = const_cast<unsigned char *>(rgba);
    frame->linesize[0] = stride;
    frame->color_primaries = codec->color_primaries;
    frame->color_trc = codec->color_trc;
    frame->colorspace = codec->colorspace;
    frame->color_range = codec->color_range;
    if (icc_size) {
        AVFrameSideData *profile =
            av_frame_new_side_data(frame.get(), AV_FRAME_DATA_ICC_PROFILE, icc_size);
        if (!profile)
            return failed(AVERROR(ENOMEM));
        std::memcpy(profile->data, icc_profile, icc_size);
    }
    result = avcodec_send_frame(codec.get(), frame.get());
    if (result < 0)
        return failed(result);
    result = avcodec_receive_packet(codec.get(), packet.get());
    if (result < 0)
        return failed(result);
    if (packet->size <= 0 || packet->size > std::numeric_limits<int>::max())
        return failed(AVERROR_INVALIDDATA);
    auto *bytes = static_cast<unsigned char *>(std::malloc(packet->size));
    if (!bytes)
        return failed(AVERROR(ENOMEM));
    std::memcpy(bytes, packet->data, packet->size);
    return {bytes, static_cast<size_t>(packet->size), 0};
}

extern "C" void maiImageEncodeFree(void *bytes) { std::free(bytes); }
