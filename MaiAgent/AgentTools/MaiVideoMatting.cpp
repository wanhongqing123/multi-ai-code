#include "MaiVideoMatting.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiRvmComposite.h"
#include "MaiRvmMatting.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace {

std::string ffmpegError(int code) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, message, sizeof(message));
    return message;
}

void requireFfmpeg(int status, const char* operation) {
    if (status < 0) throw std::runtime_error(std::string(operation) + ": " + ffmpegError(status));
}

AVPixelFormat sourceFormat(AVPixelFormat format) {
    switch (format) {
        case AV_PIX_FMT_YUVJ420P: return AV_PIX_FMT_YUV420P;
        case AV_PIX_FMT_YUVJ422P: return AV_PIX_FMT_YUV422P;
        case AV_PIX_FMT_YUVJ444P: return AV_PIX_FMT_YUV444P;
        case AV_PIX_FMT_YUVJ440P: return AV_PIX_FMT_YUV440P;
        default: return format;
    }
}

class MaiMattingPipeline {
public:
    MaiMattingPipeline(const std::string& inputPath, const std::string& outputPath,
                       const MaiVideoMattingOptions& options, const MaiToolContext& context)
        : mInputPath(inputPath), mOutputPath(outputPath), mOptions(options), mContext(context) {}

    ~MaiMattingPipeline() {
        if (mVideoPacket != nullptr) av_packet_free(&mVideoPacket);
        if (mInputPacket != nullptr) av_packet_free(&mInputPacket);
        if (mEncodedFrame != nullptr) av_frame_free(&mEncodedFrame);
        if (mDecodedFrame != nullptr) av_frame_free(&mDecodedFrame);
        if (mDecoder != nullptr) avcodec_free_context(&mDecoder);
        if (mEncoder != nullptr) avcodec_free_context(&mEncoder);
        if (mDecodeScale != nullptr) sws_freeContext(mDecodeScale);
        if (mEncodeScale != nullptr) sws_freeContext(mEncodeScale);
        if (mOutput != nullptr) {
            if (mOutput->pb != nullptr) avio_closep(&mOutput->pb);
            avformat_free_context(mOutput);
        }
        if (mInput != nullptr) avformat_close_input(&mInput);
        if (mOutputCreated && !mComplete &&
            MaiFileSystem::exists(MaiFilePath::fromUtf8(mOutputPath)))
            (void)MaiFileSystem::removeFile(MaiFilePath::fromUtf8(mOutputPath));
    }

    MaiVideoMattingResult run() {
        MaiVideoMattingResult result;
        result.outputPath = mOutputPath;
        try {
            initialize();
            while (true) {
                if (mContext.isCanceled()) throw std::runtime_error("canceled");
                const int read = av_read_frame(mInput, mInputPacket);
                if (read == AVERROR_EOF) break;
                requireFfmpeg(read, "read input packet");
                if (mInputPacket->stream_index == mVideoIndex) {
                    requireFfmpeg(avcodec_send_packet(mDecoder, mInputPacket), "send video packet");
                    drainDecoder(result);
                } else if (mInputPacket->stream_index < static_cast<int>(mAudioMap.size()) &&
                           mAudioMap[mInputPacket->stream_index] >= 0) {
                    const AVStream* inputStream = mInput->streams[mInputPacket->stream_index];
                    const int outputIndex = mAudioMap[mInputPacket->stream_index];
                    if (mInputVideo->start_time != AV_NOPTS_VALUE) {
                        const int64_t videoStart =
                            av_rescale_q(mInputVideo->start_time, mInputVideo->time_base,
                                         inputStream->time_base);
                        if (mInputPacket->pts != AV_NOPTS_VALUE) mInputPacket->pts -= videoStart;
                        if (mInputPacket->dts != AV_NOPTS_VALUE) mInputPacket->dts -= videoStart;
                    }
                    av_packet_rescale_ts(mInputPacket, inputStream->time_base,
                                         mOutput->streams[outputIndex]->time_base);
                    mInputPacket->stream_index = outputIndex;
                    mInputPacket->pos = -1;
                    requireFfmpeg(av_interleaved_write_frame(mOutput, mInputPacket),
                                  "copy original audio packet");
                }
                av_packet_unref(mInputPacket);
            }
            requireFfmpeg(avcodec_send_packet(mDecoder, nullptr), "flush video decoder");
            drainDecoder(result);
            requireFfmpeg(avcodec_send_frame(mEncoder, nullptr), "flush video encoder");
            drainEncoder();
            requireFfmpeg(av_write_trailer(mOutput), "finish output video");
            if (result.frames == 0) throw std::runtime_error("video contains no decodable frames");
            mComplete = true;
            result.audioStreamsCopied = mAudioStreamsCopied;
            result.encoderName = mEncoderName;
        } catch (const std::exception& error) {
            result.error = error.what();
            result.outputPath.clear();
        }
        return result;
    }

private:
    void initialize() {
        auto opened = MaiRvmMattingSession::open(mOptions.modelPath, mOptions.runtimePath,
                                                 mOptions.ortApiBase);
        if (!opened) throw std::runtime_error(opened.error().message());
        mMatting = std::move(opened.value());
        requireFfmpeg(avformat_open_input(&mInput, mInputPath.c_str(), nullptr, nullptr),
                      "open input video");
        requireFfmpeg(avformat_find_stream_info(mInput, nullptr), "read input streams");
        mVideoIndex = av_find_best_stream(mInput, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        requireFfmpeg(mVideoIndex, "find video stream");
        mInputVideo = mInput->streams[mVideoIndex];
        const AVCodec* decoder = avcodec_find_decoder(mInputVideo->codecpar->codec_id);
        if (decoder == nullptr) throw std::runtime_error("input video decoder is unavailable");
        mDecoder = avcodec_alloc_context3(decoder);
        if (mDecoder == nullptr) throw std::runtime_error("allocate video decoder failed");
        requireFfmpeg(avcodec_parameters_to_context(mDecoder, mInputVideo->codecpar),
                      "prepare video decoder");
        requireFfmpeg(avcodec_open2(mDecoder, decoder, nullptr), "start video decoder");
        mWidth = mDecoder->width;
        mHeight = mDecoder->height;
        if (mWidth <= 0 || mHeight <= 0 || mWidth > 3840 || mHeight > 2160 ||
            static_cast<std::uint64_t>(mWidth) * mHeight > 3840ULL * 2160ULL)
            throw std::runtime_error("video dimensions must be between 1 pixel and 4K");
        mOutputWidth = (mWidth + 1) & ~1;
        mOutputHeight = (mHeight + 1) & ~1;
        mPixels = static_cast<std::size_t>(mWidth) * mHeight;

        requireFfmpeg(
            avformat_alloc_output_context2(&mOutput, nullptr, nullptr, mOutputPath.c_str()),
            "create output container");
        if (mOutput == nullptr) throw std::runtime_error("output container is unavailable");
        const AVCodec* encoder = avcodec_find_encoder_by_name("libx264");
        if (encoder == nullptr)
            throw std::runtime_error("libx264 is required for video matting output");
        mEncoder = avcodec_alloc_context3(encoder);
        if (mEncoder == nullptr) throw std::runtime_error("allocate video encoder failed");
        mEncoder->width = mOutputWidth;
        mEncoder->height = mOutputHeight;
        mEncoder->pix_fmt = AV_PIX_FMT_YUV420P;
        mEncoder->time_base = mInputVideo->time_base.num > 0 && mInputVideo->time_base.den > 0
                                  ? mInputVideo->time_base
                                  : AVRational{1, 90000};
        mEncoder->framerate = av_guess_frame_rate(mInput, mInputVideo, nullptr);
        if (mEncoder->framerate.num <= 0) mEncoder->framerate = AVRational{30, 1};
        mEncoder->color_primaries = mDecoder->color_primaries;
        mEncoder->color_trc = mDecoder->color_trc;
        mEncoder->colorspace = mDecoder->colorspace;
        mEncoder->color_range = AVCOL_RANGE_MPEG;
        mEncoder->gop_size = mEncoder->framerate.num / mEncoder->framerate.den * 2;
        mEncoder->max_b_frames = 0;
        if (mOutput->oformat->flags & AVFMT_GLOBALHEADER)
            mEncoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        av_opt_set(mEncoder->priv_data, "preset", "veryfast", 0);
        av_opt_set(mEncoder->priv_data, "crf", "20", 0);
        requireFfmpeg(avcodec_open2(mEncoder, encoder, nullptr), "start H.264 encoder");
        mEncoderName = encoder->name;
        mOutputVideo = avformat_new_stream(mOutput, nullptr);
        if (mOutputVideo == nullptr) throw std::runtime_error("create output video stream failed");
        mOutputVideo->time_base = mEncoder->time_base;
        requireFfmpeg(avcodec_parameters_from_context(mOutputVideo->codecpar, mEncoder),
                      "set output video parameters");
        copyOrientation();

        mAudioMap.assign(mInput->nb_streams, -1);
        for (unsigned int index = 0; index < mInput->nb_streams; ++index) {
            const AVStream* original = mInput->streams[index];
            if (original->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
            if (avformat_query_codec(mOutput->oformat, original->codecpar->codec_id,
                                     FF_COMPLIANCE_NORMAL) == 0)
                throw std::runtime_error(
                    "original audio codec cannot be copied to this container; choose MKV output");
            AVStream* copied = avformat_new_stream(mOutput, nullptr);
            if (copied == nullptr) throw std::runtime_error("create output audio stream failed");
            requireFfmpeg(avcodec_parameters_copy(copied->codecpar, original->codecpar),
                          "copy original audio parameters");
            copied->codecpar->codec_tag = 0;
            copied->time_base = original->time_base;
            mAudioMap[index] = copied->index;
            ++mAudioStreamsCopied;
        }
        if (!(mOutput->oformat->flags & AVFMT_NOFILE)) {
            requireFfmpeg(avio_open(&mOutput->pb, mOutputPath.c_str(), AVIO_FLAG_WRITE),
                          "open output file");
            mOutputCreated = true;
        }
        requireFfmpeg(avformat_write_header(mOutput, nullptr), "write output header");

        mDecodedFrame = av_frame_alloc();
        mEncodedFrame = av_frame_alloc();
        mInputPacket = av_packet_alloc();
        mVideoPacket = av_packet_alloc();
        if (!mDecodedFrame || !mEncodedFrame || !mInputPacket || !mVideoPacket)
            throw std::runtime_error("allocate video buffers failed");
        mEncodedFrame->format = AV_PIX_FMT_YUV420P;
        mEncodedFrame->width = mOutputWidth;
        mEncodedFrame->height = mOutputHeight;
        requireFfmpeg(av_frame_get_buffer(mEncodedFrame, 32), "allocate encoded frame");
        mSourceRgb.resize(mPixels * 3);
        mModelRgb.resize(mPixels * 3);
        mBackgroundRgb.resize(mPixels * 3);
        mCompositedRgb.resize(mPixels * 3);
        mPaddedRgb.resize(static_cast<std::size_t>(mOutputWidth) * mOutputHeight * 3);
        prepareBackground();
    }

    void copyOrientation() {
        const AVCodecParameters* source = mInputVideo->codecpar;
        const AVPacketSideData* matrix = av_packet_side_data_get(
            source->coded_side_data, source->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
        if (matrix == nullptr) return;
        AVCodecParameters* destination = mOutputVideo->codecpar;
        AVPacketSideData* copy =
            av_packet_side_data_new(&destination->coded_side_data, &destination->nb_coded_side_data,
                                    AV_PKT_DATA_DISPLAYMATRIX, matrix->size, 0);
        if (copy == nullptr) throw std::runtime_error("copy video orientation failed");
        std::memcpy(copy->data, matrix->data, matrix->size);
    }

    void prepareBackground() {
        if (mOptions.backgroundKind == MaiMattingBackgroundKind::Solid) {
            for (std::size_t pixel = 0; pixel < mPixels; ++pixel) {
                for (std::size_t channel = 0; channel < 3; ++channel)
                    mBackgroundRgb[pixel * 3 + channel] = mOptions.solidRgb[channel];
            }
            return;
        }
        if (mOptions.backgroundKind == MaiMattingBackgroundKind::Image) {
            std::string bytes;
            bool truncated = false;
            const MaiError read =
                MaiFileSystem::readFile(MaiFilePath::fromUtf8(mOptions.backgroundImagePath), bytes,
                                        50ULL * 1024 * 1024, &truncated);
            if (read || truncated || bytes.empty())
                throw std::runtime_error("background image is missing or exceeds 50 MB");
            cv::Mat encoded(1, static_cast<int>(bytes.size()), CV_8UC1, bytes.data());
            cv::Mat image = cv::imdecode(encoded, cv::IMREAD_COLOR);
            if (image.empty()) throw std::runtime_error("background image cannot be decoded");
            cv::Mat resized(mHeight, mWidth, CV_8UC3, mBackgroundRgb.data());
            cv::resize(image, resized, resized.size(), 0, 0, cv::INTER_AREA);
            cv::cvtColor(resized, resized, cv::COLOR_BGR2RGB);
        }
    }

    void drainDecoder(MaiVideoMattingResult& result) {
        while (true) {
            const int decoded = avcodec_receive_frame(mDecoder, mDecodedFrame);
            if (decoded == AVERROR(EAGAIN) || decoded == AVERROR_EOF) break;
            requireFfmpeg(decoded, "decode video frame");
            if (mContext.isCanceled()) throw std::runtime_error("canceled");
            processFrame(result);
            av_frame_unref(mDecodedFrame);
        }
    }

    void processFrame(MaiVideoMattingResult& result) {
        if (mDecodedFrame->width != mWidth || mDecodedFrame->height != mHeight)
            throw std::runtime_error("video resolution changes during playback");
        const AVPixelFormat format = static_cast<AVPixelFormat>(mDecodedFrame->format);
        mDecodeScale = sws_getCachedContext(mDecodeScale, mWidth, mHeight, sourceFormat(format),
                                            mWidth, mHeight, AV_PIX_FMT_RGB24, SWS_BILINEAR,
                                            nullptr, nullptr, nullptr);
        if (mDecodeScale == nullptr) throw std::runtime_error("convert decoded pixels failed");
        const int colorspace =
            mDecodedFrame->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_DEFAULT;
        const int* coefficients = sws_getCoefficients(colorspace);
        requireFfmpeg(sws_setColorspaceDetails(mDecodeScale, coefficients,
                                               mDecodedFrame->color_range == AVCOL_RANGE_JPEG ||
                                                   format != sourceFormat(format),
                                               coefficients, 1, 0, 1 << 16, 1 << 16),
                      "configure input color range");
        std::uint8_t* rgbData[] = {mSourceRgb.data(), nullptr, nullptr, nullptr};
        int rgbStride[] = {mWidth * 3, 0, 0, 0};
        requireFfmpeg(sws_scale(mDecodeScale, mDecodedFrame->data, mDecodedFrame->linesize, 0,
                                mHeight, rgbData, rgbStride),
                      "convert input frame to RGB");
        for (std::size_t pixel = 0; pixel < mPixels; ++pixel) {
            for (std::size_t channel = 0; channel < 3; ++channel)
                mModelRgb[channel * mPixels + pixel] = mSourceRgb[pixel * 3 + channel] / 255.0f;
        }
        auto predicted =
            mMatting->infer(mModelRgb.data(), mWidth, mHeight, mOptions.downsampleRatio);
        if (!predicted) throw std::runtime_error(predicted.error().message());
        if (mOptions.backgroundKind == MaiMattingBackgroundKind::Blur) {
            cv::Mat original(mHeight, mWidth, CV_8UC3, mSourceRgb.data());
            cv::Mat blurred(mHeight, mWidth, CV_8UC3, mBackgroundRgb.data());
            cv::GaussianBlur(original, blurred, cv::Size(), mOptions.blurSigma);
        }
        maiRvmCompositeRgb8(mSourceRgb.data(), predicted.value().foregroundRgbPlanar.data(),
                            predicted.value().alpha.data(), mBackgroundRgb.data(),
                            mCompositedRgb.data(), mPixels);
        if (mWidth == mOutputWidth && mHeight == mOutputHeight) {
            std::memcpy(mPaddedRgb.data(), mCompositedRgb.data(), mCompositedRgb.size());
        } else {
            for (int row = 0; row < mOutputHeight; ++row) {
                const int sourceRow = std::min(row, mHeight - 1);
                for (int column = 0; column < mOutputWidth; ++column) {
                    const int sourceColumn = std::min(column, mWidth - 1);
                    std::memcpy(mPaddedRgb.data() + (row * mOutputWidth + column) * 3,
                                mCompositedRgb.data() + (sourceRow * mWidth + sourceColumn) * 3, 3);
                }
            }
        }
        mEncodeScale = sws_getCachedContext(
            mEncodeScale, mOutputWidth, mOutputHeight, AV_PIX_FMT_RGB24, mOutputWidth,
            mOutputHeight, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (mEncodeScale == nullptr) throw std::runtime_error("convert output pixels failed");
        const int* outputCoefficients = sws_getCoefficients(
            mDecodedFrame->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_DEFAULT);
        requireFfmpeg(sws_setColorspaceDetails(mEncodeScale, outputCoefficients, 1,
                                               outputCoefficients, 0, 0, 1 << 16, 1 << 16),
                      "configure output color range");
        requireFfmpeg(av_frame_make_writable(mEncodedFrame), "prepare encoded frame");
        const std::uint8_t* source[] = {mPaddedRgb.data(), nullptr, nullptr, nullptr};
        int stride[] = {mOutputWidth * 3, 0, 0, 0};
        requireFfmpeg(sws_scale(mEncodeScale, source, stride, 0, mOutputHeight, mEncodedFrame->data,
                                mEncodedFrame->linesize),
                      "convert composited frame to YUV");
        const int64_t sourcePts = mDecodedFrame->best_effort_timestamp;
        const int64_t start =
            mInputVideo->start_time == AV_NOPTS_VALUE ? 0 : mInputVideo->start_time;
        const int64_t encodedPts =
            sourcePts == AV_NOPTS_VALUE
                ? av_rescale_q(result.frames, av_inv_q(mEncoder->framerate), mEncoder->time_base)
                : av_rescale_q(sourcePts - start, mInputVideo->time_base, mEncoder->time_base);
        mEncodedFrame->pts = std::max(mPreviousPts + 1, encodedPts);
        mPreviousPts = mEncodedFrame->pts;
        requireFfmpeg(avcodec_send_frame(mEncoder, mEncodedFrame), "encode composited frame");
        drainEncoder();
        ++result.frames;
        result.durationSeconds =
            std::max(result.durationSeconds, mEncodedFrame->pts * av_q2d(mEncoder->time_base) +
                                                 av_q2d(av_inv_q(mEncoder->framerate)));
    }

    void drainEncoder() {
        while (true) {
            const int encoded = avcodec_receive_packet(mEncoder, mVideoPacket);
            if (encoded == AVERROR(EAGAIN) || encoded == AVERROR_EOF) break;
            requireFfmpeg(encoded, "read encoded packet");
            av_packet_rescale_ts(mVideoPacket, mEncoder->time_base, mOutputVideo->time_base);
            mVideoPacket->stream_index = mOutputVideo->index;
            mVideoPacket->pos = -1;
            requireFfmpeg(av_interleaved_write_frame(mOutput, mVideoPacket),
                          "write composited video packet");
            av_packet_unref(mVideoPacket);
        }
    }

    std::string mInputPath;
    std::string mOutputPath;
    std::string mEncoderName;
    MaiVideoMattingOptions mOptions;
    const MaiToolContext& mContext;
    bool mComplete = false;
    bool mOutputCreated = false;
    AVFormatContext* mInput = nullptr;
    AVFormatContext* mOutput = nullptr;
    AVCodecContext* mDecoder = nullptr;
    AVCodecContext* mEncoder = nullptr;
    AVStream* mInputVideo = nullptr;
    AVStream* mOutputVideo = nullptr;
    AVFrame* mDecodedFrame = nullptr;
    AVFrame* mEncodedFrame = nullptr;
    AVPacket* mInputPacket = nullptr;
    AVPacket* mVideoPacket = nullptr;
    SwsContext* mDecodeScale = nullptr;
    SwsContext* mEncodeScale = nullptr;
    std::unique_ptr<MaiRvmMattingSession> mMatting;
    std::vector<int> mAudioMap;
    std::vector<std::uint8_t> mSourceRgb;
    std::vector<std::uint8_t> mBackgroundRgb;
    std::vector<std::uint8_t> mCompositedRgb;
    std::vector<std::uint8_t> mPaddedRgb;
    std::vector<float> mModelRgb;
    std::size_t mPixels = 0;
    int mVideoIndex = -1;
    int mAudioStreamsCopied = 0;
    int mWidth = 0;
    int mHeight = 0;
    int mOutputWidth = 0;
    int mOutputHeight = 0;
    int64_t mPreviousPts = -1;
};

}  // namespace

MaiVideoMattingResult maiMatteVideo(const std::string& inputPath, const std::string& outputPath,
                                    const MaiVideoMattingOptions& options,
                                    const MaiToolContext& context) {
    if (inputPath.empty() || outputPath.empty() || inputPath == outputPath ||
        MaiFileSystem::exists(MaiFilePath::fromUtf8(outputPath))) {
        MaiVideoMattingResult result;
        result.error = "output must be a new file distinct from the input";
        return result;
    }
    MaiMattingPipeline pipeline(inputPath, outputPath, options, context);
    return pipeline.run();
}
