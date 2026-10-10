#include "MaiWatermarkRemovalTool.h"

#include <cstdio>
#include <string>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"

namespace {

int sProbes = 0;
int sConverts = 0;
std::string sFilter;

int fakeProbe(int argc, char** argv) {
    ++sProbes;
    std::string output;
    for (int index = 0; index + 1 < argc; ++index) {
        if (std::string(argv[index]) == "-o") output = argv[index + 1];
    }
    const std::string metadata =
        R"({"streams":[{"codec_type":"video","width":720,"height":1280,"pix_fmt":"yuv420p","color_primaries":"bt709","color_transfer":"bt709","color_space":"bt709"}],"format":{"duration":"10.0"}})";
    return output.empty() || MaiFileSystem::writeFile(MaiFilePath::fromUtf8(output), metadata) ? 1
                                                                                               : 0;
}

int fakeFfmpeg(int argc, char** argv) {
    ++sConverts;
    for (int index = 0; index + 1 < argc; ++index) {
        if (std::string(argv[index]) == "-vf") sFilter = argv[index + 1];
    }
    return MaiFileSystem::writeFile(MaiFilePath::fromUtf8(argv[argc - 1]), "processed media")
                   .hasError()
               ? 1
               : 0;
}

}  // namespace

int main() {
    const MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_watermark_test_")));
    if (MaiFileSystem::createDirectories(root)) return 1;
    const MaiFilePath source = root.append(MaiFilePath::fromUtf8("source.mp4"));
    if (MaiFileSystem::writeFile(source, "source video")) return 2;
    MaiToolContext context;
    context.root = root.toUtf8();
    const MaiFfmpegEngine engine{fakeFfmpeg, nullptr, fakeProbe, nullptr, nullptr, nullptr};
    auto tool = makeMaiWatermarkRemovalTool(engine);
    if (!tool || tool->name() != "media_remove_watermark" ||
        !nlohmann::json::parse(tool->parametersSchema()).is_object())
        return 3;
    const nlohmann::json request = {{"input_path", "source.mp4"},
                                    {"output_path", "clean.mp4"},
                                    {"regions", nlohmann::json::array({{{"x", 20},
                                                                        {"y", 30},
                                                                        {"width", 120},
                                                                        {"height", 80},
                                                                        {"start_s", 0.0},
                                                                        {"end_s", 4.0}}})}};
    const MaiToolResult processed = tool->execute(request.dump(), context);
    if (processed.hasError()) {
        std::fprintf(stderr, "%s\n", processed.error().message().c_str());
        return 4;
    }
    const nlohmann::json result = nlohmann::json::parse(processed.output());
    if (sProbes != 1 || sConverts != 1 ||
        sFilter.find("delogo=x=20:y=30:w=120:h=80") == std::string::npos ||
        sFilter.find("between(t,0.000000,4.000000)") == std::string::npos ||
        result.value("regions_processed", 0) != 1 ||
        !MaiFileSystem::exists(MaiFilePath::fromUtf8(result.value("path", std::string{}))) ||
        !MaiFileSystem::exists(source))
        return 5;
    const MaiToolResult duplicate = tool->execute(request.dump(), context);
    if (!duplicate.hasError() || sConverts != 1) return 6;
    MaiWatermarkRemovalOptions invalid;
    invalid.inputPath = "source.mp4";
    invalid.outputPath = "invalid.mp4";
    invalid.regions.push_back({700, 20, 100, 50, 0, -1});
    if (maiRemoveWatermark(invalid, engine, context)) return 7;
    if (sConverts != 1) return 8;
    MaiFileSystem::removeRecursively(root);
    return 0;
}
