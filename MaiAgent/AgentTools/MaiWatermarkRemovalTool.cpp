#include "MaiWatermarkRemovalTool.h"

#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiPathGuard.h"

namespace {

using Json = nlohmann::json;

std::string extension(const std::string& path) {
    const std::string name = MaiFilePath::fromUtf8(path).baseName().toUtf8();
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string value = name.substr(dot);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string stringField(const Json& object, const char* field) {
    if (!object.is_object() || !object.value(field, Json{}).is_string()) return {};
    return object[field].get<std::string>();
}

double durationSeconds(const Json& metadata) {
    const Json format = metadata.value("format", Json::object());
    const std::string value = stringField(format, "duration");
    if (value.empty()) return 0;
    char* end = nullptr;
    const double seconds = std::strtod(value.c_str(), &end);
    return end != value.c_str() && *end == '\0' && std::isfinite(seconds) && seconds > 0 ? seconds
                                                                                         : 0;
}

bool hasRotation(const Json& stream) {
    const Json tags = stream.value("tags", Json::object());
    const std::string text = stringField(tags, "rotate");
    if (!text.empty() && text != "0") return true;
    const Json sideData = stream.value("side_data_list", Json::array());
    if (!sideData.is_array()) return false;
    for (const Json& item : sideData) {
        if (item.is_object() && item.value("rotation", Json{}).is_number() &&
            item["rotation"].get<double>() != 0)
            return true;
    }
    return false;
}

bool isSdrVideo(const Json& stream) {
    const std::string pixFormat = stringField(stream, "pix_fmt");
    if (pixFormat.find("10") != std::string::npos || pixFormat.find("12") != std::string::npos)
        return false;
    for (const char* field : {"color_primaries", "color_transfer", "color_space"}) {
        const std::string value = stringField(stream, field);
        if (!value.empty() && value != "bt709" && value != "unknown" && value != "unspecified")
            return false;
    }
    return true;
}

class MaiWatermarkRemovalTool final : public MaiTool {
public:
    explicit MaiWatermarkRemovalTool(MaiFfmpegEngine engine) : mEngine(engine) {}

    std::string name() const override {
        return "media_remove_watermark";
    }

    std::string description() const override {
        // 只处理用户明确要求清除的静态局部水印。模型最多查看一张代表帧来填写
        // 矩形；不能为了人物替换先自行去水印，更不能循环调用 FFmpeg/OpenCV 探索。
        // 一次调用产出一个新文件，原片保留；区域内像素由邻域估算，不保证恢复原画。
        return "Remove an explicitly requested static watermark region from an accessible "
               "JPEG/PNG image or MP4/MOV video. Supply one to four pixel rectangles in the "
               "decoded frame; videos may limit each rectangle to a time window. The tool "
               "runs one FFprobe and one FFmpeg delogo pass, writes a new file, and preserves "
               "the source. It estimates covered pixels from nearby content and cannot "
               "recover the original scene. If coordinates are uncertain, inspect one "
               "representative frame or ask the user. Do not loop through FFmpeg/OpenCV. "
               "Do not use this as a default preparation for person replacement or video "
               "generation when the user did not request watermark removal.";
    }

    std::string parametersSchema() const override {
        // input_path 是已有本地图片或视频；output_path 是工作区的新文件，绝不覆盖。
        // regions 最多四个，每个矩形以原始像素给出 x/y/width/height。视频可选
        // start_s/end_s 限制生效时间；图片不能带时间。无区域时不会猜测水印位置。
        return R"({"type":"object","properties":{"input_path":{"type":"string"},"output_path":{"type":"string"},"regions":{"type":"array","minItems":1,"maxItems":4,"items":{"type":"object","properties":{"x":{"type":"integer","minimum":0},"y":{"type":"integer","minimum":0},"width":{"type":"integer","minimum":2},"height":{"type":"integer","minimum":2},"start_s":{"type":"number","minimum":0},"end_s":{"type":"number","exclusiveMinimum":0}},"required":["x","y","width","height"],"additionalProperties":false}}},"required":["input_path","output_path","regions"],"additionalProperties":false})";
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("input_path", Json{}).is_string() ||
            !args.value("output_path", Json{}).is_string() ||
            !args.value("regions", Json{}).is_array())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "input_path, output_path, and regions are required");
        MaiWatermarkRemovalOptions options;
        options.inputPath = args["input_path"].get<std::string>();
        options.outputPath = args["output_path"].get<std::string>();
        const Json& regions = args["regions"];
        if (regions.empty() || regions.size() > 4)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "regions must contain 1 to 4 rectangles");
        for (const Json& item : regions) {
            if (!item.is_object() || item.size() > 6)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "each region needs integer x, y, width and height");
            for (auto field = item.begin(); field != item.end(); ++field) {
                if (field.key() != "x" && field.key() != "y" && field.key() != "width" &&
                    field.key() != "height" && field.key() != "start_s" && field.key() != "end_s")
                    return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                  "unsupported watermark region field");
            }
            const auto boundedCoordinate = [&](const char* name) {
                const Json value = item.value(name, Json{});
                return value.is_number_integer() && value.get<double>() >= 0 &&
                       value.get<double>() <= 8192;
            };
            if (!boundedCoordinate("x") || !boundedCoordinate("y") || !boundedCoordinate("width") ||
                !boundedCoordinate("height"))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "watermark coordinates must be integers within 8192");
            if ((item.contains("start_s") && !item["start_s"].is_number()) ||
                (item.contains("end_s") && !item["end_s"].is_number()))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "start_s and end_s must be numbers");
            MaiWatermarkRegion region;
            region.x = item["x"].get<int>();
            region.y = item["y"].get<int>();
            region.width = item["width"].get<int>();
            region.height = item["height"].get<int>();
            region.startSeconds = item.value("start_s", 0.0);
            region.endSeconds = item.value("end_s", -1.0);
            options.regions.push_back(region);
        }
        const auto result = maiRemoveWatermark(options, mEngine, context);
        if (!result) return MaiToolResult::failure(result.error().code(), result.error().message());
        return MaiToolResult::success(
            Json{{"path", result.value().outputPath},
                 {"width", result.value().width},
                 {"height", result.value().height},
                 {"bytes", result.value().outputBytes},
                 {"mime_type", result.value().video                             ? "video/mp4"
                               : extension(result.value().outputPath) == ".png" ? "image/png"
                                                                                : "image/jpeg"},
                 {"regions_processed", options.regions.size()},
                 {"method", "delogo"},
                 {"note", "Covered pixels were estimated; inspect the new output visually."}}
                .dump());
    }

private:
    MaiFfmpegEngine mEngine;
};

}  // namespace

MaiResult<MaiWatermarkRemovalResult> maiRemoveWatermark(const MaiWatermarkRemovalOptions& options,
                                                        MaiFfmpegEngine engine,
                                                        const MaiToolContext& context) {
    if (engine.runFfmpeg == nullptr || engine.runFfprobe == nullptr || context.root.empty())
        return {MaiErrorCode::NotConfigured, "FFmpeg/FFprobe or Agent workspace is unavailable"};
    if (options.regions.empty() || options.regions.size() > 4)
        return {MaiErrorCode::InvalidInput, "Provide one to four watermark rectangles"};
    const std::string input = context.resolvePath(options.inputPath);
    const std::string output = maiResolvePathWithinRoot(context.root, options.outputPath);
    const std::string sourceType = extension(input);
    const std::string targetType = extension(output);
    const bool video = sourceType == ".mp4" || sourceType == ".mov";
    const bool image = sourceType == ".jpg" || sourceType == ".jpeg" || sourceType == ".png";
    if (input.empty() || output.empty() || input == output || (!video && !image) ||
        (video && targetType != ".mp4") ||
        (image && targetType != ".jpg" && targetType != ".jpeg" && targetType != ".png"))
        return {MaiErrorCode::InvalidInput,
                "Use an accessible MP4/MOV or JPEG/PNG input and a new matching output"};
    std::uint64_t sourceBytes = 0;
    if (MaiFileSystem::isSymbolicLink(MaiFilePath::fromUtf8(input)) ||
        !MaiFileSystem::fileSize(MaiFilePath::fromUtf8(input), sourceBytes) || sourceBytes == 0 ||
        sourceBytes > 500'000'000)
        return {MaiErrorCode::InvalidInput, "Input is unavailable or exceeds 500 MB"};
    if (MaiFileSystem::exists(MaiFilePath::fromUtf8(output)))
        return {MaiErrorCode::InvalidInput, "Output already exists; choose a new path"};
    const auto probe = makeMaiFfprobeTool(engine);
    const MaiToolResult inspected =
        probe->execute(Json{{"path", input}, {"include_chapters", false}}.dump(), context);
    if (inspected.hasError()) return inspected.error();
    const Json metadata = Json::parse(inspected.output(), nullptr, false);
    if (!metadata.is_object() || !metadata.value("streams", Json{}).is_array())
        return {MaiErrorCode::Protocol, "FFprobe returned no stream list"};
    Json stream;
    for (const Json& item : metadata["streams"]) {
        if (stringField(item, "codec_type") == "video") {
            stream = item;
            break;
        }
    }
    if (!stream.is_object() || !stream.value("width", Json{}).is_number_integer() ||
        !stream.value("height", Json{}).is_number_integer())
        return {MaiErrorCode::InvalidInput, "Input has no readable image dimensions"};
    const int width = stream["width"].get<int>();
    const int height = stream["height"].get<int>();
    if (width < 4 || height < 4 || width > 8192 || height > 8192)
        return {MaiErrorCode::InvalidInput, "Input dimensions are outside the supported range"};
    if (video && (!isSdrVideo(stream) || hasRotation(stream)))
        return {MaiErrorCode::NotSupported,
                "HDR, wide-gamut, or rotated video needs normalization before local delogo"};
    const double duration = video ? durationSeconds(metadata) : 0;
    std::string filter;
    for (const MaiWatermarkRegion& region : options.regions) {
        if (region.x < 0 || region.y < 0 || region.width < 2 || region.height < 2 ||
            region.x > width - region.width || region.y > height - region.height ||
            !std::isfinite(region.startSeconds) || !std::isfinite(region.endSeconds) ||
            region.startSeconds < 0 ||
            (region.endSeconds != -1 && region.endSeconds <= region.startSeconds) ||
            (duration > 0 && region.endSeconds > duration + 0.1) ||
            (!video && (region.startSeconds != 0 || region.endSeconds != -1)))
            return {MaiErrorCode::InvalidInput,
                    "Watermark rectangle or time window exceeds the input frame"};
        if (!filter.empty()) filter += ',';
        filter += "delogo=x=" + std::to_string(region.x) + ":y=" + std::to_string(region.y) +
                  ":w=" + std::to_string(region.width) + ":h=" + std::to_string(region.height) +
                  ":show=0";
        if (video && region.endSeconds != -1)
            filter += ":enable='between(t," + std::to_string(region.startSeconds) + "," +
                      std::to_string(region.endSeconds) + ")'";
        else if (video && region.startSeconds > 0)
            filter += ":enable='gte(t," + std::to_string(region.startSeconds) + ")'";
    }
    const std::string temporary =
        output + ".partial_" + MaiIdGenerator::generate("file_") + targetType;
    if (context.resolvePath(temporary).empty())
        return {MaiErrorCode::InvalidInput, "Temporary output is outside the Agent workspace"};
    std::vector<std::string> arguments = {"-i", input, "-vf", filter, "-map", "0:v:0"};
    if (video) {
        arguments.insert(arguments.end(), {"-map", "0:a?", "-map_metadata", "0", "-c:v", "libx264",
                                           "-crf", "18", "-preset", "medium", "-pix_fmt", "yuv420p",
                                           "-c:a", "copy", "-movflags", "+faststart", temporary});
    } else {
        arguments.insert(arguments.end(), {"-frames:v", "1", temporary});
    }
    const auto ffmpeg = makeMaiFfmpegTool(engine);
    const MaiToolResult processed = ffmpeg->execute(Json{{"arguments", arguments}}.dump(), context);
    if (processed.hasError()) {
        if (MaiFileSystem::exists(MaiFilePath::fromUtf8(temporary)))
            (void)MaiFileSystem::removeFile(MaiFilePath::fromUtf8(temporary));
        return processed.error();
    }
    std::uint64_t outputBytes = 0;
    if (!MaiFileSystem::fileSize(MaiFilePath::fromUtf8(temporary), outputBytes) ||
        outputBytes == 0) {
        (void)MaiFileSystem::removeFile(MaiFilePath::fromUtf8(temporary));
        return {MaiErrorCode::Internal, "FFmpeg produced no watermark-removal output"};
    }
    const MaiError published = MaiFileSystem::publishNewFile(MaiFilePath::fromUtf8(temporary),
                                                             MaiFilePath::fromUtf8(output));
    if (published) {
        (void)MaiFileSystem::removeFile(MaiFilePath::fromUtf8(temporary));
        return published;
    }
    return MaiWatermarkRemovalResult{output, width, height, outputBytes, video};
}

std::unique_ptr<MaiTool> makeMaiWatermarkRemovalTool(MaiFfmpegEngine engine) {
    return std::make_unique<MaiWatermarkRemovalTool>(engine);
}
