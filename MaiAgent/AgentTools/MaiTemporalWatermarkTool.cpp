#include "MaiTemporalWatermarkTool.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiPathGuard.h"

namespace {

using Json = nlohmann::json;

MaiToolResult invalid(const std::string& message) {
    return MaiToolResult::failure(MaiErrorCode::InvalidInput, message);
}

class MaiTemporalWatermarkTool final : public MaiTool {
public:
    MaiTemporalWatermarkTool(MaiTemporalWatermarkProcessor processor, std::string modelPath,
                             std::string runtimePath, const void* apiBase)
        : mProcessor(processor),
          mModelPath(std::move(modelPath)),
          mRuntimePath(std::move(runtimePath)),
          mApiBase(apiBase) {}

    std::string name() const override {
        return "media_remove_watermark";
    }

    std::string description() const override {
        // 一次调用处理全片：持续可见且无干净参考的区域走本地 ONNX 补画；
        // 会移动的区域借自身前后无遮挡帧，并在前景进入补画区时保留人物轮廓。
        // Agent 先用一张抽帧拼图定位移动区域，再用帧号钉住切换点；必须核对
        // before_s/after_s 指向无遮挡帧，不能让本工具在黑盒中猜测。
        return "Remove explicitly requested visible overlays from a local MP4/MOV video in "
               "one on-device job. Provide every watermark rectangle and active time window. "
               "There is one fixed repair workflow. If the same area "
               "is clean before or after an overlay appears, supply verified before_s and "
               "optional after_s timestamps. The tool interpolates those clean frames, "
               "matches color, feather-blends the background, and preserves a foreground "
               "person when foreground_edge identifies the entering side. If no clean "
               "reference exists, omit reference timestamps; the tool uses local LaMa ONNX "
               "for that region. Bright text overlapping preserved foreground is repaired "
               "locally with ONNX. The tool "
               "writes a new MP4, keeps the source, and copies audio. It estimates hidden "
               "pixels; review the output. Inspect one contact sheet sampled across the "
               "video, then use start_frame/end_frame for exact transition boundaries and "
               "call once with all regions. Clean reference times must show no overlay or "
               "foreground in the repair area. Do not run FFmpeg/OpenCV repair loops.";
    }

    std::string parametersSchema() const override {
        // 每个区域只说明位置、出现时间与已核实的无遮挡参考帧。
        // 不再让 Agent 选择处理算法：有 before_s 就借参考帧，否则由本地模型补画。
        // foreground_edge 仅标记需要保护的前景从哪侧进入，不是另一种算法。
        return R"({"type":"object","properties":{"input_path":{"type":"string","description":"Existing local MP4 or MOV"},"output_path":{"type":"string","description":"New MP4 path relative to the Agent workspace"},"regions":{"type":"array","minItems":1,"maxItems":16,"items":{"type":"object","properties":{"x":{"type":"integer","minimum":0},"y":{"type":"integer","minimum":0},"width":{"type":"integer","minimum":2},"height":{"type":"integer","minimum":2},"start_s":{"type":"number","minimum":0},"end_s":{"type":"number","exclusiveMinimum":0},"start_frame":{"type":"integer","minimum":0,"description":"Inclusive decoded frame number; takes priority over start_s"},"end_frame":{"type":"integer","minimum":1,"description":"Exclusive decoded frame number; takes priority over end_s"},"before_s":{"type":"number","minimum":0,"description":"Verified clean frame before overlay; omit if none exists"},"after_s":{"type":"number","minimum":0,"description":"Optional verified clean frame after overlay; requires before_s"},"foreground_edge":{"type":"string","enum":["none","left","right"],"description":"Side from which a person enters the repair rectangle; omit if no foreground overlaps"}},"required":["x","y","width","height"],"additionalProperties":false}}},"required":["input_path","output_path","regions"],"additionalProperties":false})";
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json request = Json::parse(raw, nullptr, false);
        if (!request.is_object() || !request.value("input_path", Json{}).is_string() ||
            !request.value("output_path", Json{}).is_string() ||
            !request.value("regions", Json{}).is_array())
            return invalid("input_path, output_path, and regions are required");
        for (auto field = request.begin(); field != request.end(); ++field) {
            if (field.key() != "input_path" && field.key() != "output_path" &&
                field.key() != "regions")
                return invalid("unsupported watermark tool parameter: " + field.key());
        }
        const std::string input = context.resolvePath(request["input_path"].get<std::string>());
        if (input.empty() || !MaiFileSystem::exists(MaiFilePath::fromUtf8(input)) ||
            MaiFileSystem::isDirectory(MaiFilePath::fromUtf8(input)) ||
            MaiFileSystem::isSymbolicLink(MaiFilePath::fromUtf8(input)))
            return MaiToolResult::failure(MaiErrorCode::NotFound,
                                          "input video must be an accessible local file");
        const std::string outputName = request["output_path"].get<std::string>();
        if (outputName.empty() || MaiFilePath::fromUtf8(outputName).isAbsolute())
            return invalid("output_path must be relative to the Agent workspace");
        const std::string output = maiResolvePathWithinRoot(context.root, outputName);
        if (output.empty() || output == input ||
            MaiFileSystem::exists(MaiFilePath::fromUtf8(output)))
            return invalid("output_path must name a new file inside the Agent workspace");
        const std::string inputName = MaiFilePath::fromUtf8(input).baseName().toUtf8();
        if (inputName.size() < 4 || (inputName.compare(inputName.size() - 4, 4, ".mp4") != 0 &&
                                     inputName.compare(inputName.size() - 4, 4, ".mov") != 0))
            return invalid("input_path must end in .mp4 or .mov");
        if (outputName.size() < 4 || outputName.compare(outputName.size() - 4, 4, ".mp4") != 0)
            return invalid("output_path must end in .mp4");
        std::uint64_t bytes = 0;
        if (!MaiFileSystem::fileSize(MaiFilePath::fromUtf8(input), bytes) || bytes == 0 ||
            bytes > 500'000'000)
            return invalid("input video must be between 1 byte and 500 MB");

        const Json& regions = request["regions"];
        if (regions.empty() || regions.size() > 16)
            return invalid("regions must contain 1 to 16 entries");
        MaiTemporalWatermarkOptions options;
        options.modelPath = mModelPath;
        options.runtimePath = mRuntimePath;
        options.ortApiBase = mApiBase;
        for (const Json& item : regions) {
            if (!item.is_object()) return invalid("each region needs x, y, width, and height");
            for (auto field = item.begin(); field != item.end(); ++field) {
                if (field.key() != "x" && field.key() != "y" && field.key() != "width" &&
                    field.key() != "height" && field.key() != "start_s" && field.key() != "end_s" &&
                    field.key() != "start_frame" && field.key() != "end_frame" &&
                    field.key() != "before_s" && field.key() != "after_s" &&
                    field.key() != "foreground_edge")
                    return invalid("unsupported watermark region parameter: " + field.key());
            }
            MaiWatermarkRepairRegion region;
            for (const char* field : {"x", "y", "width", "height"}) {
                if (!item.value(field, Json{}).is_number_integer() ||
                    item[field].get<std::int64_t>() < 0 || item[field].get<std::int64_t>() > 8192)
                    return invalid(std::string(field) + " must be an integer from 0 to 8192");
            }
            region.x = item["x"].get<int>();
            region.y = item["y"].get<int>();
            region.width = item["width"].get<int>();
            region.height = item["height"].get<int>();
            if (region.width < 2 || region.height < 2 || region.width > 400 || region.height > 400)
                return invalid("repair rectangles must fit within a 400 by 400 model crop");
            for (const char* field : {"start_s", "end_s", "before_s", "after_s"}) {
                if (item.contains(field) && !item[field].is_number())
                    return invalid(std::string(field) + " must be a number");
            }
            region.startSeconds = item.value("start_s", 0.0);
            region.endSeconds = item.value("end_s", -1.0);
            for (const char* field : {"start_frame", "end_frame"}) {
                if (item.contains(field) &&
                    (!item[field].is_number_integer() || item[field].get<std::int64_t>() < 0 ||
                     item[field].get<std::int64_t>() > 1800))
                    return invalid(std::string(field) + " must be an integer from 0 to 1800");
            }
            region.startFrame = item.value("start_frame", -1);
            region.endFrame = item.value("end_frame", -1);
            if ((region.startFrame < 0 && region.endFrame >= 0) ||
                (region.endFrame >= 0 && region.endFrame <= region.startFrame))
                return invalid("end_frame must be greater than start_frame");
            region.beforeSeconds = item.value("before_s", -1.0);
            region.afterSeconds = item.value("after_s", -1.0);
            if (!std::isfinite(region.startSeconds) || !std::isfinite(region.endSeconds) ||
                !std::isfinite(region.beforeSeconds) || !std::isfinite(region.afterSeconds) ||
                region.startSeconds < 0 ||
                (region.startFrame < 0 && region.endSeconds != -1 &&
                 region.endSeconds <= region.startSeconds))
                return invalid("region time window is invalid");
            if (region.beforeSeconds < -1 || region.afterSeconds < -1)
                return invalid("clean reference timestamps must be nonnegative");
            if (region.beforeSeconds >= 0) {
                // 精确帧窗口优先于秒数窗口；只有未指定帧时才按秒数校验前后参考。
                if ((region.startFrame < 0 && region.beforeSeconds >= region.startSeconds) ||
                    (region.afterSeconds != -1 && region.endFrame < 0 &&
                     (region.endSeconds == -1 || region.afterSeconds <= region.endSeconds)))
                    return invalid("clean reference timestamps must be outside the repair window");
            } else if (region.afterSeconds != -1) {
                return invalid("after_s requires a clean before_s reference");
            }
            const std::string edge = item.value("foreground_edge", "none");
            if (edge == "left")
                region.foregroundEdge = MaiWatermarkForegroundEdge::Left;
            else if (edge == "right")
                region.foregroundEdge = MaiWatermarkForegroundEdge::Right;
            else if (edge != "none")
                return invalid("foreground_edge must be none, left, or right");
            if (region.beforeSeconds < 0 &&
                region.foregroundEdge != MaiWatermarkForegroundEdge::None)
                return invalid("foreground_edge requires clean reference timestamps");
            options.regions.push_back(region);
        }
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "watermark repair was canceled");
        const MaiError directory =
            MaiFileSystem::createDirectories(MaiFilePath::fromUtf8(output).dirName());
        if (directory) return MaiToolResult::failure(directory.code(), directory.message());
        const MaiTemporalWatermarkResult result = mProcessor(input, output, options, context);
        if (context.isCanceled() || result.error == "canceled")
            return MaiToolResult::failure(MaiErrorCode::Canceled, "watermark repair was canceled");
        if (!result.error.empty())
            return MaiToolResult::failure(MaiErrorCode::Internal, result.error);
        return MaiToolResult::success(
            Json{{"path", result.outputPath},
                 {"method", "local_temporal_and_onnx_inpainting"},
                 {"frames", result.frames},
                 {"duration_s", result.durationSeconds},
                 {"audio_streams_copied", result.audioStreamsCopied},
                 {"onnx_calls", result.modelCalls},
                 {"localized_text_regions", result.localizedTextRegions},
                 {"inference_backend", result.inferenceBackend},
                 {"timing_ms", Json{{"model_initialization", result.modelInitializationMs},
                                    {"reference_scan", result.referenceScanMs},
                                    {"model_inference", result.modelInferenceMs},
                                    {"repair_and_encode", result.repairAndEncodeMs},
                                    {"total", result.totalMs}}},
                 {"foreground_frames_preserved", result.foregroundFrames},
                 {"note", "Hidden pixels were estimated; review the new video."}}
                .dump());
    }

private:
    MaiTemporalWatermarkProcessor mProcessor;
    std::string mModelPath;
    std::string mRuntimePath;
    const void* mApiBase = nullptr;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiTemporalWatermarkTool(MaiTemporalWatermarkProcessor processor,
                                                      std::string modelPath,
                                                      std::string runtimePath,
                                                      const void* apiBase) {
    if (processor == nullptr || modelPath.empty()) return nullptr;
    return std::make_unique<MaiTemporalWatermarkTool>(processor, std::move(modelPath),
                                                      std::move(runtimePath), apiBase);
}
