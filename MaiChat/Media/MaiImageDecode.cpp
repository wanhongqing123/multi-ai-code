#include "MaiImageDecode.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace {

constexpr long long kMaximumPixels = 64'000'000;
constexpr long long kMaximumGridPixels = 32'000'000;

MaiImageDecodeResult failed(int error) { return {nullptr, 0, 0, 0, error, nullptr, 0, 0, 0}; }

int rotationFromMatrix(const uint8_t *data, size_t size) {
    if (!data || size < 9 * sizeof(int32_t))
        return 0;
    const double angle = av_display_rotation_get(reinterpret_cast<const int32_t *>(data));
    if (!std::isfinite(angle))
        return 0;
    const int rounded = static_cast<int>(std::lround(angle));
    return rounded % 90 == 0 ? (rounded % 360 + 360) % 360 : 0;
}

int displayRotation(const AVPacketSideData *sideData, int count) {
    for (int index = 0; index < count; ++index) {
        const AVPacketSideData &entry = sideData[index];
        if (entry.type != AV_PKT_DATA_DISPLAYMATRIX)
            continue;
        return rotationFromMatrix(entry.data, entry.size);
    }
    return 0;
}

AVPixelFormat scalePixelFormat(AVPixelFormat format) {
    switch (format) {
    case AV_PIX_FMT_YUVJ420P:
        return AV_PIX_FMT_YUV420P;
    case AV_PIX_FMT_YUVJ422P:
        return AV_PIX_FMT_YUV422P;
    case AV_PIX_FMT_YUVJ444P:
        return AV_PIX_FMT_YUV444P;
    case AV_PIX_FMT_YUVJ440P:
        return AV_PIX_FMT_YUV440P;
    default:
        return format;
    }
}

int configureColor(SwsContext *scale, const AVFrame *frame) {
    int matrix = SWS_CS_ITU601;
    switch (frame->colorspace) {
    case AVCOL_SPC_BT709:
        matrix = SWS_CS_ITU709;
        break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        matrix = SWS_CS_BT2020;
        break;
    default:
        break;
    }
    const int *coefficients = sws_getCoefficients(matrix);
    const bool fullRange =
        frame->color_range == AVCOL_RANGE_JPEG || frame->format == AV_PIX_FMT_YUVJ420P ||
        frame->format == AV_PIX_FMT_YUVJ422P || frame->format == AV_PIX_FMT_YUVJ444P ||
        frame->format == AV_PIX_FMT_YUVJ440P;
    return sws_setColorspaceDetails(scale, coefficients, fullRange ? 1 : 0, coefficients, 1, 0,
                                    1 << 16, 1 << 16);
}

MaiImageDecodeResult rotateRgba(MaiImageDecodeResult image, int counterclockwise) {
    if (!image.rgba || counterclockwise == 0)
        return image;
    const int width = counterclockwise == 180 ? image.width : image.height;
    const int height = counterclockwise == 180 ? image.height : image.width;
    auto output = std::unique_ptr<unsigned char, decltype(&std::free)>(
        static_cast<unsigned char *>(std::malloc(static_cast<size_t>(width) * height * 4)),
        &std::free);
    if (!output) {
        maiImageDecodeFree(image.rgba);
        return failed(AVERROR(ENOMEM));
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int sourceX = x;
            int sourceY = y;
            if (counterclockwise == 90) {
                sourceX = image.width - 1 - y;
                sourceY = x;
            } else if (counterclockwise == 180) {
                sourceX = image.width - 1 - x;
                sourceY = image.height - 1 - y;
            } else if (counterclockwise == 270) {
                sourceX = y;
                sourceY = image.height - 1 - x;
            }
            std::memcpy(output.get() + (static_cast<size_t>(y) * width + x) * 4,
                        image.rgba + static_cast<size_t>(sourceY) * image.stride + sourceX * 4, 4);
        }
    }
    maiImageDecodeFree(image.rgba);
    return {output.release(), width, height, width * 4, 0, nullptr, 0, 0, 0};
}

MaiImageDecodeResult attachColorMetadata(MaiImageDecodeResult image, const unsigned char *profile,
                                         size_t profileSize, int primaries, int transfer) {
    image.color_primaries = primaries;
    image.color_transfer = transfer;
    if (!image.rgba || !profile || !profileSize)
        return image;
    if (profileSize > 1024 * 1024) {
        maiImageDecodeFree(image.rgba);
        return failed(AVERROR(EINVAL));
    }
    const size_t pixelBytes = static_cast<size_t>(image.stride) * image.height;
    auto *combined = static_cast<unsigned char *>(std::malloc(pixelBytes + profileSize));
    if (!combined) {
        maiImageDecodeFree(image.rgba);
        return failed(AVERROR(ENOMEM));
    }
    std::memcpy(combined, image.rgba, pixelBytes);
    std::memcpy(combined + pixelBytes, profile, profileSize);
    maiImageDecodeFree(image.rgba);
    image.rgba = combined;
    image.icc_profile = combined + pixelBytes;
    image.icc_size = static_cast<int>(profileSize);
    return image;
}

const AVPacketSideData *packetIcc(const AVPacketSideData *sideData, int count) {
    for (int index = 0; index < count; ++index)
        if (sideData[index].type == AV_PKT_DATA_ICC_PROFILE)
            return &sideData[index];
    return nullptr;
}

MaiImageDecodeResult resizeRgba(const unsigned char *pixels, int width, int height, int maxWidth,
                                int maxHeight, int rotation) {
    const int displayWidth = rotation == 90 || rotation == 270 ? height : width;
    const int displayHeight = rotation == 90 || rotation == 270 ? width : height;
    double ratio = 1.0;
    if (maxWidth > 0)
        ratio = std::min(ratio, static_cast<double>(maxWidth) / displayWidth);
    if (maxHeight > 0)
        ratio = std::min(ratio, static_cast<double>(maxHeight) / displayHeight);
    const int outputWidth = std::max(1, static_cast<int>(std::floor(width * ratio)));
    const int outputHeight = std::max(1, static_cast<int>(std::floor(height * ratio)));
    if (outputWidth > std::numeric_limits<int>::max() / 4 ||
        static_cast<long long>(outputWidth) * outputHeight > kMaximumPixels)
        return failed(AVERROR(EINVAL));
    auto output = std::unique_ptr<unsigned char, decltype(&std::free)>(
        static_cast<unsigned char *>(
            std::malloc(static_cast<size_t>(outputWidth) * outputHeight * 4)),
        &std::free);
    if (!output)
        return failed(AVERROR(ENOMEM));
    if (outputWidth == width && outputHeight == height) {
        std::memcpy(output.get(), pixels, static_cast<size_t>(width) * height * 4);
    } else {
        auto freeScale = [](SwsContext *value) { sws_freeContext(value); };
        std::unique_ptr<SwsContext, decltype(freeScale)> scale(
            sws_getContext(width, height, AV_PIX_FMT_RGBA, outputWidth, outputHeight,
                           AV_PIX_FMT_RGBA, SWS_LANCZOS, nullptr, nullptr, nullptr),
            freeScale);
        if (!scale)
            return failed(AVERROR(ENOSYS));
        const uint8_t *source[] = {pixels, nullptr, nullptr, nullptr};
        const int sourceLines[] = {width * 4, 0, 0, 0};
        uint8_t *target[] = {output.get(), nullptr, nullptr, nullptr};
        const int targetLines[] = {outputWidth * 4, 0, 0, 0};
        const int scaled =
            sws_scale(scale.get(), source, sourceLines, 0, height, target, targetLines);
        if (scaled != outputHeight)
            return failed(scaled < 0 ? scaled : AVERROR_INVALIDDATA);
    }
    return rotateRgba(
        {output.release(), outputWidth, outputHeight, outputWidth * 4, 0, nullptr, 0, 0, 0},
        rotation);
}

MaiImageDecodeResult decodeTileGrid(AVFormatContext *format, const AVStreamGroup *group,
                                    int maxWidth, int maxHeight) {
    const AVStreamGroupTileGrid *grid = group->params.tile_grid;
    if (!grid || !grid->nb_tiles || grid->nb_tiles > 128 || !group->nb_streams ||
        group->nb_streams > 128 || grid->coded_width < 1 || grid->coded_height < 1 ||
        grid->width < 1 || grid->height < 1 || grid->horizontal_offset < 0 ||
        grid->vertical_offset < 0 || grid->horizontal_offset + grid->width > grid->coded_width ||
        grid->vertical_offset + grid->height > grid->coded_height ||
        static_cast<long long>(grid->coded_width) * grid->coded_height > kMaximumGridPixels)
        return failed(AVERROR(EINVAL));

    auto freeCodec = [](AVCodecContext *value) { avcodec_free_context(&value); };
    auto freeFrame = [](AVFrame *value) { av_frame_free(&value); };
    using Codec = std::unique_ptr<AVCodecContext, decltype(freeCodec)>;
    using Frame = std::unique_ptr<AVFrame, decltype(freeFrame)>;
    std::vector<Codec> decoders;
    std::vector<Frame> frames;
    std::vector<bool> ready(group->nb_streams, false);
    std::vector<int> position(format->nb_streams, -1);
    decoders.reserve(group->nb_streams);
    frames.reserve(group->nb_streams);
    for (unsigned int index = 0; index < group->nb_streams; ++index) {
        const AVStream *stream = group->streams[index];
        if (!stream || stream->index < 0 || stream->index >= format->nb_streams)
            return failed(AVERROR_INVALIDDATA);
        const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        Codec codec(avcodec_alloc_context3(decoder), freeCodec);
        Frame frame(av_frame_alloc(), freeFrame);
        if (!decoder || !codec || !frame)
            return failed(AVERROR(ENOMEM));
        if (avcodec_parameters_to_context(codec.get(), stream->codecpar) < 0)
            return failed(AVERROR_INVALIDDATA);
        codec->thread_count = 1;
        if (avcodec_open2(codec.get(), decoder, nullptr) < 0)
            return failed(AVERROR_DECODER_NOT_FOUND);
        position[stream->index] = static_cast<int>(index);
        decoders.push_back(std::move(codec));
        frames.push_back(std::move(frame));
    }

    auto freePacket = [](AVPacket *value) { av_packet_free(&value); };
    std::unique_ptr<AVPacket, decltype(freePacket)> packet(av_packet_alloc(), freePacket);
    if (!packet)
        return failed(AVERROR(ENOMEM));
    unsigned int remaining = group->nb_streams;
    int status = 0;
    while (remaining && (status = av_read_frame(format, packet.get())) >= 0) {
        const int stream = packet->stream_index;
        const int slot = stream >= 0 && stream < position.size() ? position[stream] : -1;
        if (slot >= 0 && !ready[slot]) {
            status = avcodec_send_packet(decoders[slot].get(), packet.get());
            if (status >= 0)
                status = avcodec_receive_frame(decoders[slot].get(), frames[slot].get());
            if (status >= 0) {
                ready[slot] = true;
                --remaining;
            } else if (status != AVERROR(EAGAIN)) {
                av_packet_unref(packet.get());
                return failed(status);
            }
        }
        av_packet_unref(packet.get());
    }
    for (unsigned int index = 0; index < group->nb_streams && remaining; ++index) {
        if (ready[index])
            continue;
        status = avcodec_send_packet(decoders[index].get(), nullptr);
        if (status >= 0)
            status = avcodec_receive_frame(decoders[index].get(), frames[index].get());
        if (status < 0)
            return failed(status);
        ready[index] = true;
        --remaining;
    }

    const size_t canvasBytes = static_cast<size_t>(grid->coded_width) * grid->coded_height * 4;
    auto canvas = std::unique_ptr<unsigned char, decltype(&std::free)>(
        static_cast<unsigned char *>(std::malloc(canvasBytes)), &std::free);
    if (!canvas)
        return failed(AVERROR(ENOMEM));
    for (size_t offset = 0; offset < canvasBytes; offset += 4)
        std::memcpy(canvas.get() + offset, grid->background, 4);
    auto freeScale = [](SwsContext *value) { sws_freeContext(value); };
    std::unique_ptr<SwsContext, decltype(freeScale)> scale(nullptr, freeScale);
    for (unsigned int tile = 0; tile < grid->nb_tiles; ++tile) {
        const auto &offset = grid->offsets[tile];
        if (offset.idx >= frames.size() || offset.horizontal < 0 || offset.vertical < 0)
            return failed(AVERROR_INVALIDDATA);
        const AVFrame *frame = frames[offset.idx].get();
        if (offset.horizontal + frame->width > grid->coded_width ||
            offset.vertical + frame->height > grid->coded_height)
            return failed(AVERROR_INVALIDDATA);
        scale.reset(sws_getCachedContext(
            scale.release(), frame->width, frame->height,
            scalePixelFormat(static_cast<AVPixelFormat>(frame->format)), frame->width,
            frame->height, AV_PIX_FMT_RGBA, SWS_BICUBIC, nullptr, nullptr, nullptr));
        if (!scale)
            return failed(AVERROR(ENOSYS));
        if (configureColor(scale.get(), frame) < 0)
            return failed(AVERROR(EINVAL));
        uint8_t *target[] = {
            canvas.get() +
                (static_cast<size_t>(offset.vertical) * grid->coded_width + offset.horizontal) * 4,
            nullptr, nullptr, nullptr};
        const int lines[] = {grid->coded_width * 4, 0, 0, 0};
        status =
            sws_scale(scale.get(), frame->data, frame->linesize, 0, frame->height, target, lines);
        if (status != frame->height)
            return failed(status < 0 ? status : AVERROR_INVALIDDATA);
    }

    const size_t croppedBytes = static_cast<size_t>(grid->width) * grid->height * 4;
    auto cropped = std::unique_ptr<unsigned char, decltype(&std::free)>(
        static_cast<unsigned char *>(std::malloc(croppedBytes)), &std::free);
    if (!cropped)
        return failed(AVERROR(ENOMEM));
    for (int row = 0; row < grid->height; ++row)
        std::memcpy(cropped.get() + static_cast<size_t>(row) * grid->width * 4,
                    canvas.get() +
                        (static_cast<size_t>(row + grid->vertical_offset) * grid->coded_width +
                         grid->horizontal_offset) *
                            4,
                    static_cast<size_t>(grid->width) * 4);
    const int rotation = displayRotation(grid->coded_side_data, grid->nb_coded_side_data);
    MaiImageDecodeResult image =
        resizeRgba(cropped.get(), grid->width, grid->height, maxWidth, maxHeight, rotation);
    const AVPacketSideData *icc = packetIcc(grid->coded_side_data, grid->nb_coded_side_data);
    const AVCodecParameters *codec = group->streams[0]->codecpar;
    return attachColorMetadata(image, icc ? icc->data : nullptr, icc ? icc->size : 0,
                               codec->color_primaries, codec->color_trc);
}

} // namespace

extern "C" MaiImageDecodeResult maiImageDecodeFile(const char *path, int max_width,
                                                   int max_height) {
    if (!path || !*path || max_width < 0 || max_height < 0)
        return failed(AVERROR(EINVAL));

    AVFormatContext *rawFormat = nullptr;
    int status = avformat_open_input(&rawFormat, path, nullptr, nullptr);
    if (status < 0)
        return failed(status);
    auto closeFormat = [](AVFormatContext *value) { avformat_close_input(&value); };
    std::unique_ptr<AVFormatContext, decltype(closeFormat)> format(rawFormat, closeFormat);

    status = avformat_find_stream_info(format.get(), nullptr);
    if (status < 0)
        return failed(status);
    const AVStreamGroup *largestGrid = nullptr;
    for (unsigned int index = 0; index < format->nb_stream_groups; ++index) {
        const AVStreamGroup *group = format->stream_groups[index];
        if (group->type != AV_STREAM_GROUP_PARAMS_TILE_GRID || !group->params.tile_grid ||
            group->params.tile_grid->nb_tiles <= 1)
            continue;
        if (!largestGrid || static_cast<long long>(group->params.tile_grid->width) *
                                    group->params.tile_grid->height >
                                static_cast<long long>(largestGrid->params.tile_grid->width) *
                                    largestGrid->params.tile_grid->height)
            largestGrid = group;
    }
    if (largestGrid)
        return decodeTileGrid(format.get(), largestGrid, max_width, max_height);
    const AVCodec *decoder = nullptr;
    const int stream = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (stream < 0 || !decoder)
        return failed(stream < 0 ? stream : AVERROR_DECODER_NOT_FOUND);

    auto freeCodec = [](AVCodecContext *value) { avcodec_free_context(&value); };
    std::unique_ptr<AVCodecContext, decltype(freeCodec)> codec(avcodec_alloc_context3(decoder),
                                                               freeCodec);
    if (!codec)
        return failed(AVERROR(ENOMEM));
    status = avcodec_parameters_to_context(codec.get(), format->streams[stream]->codecpar);
    if (status < 0)
        return failed(status);
    status = avcodec_open2(codec.get(), decoder, nullptr);
    if (status < 0)
        return failed(status);

    auto freePacket = [](AVPacket *value) { av_packet_free(&value); };
    auto freeFrame = [](AVFrame *value) { av_frame_free(&value); };
    std::unique_ptr<AVPacket, decltype(freePacket)> packet(av_packet_alloc(), freePacket);
    std::unique_ptr<AVFrame, decltype(freeFrame)> frame(av_frame_alloc(), freeFrame);
    if (!packet || !frame)
        return failed(AVERROR(ENOMEM));

    bool decoded = false;
    while ((status = av_read_frame(format.get(), packet.get())) >= 0) {
        if (packet->stream_index != stream) {
            av_packet_unref(packet.get());
            continue;
        }
        status = avcodec_send_packet(codec.get(), packet.get());
        av_packet_unref(packet.get());
        if (status < 0)
            return failed(status);
        status = avcodec_receive_frame(codec.get(), frame.get());
        if (status == AVERROR(EAGAIN))
            continue;
        if (status < 0)
            return failed(status);
        decoded = true;
        break;
    }
    if (!decoded) {
        status = avcodec_send_packet(codec.get(), nullptr);
        if (status < 0)
            return failed(status);
        status = avcodec_receive_frame(codec.get(), frame.get());
        if (status < 0)
            return failed(status);
    }

    const int sourceWidth = frame->width;
    const int sourceHeight = frame->height;
    if (sourceWidth < 1 || sourceHeight < 1 ||
        static_cast<long long>(sourceWidth) * sourceHeight > kMaximumPixels)
        return failed(AVERROR(EINVAL));

    const AVFrameSideData *frameMatrix =
        av_frame_get_side_data(frame.get(), AV_FRAME_DATA_DISPLAYMATRIX);
    const int rotation = frameMatrix
                             ? rotationFromMatrix(frameMatrix->data, frameMatrix->size)
                             : displayRotation(format->streams[stream]->codecpar->coded_side_data,
                                               format->streams[stream]->codecpar->nb_coded_side_data);
    const int displayWidth = rotation == 90 || rotation == 270 ? sourceHeight : sourceWidth;
    const int displayHeight = rotation == 90 || rotation == 270 ? sourceWidth : sourceHeight;
    double ratio = 1.0;
    if (max_width > 0)
        ratio = std::min(ratio, static_cast<double>(max_width) / displayWidth);
    if (max_height > 0)
        ratio = std::min(ratio, static_cast<double>(max_height) / displayHeight);
    const int width = std::max(1, static_cast<int>(std::floor(sourceWidth * ratio)));
    const int height = std::max(1, static_cast<int>(std::floor(sourceHeight * ratio)));
    if (static_cast<long long>(width) * height > kMaximumPixels ||
        width > std::numeric_limits<int>::max() / 4)
        return failed(AVERROR(EINVAL));

    auto freeScale = [](SwsContext *value) { sws_freeContext(value); };
    std::unique_ptr<SwsContext, decltype(freeScale)> scale(
        sws_getContext(sourceWidth, sourceHeight,
                       scalePixelFormat(static_cast<AVPixelFormat>(frame->format)), width, height,
                       AV_PIX_FMT_RGBA, SWS_LANCZOS, nullptr, nullptr, nullptr),
        freeScale);
    if (!scale)
        return failed(AVERROR(ENOSYS));
    if (configureColor(scale.get(), frame.get()) < 0)
        return failed(AVERROR(EINVAL));

    const int stride = width * 4;
    auto pixels = std::unique_ptr<unsigned char, decltype(&std::free)>(
        static_cast<unsigned char *>(std::malloc(static_cast<size_t>(stride) * height)),
        &std::free);
    if (!pixels)
        return failed(AVERROR(ENOMEM));
    unsigned char *output[] = {pixels.get(), nullptr, nullptr, nullptr};
    const int outputLines[] = {stride, 0, 0, 0};
    status =
        sws_scale(scale.get(), frame->data, frame->linesize, 0, sourceHeight, output, outputLines);
    if (status != height)
        return failed(status < 0 ? status : AVERROR_INVALIDDATA);
    MaiImageDecodeResult image =
        rotateRgba({pixels.release(), width, height, stride, 0, nullptr, 0, 0, 0}, rotation);
    const AVFrameSideData *frameIcc =
        av_frame_get_side_data(frame.get(), AV_FRAME_DATA_ICC_PROFILE);
    const AVPacketSideData *codedIcc =
        packetIcc(format->streams[stream]->codecpar->coded_side_data,
                  format->streams[stream]->codecpar->nb_coded_side_data);
    const unsigned char *profile = frameIcc ? frameIcc->data : codedIcc ? codedIcc->data : nullptr;
    const size_t profileSize = frameIcc ? frameIcc->size : codedIcc ? codedIcc->size : 0;
    const int primaries = frame->color_primaries != AVCOL_PRI_UNSPECIFIED
                              ? frame->color_primaries
                              : format->streams[stream]->codecpar->color_primaries;
    const int transfer = frame->color_trc != AVCOL_TRC_UNSPECIFIED
                             ? frame->color_trc
                             : format->streams[stream]->codecpar->color_trc;
    return attachColorMetadata(image, profile, profileSize, primaries, transfer);
}

extern "C" void maiImageDecodeFree(void *pixels) { std::free(pixels); }
