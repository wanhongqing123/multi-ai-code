#include "MaiFfmpegTools.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"

using json = nlohmann::json;

namespace {

constexpr std::uint64_t kMaximumProbeBytes = 2u * 1024u * 1024u;
constexpr std::size_t kMaximumDiagnosticBytes = 4096;
std::mutex sFfmpegCommandMutex;

struct DiagnosticCapture {
    std::mutex mutex;
    std::string text;
};

int shouldCancel(void* opaque) {
    const auto* token = static_cast<const std::atomic<bool>*>(opaque);
    return token->load(std::memory_order_relaxed) ? 1 : 0;
}

void captureDiagnostic(void* opaque, const char* line) {
    if (!opaque || !line) return;
    auto& capture = *static_cast<DiagnosticCapture*>(opaque);
    std::lock_guard<std::mutex> lock(capture.mutex);
    capture.text.append(line);
    if (capture.text.size() > kMaximumDiagnosticBytes)
        capture.text.erase(0, capture.text.size() - kMaximumDiagnosticBytes);
}

int runEngine(int (*execute)(int, char**), void (*setCancelCheck)(int (*)(void*), void*),
              const MaiFfmpegEngine& engine, std::vector<std::string> arguments,
              const MaiToolContext& context, std::string& diagnostic) {
    std::lock_guard<std::mutex> commandLock(sFfmpegCommandMutex);
    if (context.isCanceled()) return -1;

    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (std::string& argument : arguments) argv.push_back(argument.data());

    if (setCancelCheck)
        setCancelCheck(context.cancel ? shouldCancel : nullptr,
                       const_cast<std::atomic<bool>*>(context.cancel));
    DiagnosticCapture capture;
    if (engine.setLogSink) engine.setLogSink(captureDiagnostic, &capture);
    const int status = execute(static_cast<int>(argv.size()), argv.data());
    if (engine.setLogSink) engine.setLogSink(nullptr, nullptr);
    diagnostic = std::move(capture.text);
    if (setCancelCheck) setCancelCheck(nullptr, nullptr);
    return status;
}

std::string errorDescription(const MaiFfmpegEngine& engine, int status,
                             const std::string& diagnostic) {
    char text[256] = {};
    std::string result;
    if (engine.describeError && engine.describeError(status, text, sizeof(text)) == 0)
        result = text;
    else
        result = "status " + std::to_string(status);
    if (!diagnostic.empty()) result += "\n" + diagnostic;
    return result;
}

class MaiFfmpegTool final : public MaiTool {
public:
    explicit MaiFfmpegTool(MaiFfmpegEngine engine) : mEngine(engine) {}

    std::string name() const override {
        return "ffmpeg";
    }

    std::string description() const override {
        return "Transform an accessible local image, audio, or video using the in-process FFmpeg "
               "engine. Supply FFmpeg arguments without the program name. Stdin is disabled "
               "and existing outputs cannot be overwritten; choose a new output path.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"arguments":{"type":"array","items":)"
               R"({"type":"string"},"minItems":1,"maxItems":64}},"required":["arguments"],)"
               R"("additionalProperties":false})";
    }

    bool requiresApproval(const std::string&) const override {
        return true;
    }

    std::string approvalKey(const std::string& argumentsJson) const override {
        const json request = json::parse(argumentsJson, nullptr, false);
        return "ffmpeg:" + (request.is_discarded() ? argumentsJson : request.dump());
    }

    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        const json request = json::parse(argumentsJson, nullptr, false);
        if (request.is_discarded() || !request.is_object() || request.size() != 1 ||
            !request.contains("arguments") || !request["arguments"].is_array() ||
            request["arguments"].empty() || request["arguments"].size() > 64)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "ffmpeg requires 1-64 string arguments");

        std::vector<std::string> arguments = {"ffmpeg", "-nostdin", "-n", "-hide_banner"};
        for (const json& item : request["arguments"]) {
            if (!item.is_string())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "every FFmpeg argument must be a string");
            const std::string value = item.get<std::string>();
            if (value.empty() || value.find('\0') != std::string::npos || value == "-y" ||
                value == "-stdin")
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "empty arguments, -y, and -stdin are not allowed");
            arguments.push_back(value);
        }
        for (std::size_t index = 5; index < arguments.size(); ++index) {
            if (arguments[index - 1] != "-i") continue;
            const bool filterInput =
                index >= 3 && arguments[index - 3] == "-f" && arguments[index - 2] == "lavfi";
            const std::string& value = arguments[index];
            if (filterInput || value == "-" || value.rfind("pipe:", 0) == 0 ||
                value.find("://") != std::string::npos)
                continue;
            if (value.rfind("file:", 0) == 0)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "use a local path instead of a file: URL");
            const std::string resolved = context.resolvePath(value);
            if (resolved.empty())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "FFmpeg input path is outside the accessible area");
            arguments[index] = resolved;
        }
        if (arguments.size() > 4 && arguments[arguments.size() - 2] != "-i") {
            std::string& output = arguments.back();
            if (output != "-" && output.rfind("pipe:", 0) != 0 &&
                output.find("://") == std::string::npos && output[0] != '-') {
                if (output.rfind("file:", 0) == 0)
                    return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                  "use a local path instead of a file: URL");
                const std::string resolved = context.resolvePath(output);
                if (resolved.empty())
                    return MaiToolResult::failure(
                        MaiErrorCode::InvalidInput,
                        "FFmpeg output path is outside the accessible area");
                output = resolved;
            }
        }
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "FFmpeg was canceled");
        std::string diagnostic;
        const int status = runEngine(mEngine.runFfmpeg, mEngine.setFfmpegCancelCheck, mEngine,
                                     std::move(arguments), context, diagnostic);
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "FFmpeg was canceled");
        if (status != 0)
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "FFmpeg failed: " + errorDescription(mEngine, status, diagnostic));
        return MaiToolResult::success("FFmpeg completed with status 0.");
    }

private:
    MaiFfmpegEngine mEngine;
};

class MaiFfprobeTool final : public MaiTool {
public:
    explicit MaiFfprobeTool(MaiFfmpegEngine engine) : mEngine(engine) {}

    std::string name() const override {
        return "ffprobe";
    }

    std::string description() const override {
        return "Read streams and container metadata from an accessible local media file "
               "with the in-process FFprobe engine. Returns JSON.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"path":{"type":"string"}},)"
               R"("required":["path"],"additionalProperties":false})";
    }

    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        const json request = json::parse(argumentsJson, nullptr, false);
        if (request.is_discarded() || !request.is_object() || request.size() != 1 ||
            !request.contains("path") || !request["path"].is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "ffprobe requires one string path");
        const std::string input = context.resolvePath(request["path"].get<std::string>());
        if (input.empty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "media path is outside the accessible area");
        const MaiFilePath inputPath = MaiFilePath::fromUtf8(input);
        if (!MaiFileSystem::exists(inputPath) || MaiFileSystem::isDirectory(inputPath))
            return MaiToolResult::failure(MaiErrorCode::NotFound,
                                          "media file does not exist: " + input);

        // fileAccessRoot is an access boundary, not a scratch directory.
        const MaiFilePath directory = context.root.empty() ? MaiFileSystem::temporaryDirectory()
                                                           : MaiFilePath::fromUtf8(context.root);
        const MaiFilePath output = directory.append(
            MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_ffprobe_") + ".json"));
        const MaiError createError = MaiFileSystem::createEmptyFile(output);
        if (createError.hasError())
            return MaiToolResult::failure(createError.code(), createError.message());

        std::vector<std::string> arguments = {"ffprobe",       "-v",  "error", "-show_format",
                                              "-show_streams", "-of", "json",  "-o",
                                              output.toUtf8(), input};
        std::string diagnostic;
        const int status = runEngine(mEngine.runFfprobe, mEngine.setFfprobeCancelCheck, mEngine,
                                     std::move(arguments), context, diagnostic);
        std::string result;
        bool truncated = false;
        const MaiError readError =
            MaiFileSystem::readFile(output, result, kMaximumProbeBytes, &truncated);
        MaiFileSystem::removeFile(output);
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "FFprobe was canceled");
        if (status != 0)
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "FFprobe failed to read media: " + errorDescription(mEngine, status, diagnostic));
        if (readError.hasError())
            return MaiToolResult::failure(readError.code(), readError.message());
        if (result.empty())
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "FFprobe produced no metadata for: " + input);
        return MaiToolResult::success(std::move(result), truncated);
    }

private:
    MaiFfmpegEngine mEngine;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiFfmpegTool(MaiFfmpegEngine engine) {
    return engine.runFfmpeg ? std::make_unique<MaiFfmpegTool>(engine) : nullptr;
}

std::unique_ptr<MaiTool> makeMaiFfprobeTool(MaiFfmpegEngine engine) {
    return engine.runFfprobe ? std::make_unique<MaiFfprobeTool>(engine) : nullptr;
}

MaiImagePreviewCallback makeMaiFfmpegImagePreview(MaiFfmpegEngine engine) {
    return [engine](const std::string& source,
                    const MaiToolContext& context) -> MaiResult<std::string> {
        if (!engine.runFfmpeg || context.root.empty())
            return {
                MaiErrorCode::NotConfigured,
                "a writable Agent workspace and embedded FFmpeg are required for image previews"};
        const MaiFilePath directory = MaiFilePath::fromUtf8(context.root)
                                          .append(MaiFilePath::fromUtf8(".maiagent"))
                                          .append(MaiFilePath::fromUtf8("model-previews"));
        const MaiError createError = MaiFileSystem::createDirectories(directory);
        if (createError.hasError()) return createError;
        const MaiFilePath output = directory.append(
            MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_model_preview_") + ".jpg"));
        const json arguments = {{"arguments",
                                 {"-i", source, "-vf",
                                  "scale=w='min(iw,1280)':h='min(ih,1280)':"
                                  "force_original_aspect_ratio=decrease",
                                  "-frames:v", "1", "-q:v", "5", "-f", "image2", output.toUtf8()}}};
        MaiFfmpegTool tool(engine);
        const MaiToolResult result = tool.execute(arguments.dump(), context);
        if (result.hasError()) {
            if (MaiFileSystem::exists(output)) MaiFileSystem::removeFile(output);
            return result.error();
        }
        std::uint64_t bytes = 0;
        if (!MaiFileSystem::fileSize(output, bytes) || bytes == 0 || bytes > 2u * 1024u * 1024u) {
            if (MaiFileSystem::exists(output)) MaiFileSystem::removeFile(output);
            return {MaiErrorCode::InvalidInput,
                    "the model preview could not be limited to 2 MB; resize the source image"};
        }
        return output.toUtf8();
    };
}

MaiModelImagePreparer makeMaiFfmpegModelImagePreparer(MaiFfmpegEngine engine) {
    MaiImagePreviewCallback preview = makeMaiFfmpegImagePreview(engine);
    return [preview = std::move(preview)](const std::string& source, const std::string& workspace) {
        MaiToolContext context;
        context.root = workspace;
        context.allowOutsideWorkingDirectory = true;
        return preview(source, context);
    };
}
