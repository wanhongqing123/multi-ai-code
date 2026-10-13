#include "MaiTemporalWatermarkTool.h"

#include <cassert>
#include <string>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"

namespace {

int sCalls = 0;
MaiTemporalWatermarkOptions sOptions;

MaiTemporalWatermarkResult fakeProcessor(const std::string&, const std::string& output,
                                         const MaiTemporalWatermarkOptions& options,
                                         const MaiToolContext&) {
    ++sCalls;
    sOptions = options;
    MaiTemporalWatermarkResult result;
    result.outputPath = output;
    result.frames = 90;
    result.audioStreamsCopied = 1;
    result.modelCalls = 3;
    result.foregroundFrames = 2;
    result.durationSeconds = 3.0;
    result.inferenceBackend = "cpu";
    result.modelInferenceMs = 120.0;
    return result;
}

}  // namespace

int main() {
    const MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_watermark_tool_")));
    assert(!MaiFileSystem::createDirectories(root));
    assert(!MaiFileSystem::writeFile(root.append(MaiFilePath::fromUtf8("input.mov")), "media"));
    MaiToolContext context;
    context.root = root.toUtf8();
    auto tool = makeMaiTemporalWatermarkTool(fakeProcessor, "trusted-model.onnx", {},
                                             reinterpret_cast<const void*>(0x1));
    assert(tool != nullptr && tool->name() == "media_remove_watermark");
    const auto schema = nlohmann::json::parse(tool->parametersSchema());
    assert(schema["properties"]["regions"]["items"]["properties"].contains("start_frame"));
    assert(!schema["properties"]["regions"]["items"]["properties"].contains("mode"));
    const std::string valid =
        R"({"input_path":"input.mov","output_path":"result.mp4","regions":[{"x":10,"y":20,"width":100,"height":50},{"x":30,"y":40,"width":120,"height":60,"start_s":2,"end_s":4,"start_frame":60,"end_frame":120,"before_s":1.5,"after_s":4.5,"foreground_edge":"right"}]})";
    const MaiToolResult processed = tool->execute(valid, context);
    assert(!processed.hasError() && sCalls == 1);
    assert(sOptions.modelPath == "trusted-model.onnx" && sOptions.regions.size() == 2);
    assert(sOptions.regions[0].beforeSeconds == -1);
    assert(sOptions.regions[1].beforeSeconds == 1.5);
    assert(sOptions.regions[1].startFrame == 60 && sOptions.regions[1].endFrame == 120);
    assert(sOptions.regions[1].foregroundEdge == MaiWatermarkForegroundEdge::Right);
    assert(nlohmann::json::parse(processed.output()).value("onnx_calls", 0) == 3);
    assert(nlohmann::json::parse(processed.output())["timing_ms"]["model_inference"] == 120.0);

    // 只给精确帧窗口时，before_s 是参考帧时间，不要求额外填写 start_s。
    const std::string frameOnly =
        R"({"input_path":"input.mov","output_path":"frame-only.mp4","regions":[{"x":30,"y":40,"width":120,"height":60,"start_frame":60,"end_frame":120,"before_s":1.5,"after_s":4.5}]})";
    assert(!tool->execute(frameOnly, context).hasError() && sCalls == 2);
    assert(sOptions.regions[0].startFrame == 60 && sOptions.regions[0].startSeconds == 0);

    for (
        const std::string& invalid : {
            R"({"input_path":"input.mov","output_path":"result.mp4","regions":[{"mode":"inpaint","x":30,"y":40,"width":120,"height":60}]})",
            R"({"input_path":"input.mov","output_path":"result.mp4","regions":[{"x":30,"y":40,"width":120,"height":60,"after_s":1}]})",
            R"({"input_path":"input.mov","output_path":"result.mp4","regions":[{"x":30,"y":40,"width":120,"height":60,"foreground_edge":"right"}]})",
            R"({"input_path":"input.mov","output_path":"result.mp4","regions":[{"x":30,"y":40,"width":120,"height":60,"start_frame":30,"end_frame":20}]})",
            R"({"input_path":"input.mov","output_path":"../outside.mp4","regions":[{"x":30,"y":40,"width":120,"height":60}]})",
            R"({"input_path":"input.mov","output_path":"result.mp4","regions":[{"x":30,"y":40,"width":120,"height":60,"start_s":2,"end_s":4,"before_s":3}]})",
        }) {
        assert(tool->execute(invalid, context).hasError());
        assert(sCalls == 2);
    }
    assert(makeMaiTemporalWatermarkTool(nullptr, "model.onnx") == nullptr);
    assert(makeMaiTemporalWatermarkTool(fakeProcessor, {}) == nullptr);
    MaiFileSystem::removeRecursively(root);
    return 0;
}
