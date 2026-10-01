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
std::string sCapturedProbeOutput;
std::string sCapturedProbeInput;
std::string sCapturedConvertInput;
std::string sCapturedConvertOutput;

void setFakeCancelCheck(int (*check)(void*), void* opaque) {
    sFakeCancelCheck = check;
    sFakeCancelOpaque = opaque;
}

int runUntilCanceled(int, char**) {
    while (!sFakeCancelCheck || !sFakeCancelCheck(sFakeCancelOpaque))
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return 255;
}

int captureProbePaths(int argc, char** argv) {
    for (int index = 0; index + 1 < argc; ++index)
        if (std::string(argv[index]) == "-o") sCapturedProbeOutput = argv[index + 1];
    sCapturedProbeInput = argv[argc - 1];
    return MaiFileSystem::writeFile(MaiFilePath::fromUtf8(sCapturedProbeOutput),
                                    R"({"streams":[{"codec_type":"video"}]})")
                   .hasError()
               ? 1 : 0;
}

int captureConvertPaths(int argc, char** argv) {
    for (int index = 0; index + 1 < argc; ++index)
        if (std::string(argv[index]) == "-i") sCapturedConvertInput = argv[index + 1];
    sCapturedConvertOutput = argv[argc - 1];
    return 0;
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

    const MaiFilePath workspace = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_media_workspace_")));
    if (MaiFileSystem::createDirectories(workspace).hasError()) return 16;
    const MaiFilePath workspaceInput = workspace.append(MaiFilePath::fromUtf8("gallery.mp4"));
    if (MaiFileSystem::writeFile(workspaceInput, "media").hasError()) return 17;
    MaiToolContext workspaceContext = context;
    workspaceContext.root = workspace.toUtf8();
    workspaceContext.fileAccessRoot = workspace.dirName().toUtf8();
    workspaceContext.allowOutsideWorkingDirectory = false;
    MaiFfmpegEngine pathEngine{captureConvertPaths, nullptr, captureProbePaths, nullptr};
    const MaiToolResult workspaceProbe = makeMaiFfprobeTool(pathEngine)->execute(
        R"({"path":"gallery.mp4"})", workspaceContext);
    const MaiToolResult workspaceConvert = makeMaiFfmpegTool(pathEngine)->execute(
        R"({"arguments":["-i","gallery.mp4","-frames:v","1","frame.png"]})",
        workspaceContext);
    const std::string expectedInput = MaiFileSystem::resolve(workspaceInput).toUtf8();
    const std::string expectedOutput = MaiFileSystem::resolve(
        workspace.append(MaiFilePath::fromUtf8("frame.png"))).toUtf8();
    const std::string outputDirectory = MaiFileSystem::resolve(
        MaiFilePath::fromUtf8(sCapturedProbeOutput)).dirName().toUtf8();
    const bool workspacePathsCorrect = !workspaceProbe.hasError() &&
        !workspaceConvert.hasError() && sCapturedProbeInput == expectedInput &&
        outputDirectory == MaiFileSystem::resolve(workspace).toUtf8() &&
        sCapturedConvertInput == expectedInput && sCapturedConvertOutput == expectedOutput;
    MaiFileSystem::removeRecursively(workspace);
    if (!workspacePathsCorrect) {
        std::fprintf(stderr, "workspace probe error=%s convert error=%s\n"
                             "probe input=%s output=%s\nconvert input=%s output=%s\n",
                     workspaceProbe.hasError() ? workspaceProbe.error().message().c_str() : "none",
                     workspaceConvert.hasError() ? workspaceConvert.error().message().c_str() : "none",
                     sCapturedProbeInput.c_str(), sCapturedProbeOutput.c_str(),
                     sCapturedConvertInput.c_str(), sCapturedConvertOutput.c_str());
        return 18;
    }

#ifdef _WIN32
    std::string fixtureBytes;
    if (MaiFileSystem::readFile(input, fixtureBytes).hasError())
        return 12;
    const MaiFilePath utf8Input = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_") + "_\xE6\xB5\x8B\xE8\xAF\x95.mp4"));
    if (MaiFileSystem::writeFile(utf8Input, fixtureBytes).hasError())
        return 13;
    const MaiToolResult utf8Metadata =
        probe->execute(nlohmann::json{{"path", utf8Input.toUtf8()}}.dump(), context);
    if (utf8Metadata.hasError()) {
        std::fprintf(stderr, "UTF-8 path probe failed: %s\n",
                     utf8Metadata.error().message().c_str());
        MaiFileSystem::removeFile(utf8Input);
        return 14;
    }
    const MaiFilePath utf8Output = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_") + "_\xE6\xB5\x8B\xE8\xAF\x95.ppm"));
    const nlohmann::json utf8Request = {
        {"arguments",
         {"-i", utf8Input.toUtf8(), "-frames:v", "1", "-f", "image2", utf8Output.toUtf8()}}};
    const MaiToolResult utf8Conversion = convert->execute(utf8Request.dump(), context);
    std::string utf8OutputBytes;
    const MaiError utf8ReadError = MaiFileSystem::readFile(utf8Output, utf8OutputBytes, 16);
    MaiFileSystem::removeFile(utf8Input);
    MaiFileSystem::removeFile(utf8Output);
    if (utf8Conversion.hasError() || utf8ReadError.hasError() ||
        utf8OutputBytes.compare(0, 2, "P6") != 0)
        return 15;
#endif

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
        if (readError.hasError() || bytes.compare(0, 2, "P6") != 0)
            return 8;
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
