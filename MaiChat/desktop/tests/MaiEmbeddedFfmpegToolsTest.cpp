#include "MaiFfmpegTools.h"

#include <cstdio>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

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

MaiToolResult runFilterGraph(MaiTool& convert, const MaiToolContext& context,
                             const std::vector<std::string>& sources,
                             const std::string& graph) {
    std::vector<std::string> arguments;
    for (const std::string& source : sources)
        arguments.insert(arguments.end(), {"-f", "lavfi", "-i", source});
    arguments.insert(arguments.end(), {"-filter_complex", graph, "-map", "[out]",
                                       "-frames:v", "1", "-f", "null", "-"});
    return convert.execute(nlohmann::json{{"arguments", arguments}}.dump(), context);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const MaiFilePath input = MaiFilePath::fromUtf8(argv[1]);
    if (!MaiFileSystem::exists(input)) return 3;

    MaiFfmpegEngine engine{mai_ffmpeg_execute, mai_ffmpeg_set_cancel_check, mai_ffprobe_execute,
                           mai_ffprobe_set_cancel_check, mai_fftools_set_log_sink,
                           mai_fftools_error_string};
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

    const MaiFilePath av1Input = input.dirName().append(MaiFilePath::fromUtf8("tiny-red-av1.webm"));
    if (!MaiFileSystem::exists(av1Input)) return 19;
    const MaiFilePath av1Output = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_av1_test_") + ".ppm"));
    const MaiToolResult av1Result = convert->execute(
        nlohmann::json{
            {"arguments",
             {"-i", av1Input.toUtf8(), "-frames:v", "1", "-f", "image2", av1Output.toUtf8()}}}
            .dump(),
        context);
    std::string av1Bytes;
    const MaiError av1Read = MaiFileSystem::readFile(av1Output, av1Bytes, 16);
    MaiFileSystem::removeFile(av1Output);
    if (av1Result.hasError() || av1Read.hasError() || av1Bytes.compare(0, 2, "P6") != 0) {
        std::fprintf(stderr, "AV1 software decode failed: %s\n",
                     av1Result.hasError() ? av1Result.error().message().c_str()
                                          : "no PPM output");
        return 20;
    }

    const MaiFilePath lavfiOutput = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_lavfi_test_") + ".ppm"));
    const MaiToolResult lavfiResult =
        convert->execute(nlohmann::json{{"arguments",
                                         {"-f", "lavfi", "-i", "testsrc=size=16x16:rate=1",
                                          "-frames:v", "1", "-f", "image2", lavfiOutput.toUtf8()}}}
                             .dump(),
                         context);
    std::string lavfiBytes;
    const MaiError lavfiRead = MaiFileSystem::readFile(lavfiOutput, lavfiBytes, 16);
    MaiFileSystem::removeFile(lavfiOutput);
    if (lavfiResult.hasError() || lavfiRead.hasError() || lavfiBytes.compare(0, 2, "P6") != 0) {
        std::fprintf(
            stderr, "lavfi source failed: %s\n",
            lavfiResult.hasError() ? lavfiResult.error().message().c_str() : "no PPM output");
        return 21;
    }

    const std::vector<std::string> fourSources = {
        "color=c=red:s=64x64:d=0.1:r=1", "color=c=white:s=64x64:d=0.1:r=1",
        "color=c=gray:s=64x64:d=0.1:r=1", "color=c=blue:s=64x64:d=0.1:r=1"};
    const std::string maskedMergeGraph =
        "[0:v]format=rgb24[fg];[1:v]format=gray[pm];"
        "[2:v]format=gray[dm];[pm][dm]blend=all_mode=lighten[cm];"
        "[3:v]format=rgb24[bg];[bg][fg][cm]maskedmerge[out]";
    const MaiToolResult mixedFormats = runFilterGraph(*convert, context, fourSources,
                                                     maskedMergeGraph);
    if (mixedFormats.hasError()) {
        std::fprintf(stderr, "mixed-format maskedmerge failed: %s\n",
                     mixedFormats.error().message().c_str());
        return 29;
    }
    const MaiToolResult missingFourthInput = runFilterGraph(
        *convert, context, {fourSources[0], fourSources[1], fourSources[2]}, maskedMergeGraph);
    if (!missingFourthInput.hasError() ||
        missingFourthInput.error().message().find("Invalid file index 3") ==
            std::string::npos ||
        missingFourthInput.error().message().find("needs a fourth -i input") ==
            std::string::npos) {
        std::fprintf(stderr, "fourth-input diagnostic missing: %s\n",
                     missingFourthInput.hasError() ? missingFourthInput.error().message().c_str()
                                                   : "no failure");
        return 30;
    }
    const MaiToolResult extraOutputLabel = runFilterGraph(
        *convert, context, {fourSources[0], fourSources[1], fourSources[2]},
        "[0:v][1:v][2:v]maskedmerge[out][extra]");
    if (!extraOutputLabel.hasError() ||
        extraOutputLabel.error().message().find("More output link labels specified") ==
            std::string::npos ||
        extraOutputLabel.error().message().find("Pixel formats are negotiated later") ==
            std::string::npos ||
        extraOutputLabel.error().message().find("filter_complex as received") ==
            std::string::npos) {
        std::fprintf(stderr, "maskedmerge parser diagnostic missing: %s\n",
                     extraOutputLabel.hasError() ? extraOutputLabel.error().message().c_str()
                                                 : "no failure");
        return 31;
    }
    const MaiToolResult nestedExpression = runFilterGraph(
        *convert, context, {"testsrc2=s=128x128:d=0.1:r=1"},
        R"([0:v]format=rgb24,split[t1][t2];[t1]boxblur=8[norm];[t2][norm]blend=all_mode=difference,geq=lum='255-100*pow(p(X\,Y)/255\,2)'[out])");
    if (nestedExpression.hasError()) {
        std::fprintf(stderr, "nested geq expression failed: %s\n",
                     nestedExpression.error().message().c_str());
        return 32;
    }

    const MaiFilePath unsupportedOutput = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_encoder_test_") + ".mp4"));
    const MaiToolResult unsupportedEncoder = convert->execute(
        nlohmann::json{
            {"arguments",
             {"-i", input.toUtf8(), "-c:v", "maichat_missing_encoder", unsupportedOutput.toUtf8()}}}
            .dump(),
        context);
    MaiFileSystem::removeFile(unsupportedOutput);
    if (!unsupportedEncoder.hasError() ||
        unsupportedEncoder.error().message().find("Encoder not found") == std::string::npos) {
        std::fprintf(stderr, "encoder diagnostic missing: %s\n",
                     unsupportedEncoder.hasError() ? unsupportedEncoder.error().message().c_str()
                                                   : "no failure");
        return 22;
    }

    const MaiFilePath x264Output = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_x264_test_") + ".mp4"));
    const MaiToolResult x264Encode = convert->execute(
        nlohmann::json{{"arguments", {"-i", input.toUtf8(), "-an", "-vf",
                                       "hqdn3d=1.5:1.5:6:6,eq=contrast=1.05:brightness=0.02,"
                                       "unsharp=3:3:0.5", "-c:v", "libx264", "-preset",
                                       "ultrafast", "-crf", "28", x264Output.toUtf8()}}}.dump(),
        context);
    const MaiToolResult x264Metadata = x264Encode.hasError()
        ? x264Encode
        : probe->execute(nlohmann::json{{"path", x264Output.toUtf8()}}.dump(), context);
    MaiFileSystem::removeFile(x264Output);
    if (x264Metadata.hasError() ||
        x264Metadata.output().find("\"codec_name\": \"h264\"") == std::string::npos) {
        std::fprintf(stderr, "libx264/filter encode failed: %s\n",
                     x264Metadata.hasError() ? x264Metadata.error().message().c_str()
                                             : x264Metadata.output().c_str());
        return 24;
    }

    const MaiFilePath mp3Output = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_lame_test_") + ".mp3"));
    const MaiToolResult mp3Encode = convert->execute(
        nlohmann::json{{"arguments", {"-f", "lavfi", "-i",
                                       "sine=frequency=440:sample_rate=44100", "-t", "1",
                                       "-c:a", "libmp3lame", mp3Output.toUtf8()}}}.dump(),
        context);
    const MaiToolResult mp3Metadata = mp3Encode.hasError()
        ? mp3Encode
        : probe->execute(nlohmann::json{{"path", mp3Output.toUtf8()}}.dump(), context);
    MaiFileSystem::removeFile(mp3Output);
    if (mp3Metadata.hasError() ||
        mp3Metadata.output().find("\"codec_name\": \"mp3\"") == std::string::npos) {
        std::fprintf(stderr, "libmp3lame encode failed: %s\n",
                     mp3Metadata.hasError() ? mp3Metadata.error().message().c_str()
                                            : mp3Metadata.output().c_str());
        return 25;
    }

    const MaiFilePath previewWorkspace = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_preview_test_")));
    if (MaiFileSystem::createDirectories(previewWorkspace).hasError()) return 26;
    MaiToolContext previewContext = context;
    previewContext.root = previewWorkspace.toUtf8();
    previewContext.allowOutsideWorkingDirectory = false;
    const MaiFilePath largeImage = previewWorkspace.append(MaiFilePath::fromUtf8("large.png"));
    const MaiToolResult generatedImage = convert->execute(
        nlohmann::json{{"arguments", {"-f", "lavfi", "-i", "testsrc=size=1920x1080:rate=1",
                                       "-frames:v", "1", largeImage.toUtf8()}}}.dump(),
        previewContext);
    if (generatedImage.hasError()) {
        std::fprintf(stderr, "image fixture generation failed: %s\n",
                     generatedImage.error().message().c_str());
        MaiFileSystem::removeRecursively(previewWorkspace);
        return 27;
    }
    const MaiToolResult preview = makeMaiViewImageTool(makeMaiFfmpegImagePreview(engine))
                                      ->execute(R"({"path":"large.png"})", previewContext);
    bool previewValid = !preview.hasError() && preview.images().size() == 1;
    if (previewValid) {
        const auto& image = preview.images().front();
        std::uint64_t previewBytes = 0;
        previewValid = image.mimeType == "image/jpeg" && image.path != largeImage.toUtf8() &&
                       MaiFileSystem::fileSize(MaiFilePath::fromUtf8(image.path), previewBytes) &&
                       previewBytes > 0 && previewBytes <= 2u * 1024u * 1024u;
        if (previewValid) {
            const MaiToolResult dimensions = probe->execute(
                nlohmann::json{{"path", image.path}}.dump(), previewContext);
            const nlohmann::json metadata = nlohmann::json::parse(
                dimensions.hasError() ? "" : dimensions.output(), nullptr, false);
            previewValid = !metadata.is_discarded() && metadata.contains("streams") &&
                           !metadata["streams"].empty() &&
                           metadata["streams"][0].value("width", 0) <= 1280 &&
                           metadata["streams"][0].value("height", 0) <= 1280;
        }
    }
    MaiFileSystem::removeRecursively(previewWorkspace);
    if (!previewValid) {
        std::fprintf(stderr, "bounded image preview failed: %s\n",
                     preview.hasError() ? preview.error().message().c_str() : "invalid preview");
        return 28;
    }

#if defined(__APPLE__) && !TARGET_OS_SIMULATOR
    const MaiFilePath hardwareOutput = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_videotoolbox_test_") + ".mp4"));
    const MaiToolResult hardwareEncode =
        convert->execute(nlohmann::json{{"arguments",
                                         {"-i", input.toUtf8(), "-an", "-c:v", "h264_videotoolbox",
                                          "-b:v", "200k", hardwareOutput.toUtf8()}}}
                             .dump(),
                         context);
    const MaiToolResult hardwareMetadata =
        hardwareEncode.hasError()
            ? hardwareEncode
            : probe->execute(nlohmann::json{{"path", hardwareOutput.toUtf8()}}.dump(), context);
    MaiFileSystem::removeFile(hardwareOutput);
    if (hardwareMetadata.hasError() ||
        hardwareMetadata.output().find("\"codec_name\": \"h264\"") == std::string::npos) {
        std::fprintf(stderr, "VideoToolbox encode failed: %s\n",
                     hardwareMetadata.hasError() ? hardwareMetadata.error().message().c_str()
                                                 : hardwareMetadata.output().c_str());
        return 23;
    }
#endif

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
