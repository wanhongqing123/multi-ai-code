#include "MaiVideoGeometryTool.h"

#include <json.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"

namespace {

using Json = nlohmann::json;

std::string field(const Json& object, const char* name) {
    if (!object.is_object() || !object.value(name, Json{}).is_string()) return {};
    return object[name].get<std::string>();
}

bool compatibleColor(const Json& stream) {
    if (field(stream, "pix_fmt") != "yuv420p") return false;
    for (const char* name : {"color_primaries", "color_transfer", "color_space"}) {
        const std::string value = field(stream, name);
        if (!value.empty() && value != "bt709" && value != "unknown" && value != "unspecified")
            return false;
    }
    const std::string range = field(stream, "color_range");
    return range.empty() || range == "tv" || range == "unknown" || range == "unspecified";
}

class StretchLowerTool final : public MaiTool {
public:
    explicit StretchLowerTool(MaiFfmpegEngine engine) : mEngine(engine) {}

    std::string name() const override {
        return "video_stretch_lower";
    }
    std::string description() const override {
        // 仅对 SDR 视频下半部分做几何拉伸，上半部分保持不动；这是像素处理，
        // 不会改变人物身份或生成新动作。
        return "Stretch only the lower region of an SDR video (for example, lengthen legs) "
               "while keeping the upper region geometrically unchanged. Uses a fixed "
               "crop/scale/vstack FFmpeg recipe and lossless H.264; no paid model or identity "
               "redraw. Source must be 8-bit yuv420p SDR; HDR/P3 inputs are rejected. "
               "Output can be much larger than the original. Choose an even split_y.";
    }
    std::string parametersSchema() const override {
        // input_path/output_path 是不同文件；split_y 指上下区域分界像素，
        // factor 仅在 1.01–1.3 范围内拉伸下半部分。
        return R"({"type":"object","properties":{"input_path":{"type":"string"},"output_path":{"type":"string"},"split_y":{"type":"integer","minimum":2},"factor":{"type":"number","minimum":1.01,"maximum":1.3}},"required":["input_path","output_path","split_y","factor"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override {
        return true;
    }
    std::string approvalKey(const std::string& raw) const override {
        const Json args = Json::parse(raw, nullptr, false);
        return "video_stretch_lower:" + field(args, "output_path");
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("input_path", Json{}).is_string() ||
            !args.value("output_path", Json{}).is_string() ||
            !args.value("split_y", Json{}).is_number_integer() ||
            !args.value("factor", Json{}).is_number())
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "input_path, output_path, split_y and factor are required");
        MaiStretchLowerOptions options;
        options.inputPath = args["input_path"].get<std::string>();
        options.outputPath = args["output_path"].get<std::string>();
        options.splitY = args["split_y"].get<int>();
        options.factor = args["factor"].get<double>();
        const auto result = maiStretchLowerVideo(options, mEngine, context);
        if (!result) return MaiToolResult::failure(result.error().code(), result.error().message());
        return MaiToolResult::success(Json{{"path", result.value().outputPath},
                                           {"width", result.value().inputWidth},
                                           {"input_height", result.value().inputHeight},
                                           {"output_height", result.value().outputHeight},
                                           {"bytes", result.value().outputBytes},
                                           {"upper_region_scaled", false},
                                           {"video_encoding", "lossless_h264"}}
                                          .dump());
    }

private:
    MaiFfmpegEngine mEngine;
};

}  // namespace

MaiResult<MaiStretchLowerResult> maiStretchLowerVideo(const MaiStretchLowerOptions& options,
                                                      MaiFfmpegEngine engine,
                                                      const MaiToolContext& context) {
    if (!engine.runFfmpeg || !engine.runFfprobe || context.root.empty() ||
        options.inputPath.empty() || options.outputPath.empty() ||
        options.inputPath == options.outputPath || options.splitY < 2 || options.splitY % 2 != 0 ||
        !std::isfinite(options.factor) || options.factor < 1.01 || options.factor > 1.3 ||
        options.outputPath.size() < 5 ||
        options.outputPath.compare(options.outputPath.size() - 4, 4, ".mp4") != 0)
        return {MaiErrorCode::InvalidInput,
                "use distinct MP4 output, even split_y, and factor between 1.01 and 1.3"};
    const auto probe = makeMaiFfprobeTool(engine);
    const MaiToolResult inspected = probe->execute(
        Json{{"path", options.inputPath}, {"include_chapters", false}}.dump(), context);
    if (inspected.hasError()) return inspected.error();
    const Json metadata = Json::parse(inspected.output(), nullptr, false);
    if (!metadata.is_object() || !metadata.value("streams", Json{}).is_array())
        return {MaiErrorCode::Protocol, "FFprobe returned no stream list"};
    Json video;
    for (const Json& stream : metadata["streams"]) {
        if (field(stream, "codec_type") == "video") {
            video = stream;
            break;
        }
    }
    if (!video.is_object() || !video.value("width", Json{}).is_number_integer() ||
        !video.value("height", Json{}).is_number_integer())
        return {MaiErrorCode::InvalidInput, "input has no video dimensions"};
    const int width = video["width"].get<int>();
    const int height = video["height"].get<int>();
    if (width < 2 || width % 2 != 0 || height < 4 || height % 2 != 0 ||
        options.splitY >= height - 1)
        return {MaiErrorCode::InvalidInput, "input dimensions or split_y are invalid"};
    if (!compatibleColor(video))
        return {MaiErrorCode::NotSupported,
                "this lossless geometry recipe supports 8-bit yuv420p SDR only; HDR and wide-gamut "
                "video need a color-managed path"};
    const int lowerHeight =
        static_cast<int>(std::floor((height - options.splitY) * options.factor / 2)) * 2;
    const int outputHeight = options.splitY + lowerHeight;
    if (outputHeight <= height || outputHeight > 8192)
        return {MaiErrorCode::InvalidInput, "stretched output height is invalid"};
    const std::string input = context.resolvePath(options.inputPath);
    const std::string output = context.resolvePath(options.outputPath);
    if (input.empty() || output.empty() || input == output ||
        MaiFileSystem::exists(MaiFilePath::fromUtf8(output)))
        return {MaiErrorCode::InvalidInput, "output path must be a new accessible file"};
    const std::string split = std::to_string(options.splitY);
    const std::string graph =
        "[0:v]split=2[t][b];[t]crop=iw:" + split + ":0:0[upper];[b]crop=iw:ih-" + split +
        ":0:" + split + ",scale=iw:trunc(ih*" + std::to_string(options.factor) +
        "/2)*2:flags=lanczos[lower];[upper][lower]vstack=inputs=2,format=yuv420p[v]";
    const std::vector<std::string> arguments = {
        "-i",
        options.inputPath,
        "-filter_complex",
        graph,
        "-map",
        "[v]",
        "-map",
        "0:a?",
        "-map_metadata",
        "0",
        "-c:v",
        "libx264",
        "-qp",
        "0",
        "-pix_fmt",
        "yuv420p",
        "-x264-params",
        "colorprim=bt709:transfer=bt709:colormatrix=bt709:range=tv",
        "-color_primaries",
        "bt709",
        "-color_trc",
        "bt709",
        "-colorspace",
        "bt709",
        "-color_range",
        "tv",
        "-fps_mode",
        "passthrough",
        "-c:a",
        "copy",
        "-movflags",
        "+faststart",
        options.outputPath};
    const auto ffmpeg = makeMaiFfmpegTool(engine);
    const MaiToolResult processed = ffmpeg->execute(Json{{"arguments", arguments}}.dump(), context);
    if (processed.hasError()) return processed.error();
    std::uint64_t outputBytes = 0;
    if (!MaiFileSystem::fileSize(MaiFilePath::fromUtf8(output), outputBytes) || outputBytes == 0)
        return {MaiErrorCode::Internal, "FFmpeg produced no output video"};
    return MaiStretchLowerResult{options.outputPath, width, height, outputHeight, outputBytes};
}

std::unique_ptr<MaiTool> makeMaiStretchLowerVideoTool(MaiFfmpegEngine engine) {
    return std::make_unique<StretchLowerTool>(engine);
}
