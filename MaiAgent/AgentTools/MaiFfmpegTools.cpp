#include "MaiFfmpegTools.h"

#include <atomic>
#include <cmath>
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
    std::size_t limit = kMaximumDiagnosticBytes;
    bool truncated = false;
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
    if (capture.text.size() > capture.limit) {
        capture.text.erase(0, capture.text.size() - capture.limit);
        capture.truncated = true;
    }
}

int runEngine(int (*execute)(int, char**), void (*setCancelCheck)(int (*)(void*), void*),
              const MaiFfmpegEngine& engine, std::vector<std::string> arguments,
              const MaiToolContext& context, std::string& diagnostic,
              bool* diagnosticTruncated = nullptr,
              std::size_t diagnosticLimit = kMaximumDiagnosticBytes) {
    std::lock_guard<std::mutex> commandLock(sFfmpegCommandMutex);
    if (context.isCanceled()) return -1;

    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (std::string& argument : arguments) argv.push_back(argument.data());

    if (setCancelCheck)
        setCancelCheck(context.cancel ? shouldCancel : nullptr,
                       const_cast<std::atomic<bool>*>(context.cancel));
    DiagnosticCapture capture;
    capture.limit = diagnosticLimit;
    if (engine.setLogSink) engine.setLogSink(captureDiagnostic, &capture);
    const int status = execute(static_cast<int>(argv.size()), argv.data());
    if (engine.setLogSink) engine.setLogSink(nullptr, nullptr);
    diagnostic = std::move(capture.text);
    if (diagnosticTruncated != nullptr) *diagnosticTruncated = capture.truncated;
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

std::string filterGraphFailureContext(const std::string& graph, const std::string& diagnostic) {
    if (graph.empty()) return {};
    std::string context;
    if (diagnostic.find("More output link labels specified for filter 'maskedmerge'") !=
        std::string::npos) {
        context =
            "\nmaskedmerge has one output. FFmpeg parsed two labels after that filter; "
            "check the graph syntax near maskedmerge. Pixel formats are negotiated later.";
    } else if (diagnostic.find("Invalid file index") != std::string::npos) {
        context = "\nFilter input labels are zero-based: [3:v] needs a fourth -i input.";
    }
    constexpr std::size_t kMaximumGraphBytes = 1200;
    const bool truncated = graph.size() > kMaximumGraphBytes;
    context += "\nfilter_complex as received: " + json(graph.substr(0, kMaximumGraphBytes)).dump();
    if (truncated) context += " [truncated]";
    return context;
}

class MaiFfmpegTool final : public MaiTool {
public:
    explicit MaiFfmpegTool(MaiFfmpegEngine engine) : mEngine(engine) {}

    std::string name() const override {
        return "ffmpeg";
    }

    std::string description() const override {
        // FFmpeg 在进程内执行：只接参数数组，不提供 stdin，目标文件必须是新路径。
        // 分析滤镜需要 capture_log=true 才会把诊断日志交回主模型。
        return "Transform an accessible local image, audio, or video using the in-process FFmpeg "
               "engine. Supply FFmpeg arguments without the program name. Stdin is disabled "
               "and existing outputs cannot be overwritten; choose a new output path. For "
               "analysis filters such as volumedetect or blackdetect, set capture_log=true to "
               "receive up to 16 KiB of FFmpeg diagnostic output.";
    }

    std::string parametersSchema() const override {
        // arguments 不包含 ffmpeg 程序名；capture_log 只在需要诊断滤镜日志时开启。
        return R"({"type":"object","properties":{"arguments":{"type":"array","items":)"
               R"({"type":"string"},"minItems":1,"maxItems":64},)"
               R"("capture_log":{"type":"boolean"}},"required":["arguments"],)"
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
        if (request.is_discarded() || !request.is_object() || request.size() > 2 ||
            !request.contains("arguments") || !request["arguments"].is_array() ||
            request["arguments"].empty() || request["arguments"].size() > 64)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "ffmpeg requires 1-64 string arguments");
        if (request.contains("capture_log") && !request["capture_log"].is_boolean())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "capture_log must be boolean");
        if (request.size() == 2 && !request.contains("capture_log"))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "unsupported FFmpeg argument field");
        const bool captureLog = request.value("capture_log", false);

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
        std::string filterGraph;
        for (std::size_t index = 0; index + 1 < arguments.size(); ++index) {
            if (arguments[index] == "-filter_complex") filterGraph = arguments[index + 1];
        }
        std::string diagnostic;
        bool logTruncated = false;
        const int status = runEngine(mEngine.runFfmpeg, mEngine.setFfmpegCancelCheck, mEngine,
                                     std::move(arguments), context, diagnostic, &logTruncated,
                                     captureLog ? 16 * 1024 : kMaximumDiagnosticBytes);
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "FFmpeg was canceled");
        if (status != 0)
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "FFmpeg failed: " + errorDescription(mEngine, status, diagnostic) +
                    filterGraphFailureContext(filterGraph, diagnostic));
        if (captureLog)
            return MaiToolResult::success(
                json{{"status", 0}, {"log_tail", diagnostic}, {"log_truncated", logTruncated}}
                    .dump(),
                logTruncated);
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
        // FFprobe 仅读取容器和码流元数据；帧/包模式有范围限制，不会改动媒体。
        return "Read streams and container metadata from an accessible local media file "
               "with the in-process FFprobe engine. Metadata mode returns format, streams and "
               "chapters, including codec, duration, frame rate, rotation and color/HDR fields "
               "when present. Keyframes, frames and packets modes return bounded timestamp "
               "ranges for timing analysis. This reads the file without modifying it and "
               "returns complete JSON or an explicit size error.";
    }

    std::string parametersSchema() const override {
        // path 为输入媒体；mode 选择整体元数据或有界帧/包时间窗。
        return R"({"type":"object","properties":{"path":{"type":"string"},)"
               R"("mode":{"type":"string","enum":["metadata","keyframes","frames","packets"]},)"
               R"("include_chapters":{"type":"boolean"},"include_programs":{"type":"boolean"},)"
               R"("count_frames":{"type":"boolean"},"select_streams":{"type":"string"},)"
               R"("start_s":{"type":"number","minimum":0},)"
               R"("duration_s":{"type":"number","minimum":0.1,"maximum":120}},)"
               R"("required":["path"],"additionalProperties":false})";
    }

    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        const json request = json::parse(argumentsJson, nullptr, false);
        if (request.is_discarded() || !request.is_object() || !request.contains("path") ||
            !request["path"].is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "ffprobe requires one string path");
        if (request.contains("mode") && !request["mode"].is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "ffprobe mode must be a string");
        const std::string mode = request.value("mode", std::string("metadata"));
        if (mode != "metadata" && mode != "keyframes" && mode != "frames" && mode != "packets")
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "ffprobe mode must be metadata, keyframes, frames, or packets");
        for (const char* field : {"include_chapters", "include_programs", "count_frames"}) {
            if (request.contains(field) && !request[field].is_boolean())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              std::string(field) + " must be boolean");
        }
        if (request.contains("select_streams") && !request["select_streams"].is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "select_streams must be a string");
        const std::string selector = request.value("select_streams", std::string{});
        if (!selector.empty() && !validStreamSelector(selector))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "select_streams must be v, a, s, d or TYPE:INDEX");
        if (mode == "keyframes" && !selector.empty() && selector[0] != 'v')
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "keyframes mode requires a video stream selector");
        if (mode == "metadata" && (request.contains("start_s") || request.contains("duration_s")))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "start_s and duration_s require a timing mode");
        double start = 0;
        double duration = mode == "keyframes" ? 30 : 5;
        const double maximumDuration = mode == "keyframes" ? 120 : 30;
        if (mode != "metadata" && (!number(request, "start_s", start, 0, 1'000'000) ||
                                   !number(request, "duration_s", duration, 0.1, maximumDuration)))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "invalid timing interval; keyframes allow 120 s, "
                                          "frames and packets allow 30 s");
        const std::string input = context.resolvePath(request["path"].get<std::string>());
        if (input.empty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "media path is outside the accessible area");
        const MaiFilePath inputPath = MaiFilePath::fromUtf8(input);
        if (!MaiFileSystem::exists(inputPath) || MaiFileSystem::isDirectory(inputPath))
            return MaiToolResult::failure(MaiErrorCode::NotFound,
                                          "media file does not exist: " + input);

        // fileAccessRoot 是文件访问边界，不是可随意写入的临时目录。
        const MaiFilePath directory = context.root.empty() ? MaiFileSystem::temporaryDirectory()
                                                           : MaiFilePath::fromUtf8(context.root);
        const MaiFilePath output = directory.append(
            MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_ffprobe_") + ".json"));
        const MaiError createError = MaiFileSystem::createEmptyFile(output);
        if (createError.hasError())
            return MaiToolResult::failure(createError.code(), createError.message());

        std::vector<std::string> arguments = {"ffprobe", "-v", "error"};
        if (mode == "metadata") {
            arguments.insert(arguments.end(), {"-show_format", "-show_streams"});
            if (request.value("include_chapters", true)) arguments.push_back("-show_chapters");
            if (request.value("include_programs", false)) arguments.push_back("-show_programs");
            if (request.value("count_frames", false)) arguments.push_back("-count_frames");
        } else if (mode == "keyframes") {
            arguments.insert(
                arguments.end(),
                {"-skip_frame", "nokey", "-show_frames", "-show_entries",
                 "frame=best_effort_timestamp_time,pkt_pts_time,pict_type,key_frame,"
                 "pkt_pos,width,height",
                 "-read_intervals", std::to_string(start) + "%+" + std::to_string(duration)});
        } else if (mode == "frames") {
            arguments.insert(
                arguments.end(),
                {"-show_frames", "-show_entries",
                 "frame=media_type,stream_index,best_effort_timestamp_time,"
                 "pkt_duration_time,pict_type,key_frame,width,height",
                 "-read_intervals", std::to_string(start) + "%+" + std::to_string(duration)});
        } else {
            arguments.insert(
                arguments.end(),
                {"-show_packets", "-show_entries",
                 "packet=stream_index,pts_time,dts_time,duration_time,size,flags,pos",
                 "-read_intervals", std::to_string(start) + "%+" + std::to_string(duration)});
        }
        if (!selector.empty())
            arguments.insert(arguments.end(), {"-select_streams", selector});
        else if (mode != "metadata")
            arguments.insert(arguments.end(), {"-select_streams", "v:0"});
        arguments.insert(arguments.end(), {"-of", "json", "-o", output.toUtf8(), input});
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
        if (truncated)
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "FFprobe JSON exceeded 2 MiB; select one stream or shorten the interval");
        if (result.empty())
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "FFprobe produced no metadata for: " + input);
        if (json::parse(result, nullptr, false).is_discarded())
            return MaiToolResult::failure(MaiErrorCode::Protocol, "FFprobe returned invalid JSON");
        return MaiToolResult::success(std::move(result));
    }

private:
    static bool validStreamSelector(const std::string& selector) {
        if (selector.empty() || selector.size() > 8 ||
            std::string("vasd").find(selector[0]) == std::string::npos)
            return false;
        if (selector.size() == 1) return true;
        if (selector[1] != ':' || selector.size() < 3) return false;
        for (std::size_t index = 2; index < selector.size(); ++index) {
            if (selector[index] < '0' || selector[index] > '9') return false;
        }
        return true;
    }

    static bool number(const json& request, const char* field, double& value, double minimum,
                       double maximum) {
        if (!request.contains(field)) return true;
        if (!request[field].is_number()) return false;
        value = request[field].get<double>();
        return std::isfinite(value) && value >= minimum && value <= maximum;
    }

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
