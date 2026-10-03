#include "MaiMobileAgent.h"
#include "MaiMobileHostTools.h"

#include "MaiAgent.h"
#include "MaiAgentSendMediaTool.h"
#include "MaiApplyPatchTool.h"
#include "MaiCvVideoAnalysis.h"
#include "MaiCvVideoTools.h"
#include "MaiDownloadFileTool.h"
#include "MaiEditTool.h"
#include "MaiFaceBeautify.h"
#include "MaiFaceBeautifyTool.h"
#include "MaiFaceImageCodecBridge.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiFileTools.h"
#include "MaiFfmpegTools.h"
#include "MaiOpenAiClient.h"
#include "MaiPdfTool.h"
#include "MaiQuestionTool.h"
#include "MaiSqliteStore.h"
#include "MaiTimeTool.h"
#include "MaiTodoWriteTool.h"
#include "MaiViewImageTool.h"
#include "MaiVideoMatting.h"
#include "MaiVideoMattingTool.h"
#include "MaiWebFetchTool.h"
#include "mai_fftools_embed.h"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <json.hpp>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

using Json = nlohmann::json;

namespace {

constexpr auto kMarkdownBaseInstructions =
    "Write user-facing responses in valid GitHub-Flavored Markdown. Preserve real line breaks. "
    "For tables, put the header, separator, and every row on separate lines, with a blank line "
    "before and after the table. Use headings, lists, fenced code blocks, and tables only when "
    "they improve readability. Never emit table pipes as one continuous line. Images included "
    "in a user message are already available as visual input; analyze them directly and do not "
    "call the read tool for image files. When a mobile task lacks an OS permission, call "
    "mobile_request_permission for that capability and retry once if granted. A limited grant "
    "may still allow the operation, such as reading selected photos or using approximate "
    "location; retry once when that access is sufficient. If the OS reports settings_required, "
    "tell the user which permission to enable instead of retrying in a loop. For any gallery "
    "photo edit, use mobile_export_photo_original as the input; mobile_read_photo returns a "
    "bounded JPEG preview that loses resolution and wide-gamut metadata.";

std::string encodeBase64(const std::string& input) {
    constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    for (std::size_t offset = 0; offset < input.size(); offset += 3) {
        const auto first = static_cast<unsigned char>(input[offset]);
        const auto second = offset + 1 < input.size()
                                ? static_cast<unsigned char>(input[offset + 1]) : 0;
        const auto third = offset + 2 < input.size()
                               ? static_cast<unsigned char>(input[offset + 2]) : 0;
        output.push_back(kAlphabet[first >> 2]);
        output.push_back(kAlphabet[((first & 0x03) << 4) | (second >> 4)]);
        output.push_back(offset + 1 < input.size()
                             ? kAlphabet[((second & 0x0F) << 2) | (third >> 6)] : '=');
        output.push_back(offset + 2 < input.size() ? kAlphabet[third & 0x3F] : '=');
    }
    return output;
}

char* ownedResponse(const std::string& response) {
    char* result = static_cast<char*>(std::malloc(response.size() + 1));
    if (result) std::memcpy(result, response.c_str(), response.size() + 1);
    return result;
}

}  // namespace

// 这是移动端适配器，JSON 只在语言边界，MaiAgent 的公开接口仍是领域对象。
struct MaiMobileAgent {
    std::mutex mutex;
    bool dirty = true;
    bool structureChanged = true;
    std::string cachedSession;
    int cachedLimit = -1;
    std::vector<MaiMessage> cachedMessages;
    std::unordered_map<std::string, std::string> pendingDeltas;
    std::unordered_map<std::string, std::string> liveParts;
    std::unordered_map<std::string, std::string> errors;
    std::string workspace;
    std::string model;
    MaiModelConfig suggestionConfig;
    bool configured = false;
    const void* ortApiBase = nullptr;
    std::shared_ptr<MaiMobileHostDispatcher> hostTools =
        makeMaiMobileHostDispatcher();
    // 最后销毁 agent：回调捕获的字段必须活到工作线程退出。
    std::unique_ptr<MaiAgent> agent;

    ~MaiMobileAgent() {
        agent.reset();
        clearMaiMobileHostToolHandler(hostTools);
    }

    static std::string result(MaiResult<std::string> value) {
        if (!value) throw std::runtime_error(value.error().message());
        return value.value();
    }

    bool anyBusy() const {
        if (!agent) return false;
        for (const auto& session : agent->listSessions())
            if (agent->isBusy(session.id)) return true;
        return false;
    }

    void configure(const Json& request) {
        if (anyBusy()) throw std::runtime_error("Stop active tasks before changing the model.");
        auto store = makeMaiSqliteStore(request.at("database").get<std::string>());
        if (!store) throw std::runtime_error(store.error().message());
        MaiModelConfig config;
        config.baseUrl = request.at("baseUrl").get<std::string>();
        config.apiKey = request.value("apiKey", "");
        config.caBundlePath = request.value("caBundle", "");
        MaiFfmpegEngine ffmpegEngine{mai_ffmpeg_execute, mai_ffmpeg_set_cancel_check,
                                     mai_ffprobe_execute, mai_ffprobe_set_cancel_check,
                                     mai_fftools_set_log_sink, mai_fftools_error_string};
        config.prepareImage = makeMaiFfmpegModelImagePreparer(ffmpegEngine);
        suggestionConfig = config;
        auto tools = std::make_unique<MaiToolRegistry>();
        // 两个移动端只提供真实可用的本地文件和网络工具，不暴露桌面 shell。
        tools->add(makeMaiReadTool());
        tools->add(makeMaiCreateFileTool());
        tools->add(makeMaiCreateDirectoryTool());
        tools->add(makeMaiDeleteFileTool());
        tools->add(makeMaiWriteTool());
        tools->add(makeMaiEditTool());
        tools->add(makeMaiApplyPatchTool());
        tools->add(makeMaiGlobTool());
        tools->add(makeMaiGrepTool());
        tools->add(makeMaiWebFetchTool(config.caBundlePath));
        tools->add(makeMaiDownloadFileTool());
        tools->add(makeMaiAgentSendMediaTool());
        tools->add(makeMaiQuestionTool());
        tools->add(makeMaiCurrentTimeTool());
        tools->add(makeMaiTodoWriteTool());
        tools->add(makeMaiViewImageTool(makeMaiFfmpegImagePreview(ffmpegEngine)));
        tools->add(makeMaiFfmpegTool(ffmpegEngine));
        tools->add(makeMaiFfprobeTool(ffmpegEngine));
        tools->add(makeMaiCvSceneDetectTool(analyzeMaiCvVideo));
        tools->add(makeMaiCvMotionDetectTool(analyzeMaiCvVideo));
        const std::string rvmModel = request.value("rvmModelPath", "");
        const std::string ortRuntime = request.value("ortRuntimePath", "");
        if (!rvmModel.empty() && MaiFileSystem::exists(MaiFilePath::fromUtf8(rvmModel))) {
            if (ortApiBase != nullptr ||
                (!ortRuntime.empty() && MaiFileSystem::exists(MaiFilePath::fromUtf8(ortRuntime)))) {
                tools->add(makeMaiVideoMattingTool(maiMatteVideo, rvmModel, ortRuntime,
                                                    ortApiBase));
            }
        }
        const std::string faceDetector = request.value("faceDetectorModelPath", "");
        const std::string faceLandmarker = request.value("faceLandmarkerModelPath", "");
        if (!faceDetector.empty() && !faceLandmarker.empty() &&
            MaiFileSystem::exists(MaiFilePath::fromUtf8(faceDetector)) &&
            MaiFileSystem::exists(MaiFilePath::fromUtf8(faceLandmarker)) &&
            (ortApiBase != nullptr ||
             (!ortRuntime.empty() && MaiFileSystem::exists(MaiFilePath::fromUtf8(ortRuntime))))) {
            tools->add(makeMaiFaceBeautifyTool(maiBeautifyFaceImage, faceDetector,
                                                faceLandmarker, ortRuntime, ortApiBase,
                                                maiDecodeFaceImage, maiEncodeFacePng));
        }
        tools->add(makeMaiPdfTool([dispatcher = hostTools](
                                      const std::string& html, const std::string& output,
                                      const std::atomic<bool>* cancel) {
            if (cancel != nullptr && cancel->load(std::memory_order_relaxed))
                return MaiToolResult::failure(MaiErrorCode::Canceled,
                                              "PDF generation was canceled");
            return callMaiMobileHostTool(dispatcher, "generate_pdf", Json{{"html", html}, {"output_path", output}}.dump());
        }));
        registerMaiChatHostTools(*tools, hostTools);
        registerMobilePhotoTools(*tools, hostTools);
        MaiAgent::Options options;
        options.defaultModel = request.at("model").get<std::string>();
        options.fileAccessRoot = request.value("appRoot", request.at("workspace").get<std::string>());
        options.decodeText = [dispatcher = hostTools](const std::string& bytes,
                                                       const std::string& encoding)
            -> MaiResult<std::string> {
            const MaiToolResult result = callMaiMobileHostTool(
                dispatcher, "mobile_decode_text",
                Json{{"base64", encodeBase64(bytes)}, {"encoding", encoding}}.dump());
            if (result.hasError()) return result.error();
            return result.output();
        };
        options.baseInstructions = kMarkdownBaseInstructions;
        const auto policy = request.value("policy", "on-request");
        if (policy == "never")
            options.approvalPolicy = MaiApprovalPolicy::Never;
        else if (policy == "unless-trusted")
            options.approvalPolicy = MaiApprovalPolicy::UnlessTrusted;
        else if (policy != "on-request")
            throw std::runtime_error("Unknown approval policy.");
        auto replacement = std::make_unique<MaiAgent>(
            std::move(store.value()), makeMaiModelClient(config), std::move(tools), options);
        agent.reset();
        pendingDeltas.clear();
        liveParts.clear();
        errors.clear();
        agent = std::move(replacement);
        workspace = request.at("workspace").get<std::string>();
        model = options.defaultModel;
        configured = !config.apiKey.empty() && !model.empty();
        dirty = true;
        structureChanged = true;
        cachedSession.clear();
        cachedLimit = -1;
        cachedMessages.clear();
        agent->eventBus().subscribe([this](const MaiEvent& event) {
            // 网络线程只合并增量，不读数据库、不解析 Markdown、不调用 UI。
            std::lock_guard<std::mutex> lock(mutex);
            dirty = true;
            if (event.type == MaiEventType::MessagePartDelta)
                pendingDeltas[event.partId] += event.delta;
            else
                structureChanged = true;
            if (event.type == MaiEventType::SessionError) errors[event.sessionId] = event.detail;
        });
    }

    Json serializeMessages(const std::vector<MaiMessage>& source, bool sessionBusy) {
        Json messages = Json::array();
        std::string latestId;
        for (const auto& message : source)
            if (message.id > latestId) latestId = message.id;
        for (const auto& message : source) {
            Json parts = Json::array();
            for (const auto& part : message.parts) {
                Json value = {{"id", part.id}};
                std::visit(
                    [&](const auto& body) {
                        using T = std::decay_t<decltype(body)>;
                        if constexpr (std::is_same_v<T, MaiToolPart>) {
                            value.update({{"kind", "tool"},
                                          {"tool", body.tool},
                                          {"input", body.input},
                                          {"output", body.output},
                                          {"error", body.error},
                                          {"state", maiToolStateToString(body.state)}});
                        } else if constexpr (std::is_same_v<T, MaiImagePart>) {
                            value.update({{"kind", "image"},
                                          {"path", body.path},
                                          {"mimeType", body.mimeType}});
                        } else if constexpr (std::is_same_v<T, MaiVideoPart>) {
                            value.update({{"kind", "video"},
                                          {"path", body.path},
                                          {"mimeType", body.mimeType}});
                        } else {
                            std::string text = body.text;
                            auto live = liveParts.find(part.id);
                            if (message.isInProgress() && live != liveParts.end() &&
                                live->second.size() > text.size())
                                text = live->second;
                            value.update(
                                {{"kind", std::is_same_v<T, MaiTextPart> ? "text" : "reasoning"},
                                 {"text", text}});
                        }
                    },
                    part.body);
                parts.push_back(std::move(value));
                if (!message.isInProgress()) liveParts.erase(part.id);
            }
            messages.push_back({{"id", message.id},
                                {"role", maiRoleToString(message.role)},
                                {"created", message.created},
                                {"completed", message.completed},
                                {"active", message.isInProgress() && sessionBusy &&
                                               message.id == latestId},
                                {"parts", parts}});
        }
        return messages;
    }

    Json snapshot(const std::string& selected, bool force, int limit) {
        std::unordered_map<std::string, std::string> errorCopy;
        std::unordered_set<std::string> changedParts;
        bool reload = force || cachedSession != selected || cachedLimit != limit;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!dirty && !force) return {{"ok", true}, {"changed", false}};
            dirty = false;
            reload |= structureChanged;
            structureChanged = false;
            for (auto& delta : pendingDeltas) {
                liveParts[delta.first] += delta.second;
                changedParts.insert(delta.first);
            }
            pendingDeltas.clear();
            errorCopy = errors;
        }
        Json sessions = Json::array();
        bool busy = false;
        for (const auto& session : agent->listSessions()) {
            if (!session.isRoot()) continue;
            const bool active = agent->isBusy(session.id);
            busy |= active;
            sessions.push_back({{"id", session.id}, {"title", session.title}, {"busy", active}});
        }
        // 增量期间复用消息结构，避免每个批次重新读整段 SQLite 历史。
        if (reload) {
            if (limit < 0)
                cachedMessages = agent->listMessages(selected);
            else if (limit == 0)
                cachedMessages.clear();
            else
                cachedMessages = agent->listMessagesPage(selected, "", limit);
            cachedSession = selected;
            cachedLimit = limit;
        }
        const bool sessionBusy = agent->isBusy(selected);
        Json messages;
        if (limit >= 0 && !reload && !changedParts.empty()) {
            std::vector<MaiMessage> changedMessages;
            for (const auto& message : cachedMessages) {
                for (const auto& part : message.parts) {
                    if (changedParts.count(part.id) == 0) continue;
                    changedMessages.push_back(message);
                    break;
                }
            }
            messages = serializeMessages(changedMessages, sessionBusy);
        } else {
            messages = serializeMessages(cachedMessages, sessionBusy);
        }
        Json permissions = Json::array(), questions = Json::array();
        for (const auto& p : agent->listPendingPermissions())
            if (p.sessionId == selected)
                permissions.push_back({{"id", p.id},
                                       {"tool", p.toolName},
                                       {"input", p.arguments},
                                       {"allowForSession", p.allowForSession},
                                       {"rememberOnApproval", p.rememberOnApproval},
                                       {"fileCount", p.approvalKeys.size()}});
        for (const auto& q : agent->listPendingQuestions())
            if (q.sessionId == selected)
                questions.push_back(
                    {{"id", q.id}, {"question", q.question}, {"options", q.options}});
        if (!busy) liveParts.clear();
        return {{"ok", true},
                {"changed", true},
                {"sessions", sessions},
                {"messages", messages},
                {"permissions", permissions},
                {"questions", questions},
                {"busy", busy},
                {"configured", configured},
                {"error", errorCopy[selected]}};
    }

    Json request(const Json& r) {
        const std::string op = r.at("op");
        if (op == "configure") {
            configure(r);
            return {{"ok", true}};
        }
        if (!agent) throw std::runtime_error("Agent is not initialized.");
        const std::string session = r.value("session", "");
        if (op == "snapshot") {
            const int limit = r.value("messageLimit", -1);
            if (limit < -1 || limit > 100)
                throw std::runtime_error("messageLimit must be between 0 and 100.");
            return snapshot(session, r.value("force", false), limit);
        }
        if (op == "messages_page") {
            const int limit = r.value("limit", 30);
            if (limit < 1 || limit > 100)
                throw std::runtime_error("limit must be between 1 and 100.");
            const std::string before = r.value("before", "");
            const auto page = agent->listMessagesPage(session, before, limit);
            return {{"ok", true},
                    {"messages", serializeMessages(page,
                                                    before.empty() && agent->isBusy(session))}};
        }
        if (op == "suggest_replies") {
            if (!configured) throw std::runtime_error("Configure a model and API key first.");
            if (!r.contains("messages") || !r["messages"].is_array() || r["messages"].empty() ||
                r["messages"].size() > 30)
                throw std::runtime_error("Messages must contain between 1 and 30 items.");
            MaiModelRequest modelRequest;
            modelRequest.model = model;
            modelRequest.temperature = 0.5;
            modelRequest.baseInstructions =
                "You generate reply suggestions for a private chat. Return only one JSON object "
                "with exactly these string fields: natural, casual, professional. Each value must "
                "be a short reply in the conversation language. Do not send a message, call a "
                "tool, use Markdown fences, or include explanations.";
            MaiModelMessage context;
            context.role = MaiModelRole::User;
            context.content = "Recent conversation, oldest first:\n" + r["messages"].dump();
            modelRequest.messages.push_back(std::move(context));
            std::string response;
            MaiStreamSink sink;
            sink.onText = [&](std::string_view delta) {
                response.append(delta.data(), delta.size());
            };
            std::atomic<bool> cancel{false};
            const MaiError error =
                makeMaiModelClient(suggestionConfig)->stream(modelRequest, sink, cancel);
            if (error) throw std::runtime_error(error.message());
            std::string clean = response;
            const auto firstLine = clean.find('\n');
            const auto closingFence = clean.rfind("```");
            if (clean.rfind("```", 0) == 0 && firstLine != std::string::npos &&
                closingFence > firstLine)
                clean = clean.substr(firstLine + 1, closingFence - firstLine - 1);
            const Json suggestions = Json::parse(clean);
            for (const char* key : {"natural", "casual", "professional"})
                if (!suggestions.contains(key) || !suggestions[key].is_string() ||
                    suggestions[key].get<std::string>().empty())
                    throw std::runtime_error("The model returned incomplete reply suggestions.");
            return {{"ok", true},
                    {"natural", suggestions["natural"]},
                    {"casual", suggestions["casual"]},
                    {"professional", suggestions["professional"]}};
        }
        std::string id;
        if (op == "create")
            id = result(agent->submit(MaiCreateSession{workspace, "", model}));
        else if (op == "send") {
            if (!configured) throw std::runtime_error("Configure a model and API key first.");
            // iOS 在升级或恢复后可能给同一数据容器分配新的绝对路径。图片片段只保存
            // 工作区相对路径，所以每轮发送前都把会话根目录迁到当前容器位置。
            result(agent->submit(MaiUpdateSession{session, "", model, "", workspace}));
            {
                std::lock_guard<std::mutex> lock(mutex);
                errors.erase(session);
            }
            std::vector<MaiModelImage> images;
            if (r.contains("images")) {
                if (!r["images"].is_array() || r["images"].size() > 10)
                    throw std::runtime_error("Images must be an array with at most 10 items.");
                for (const auto& value : r["images"]) {
                    if (!value.is_object() || !value.contains("path") ||
                        !value["path"].is_string() || !value.contains("mimeType") ||
                        !value["mimeType"].is_string())
                        throw std::runtime_error("Each image needs path and mimeType strings.");
                    images.push_back(
                        {value["path"].get<std::string>(), value["mimeType"].get<std::string>()});
                }
            }
            std::vector<MaiVideoPart> videos;
            if (r.contains("videos")) {
                if (!r["videos"].is_array() || r["videos"].size() > 10)
                    throw std::runtime_error("Videos must be an array with at most 10 items.");
                for (const auto& value : r["videos"]) {
                    if (!value.is_object() || !value.contains("path") ||
                        !value["path"].is_string() || !value.contains("mimeType") ||
                        !value["mimeType"].is_string())
                        throw std::runtime_error("Each video needs path and mimeType strings.");
                    videos.push_back(
                        {value["path"].get<std::string>(), value["mimeType"].get<std::string>()});
                }
            }
            id = result(agent->submit(MaiSendPrompt{session, r.at("text"), std::move(images),
                                                    std::move(videos)}));
        } else if (op == "stop")
            result(agent->submit(MaiInterrupt{session}));
        else if (op == "delete")
            result(agent->submit(MaiDeleteSession{session}));
        else if (op == "clear")
            result(agent->submit(MaiClearMessages{session}));
        else if (op == "rename")
            result(agent->submit(MaiUpdateSession{session, r.at("title"), "", ""}));
        else if (op == "permission") {
            MaiPermissionDecision decision = MaiPermissionDecision::Denied;
            if (!maiParsePermissionDecision(r.at("decision"), decision))
                throw std::runtime_error("Unknown permission decision.");
            result(agent->submit(MaiReplyPermission{r.at("id"), decision}));
        } else if (op == "answer")
            result(agent->submit(MaiReplyQuestion{r.at("id"), r.at("text")}));
        else
            throw std::runtime_error("Unknown operation.");
        return {{"ok", true}, {"id", id}};
    }
};

void* maiMobileAgentCreate(void) {
    try {
        return new MaiMobileAgent();
    } catch (...) {
        return nullptr;
    }
}
void maiMobileAgentDestroy(void* handle) {
    delete static_cast<MaiMobileAgent*>(handle);
}
int maiMobileAgentSetHostToolHandler(void* handle, void* context,
                                    MaiMobileHostToolHandler handler,
                                    MaiMobileHostToolResponseFree responseFree,
                                    MaiMobileHostToolContextRelease contextRelease) {
    if (handle == nullptr) return 0;
    auto* mobile = static_cast<MaiMobileAgent*>(handle);
    if (mobile->anyBusy()) return 0;
    return setMaiMobileHostToolHandler(mobile->hostTools, context, handler, responseFree, contextRelease) ? 1 : 0;
}
int maiMobileAgentSetOrtApiBase(void* handle, const void* apiBase) {
    if (handle == nullptr || apiBase == nullptr) return 0;
    auto* mobile = static_cast<MaiMobileAgent*>(handle);
    if (mobile->agent != nullptr) return 0;
    mobile->ortApiBase = apiBase;
    return 1;
}
char* maiMobileMatteVideo(const char* argumentsJson, const char* workspace,
                          const char* modelPath, const char* runtimePath,
                          const void* apiBase) {
    try {
        if (argumentsJson == nullptr || workspace == nullptr || modelPath == nullptr)
            return ownedResponse(R"({"ok":false,"error":"invalid matting arguments"})");
        auto tool = makeMaiVideoMattingTool(maiMatteVideo, modelPath,
                                             runtimePath != nullptr ? runtimePath : "", apiBase);
        if (!tool)
            return ownedResponse(R"({"ok":false,"error":"video matting is unavailable"})");
        MaiToolContext context;
        context.root = workspace;
        context.allowOutsideWorkingDirectory = true;
        const MaiToolResult result = tool->execute(argumentsJson, context);
        if (result.hasError())
            return ownedResponse(Json{{"ok", false},
                                      {"errorCode", maiErrorCodeToString(result.error().code())},
                                      {"error", result.error().message()}}.dump());
        return ownedResponse(Json{{"ok", true}, {"output", result.output()}}.dump());
    } catch (const std::exception& error) {
        return ownedResponse(Json{{"ok", false}, {"error", error.what()}}.dump());
    }
}
char* maiMobileAgentRequest(void* handle, const char* request) {
    std::string response;
    try {
        if (!handle || !request) throw std::runtime_error("Invalid bridge handle.");
        response = static_cast<MaiMobileAgent*>(handle)->request(Json::parse(request)).dump();
    } catch (const std::exception& e) {
        response = Json{{"ok", false}, {"error", e.what()}}.dump();
    } catch (...) {
        response = "{\"ok\":false,\"error\":\"Native agent failure.\"}";
    }
    return ownedResponse(response);
}
void maiMobileAgentFree(char* response) {
    std::free(response);
}
