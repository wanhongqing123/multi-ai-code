#include "MaiFfmpegTools.h"

#include <cstdio>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiThread.h"
#include "mai_fftools_embed.h"

namespace {

int (*sFakeCancelCheck)(void*) = nullptr;
void* sFakeCancelOpaque = nullptr;

void setFakeCancelCheck(int (*check)(void*), void* opaque) {
    sFakeCancelCheck = check;
    sFakeCancelOpaque = opaque;
}

int runUntilCanceled(int, char**) {
    while (!sFakeCancelCheck || !sFakeCancelCheck(sFakeCancelOpaque))
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return 255;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const MaiFilePath input = MaiFilePath::fromUtf8(argv[1]);
    if (!MaiFileSystem::exists(input)) return 3;

    MaiFfmpegEngine engine{mai_ffmpeg_execute, mai_ffmpeg_set_cancel_check, mai_ffprobe_execute,
                           mai_ffprobe_set_cancel_check};
    auto probe = makeMaiFfprobeTool(engine);
    auto convert = makeMaiFfmpegTool(engine);
    MaiToolContext context;
    context.root = input.dirName().toUtf8();
    context.allowOutsideWorkingDirectory = true;
    context.fileAccessRoot = MaiFileSystem::temporaryDirectory().toUtf8();

    const MaiToolResult missing = probe->execute(R"({"path":"no-such-video.mp4"})", context);
    if (!missing.hasError()) return 4;
    const MaiToolResult invalidConversion = convert->execute(
        nlohmann::json{{"arguments", {"-i", "no-such-video.mp4", "-f", "null", "-"}}}.dump(),
        context);
    if (!invalidConversion.hasError()) return 9;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const MaiToolResult metadata =
            probe->execute(nlohmann::json{{"path", input.toUtf8()}}.dump(), context);
        if (metadata.hasError()) {
            std::fprintf(stderr, "probe failed: %s\n", metadata.error().message().c_str());
            return 5;
        }
        const nlohmann::json parsed = nlohmann::json::parse(metadata.output(), nullptr, false);
        if (parsed.is_discarded() || !parsed.contains("streams") || !parsed["streams"].is_array() ||
            parsed["streams"].empty())
            return 6;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        const MaiFilePath output = MaiFileSystem::temporaryDirectory().append(
            MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_ffmpeg_test_") + ".ppm"));
        const nlohmann::json request = {
            {"arguments",
             {"-i", input.toUtf8(), "-frames:v", "1", "-f", "image2", output.toUtf8()}}};
        const MaiToolResult result = convert->execute(request.dump(), context);
        if (result.hasError()) {
            std::fprintf(stderr, "conversion failed: %s\n", result.error().message().c_str());
            return 7;
        }
        std::string bytes;
        const MaiError readError = MaiFileSystem::readFile(output, bytes, 16);
        MaiFileSystem::removeFile(output);
        if (readError.hasError() || bytes.compare(0, 2, "P6") != 0) return 8;
    }

    std::atomic<bool> canceled{false};
    MaiFfmpegEngine fakeEngine{runUntilCanceled, setFakeCancelCheck, nullptr, nullptr};
    auto fakeTool = makeMaiFfmpegTool(fakeEngine);
    context.cancel = &canceled;
    std::thread requestCancel([&] {
        MaiThread::setCurrentName("mai-test-cancel");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        canceled.store(true, std::memory_order_release);
    });
    const MaiToolResult canceledResult =
        fakeTool->execute(R"({"arguments":["-version"]})", context);
    requestCancel.join();
    if (!canceledResult.hasError() || canceledResult.error().code() != MaiErrorCode::Canceled)
        return 10;

    canceled.store(false, std::memory_order_release);
    std::thread cancelRealRun([&] {
        MaiThread::setCurrentName("mai-test-cancel");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        canceled.store(true, std::memory_order_release);
    });
    const nlohmann::json longRun = {
        {"arguments",
         {"-re", "-stream_loop", "-1", "-i", input.toUtf8(), "-t", "10", "-f", "null", "-"}}};
    const MaiToolResult interrupted = convert->execute(longRun.dump(), context);
    cancelRealRun.join();
    if (!interrupted.hasError() || interrupted.error().code() != MaiErrorCode::Canceled) return 11;
    return 0;
}
