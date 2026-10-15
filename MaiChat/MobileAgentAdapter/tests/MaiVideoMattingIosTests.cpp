// 文件作用：移动端 C++ Agent 适配层；MaiVideoMattingIosTests 负责行为与回归验证，本文件给出其验证。
// 运行边界：仅供自动化验证使用，不作为正式 App 或 Agent 的用户功能入口。
// 直接依赖：MaiFilePath.h、MaiFileSystem.h、MaiIdGenerator.h、MaiMobileAgent.h；实现与这些接口协作。
#include <cstdio>
#include <string>

#include <json.hpp>
#include <onnxruntime_c_api.h>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiMobileAgent.h"

using Json = nlohmann::json;

int main(int argc, char** argv) {
    if (argc < 3) return 1;
    const std::string input = argv[1];
    const std::string model = argv[2];
    const MaiFilePath workspace = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_ios_rvm_")));
    if (MaiFileSystem::createDirectories(workspace)) return 2;
    const std::string root = workspace.toUtf8();
    const std::string arguments = Json{
        {"input_path", input}, {"output_path", "result.mp4"},
        {"background", {{"mode", "solid"}, {"color", "#008040"}}}}.dump();
    const OrtApiBase* apiBase = OrtGetApiBase();
    if (apiBase == nullptr) return 3;
    char* response = maiMobileMatteVideo(arguments.c_str(), root.c_str(), model.c_str(),
                                         nullptr, apiBase);
    if (response == nullptr) return 4;
    const Json result = Json::parse(response, nullptr, false);
    maiMobileAgentFree(response);
    if (!result.is_object() || !result.value("ok", false)) {
        std::fprintf(stderr, "iOS RVM tool failed: %s\n", result.dump().c_str());
        return 5;
    }
    const Json output = Json::parse(result["output"].get<std::string>());
    const MaiFilePath file = workspace.append(MaiFilePath::fromUtf8("result.mp4"));
    std::uint64_t bytes = 0;
    const bool passed = output.value("frames", 0) > 1 &&
                        output.value("audio_streams_copied", 0) == 1 &&
                        MaiFileSystem::fileSize(file, bytes) && bytes > 0;
    MaiFileSystem::removeRecursively(workspace);
    return passed ? 0 : 6;
}
