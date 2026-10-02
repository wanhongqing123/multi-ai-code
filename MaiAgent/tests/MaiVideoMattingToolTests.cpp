#include <array>
#include <cstdio>
#include <cstdint>
#include <string>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiVideoMattingTool.h"

using Json = nlohmann::json;

namespace {

MaiVideoMattingResult fakeProcess(const std::string&, const std::string& outputPath,
                                  const MaiVideoMattingOptions& options, const MaiToolContext&) {
    MaiVideoMattingResult result;
    result.outputPath = outputPath;
    result.frames = 12;
    result.audioStreamsCopied = 1;
    result.durationSeconds = 1.0;
    if (options.solidRgb != std::array<std::uint8_t, 3>{1, 2, 3})
        result.error = "incorrect background color";
    else if (const MaiError written =
                 MaiFileSystem::writeFile(MaiFilePath::fromUtf8(outputPath), "video");
             written)
        result.error = written.message();
    return result;
}

}  // namespace

int main() {
    const MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_matting_tool_test_")));
    if (MaiFileSystem::createDirectories(root)) return 1;
    if (MaiFileSystem::writeFile(root.append(MaiFilePath::fromUtf8("source.mp4")), "source"))
        return 2;
    MaiToolContext context;
    context.root = root.toUtf8();
    auto tool = makeMaiVideoMattingTool(fakeProcess, "bundled-rvm.onnx");
    if (!tool || tool->name() != "cv_video_matting" || !tool->requiresApproval("{}")) return 3;
    const auto invalid = tool->execute(
        R"({"input_path":"source.mp4","output_path":"../out.mp4","background":{"mode":"solid","color":"#010203"}})",
        context);
    if (!invalid.hasError()) return 4;
    const auto valid = tool->execute(
        R"({"input_path":"source.mp4","output_path":"out.mp4","background":{"mode":"solid","color":"#010203"}})",
        context);
    if (valid.hasError()) {
        std::fprintf(stderr, "%s\n", valid.error().message().c_str());
        return 5;
    }
    const Json output = Json::parse(valid.output());
    if (output.value("frames", 0) != 12 || output.value("audio_streams_copied", 0) != 1 ||
        !MaiFileSystem::exists(MaiFilePath::fromUtf8(output.value("path", ""))))
        return 6;
    const auto duplicate = tool->execute(
        R"({"input_path":"source.mp4","output_path":"out.mp4","background":{"mode":"solid","color":"#010203"}})",
        context);
    if (!duplicate.hasError()) return 7;
    MaiFileSystem::removeRecursively(root);
    return 0;
}
