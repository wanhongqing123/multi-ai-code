#include "MaiMobileAgent.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
#include <string>
#include <json.hpp>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
}

namespace {

using Json = nlohmann::json;
constexpr long long kMaximumPixels = 12'000'000;

char* copiedString(const std::string& value) {
    char* copy = static_cast<char*>(std::malloc(value.size() + 1));
    if (copy != nullptr) std::memcpy(copy, value.c_str(), value.size() + 1);
    return copy;
}

MaiImageFilterResult failure(const std::string& message) {
    return {nullptr, 0, 0, copiedString(message)};
}

std::string ffmpegError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

bool integer(const Json& input, const char* key, int* value) {
    if (!input.contains(key) || !input[key].is_number_integer()) return false;
    const long long number = input[key].get<long long>();
    if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max())
        return false;
    *value = static_cast<int>(number);
    return true;
}

bool decimal(const Json& input, const char* key, double fallback, double minimum, double maximum,
             double* value) {
    if (!input.contains(key)) {
        *value = fallback;
        return true;
    }
    if (!input[key].is_number()) return false;
    const double number = input[key].get<double>();
    if (!std::isfinite(number) || number < minimum || number > maximum) return false;
    *value = number;
    return true;
}

std::string number(double value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(4) << value;
    return stream.str();
}

std::string filterDescription(const Json& input, int width, int height, std::string* error) {
    if (!input.is_object() || !input.value("operation", Json()).is_string()) {
        *error = "operation is required";
        return {};
    }
    const std::string operation = input["operation"].get<std::string>();
    if (operation == "crop") {
        int x = 0, y = 0, cropWidth = 0, cropHeight = 0;
        if (!integer(input, "x", &x) || !integer(input, "y", &y) ||
            !integer(input, "width", &cropWidth) || !integer(input, "height", &cropHeight) ||
            x < 0 || y < 0 || cropWidth < 1 || cropHeight < 1 ||
            static_cast<long long>(x) + cropWidth > width ||
            static_cast<long long>(y) + cropHeight > height) {
            *error = "crop rectangle must be inside the source image";
            return {};
        }
        return "crop=" + std::to_string(cropWidth) + ":" + std::to_string(cropHeight) + ":" +
               std::to_string(x) + ":" + std::to_string(y);
    }
    if (operation == "rotate") {
        int degrees = 0;
        if (!integer(input, "degrees", &degrees)) {
            *error = "degrees must be 90, 180, or 270";
            return {};
        }
        if (degrees == 90) return "transpose=clock";
        if (degrees == 180) return "hflip,vflip";
        if (degrees == 270) return "transpose=cclock";
        *error = "degrees must be 90, 180, or 270";
        return {};
    }
    if (operation == "resize") {
        int targetWidth = 0, targetHeight = 0;
        if (!integer(input, "width", &targetWidth) || !integer(input, "height", &targetHeight) ||
            targetWidth < 1 || targetHeight < 1 || targetWidth > 8192 || targetHeight > 8192 ||
            static_cast<long long>(targetWidth) * targetHeight > kMaximumPixels) {
            *error = "resize dimensions must fit within 12 megapixels";
            return {};
        }
        return "scale=" + std::to_string(targetWidth) + ":" + std::to_string(targetHeight) +
               ":flags=lanczos";
    }
    if (operation == "flip_horizontal") return "hflip";
    if (operation == "flip_vertical") return "vflip";
    if (operation == "grayscale") return "hue=s=0";
    if (operation == "sharpen") {
        double amount = 1.0;
        if (!decimal(input, "amount", 1, 0, 2, &amount)) {
            *error = "sharpen amount must be 0..2";
            return {};
        }
        return "unsharp=luma_msize_x=3:luma_msize_y=3:luma_amount=" + number(amount);
    }
    if (operation == "adjust") {
        double brightness = 0, contrast = 1, saturation = 1;
        if (!decimal(input, "brightness", 0, -1, 1, &brightness) ||
            !decimal(input, "contrast", 1, 0, 2, &contrast) ||
            !decimal(input, "saturation", 1, 0, 2, &saturation)) {
            *error = "brightness must be -1..1; contrast and saturation must be 0..2";
            return {};
        }
        const std::string expression =
            "(val-128)*" + number(contrast) + "+128+" + number(brightness * 255.0);
        return "lutrgb=r='" + expression + "':g='" + expression + "':b='" + expression +
               "',hue=s=" + number(saturation);
    }
    if (operation == "beautify") {
        double strength = 0.5;
        if (!decimal(input, "strength", 0.5, 0, 1, &strength)) {
            *error = "beautify strength must be 0..1";
            return {};
        }
        const std::string expression =
            "(val-128)*" + number(1.0 + 0.05 * strength) + "+128+" + number(20.0 * strength);
        return "gblur=sigma=" + number(0.2 + 1.2 * strength) + ":steps=1," + "lutrgb=r='" +
               expression + "':g='" + expression + "':b='" + expression +
               "',hue=s=" + number(1.0 + 0.08 * strength);
    }
    *error = "unsupported image operation";
    return {};
}

}  // namespace

extern "C" MaiImageFilterResult maiImageFilterRgba(const unsigned char* pixels, int width,
                                                   int height, int stride,
                                                   const char* operationJson) {
    if (pixels == nullptr || operationJson == nullptr || width < 1 || height < 1 ||
        static_cast<long long>(width) * height > kMaximumPixels ||
        static_cast<long long>(width) * 4 > std::numeric_limits<int>::max() || stride < width * 4)
        return failure("invalid RGBA image or image exceeds 12 megapixels");
    try {
        const Json arguments = Json::parse(operationJson);
        std::string validationError;
        const std::string filter = filterDescription(arguments, width, height, &validationError);
        if (filter.empty()) return failure(validationError);

        auto graph = std::unique_ptr<AVFilterGraph, void (*)(AVFilterGraph*)>(
            avfilter_graph_alloc(), [](AVFilterGraph* value) { avfilter_graph_free(&value); });
        auto frame = std::unique_ptr<AVFrame, void (*)(AVFrame*)>(
            av_frame_alloc(), [](AVFrame* value) { av_frame_free(&value); });
        auto output = std::unique_ptr<AVFrame, void (*)(AVFrame*)>(
            av_frame_alloc(), [](AVFrame* value) { av_frame_free(&value); });
        if (!graph || !frame || !output) return failure("FFmpeg could not allocate image buffers");

        AVFilterContext* source = nullptr;
        AVFilterContext* sink = nullptr;
        const AVFilter* sourceFilter = avfilter_get_by_name("buffer");
        const AVFilter* sinkFilter = avfilter_get_by_name("buffersink");
        if (sourceFilter == nullptr || sinkFilter == nullptr)
            return failure("FFmpeg image buffer filters are unavailable");
        const std::string sourceArguments =
            "video_size=" + std::to_string(width) + "x" + std::to_string(height) +
            ":pix_fmt=" + std::to_string(AV_PIX_FMT_RGBA) + ":time_base=1/25:pixel_aspect=1/1";
        int status = avfilter_graph_create_filter(&source, sourceFilter, "in",
                                                  sourceArguments.c_str(), nullptr, graph.get());
        if (status < 0) return failure("FFmpeg buffer source: " + ffmpegError(status));
        status =
            avfilter_graph_create_filter(&sink, sinkFilter, "out", nullptr, nullptr, graph.get());
        if (status < 0) return failure("FFmpeg buffer sink: " + ffmpegError(status));

        AVFilterInOut* inputs = avfilter_inout_alloc();
        AVFilterInOut* outputs = avfilter_inout_alloc();
        if (inputs == nullptr || outputs == nullptr) {
            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
            return failure("FFmpeg could not connect image filters");
        }
        inputs->name = av_strdup("out");
        inputs->filter_ctx = sink;
        inputs->pad_idx = 0;
        outputs->name = av_strdup("in");
        outputs->filter_ctx = source;
        outputs->pad_idx = 0;
        const std::string graphDescription = filter + ",format=pix_fmts=rgba";
        status = avfilter_graph_parse_ptr(graph.get(), graphDescription.c_str(), &inputs, &outputs,
                                          nullptr);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        if (status < 0) return failure("FFmpeg filter: " + ffmpegError(status));
        status = avfilter_graph_config(graph.get(), nullptr);
        if (status < 0) return failure("FFmpeg filter setup: " + ffmpegError(status));

        frame->format = AV_PIX_FMT_RGBA;
        frame->width = width;
        frame->height = height;
        status = av_frame_get_buffer(frame.get(), 32);
        if (status < 0) return failure("FFmpeg input allocation: " + ffmpegError(status));
        for (int row = 0; row < height; ++row)
            std::memcpy(frame->data[0] + row * frame->linesize[0], pixels + row * stride,
                        static_cast<std::size_t>(width) * 4);
        status = av_buffersrc_add_frame_flags(source, frame.get(), AV_BUFFERSRC_FLAG_KEEP_REF);
        if (status < 0) return failure("FFmpeg input: " + ffmpegError(status));
        status = av_buffersrc_add_frame_flags(source, nullptr, 0);
        if (status < 0) return failure("FFmpeg input finish: " + ffmpegError(status));
        status = av_buffersink_get_frame(sink, output.get());
        if (status < 0) return failure("FFmpeg output: " + ffmpegError(status));
        if (output->format != AV_PIX_FMT_RGBA || output->width < 1 || output->height < 1 ||
            static_cast<long long>(output->width) * output->height > kMaximumPixels)
            return failure("FFmpeg returned an invalid image size or pixel format");

        const std::size_t bytes = static_cast<std::size_t>(output->width) * output->height * 4;
        auto* copy = static_cast<unsigned char*>(std::malloc(bytes));
        if (copy == nullptr) return failure("image result is too large for memory");
        for (int row = 0; row < output->height; ++row)
            std::memcpy(copy + static_cast<std::size_t>(row) * output->width * 4,
                        output->data[0] + row * output->linesize[0],
                        static_cast<std::size_t>(output->width) * 4);
        return {copy, output->width, output->height, nullptr};
    } catch (const std::exception& error) {
        return failure(error.what());
    } catch (...) {
        return failure("unknown FFmpeg image filter error");
    }
}

extern "C" void maiImageFilterFree(void* pointer) {
    std::free(pointer);
}
