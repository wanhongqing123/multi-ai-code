#include "MaiTemporalWatermark.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <onnxruntime_c_api.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace {

constexpr int kModelSize = 512;
constexpr int kInpaintStride = 30;
constexpr double kMaximumDurationSeconds = 30.0;

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

class MaiLamaSession {
public:
    explicit MaiLamaSession(const MaiTemporalWatermarkOptions& config) {
        try {
            if (config.ortApiBase == nullptr)
                throw std::runtime_error("ONNX Runtime is not configured by the host");
            const auto* base = static_cast<const OrtApiBase*>(config.ortApiBase);
            mApi = base->GetApi(ORT_API_VERSION);
            if (mApi == nullptr)
                throw std::runtime_error("ONNX Runtime API version is unsupported");
            check(mApi->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "MaiWatermarkRepair", &mEnvironment),
                  "create ONNX environment");
            check(mApi->CreateSessionOptions(&mOptions), "create ONNX session options");
            const MaiFilePath model = MaiFilePath::fromUtf8(config.modelPath);
            if (!MaiFileSystem::exists(model) || MaiFileSystem::isDirectory(model))
                throw std::runtime_error("LaMa ONNX model is missing");
            check(mApi->CreateSession(mEnvironment, model.value().c_str(), mOptions, &mSession),
                  "load LaMa ONNX model");
            check(mApi->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mMemory),
                  "create ONNX tensor memory");
        } catch (...) {
            release();
            throw;
        }
    }

    ~MaiLamaSession() {
        release();
    }

    void release() {
        if (mApi == nullptr) return;
        if (mMemory != nullptr) mApi->ReleaseMemoryInfo(mMemory);
        if (mSession != nullptr) mApi->ReleaseSession(mSession);
        if (mOptions != nullptr) mApi->ReleaseSessionOptions(mOptions);
        if (mEnvironment != nullptr) mApi->ReleaseEnv(mEnvironment);
        mMemory = nullptr;
        mSession = nullptr;
        mOptions = nullptr;
        mEnvironment = nullptr;
    }

    MaiLamaSession(const MaiLamaSession&) = delete;
    MaiLamaSession& operator=(const MaiLamaSession&) = delete;

    cv::Mat infer(const cv::Mat& image, const cv::Mat& missingMask) const {
        if (image.type() != CV_8UC3 || missingMask.type() != CV_8UC1 || image.cols != kModelSize ||
            image.rows != kModelSize || missingMask.size() != image.size())
            throw std::runtime_error("LaMa expects 512 by 512 RGB and grayscale mask");
        constexpr std::size_t pixels = kModelSize * kModelSize;
        std::vector<float> rgb(pixels * 3);
        std::vector<float> mask(pixels);
        for (int row = 0; row < kModelSize; ++row) {
            const auto* source = image.ptr<cv::Vec3b>(row);
            const auto* marking = missingMask.ptr<std::uint8_t>(row);
            for (int column = 0; column < kModelSize; ++column) {
                const std::size_t pixel = static_cast<std::size_t>(row) * kModelSize + column;
                for (std::size_t channel = 0; channel < 3; ++channel)
                    rgb[channel * pixels + pixel] = source[column][channel] / 255.0f;
                mask[pixel] = marking[column] / 255.0f;
            }
        }
        constexpr std::array<std::int64_t, 4> imageShape = {1, 3, kModelSize, kModelSize};
        constexpr std::array<std::int64_t, 4> maskShape = {1, 1, kModelSize, kModelSize};
        constexpr std::array<const char*, 2> inputNames = {"image", "mask"};
        constexpr std::array<const char*, 1> outputNames = {"output"};
        OrtValue* values[2] = {nullptr, nullptr};
        OrtValue* output = nullptr;
        try {
            check(mApi->CreateTensorWithDataAsOrtValue(
                      mMemory, rgb.data(), rgb.size() * sizeof(float), imageShape.data(),
                      imageShape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &values[0]),
                  "create LaMa image tensor");
            check(mApi->CreateTensorWithDataAsOrtValue(
                      mMemory, mask.data(), mask.size() * sizeof(float), maskShape.data(),
                      maskShape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &values[1]),
                  "create LaMa mask tensor");
            const std::array<const OrtValue*, 2> inputs = {values[0], values[1]};
            check(mApi->Run(mSession, nullptr, inputNames.data(), inputs.data(), inputs.size(),
                            outputNames.data(), 1, &output),
                  "run LaMa inpainting");
            float* prediction = nullptr;
            check(mApi->GetTensorMutableData(output, reinterpret_cast<void**>(&prediction)),
                  "read LaMa output");
            if (prediction == nullptr) throw std::runtime_error("LaMa output is empty");
            cv::Mat result(kModelSize, kModelSize, CV_8UC3);
            for (int row = 0; row < kModelSize; ++row) {
                auto* target = result.ptr<cv::Vec3b>(row);
                for (int column = 0; column < kModelSize; ++column) {
                    const std::size_t pixel = static_cast<std::size_t>(row) * kModelSize + column;
                    for (std::size_t channel = 0; channel < 3; ++channel)
                        target[column][channel] =
                            cv::saturate_cast<std::uint8_t>(prediction[channel * pixels + pixel]);
                }
            }
            mApi->ReleaseValue(output);
            mApi->ReleaseValue(values[1]);
            mApi->ReleaseValue(values[0]);
            return result;
        } catch (...) {
            if (output != nullptr) mApi->ReleaseValue(output);
            for (OrtValue* value : values) {
                if (value != nullptr) mApi->ReleaseValue(value);
            }
            throw;
        }
    }

private:
    void check(OrtStatus* status, const char* action) const {
        if (status == nullptr) return;
        const std::string message = mApi->GetErrorMessage(status);
        mApi->ReleaseStatus(status);
        throw std::runtime_error(std::string(action) + ": " + message);
    }

    const OrtApi* mApi = nullptr;
    OrtEnv* mEnvironment = nullptr;
    OrtSessionOptions* mOptions = nullptr;
    OrtSession* mSession = nullptr;
    OrtMemoryInfo* mMemory = nullptr;
};

struct VideoMetadata {
    int width = 0;
    int height = 0;
    double durationSeconds = 0;
    AVRational timeBase = {1, 30};
    AVRational frameRate = {30, 1};
    int frames = 0;
};

// 第一次扫描只在内存里保留少量参考帧和模型关键帧。原视频可能是 VFR，
// 每帧时刻取解码器 best_effort_timestamp，而不是假设 frameIndex/30。
VideoMetadata decodeVideoFrames(const std::string& path, const MaiToolContext& context,
                                const std::function<void(const cv::Mat&, int, double)>& onFrame) {
    AVFormatContext* input = nullptr;
    AVCodecContext* decoder = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    SwsContext* scale = nullptr;
    try {
        requireFfmpeg(avformat_open_input(&input, path.c_str(), nullptr, nullptr),
                      "open input video");
        requireFfmpeg(avformat_find_stream_info(input, nullptr), "read input streams");
        const int videoIndex = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        requireFfmpeg(videoIndex, "find video stream");
        AVStream* stream = input->streams[videoIndex];
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (codec == nullptr) throw std::runtime_error("video decoder is unavailable");
        decoder = avcodec_alloc_context3(codec);
        if (decoder == nullptr) throw std::runtime_error("allocate video decoder failed");
        requireFfmpeg(avcodec_parameters_to_context(decoder, stream->codecpar),
                      "prepare video decoder");
        requireFfmpeg(avcodec_open2(decoder, codec, nullptr), "start video decoder");
        VideoMetadata metadata;
        metadata.width = decoder->width;
        metadata.height = decoder->height;
        metadata.timeBase = stream->time_base;
        metadata.frameRate = av_guess_frame_rate(input, stream, nullptr);
        if (metadata.frameRate.num <= 0 || metadata.frameRate.den <= 0)
            metadata.frameRate = {30, 1};
        metadata.durationSeconds =
            input->duration == AV_NOPTS_VALUE ? 0 : input->duration * av_q2d(AV_TIME_BASE_Q);
        if (metadata.width < kModelSize || metadata.height < kModelSize || metadata.width > 3840 ||
            metadata.height > 3840 ||
            static_cast<std::uint64_t>(metadata.width) * metadata.height > 3840ULL * 2160ULL ||
            metadata.durationSeconds <= 0 || metadata.durationSeconds > kMaximumDurationSeconds)
            throw std::runtime_error("video must be 512 pixels to 4K and at most 30 seconds");
        if (decoder->pix_fmt == AV_PIX_FMT_YUV420P10LE ||
            decoder->color_trc == AVCOL_TRC_SMPTE2084 ||
            decoder->color_trc == AVCOL_TRC_ARIB_STD_B67)
            throw std::runtime_error("HDR video needs SDR normalization before local repair");
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if (packet == nullptr || frame == nullptr)
            throw std::runtime_error("allocate video buffers failed");
        cv::Mat rgb(metadata.height, metadata.width, CV_8UC3);
        int index = 0;
        const auto drain = [&] {
            while (true) {
                const int status = avcodec_receive_frame(decoder, frame);
                if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) break;
                requireFfmpeg(status, "decode video frame");
                if (context.isCanceled()) throw std::runtime_error("canceled");
                if (frame->width != metadata.width || frame->height != metadata.height)
                    throw std::runtime_error("video resolution changed during processing");
                const auto format = static_cast<AVPixelFormat>(frame->format);
                scale = sws_getCachedContext(
                    scale, metadata.width, metadata.height, sourceFormat(format), metadata.width,
                    metadata.height, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (scale == nullptr) throw std::runtime_error("convert video to RGB failed");
                std::uint8_t* target[] = {rgb.data, nullptr, nullptr, nullptr};
                int stride[] = {static_cast<int>(rgb.step), 0, 0, 0};
                requireFfmpeg(sws_scale(scale, frame->data, frame->linesize, 0, metadata.height,
                                        target, stride),
                              "convert decoded frame to RGB");
                const std::int64_t start =
                    stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
                const double seconds =
                    frame->best_effort_timestamp == AV_NOPTS_VALUE
                        ? index * av_q2d(av_inv_q(metadata.frameRate))
                        : (frame->best_effort_timestamp - start) * av_q2d(stream->time_base);
                onFrame(rgb, index, std::max(0.0, seconds));
                ++index;
                av_frame_unref(frame);
            }
        };
        while (true) {
            const int status = av_read_frame(input, packet);
            if (status == AVERROR_EOF) break;
            requireFfmpeg(status, "read video packet");
            if (packet->stream_index == videoIndex) {
                requireFfmpeg(avcodec_send_packet(decoder, packet), "send video packet");
                drain();
            }
            av_packet_unref(packet);
        }
        requireFfmpeg(avcodec_send_packet(decoder, nullptr), "flush video decoder");
        drain();
        if (index == 0) throw std::runtime_error("video contains no decodable frames");
        metadata.frames = index;
        if (scale != nullptr) sws_freeContext(scale);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
        avformat_close_input(&input);
        return metadata;
    } catch (...) {
        if (scale != nullptr) sws_freeContext(scale);
        if (frame != nullptr) av_frame_free(&frame);
        if (packet != nullptr) av_packet_free(&packet);
        if (decoder != nullptr) avcodec_free_context(&decoder);
        if (input != nullptr) avformat_close_input(&input);
        throw;
    }
}

struct InpaintKeyframe {
    int frameIndex = 0;
    cv::Mat predictedRgb;
};

struct RegionState {
    MaiWatermarkRepairRegion region;
    cv::Rect modelCrop;
    cv::Rect expanded;
    cv::Mat modelMask;
    cv::Mat blendAlpha;
    cv::Mat beforeFrame;
    cv::Mat afterFrame;
    double beforeDistance = std::numeric_limits<double>::infinity();
    double afterDistance = std::numeric_limits<double>::infinity();
    std::vector<InpaintKeyframe> keyframes;
};

cv::Rect modelCropFor(const cv::Rect& rectangle, int width, int height) {
    const int x =
        std::clamp(rectangle.x + rectangle.width / 2 - kModelSize / 2, 0, width - kModelSize);
    const int y =
        std::clamp(rectangle.y + rectangle.height / 2 - kModelSize / 2, 0, height - kModelSize);
    return {x, y, kModelSize, kModelSize};
}

cv::Rect expandedFor(const cv::Rect& rectangle, int width, int height) {
    const int x0 = std::max(0, rectangle.x - 50);
    const int y0 = std::max(0, rectangle.y - 35);
    const int x1 = std::min(width, rectangle.x + rectangle.width + 50);
    const int y1 = std::min(height, rectangle.y + rectangle.height + 35);
    return {x0, y0, x1 - x0, y1 - y0};
}

cv::Mat featherMask(const cv::Rect& rectangle, int width, int height, int feather) {
    cv::Mat alpha(rectangle.height, rectangle.width, CV_32FC1);
    for (int row = 0; row < rectangle.height; ++row) {
        float* target = alpha.ptr<float>(row);
        for (int column = 0; column < rectangle.width; ++column) {
            const int left = rectangle.x == 0 ? feather : column + 1;
            const int top = rectangle.y == 0 ? feather : row + 1;
            const int right =
                rectangle.x + rectangle.width == width ? feather : rectangle.width - column;
            const int bottom =
                rectangle.y + rectangle.height == height ? feather : rectangle.height - row;
            const int distance = std::min({left, top, right, bottom});
            target[column] = std::clamp(static_cast<float>(distance) / feather, 0.0f, 1.0f);
        }
    }
    return alpha;
}

bool isActive(const MaiWatermarkRepairRegion& region, int frameIndex, double seconds) {
    if (region.startFrame >= 0)
        return frameIndex >= region.startFrame &&
               (region.endFrame < 0 || frameIndex < region.endFrame);
    return seconds + 0.0001 >= region.startSeconds &&
           (region.endSeconds == -1 || seconds < region.endSeconds);
}

std::vector<RegionState> prepareRegions(const MaiTemporalWatermarkOptions& options, int width,
                                        int height, double durationSeconds) {
    if (options.regions.empty() || options.regions.size() > 16)
        throw std::runtime_error("provide 1 to 16 watermark regions");
    std::vector<RegionState> states;
    states.reserve(options.regions.size());
    int temporalCount = 0;
    int inpaintCount = 0;
    for (const MaiWatermarkRepairRegion& region : options.regions) {
        const cv::Rect rectangle(region.x, region.y, region.width, region.height);
        if (region.x < 0 || region.y < 0 || region.width < 2 || region.height < 2 ||
            region.width > 400 || region.height > 400 || region.x > width - region.width ||
            region.y > height - region.height || region.startSeconds < 0 ||
            (region.startFrame < 0 && region.startSeconds >= durationSeconds) ||
            (region.endFrame < 0 && region.endSeconds != -1 &&
             (region.endSeconds <= region.startSeconds ||
              region.endSeconds > durationSeconds + 0.1)))
            throw std::runtime_error("watermark region exceeds video frame or duration");
        RegionState state;
        state.region = region;
        state.modelCrop = modelCropFor(rectangle, width, height);
        // 有已核实的无遮挡帧时借背景；没有时在同一流程内用本地模型补画。
        const bool hasCleanReference = region.beforeSeconds >= 0;
        state.expanded = hasCleanReference ? expandedFor(rectangle, width, height) : rectangle;
        state.blendAlpha = featherMask(state.expanded, width, height, hasCleanReference ? 22 : 4);
        state.modelMask = cv::Mat::zeros(kModelSize, kModelSize, CV_8UC1);
        const cv::Rect local(region.x - state.modelCrop.x, region.y - state.modelCrop.y,
                             region.width, region.height);
        state.modelMask(local).setTo(255);
        if (hasCleanReference) {
            ++temporalCount;
            // 帧窗口与秒数窗口二选一；参考帧时间仍须落在片段内。
            if ((region.startFrame < 0 && region.beforeSeconds >= region.startSeconds) ||
                (region.afterSeconds != -1 && region.endFrame < 0 &&
                 (region.endSeconds == -1 || region.afterSeconds <= region.endSeconds)) ||
                region.beforeSeconds >= durationSeconds || region.afterSeconds >= durationSeconds)
                throw std::runtime_error("temporal references are outside valid clean windows");
        } else {
            ++inpaintCount;
        }
        states.push_back(std::move(state));
    }
    if (temporalCount > 8 || inpaintCount > 8)
        throw std::runtime_error("limit processing to 8 temporal and 8 inpaint regions");
    return states;
}

int buildReferenceCache(const std::string& inputPath, const MaiTemporalWatermarkOptions& options,
                        const MaiToolContext& context, MaiLamaSession& model,
                        VideoMetadata& metadata, std::vector<RegionState>& states) {
    int modelCalls = 0;
    bool initialized = false;
    metadata = decodeVideoFrames(
        inputPath, context, [&](const cv::Mat& image, int frameIndex, double seconds) {
            if (!initialized) {
                metadata.width = image.cols;
                metadata.height = image.rows;
                // 时间长度由首次打开的容器获取；回调执行时只知道帧尺寸，
                // 这里先用 30 秒上界校验，扫描结束后再按真实时长复核。
                states = prepareRegions(options, image.cols, image.rows,
                                        kMaximumDurationSeconds + 0.001);
                initialized = true;
            }
            for (RegionState& state : states) {
                if (state.region.beforeSeconds >= 0) {
                    const double before = std::abs(seconds - state.region.beforeSeconds);
                    if (before <= 0.15 && before < state.beforeDistance) {
                        state.beforeFrame = image.clone();
                        state.beforeDistance = before;
                    }
                    if (state.region.afterSeconds >= 0) {
                        const double after = std::abs(seconds - state.region.afterSeconds);
                        if (after <= 0.15 && after < state.afterDistance) {
                            state.afterFrame = image.clone();
                            state.afterDistance = after;
                        }
                    }
                    continue;
                }
                if (!isActive(state.region, frameIndex, seconds)) continue;
                if (!state.keyframes.empty() && frameIndex % kInpaintStride != 0) continue;
                const cv::Mat source = image(state.modelCrop).clone();
                state.keyframes.push_back({frameIndex, model.infer(source, state.modelMask)});
                ++modelCalls;
            }
        });
    for (RegionState& state : states) {
        if (state.region.startFrame >= metadata.frames || state.region.endFrame > metadata.frames ||
            (state.region.startFrame < 0 &&
             state.region.startSeconds >= metadata.durationSeconds) ||
            (state.region.endFrame < 0 && state.region.endSeconds != -1 &&
             state.region.endSeconds > metadata.durationSeconds + 0.1))
            throw std::runtime_error("watermark region exceeds video duration");
        if (state.region.beforeSeconds >= 0) {
            if (state.beforeFrame.empty() ||
                (state.region.afterSeconds >= 0 && state.afterFrame.empty()))
                throw std::runtime_error("clean temporal reference frame is unavailable");
        } else if (state.keyframes.empty()) {
            throw std::runtime_error("inpaint region produced no model keyframes");
        }
    }
    return modelCalls;
}

void blendRegion(cv::Mat& target, const cv::Rect& rectangle, const cv::Mat& filling,
                 const cv::Mat& alpha) {
    if (filling.type() != CV_8UC3 || filling.size() != rectangle.size() ||
        alpha.type() != CV_32FC1 || alpha.size() != rectangle.size())
        throw std::runtime_error("watermark blend buffers have incompatible dimensions");
    for (int row = 0; row < rectangle.height; ++row) {
        auto* destination = target.ptr<cv::Vec3b>(rectangle.y + row) + rectangle.x;
        const auto* source = filling.ptr<cv::Vec3b>(row);
        const float* weights = alpha.ptr<float>(row);
        for (int column = 0; column < rectangle.width; ++column) {
            for (int channel = 0; channel < 3; ++channel) {
                const float weight = weights[column];
                destination[column][channel] =
                    cv::saturate_cast<std::uint8_t>(destination[column][channel] * (1.0f - weight) +
                                                    source[column][channel] * weight);
            }
        }
    }
}

cv::Mat fixedPrediction(const RegionState& state, int frameIndex) {
    const InpaintKeyframe* first = &state.keyframes.front();
    const InpaintKeyframe* second = first;
    for (const InpaintKeyframe& current : state.keyframes) {
        if (current.frameIndex > frameIndex) {
            second = &current;
            break;
        }
        first = &current;
        second = first;
    }
    const cv::Rect local(state.region.x - state.modelCrop.x, state.region.y - state.modelCrop.y,
                         state.region.width, state.region.height);
    const cv::Mat earlier = first->predictedRgb(local);
    if (first == second) return earlier.clone();
    const cv::Mat later = second->predictedRgb(local);
    const double fraction = std::clamp(static_cast<double>(frameIndex - first->frameIndex) /
                                           (second->frameIndex - first->frameIndex),
                                       0.0, 1.0);
    cv::Mat combined;
    cv::addWeighted(earlier, 1.0 - fraction, later, fraction, 0, combined);
    return combined;
}

double referenceFraction(const MaiWatermarkRepairRegion& region, double seconds) {
    if (region.afterSeconds < 0) return 0;
    return std::clamp(
        (seconds - region.beforeSeconds) / (region.afterSeconds - region.beforeSeconds), 0.0, 1.0);
}

std::array<int, 3> ringOffset(const cv::Mat& original, const cv::Mat& before, const cv::Mat& after,
                              const cv::Rect& rectangle, double fraction) {
    std::array<std::vector<int>, 3> differences;
    const auto addBand = [&](const cv::Rect& band) {
        if (band.width <= 0 || band.height <= 0) return;
        for (int row = band.y; row < band.y + band.height; row += 3) {
            const auto* source = original.ptr<cv::Vec3b>(row);
            const auto* earlier = before.ptr<cv::Vec3b>(row);
            const auto* later = after.ptr<cv::Vec3b>(row);
            for (int column = band.x; column < band.x + band.width; column += 3) {
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const int reference =
                        cv::saturate_cast<int>(earlier[column][channel] * (1.0 - fraction) +
                                               later[column][channel] * fraction);
                    differences[channel].push_back(source[column][channel] - reference);
                }
            }
        }
    };
    const int top = std::min(40, rectangle.y);
    const int bottom = std::min(40, original.rows - rectangle.br().y);
    const int left = std::min(40, rectangle.x);
    const int right = std::min(40, original.cols - rectangle.br().x);
    addBand({rectangle.x, rectangle.y - top, rectangle.width, top});
    addBand({rectangle.x, rectangle.br().y, rectangle.width, bottom});
    addBand({rectangle.x - left, rectangle.y, left, rectangle.height});
    addBand({rectangle.br().x, rectangle.y, right, rectangle.height});
    std::array<int, 3> offset{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        auto& values = differences[channel];
        if (values.empty()) continue;
        const auto middle = values.begin() + values.size() / 2;
        std::nth_element(values.begin(), middle, values.end());
        offset[channel] = std::clamp(*middle, -40, 40);
    }
    return offset;
}

cv::Mat temporalPrediction(const cv::Mat& original, const RegionState& state, double seconds) {
    const cv::Mat& before = state.beforeFrame;
    const cv::Mat& after = state.afterFrame.empty() ? state.beforeFrame : state.afterFrame;
    const double fraction = referenceFraction(state.region, seconds);
    cv::Mat result;
    cv::addWeighted(before(state.expanded), 1.0 - fraction, after(state.expanded), fraction, 0,
                    result);
    const auto offset = ringOffset(original, before, after, state.expanded, fraction);
    if (offset != std::array<int, 3>{0, 0, 0})
        cv::add(result, cv::Scalar(offset[0], offset[1], offset[2]), result);
    return result;
}

cv::Mat foregroundAlpha(const cv::Mat& original, const cv::Mat& background,
                        const RegionState& state) {
    cv::Mat alpha = cv::Mat::zeros(state.expanded.size(), CV_8UC1);
    if (state.region.foregroundEdge == MaiWatermarkForegroundEdge::None) return alpha;
    for (int row = 0; row < state.expanded.height; ++row) {
        const auto* source = original.ptr<cv::Vec3b>(state.expanded.y + row) + state.expanded.x;
        const auto* clean = background.ptr<cv::Vec3b>(row);
        auto* target = alpha.ptr<std::uint8_t>(row);
        for (int column = 0; column < state.expanded.width; ++column) {
            const bool nearEdge = state.region.foregroundEdge == MaiWatermarkForegroundEdge::Right
                                      ? column >= state.expanded.width - 190
                                      : column < 190;
            if (!nearEdge) continue;
            const int foreground =
                (source[column][0] * 3 + source[column][1] * 6 + source[column][2]) / 10;
            const int backdrop =
                (clean[column][0] * 3 + clean[column][1] * 6 + clean[column][2]) / 10;
            if (foreground < 115 && backdrop - foreground > 28) target[column] = 255;
        }
    }
    cv::dilate(alpha, alpha, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(15, 15)));
    cv::GaussianBlur(alpha, alpha, cv::Size(), 2);
    return alpha;
}

cv::Mat brightOverlayMask(const cv::Mat& original, const RegionState& state) {
    cv::Mat mask = cv::Mat::zeros(kModelSize, kModelSize, CV_8UC1);
    const cv::Rect local(state.region.x - state.modelCrop.x, state.region.y - state.modelCrop.y,
                         state.region.width, state.region.height);
    for (int row = local.y; row < local.br().y; ++row) {
        const auto* source = original.ptr<cv::Vec3b>(state.modelCrop.y + row) + state.modelCrop.x;
        auto* target = mask.ptr<std::uint8_t>(row);
        for (int column = local.x; column < local.br().x; ++column) {
            const cv::Vec3b& pixel = source[column];
            const bool white = std::min({pixel[0], pixel[1], pixel[2]}) > 155;
            const bool cyan = pixel[1] > 170 && pixel[2] > 165 && pixel[0] > 90;
            if (white || cyan) target[column] = 255;
        }
    }
    cv::dilate(mask, mask, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(11, 11)));
    return mask;
}

bool intersectsForeground(const cv::Mat& foreground, const cv::Mat& glyphs,
                          const RegionState& state) {
    const cv::Rect overlap = state.expanded & state.modelCrop;
    if (overlap.empty()) return false;
    int count = 0;
    for (int row = overlap.y; row < overlap.br().y; ++row) {
        const auto* person = foreground.ptr<std::uint8_t>(row - state.expanded.y);
        const auto* text = glyphs.ptr<std::uint8_t>(row - state.modelCrop.y);
        for (int column = overlap.x; column < overlap.br().x; ++column) {
            if (person[column - state.expanded.x] > 80 && text[column - state.modelCrop.x] > 0)
                ++count;
        }
    }
    return count > 10;
}

void preserveForeground(cv::Mat& output, const cv::Mat& original, const cv::Mat& background,
                        const RegionState& state, MaiLamaSession& model,
                        MaiTemporalWatermarkResult& result) {
    const cv::Mat alpha = foregroundAlpha(original, background, state);
    if (cv::countNonZero(alpha) == 0) return;
    cv::Mat person = original(state.expanded).clone();
    const cv::Mat glyphs = brightOverlayMask(original, state);
    if (intersectsForeground(alpha, glyphs, state)) {
        const cv::Mat predicted = model.infer(original(state.modelCrop).clone(), glyphs);
        const cv::Rect overlap = state.expanded & state.modelCrop;
        for (int row = overlap.y; row < overlap.br().y; ++row) {
            auto* target = person.ptr<cv::Vec3b>(row - state.expanded.y);
            const auto* source = predicted.ptr<cv::Vec3b>(row - state.modelCrop.y);
            const auto* marked = glyphs.ptr<std::uint8_t>(row - state.modelCrop.y);
            for (int column = overlap.x; column < overlap.br().x; ++column) {
                if (marked[column - state.modelCrop.x] > 0)
                    target[column - state.expanded.x] = source[column - state.modelCrop.x];
            }
        }
        ++result.modelCalls;
    }
    cv::Mat weights;
    alpha.convertTo(weights, CV_32FC1, 1.0 / 255.0);
    blendRegion(output, state.expanded, person, weights);
    ++result.foregroundFrames;
}

void repairFrame(cv::Mat& output, const cv::Mat& original, int frameIndex, double seconds,
                 const std::vector<RegionState>& states, MaiLamaSession& model,
                 MaiTemporalWatermarkResult& result) {
    for (const RegionState& state : states) {
        if (!isActive(state.region, frameIndex, seconds)) continue;
        if (state.region.beforeSeconds < 0) {
            blendRegion(output, state.expanded, fixedPrediction(state, frameIndex),
                        state.blendAlpha);
            continue;
        }
        const cv::Mat background = temporalPrediction(original, state, seconds);
        blendRegion(output, state.expanded, background, state.blendAlpha);
        preserveForeground(output, original, background, state, model, result);
    }
}

class MaiWatermarkVideoPipeline {
public:
    MaiWatermarkVideoPipeline(const std::string& inputPath, const std::string& outputPath,
                              const std::vector<RegionState>& states, MaiLamaSession& model,
                              const MaiToolContext& context, MaiTemporalWatermarkResult& result)
        : mInputPath(inputPath),
          mOutputPath(outputPath),
          mStates(states),
          mModel(model),
          mContext(context),
          mResult(result) {}

    ~MaiWatermarkVideoPipeline() {
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

    void run() {
        initialize();
        while (true) {
            if (mContext.isCanceled()) throw std::runtime_error("canceled");
            const int status = av_read_frame(mInput, mInputPacket);
            if (status == AVERROR_EOF) break;
            requireFfmpeg(status, "read input packet");
            if (mInputPacket->stream_index == mVideoIndex) {
                requireFfmpeg(avcodec_send_packet(mDecoder, mInputPacket), "send video packet");
                drainDecoder();
            } else if (mInputPacket->stream_index < static_cast<int>(mAudioMap.size()) &&
                       mAudioMap[mInputPacket->stream_index] >= 0) {
                const AVStream* inputStream = mInput->streams[mInputPacket->stream_index];
                const int outputIndex = mAudioMap[mInputPacket->stream_index];
                if (mInputVideo->start_time != AV_NOPTS_VALUE) {
                    const std::int64_t start = av_rescale_q(
                        mInputVideo->start_time, mInputVideo->time_base, inputStream->time_base);
                    if (mInputPacket->pts != AV_NOPTS_VALUE) mInputPacket->pts -= start;
                    if (mInputPacket->dts != AV_NOPTS_VALUE) mInputPacket->dts -= start;
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
        drainDecoder();
        requireFfmpeg(avcodec_send_frame(mEncoder, nullptr), "flush video encoder");
        drainEncoder();
        requireFfmpeg(av_write_trailer(mOutput), "finish output video");
        if (mResult.frames == 0) throw std::runtime_error("video contains no frames");
        mComplete = true;
    }

private:
    void initialize() {
        requireFfmpeg(avformat_open_input(&mInput, mInputPath.c_str(), nullptr, nullptr),
                      "open input video");
        requireFfmpeg(avformat_find_stream_info(mInput, nullptr), "read input streams");
        mVideoIndex = av_find_best_stream(mInput, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        requireFfmpeg(mVideoIndex, "find video stream");
        mInputVideo = mInput->streams[mVideoIndex];
        const AVCodec* decoder = avcodec_find_decoder(mInputVideo->codecpar->codec_id);
        if (decoder == nullptr) throw std::runtime_error("video decoder is unavailable");
        mDecoder = avcodec_alloc_context3(decoder);
        if (mDecoder == nullptr) throw std::runtime_error("allocate video decoder failed");
        requireFfmpeg(avcodec_parameters_to_context(mDecoder, mInputVideo->codecpar),
                      "prepare video decoder");
        requireFfmpeg(avcodec_open2(mDecoder, decoder, nullptr), "start video decoder");
        mWidth = mDecoder->width;
        mHeight = mDecoder->height;
        if (mWidth < kModelSize || mHeight < kModelSize || mWidth > 3840 || mHeight > 3840 ||
            static_cast<std::uint64_t>(mWidth) * mHeight > 3840ULL * 2160ULL)
            throw std::runtime_error("video dimensions are unsupported");
        mOutputWidth = (mWidth + 1) & ~1;
        mOutputHeight = (mHeight + 1) & ~1;
        requireFfmpeg(
            avformat_alloc_output_context2(&mOutput, nullptr, nullptr, mOutputPath.c_str()),
            "create output container");
        if (mOutput == nullptr) throw std::runtime_error("output container is unavailable");
        const AVCodec* encoder = avcodec_find_encoder_by_name("libx264");
        if (encoder == nullptr) throw std::runtime_error("libx264 is required");
        mEncoder = avcodec_alloc_context3(encoder);
        if (mEncoder == nullptr) throw std::runtime_error("allocate video encoder failed");
        mEncoder->width = mOutputWidth;
        mEncoder->height = mOutputHeight;
        mEncoder->pix_fmt = AV_PIX_FMT_YUV420P;
        mEncoder->time_base = mInputVideo->time_base.num > 0 && mInputVideo->time_base.den > 0
                                  ? mInputVideo->time_base
                                  : AVRational{1, 90000};
        mEncoder->framerate = av_guess_frame_rate(mInput, mInputVideo, nullptr);
        if (mEncoder->framerate.num <= 0 || mEncoder->framerate.den <= 0)
            mEncoder->framerate = {30, 1};
        mEncoder->color_primaries = mDecoder->color_primaries;
        mEncoder->color_trc = mDecoder->color_trc;
        mEncoder->colorspace = mDecoder->colorspace;
        mEncoder->color_range = AVCOL_RANGE_MPEG;
        mEncoder->gop_size =
            std::max(1, static_cast<int>(std::round(av_q2d(mEncoder->framerate) * 2)));
        mEncoder->max_b_frames = 0;
        if (mOutput->oformat->flags & AVFMT_GLOBALHEADER)
            mEncoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        av_opt_set(mEncoder->priv_data, "preset", "medium", 0);
        av_opt_set(mEncoder->priv_data, "crf", "18", 0);
        requireFfmpeg(avcodec_open2(mEncoder, encoder, nullptr), "start H.264 encoder");
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
                throw std::runtime_error("original audio codec cannot be copied to MP4");
            AVStream* copied = avformat_new_stream(mOutput, nullptr);
            if (copied == nullptr) throw std::runtime_error("create output audio stream failed");
            requireFfmpeg(avcodec_parameters_copy(copied->codecpar, original->codecpar),
                          "copy original audio parameters");
            copied->codecpar->codec_tag = 0;
            copied->time_base = original->time_base;
            mAudioMap[index] = copied->index;
            ++mResult.audioStreamsCopied;
        }
        requireFfmpeg(avio_open(&mOutput->pb, mOutputPath.c_str(), AVIO_FLAG_WRITE),
                      "open output file");
        mOutputCreated = true;
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
        mRgb.create(mHeight, mWidth, CV_8UC3);
        mPadded.create(mOutputHeight, mOutputWidth, CV_8UC3);
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

    void drainDecoder() {
        while (true) {
            const int status = avcodec_receive_frame(mDecoder, mDecodedFrame);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) break;
            requireFfmpeg(status, "decode video frame");
            if (mContext.isCanceled()) throw std::runtime_error("canceled");
            if (mDecodedFrame->width != mWidth || mDecodedFrame->height != mHeight)
                throw std::runtime_error("video resolution changed during processing");
            processFrame();
            av_frame_unref(mDecodedFrame);
        }
    }

    void processFrame() {
        const auto format = static_cast<AVPixelFormat>(mDecodedFrame->format);
        mDecodeScale = sws_getCachedContext(mDecodeScale, mWidth, mHeight, sourceFormat(format),
                                            mWidth, mHeight, AV_PIX_FMT_RGB24, SWS_BILINEAR,
                                            nullptr, nullptr, nullptr);
        if (mDecodeScale == nullptr) throw std::runtime_error("convert decoded video failed");
        std::uint8_t* rgbData[] = {mRgb.data, nullptr, nullptr, nullptr};
        int rgbStride[] = {static_cast<int>(mRgb.step), 0, 0, 0};
        requireFfmpeg(sws_scale(mDecodeScale, mDecodedFrame->data, mDecodedFrame->linesize, 0,
                                mHeight, rgbData, rgbStride),
                      "convert input frame to RGB");
        const std::int64_t start =
            mInputVideo->start_time == AV_NOPTS_VALUE ? 0 : mInputVideo->start_time;
        const std::int64_t sourcePts = mDecodedFrame->best_effort_timestamp;
        const double seconds =
            sourcePts == AV_NOPTS_VALUE
                ? mResult.frames * av_q2d(av_inv_q(mEncoder->framerate))
                : std::max(0.0, (sourcePts - start) * av_q2d(mInputVideo->time_base));
        cv::Mat composited = mRgb.clone();
        repairFrame(composited, mRgb, mResult.frames, seconds, mStates, mModel, mResult);
        if (mWidth == mOutputWidth && mHeight == mOutputHeight) {
            composited.copyTo(mPadded);
        } else {
            cv::copyMakeBorder(composited, mPadded, 0, mOutputHeight - mHeight, 0,
                               mOutputWidth - mWidth, cv::BORDER_REPLICATE);
        }
        mEncodeScale = sws_getCachedContext(
            mEncodeScale, mOutputWidth, mOutputHeight, AV_PIX_FMT_RGB24, mOutputWidth,
            mOutputHeight, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (mEncodeScale == nullptr) throw std::runtime_error("convert output video failed");
        requireFfmpeg(av_frame_make_writable(mEncodedFrame), "prepare output frame");
        const std::uint8_t* source[] = {mPadded.data, nullptr, nullptr, nullptr};
        int stride[] = {static_cast<int>(mPadded.step), 0, 0, 0};
        requireFfmpeg(sws_scale(mEncodeScale, source, stride, 0, mOutputHeight, mEncodedFrame->data,
                                mEncodedFrame->linesize),
                      "convert repaired frame to YUV");
        const std::int64_t encodedPts =
            sourcePts == AV_NOPTS_VALUE
                ? av_rescale_q(mResult.frames, av_inv_q(mEncoder->framerate), mEncoder->time_base)
                : av_rescale_q(sourcePts - start, mInputVideo->time_base, mEncoder->time_base);
        mEncodedFrame->pts = std::max(mPreviousPts + 1, encodedPts);
        mPreviousPts = mEncodedFrame->pts;
        requireFfmpeg(avcodec_send_frame(mEncoder, mEncodedFrame), "encode repaired frame");
        drainEncoder();
        ++mResult.frames;
        mResult.durationSeconds =
            std::max(mResult.durationSeconds, mEncodedFrame->pts * av_q2d(mEncoder->time_base) +
                                                  av_q2d(av_inv_q(mEncoder->framerate)));
    }

    void drainEncoder() {
        while (true) {
            const int status = avcodec_receive_packet(mEncoder, mVideoPacket);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) break;
            requireFfmpeg(status, "read encoded packet");
            av_packet_rescale_ts(mVideoPacket, mEncoder->time_base, mOutputVideo->time_base);
            mVideoPacket->stream_index = mOutputVideo->index;
            mVideoPacket->pos = -1;
            requireFfmpeg(av_interleaved_write_frame(mOutput, mVideoPacket),
                          "write repaired video packet");
            av_packet_unref(mVideoPacket);
        }
    }

    std::string mInputPath;
    std::string mOutputPath;
    const std::vector<RegionState>& mStates;
    MaiLamaSession& mModel;
    const MaiToolContext& mContext;
    MaiTemporalWatermarkResult& mResult;
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
    cv::Mat mRgb;
    cv::Mat mPadded;
    std::vector<int> mAudioMap;
    int mVideoIndex = -1;
    int mWidth = 0;
    int mHeight = 0;
    int mOutputWidth = 0;
    int mOutputHeight = 0;
    std::int64_t mPreviousPts = -1;
};

}  // namespace

MaiTemporalWatermarkResult maiRepairVideoWatermarks(const std::string& inputPath,
                                                    const std::string& outputPath,
                                                    const MaiTemporalWatermarkOptions& options,
                                                    const MaiToolContext& context) {
    MaiTemporalWatermarkResult result;
    if (inputPath.empty() || outputPath.empty() || inputPath == outputPath ||
        options.modelPath.empty() || options.regions.empty() ||
        MaiFileSystem::exists(MaiFilePath::fromUtf8(outputPath))) {
        result.error = "new output, existing input, LaMa model, and regions are required";
        return result;
    }
    const std::string temporary =
        outputPath + ".partial_" + MaiIdGenerator::generate("file_") + ".mp4";
    if (context.resolvePath(temporary).empty()) {
        result.error = "temporary output must remain in the Agent workspace";
        return result;
    }
    try {
        MaiLamaSession model(options);
        VideoMetadata metadata;
        std::vector<RegionState> states;
        result.modelCalls =
            buildReferenceCache(inputPath, options, context, model, metadata, states);
        {
            MaiWatermarkVideoPipeline pipeline(inputPath, temporary, states, model, context,
                                               result);
            pipeline.run();
        }
        const MaiError published = MaiFileSystem::publishNewFile(MaiFilePath::fromUtf8(temporary),
                                                                 MaiFilePath::fromUtf8(outputPath));
        if (published) throw std::runtime_error(published.message());
        result.outputPath = outputPath;
    } catch (const std::exception& error) {
        result.error = context.isCanceled() ? "canceled" : error.what();
        result.outputPath.clear();
        if (MaiFileSystem::exists(MaiFilePath::fromUtf8(temporary)))
            (void)MaiFileSystem::removeFile(MaiFilePath::fromUtf8(temporary));
    }
    return result;
}
