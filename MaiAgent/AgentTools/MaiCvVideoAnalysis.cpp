#include "MaiCvVideoAnalysis.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "MaiTool.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace {

std::string ffmpegError(int code) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, message, sizeof(message));
    return message;
}

AVPixelFormat swscaleFormat(AVPixelFormat format) {
    switch (format) {
        case AV_PIX_FMT_YUVJ420P: return AV_PIX_FMT_YUV420P;
        case AV_PIX_FMT_YUVJ422P: return AV_PIX_FMT_YUV422P;
        case AV_PIX_FMT_YUVJ444P: return AV_PIX_FMT_YUV444P;
        case AV_PIX_FMT_YUVJ440P: return AV_PIX_FMT_YUV440P;
        default: return format;
    }
}

bool isFullRange(AVPixelFormat format, AVColorRange range) {
    return range == AVCOL_RANGE_JPEG || format == AV_PIX_FMT_YUVJ420P ||
           format == AV_PIX_FMT_YUVJ422P || format == AV_PIX_FMT_YUVJ444P ||
           format == AV_PIX_FMT_YUVJ440P;
}

struct MaiCvFormatCloser {
    void operator()(AVFormatContext* value) const {
        if (value) avformat_close_input(&value);
    }
};
struct MaiCvCodecCloser {
    void operator()(AVCodecContext* value) const {
        avcodec_free_context(&value);
    }
};
struct MaiCvFrameCloser {
    void operator()(AVFrame* value) const {
        av_frame_free(&value);
    }
};
struct MaiCvPacketCloser {
    void operator()(AVPacket* value) const {
        av_packet_free(&value);
    }
};
struct MaiCvScaleCloser {
    void operator()(SwsContext* value) const {
        sws_freeContext(value);
    }
};

class MaiCvFrameAnalyzer {
public:
    explicit MaiCvFrameAnalyzer(const MaiCvVideoAnalysisOptions& options)
        : mKind(options.kind),
          mSampleIntervalSeconds(options.sampleIntervalSeconds),
          mThreshold(options.threshold),
          mPixelThreshold(options.pixelThreshold),
          mMinimumSegmentSeconds(options.minimumSegmentSeconds) {}

    void accept(const AVFrame* frame, AVRational timeBase, double startTime) {
        ++mDecodedFrames;
        if (frame->best_effort_timestamp == AV_NOPTS_VALUE) return;
        const double seconds =
            std::max(0.0, frame->best_effort_timestamp * av_q2d(timeBase) - startTime);
        mLastFrameSeconds = std::max(mLastFrameSeconds, seconds);
        if (seconds + 0.0001 < mNextSampleSeconds) return;
        mNextSampleSeconds = seconds + mSampleIntervalSeconds;
        ++mSampledFrames;

        cv::Mat bgr(90, 160, CV_8UC3);
        uint8_t* target[] = {bgr.data, nullptr, nullptr, nullptr};
        int stride[] = {static_cast<int>(bgr.step), 0, 0, 0};
        const auto sourceFormat = static_cast<AVPixelFormat>(frame->format);
        mScale.reset(sws_getCachedContext(
            mScale.release(), frame->width, frame->height, swscaleFormat(sourceFormat), bgr.cols,
            bgr.rows, AV_PIX_FMT_BGR24, SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!mScale) throw std::runtime_error("could not scale a video frame");
        const int colorspace =
            frame->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_DEFAULT;
        const int* coefficients = sws_getCoefficients(colorspace);
        if (sws_setColorspaceDetails(mScale.get(), coefficients,
                                     isFullRange(sourceFormat, frame->color_range), coefficients, 1,
                                     0, 1 << 16, 1 << 16) < 0) {
            throw std::runtime_error("could not configure video color range");
        }
        if (sws_scale(mScale.get(), frame->data, frame->linesize, 0, frame->height, target,
                      stride) <= 0) {
            throw std::runtime_error("could not decode video pixels");
        }
        if (mKind == MaiCvVideoAnalysisKind::Scene)
            acceptScene(bgr, seconds);
        else
            acceptMotion(bgr, seconds);
    }

    MaiCvVideoAnalysisResult finish(double duration) {
        if (mSampledFrames == 0) throw std::runtime_error("video has no timestamped frames");
        duration = std::max(duration, mLastFrameSeconds);
        MaiCvVideoAnalysisResult output;
        output.durationSeconds = duration;
        output.sampledFrames = mSampledFrames;
        output.decodedFrames = mDecodedFrames;
        if (mKind == MaiCvVideoAnalysisKind::Scene) {
            double start = 0;
            for (const double cut : mCuts) {
                output.segments.push_back({start, cut});
                start = cut;
            }
            output.segments.push_back({start, duration});
        } else {
            for (const MaiCvTimeSegment& segment : mMotion) {
                const double end = std::min(duration, segment.endSeconds);
                if (end - segment.startSeconds >= mMinimumSegmentSeconds) {
                    output.segments.push_back({segment.startSeconds, end});
                }
            }
        }
        return output;
    }

private:
    void acceptScene(const cv::Mat& bgr, double seconds) {
        cv::Mat hsv, histogram;
        cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
        const int channels[] = {0, 1, 2};
        const int bins[] = {16, 4, 4};
        const float hue[] = {0, 180};
        const float saturation[] = {0, 256};
        const float value[] = {0, 256};
        const float* ranges[] = {hue, saturation, value};
        cv::calcHist(&hsv, 1, channels, cv::Mat(), histogram, 3, bins, ranges);
        cv::normalize(histogram, histogram, 1.0, 0.0, cv::NORM_L1);
        if (!mPrevious.empty() &&
            cv::compareHist(mPrevious, histogram, cv::HISTCMP_BHATTACHARYYA) >= mThreshold &&
            seconds - mLastCutSeconds >= mMinimumSegmentSeconds) {
            mCuts.push_back(seconds);
            mLastCutSeconds = seconds;
        }
        mPrevious = histogram;
    }

    void acceptMotion(const cv::Mat& bgr, double seconds) {
        cv::Mat gray, difference, mask;
        cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
        cv::GaussianBlur(gray, gray, cv::Size(5, 5), 0);
        if (!mPrevious.empty()) {
            cv::absdiff(mPrevious, gray, difference);
            cv::threshold(difference, mask, mPixelThreshold, 255, cv::THRESH_BINARY);
            cv::morphologyEx(mask, mask, cv::MORPH_OPEN,
                             cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)));
            const double ratio = static_cast<double>(cv::countNonZero(mask)) / mask.total();
            if (ratio >= mThreshold) {
                const double start = std::max(0.0, seconds - mSampleIntervalSeconds);
                const double end = seconds + mSampleIntervalSeconds;
                if (!mMotion.empty() &&
                    start <= mMotion.back().endSeconds + mSampleIntervalSeconds * 0.5) {
                    mMotion.back().endSeconds = end;
                } else {
                    mMotion.push_back({start, end});
                }
            }
        }
        mPrevious = gray;
    }

    MaiCvVideoAnalysisKind mKind;
    double mSampleIntervalSeconds;
    double mThreshold;
    int mPixelThreshold;
    double mMinimumSegmentSeconds;
    double mNextSampleSeconds = 0;
    double mLastFrameSeconds = 0;
    double mLastCutSeconds = 0;
    int mSampledFrames = 0;
    int mDecodedFrames = 0;
    cv::Mat mPrevious;
    std::vector<double> mCuts;
    std::vector<MaiCvTimeSegment> mMotion;
    std::unique_ptr<SwsContext, MaiCvScaleCloser> mScale;
};

MaiCvVideoAnalysisResult analyze(const std::string& path, const MaiCvVideoAnalysisOptions& options,
                                 const MaiToolContext& context) {
    AVFormatContext* opened = nullptr;
    int result = avformat_open_input(&opened, path.c_str(), nullptr, nullptr);
    if (result < 0) throw std::runtime_error("could not open video: " + ffmpegError(result));
    std::unique_ptr<AVFormatContext, MaiCvFormatCloser> format(opened);
    result = avformat_find_stream_info(format.get(), nullptr);
    if (result < 0)
        throw std::runtime_error("could not read video streams: " + ffmpegError(result));
    const int streamIndex =
        av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (streamIndex < 0) throw std::runtime_error("file has no decodable video stream");
    AVStream* stream = format->streams[streamIndex];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) throw std::runtime_error("video codec is unavailable");
    std::unique_ptr<AVCodecContext, MaiCvCodecCloser> codec(avcodec_alloc_context3(decoder));
    if (!codec) throw std::runtime_error("could not allocate video decoder");
    result = avcodec_parameters_to_context(codec.get(), stream->codecpar);
    if (result >= 0) result = avcodec_open2(codec.get(), decoder, nullptr);
    if (result < 0)
        throw std::runtime_error("could not start video decoder: " + ffmpegError(result));
    std::unique_ptr<AVFrame, MaiCvFrameCloser> frame(av_frame_alloc());
    std::unique_ptr<AVPacket, MaiCvPacketCloser> packet(av_packet_alloc());
    if (!frame || !packet) throw std::runtime_error("could not allocate video buffers");

    MaiCvFrameAnalyzer analyzer(options);
    const double startTime =
        stream->start_time == AV_NOPTS_VALUE ? 0.0 : stream->start_time * av_q2d(stream->time_base);
    const auto receive = [&] {
        while ((result = avcodec_receive_frame(codec.get(), frame.get())) >= 0) {
            if (context.isCanceled()) throw std::runtime_error("canceled");
            analyzer.accept(frame.get(), stream->time_base, startTime);
            av_frame_unref(frame.get());
        }
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF)
            throw std::runtime_error("video decode failed: " + ffmpegError(result));
    };
    while ((result = av_read_frame(format.get(), packet.get())) >= 0) {
        if (context.isCanceled()) throw std::runtime_error("canceled");
        if (packet->stream_index == streamIndex) {
            const int sent = avcodec_send_packet(codec.get(), packet.get());
            av_packet_unref(packet.get());
            if (sent < 0) throw std::runtime_error("video decode failed: " + ffmpegError(sent));
            receive();
        } else {
            av_packet_unref(packet.get());
        }
    }
    if (result != AVERROR_EOF)
        throw std::runtime_error("video read failed: " + ffmpegError(result));
    result = avcodec_send_packet(codec.get(), nullptr);
    if (result >= 0) receive();
    const double duration =
        format->duration > 0 ? format->duration / static_cast<double>(AV_TIME_BASE) : 0.0;
    return analyzer.finish(duration);
}

}  // namespace

MaiCvVideoAnalysisResult analyzeMaiCvVideo(const std::string& path,
                                           const MaiCvVideoAnalysisOptions& options,
                                           const MaiToolContext& context) {
    if (path.empty() ||
        (options.kind != MaiCvVideoAnalysisKind::Scene &&
         options.kind != MaiCvVideoAnalysisKind::Motion) ||
        !std::isfinite(options.sampleIntervalSeconds) || options.sampleIntervalSeconds < 0.1 ||
        options.sampleIntervalSeconds > 5 || !std::isfinite(options.threshold) ||
        options.threshold <= 0 || options.threshold >= 1 || options.pixelThreshold < 1 ||
        options.pixelThreshold > 255 || !std::isfinite(options.minimumSegmentSeconds) ||
        options.minimumSegmentSeconds < 0 || options.minimumSegmentSeconds > 30) {
        MaiCvVideoAnalysisResult result;
        result.error = "invalid video analysis parameters";
        return result;
    }
    if (context.isCanceled()) {
        MaiCvVideoAnalysisResult result;
        result.error = "canceled";
        return result;
    }
    try {
        return analyze(path, options, context);
    } catch (const std::exception& failure) {
        MaiCvVideoAnalysisResult result;
        result.error = failure.what();
        return result;
    } catch (...) {
        MaiCvVideoAnalysisResult result;
        result.error = "unknown video analysis error";
        return result;
    }
}
