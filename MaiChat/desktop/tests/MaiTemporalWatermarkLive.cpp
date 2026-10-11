#include <dlfcn.h>

#include <iostream>
#include <string>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiTemporalWatermark.h"
#include "MaiTemporalWatermarkTool.h"

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: MaiTemporalWatermarkLive workspace model runtime request.json\n";
        return 2;
    }
    void* runtime = dlopen(argv[3], RTLD_NOW | RTLD_LOCAL);
    if (runtime == nullptr) {
        std::cerr << "ONNX Runtime could not be loaded\n";
        return 3;
    }
    using GetApiBase = const void* (*)();
    auto* symbol = dlsym(runtime, "OrtGetApiBase");
    if (symbol == nullptr) {
        std::cerr << "ONNX Runtime API symbol is missing\n";
        dlclose(runtime);
        return 4;
    }
    const void* apiBase = reinterpret_cast<GetApiBase>(symbol)();
    auto tool = makeMaiTemporalWatermarkTool(maiRepairVideoWatermarks, argv[2], argv[3], apiBase);
    if (tool == nullptr) {
        dlclose(runtime);
        return 4;
    }
    std::string request;
    if (const MaiError error = MaiFileSystem::readFile(MaiFilePath::fromUtf8(argv[4]), request)) {
        std::cerr << error.message() << '\n';
        dlclose(runtime);
        return 5;
    }
    MaiToolContext context;
    context.root = argv[1];
    context.allowOutsideWorkingDirectory = true;
    const MaiToolResult result = tool->execute(request, context);
    if (result.hasError()) {
        std::cerr << result.error().message() << '\n';
        dlclose(runtime);
        return 6;
    }
    std::cout << result.output() << '\n';
    dlclose(runtime);
    return 0;
}
