#include "MaiMobileAgent.h"

#include "MaiAgent.h"
#include "MaiApplyPatchTool.h"
#include "MaiEditTool.h"
#include "MaiFileTools.h"
#include "MaiOpenAiClient.h"
#include "MaiQuestionTool.h"
#include "MaiSqliteStore.h"
#include "MaiTimeTool.h"
#include "MaiTodoWriteTool.h"
#include "MaiViewImageTool.h"
#include "MaiWebFetchTool.h"
#include <cstdlib>
#include <cstring>
#include <json.hpp>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

using Json = nlohmann::json;

namespace {

constexpr auto kMarkdownBaseInstructions =
    "Write user-facing responses in valid GitHub-Flavored Markdown. Preserve real line breaks. "
    "For tables, put the header, separator, and every row on separate lines, with a blank line "
    "before and after the table. Use headings, lists, fenced code blocks, and tables only when "
    "they improve readability. Never emit table pipes as one continuous line. Images included "
    "in a user message are already available as visual input; analyze them directly and do not "
    "call the read tool for image files.";

class MaiMobileHostDispatcher {
public:
    ~MaiMobileHostDispatcher() {
        clear();
    }

    bool set(void* context, MaiMobileHostToolHandler handler,
             MaiMobileHostToolResponseFree responseFree,
             MaiMobileHostToolContextRelease contextRelease) {
        if (handler == nullptr && context != nullptr) return false;
        if ((handler == nullptr) != (responseFree == nullptr) ||
            (handler == nullptr) != (contextRelease == nullptr))
            return false;
        void* previousContext = nullptr;
        MaiMobileHostToolContextRelease previousRelease = nullptr;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mActiveCalls != 0) return false;
            previousContext = mContext;
            previousRelease = mContextRelease;
            mContext = context;
            mHandler = handler;
            mResponseFree = responseFree;
            mContextRelease = contextRelease;
        }
        if (previousRelease != nullptr) previousRelease(previousContext);
        return true;
    }

    MaiToolResult call(const std::string& toolName, const std::string& argumentsJson) {
        void* context = nullptr;
        MaiMobileHostToolHandler handler = nullptr;
        MaiMobileHostToolResponseFree responseFree = nullptr;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mHandler == nullptr)
                return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                              "the MaiChat host tool handler is not registered");
            ++mActiveCalls;
            context = mContext;
            handler = mHandler;
            responseFree = mResponseFree;
        }

        const char* response = handler(context, toolName.c_str(), argumentsJson.c_str());
        std::string responseJson;
        if (response != nullptr) responseJson = response;
        if (response != nullptr) responseFree(context, response);
        {
            std::lock_guard<std::mutex> lock(mMutex);
            --mActiveCalls;
        }
        if (responseJson.empty())
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "the MaiChat host returned an empty response");
        try {
            const Json parsed = Json::parse(responseJson);
            const bool success = parsed.value("ok", parsed.value("success", false));
            if (!success) {
                const std::string code = parsed.value("errorCode", "internal");
                MaiErrorCode errorCode = MaiErrorCode::Internal;
                if (code == "invalid_input") errorCode = MaiErrorCode::InvalidInput;
                else if (code == "not_found") errorCode = MaiErrorCode::NotFound;
                else if (code == "not_configured") errorCode = MaiErrorCode::NotConfigured;
                else if (code == "canceled") errorCode = MaiErrorCode::Canceled;
                return MaiToolResult::failure(
                    errorCode, parsed.value("error", parsed.value(
                        "errorMessage", "the MaiChat host tool failed")));
            }
            if (!parsed.contains("output"))
                return MaiToolResult::failure(MaiErrorCode::Protocol,
                                              "the MaiChat host response has no output");
            return MaiToolResult::success(parsed["output"].is_string()
                                              ? parsed["output"].get<std::string>()
                                              : parsed["output"].dump());
        } catch (const std::exception& error) {
            return MaiToolResult::failure(MaiErrorCode::Protocol,
                                          std::string("invalid MaiChat host response: ") +
                                              error.what());
        }
    }

    void clear() {
        void* context = nullptr;
        MaiMobileHostToolContextRelease contextRelease = nullptr;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mActiveCalls != 0) return;
            context = mContext;
            contextRelease = mContextRelease;
            mContext = nullptr;
            mHandler = nullptr;
            mResponseFree = nullptr;
            mContextRelease = nullptr;
        }
        if (contextRelease != nullptr) contextRelease(context);
    }

private:
    std::mutex mMutex;
    void* mContext = nullptr;
    MaiMobileHostToolHandler mHandler = nullptr;
    MaiMobileHostToolResponseFree mResponseFree = nullptr;
    MaiMobileHostToolContextRelease mContextRelease = nullptr;
    int mActiveCalls = 0;
};

class MaiMobileHostTool final : public MaiTool {
public:
    MaiMobileHostTool(std::string name, std::string description, std::string schema,
                      std::shared_ptr<MaiMobileHostDispatcher> dispatcher, bool approval)
        : mName(std::move(name)),
          mDescription(std::move(description)),
          mSchema(std::move(schema)),
          mDispatcher(std::move(dispatcher)),
          mApproval(approval) {}

    std::string name() const override { return mName; }
    std::string description() const override { return mDescription; }
    std::string parametersSchema() const override { return mSchema; }
    bool requiresApproval(const std::string&) const override { return mApproval; }
    bool requiresPerCallApproval(const std::string&) const override { return mApproval; }
    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext&) override {
        return mDispatcher->call(mName, argumentsJson);
    }

private:
    std::string mName;
    std::string mDescription;
    std::string mSchema;
    std::shared_ptr<MaiMobileHostDispatcher> mDispatcher;
    bool mApproval = false;
};

void registerMaiChatHostTools(MaiToolRegistry& tools,
                              const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher) {
    const auto add = [&](const char* name, const char* description, const char* schema,
                         bool approval = false) {
        tools.add(std::make_unique<MaiMobileHostTool>(name, description, schema, dispatcher,
                                                      approval));
    };
    add("maichat_list_contacts",
        "List MaiChat contacts. Use query to filter by user ID or display name.",
        R"({"type":"object","properties":{"query":{"type":"string"},"limit":{"type":"integer","minimum":1,"maximum":200}}})");
    add("maichat_list_conversations",
        "List MaiChat conversations ordered by latest message, including unread counts.",
        R"({"type":"object","properties":{"limit":{"type":"integer","minimum":1,"maximum":200}}})");
    add("maichat_get_messages",
        "Read recent messages in a MaiChat conversation with a specific contact.",
        R"({"type":"object","properties":{"peer_id":{"type":"string"},"limit":{"type":"integer","minimum":1,"maximum":200}},"required":["peer_id"]})");
    add("maichat_search_messages",
        "Search MaiChat message text across all contacts or within one contact.",
        R"({"type":"object","properties":{"query":{"type":"string"},"peer_id":{"type":"string"},"limit":{"type":"integer","minimum":1,"maximum":200}},"required":["query"]})");
    add("maichat_get_unread_summary", "Summarize unread MaiChat messages by contact.",
        R"({"type":"object","properties":{}})");
    add("maichat_send_text",
        "Send text to a MaiChat contact through the host. Requires user approval.",
        R"({"type":"object","properties":{"peer_id":{"type":"string"},"text":{"type":"string"}},"required":["peer_id","text"]})",
        true);
    add("maichat_reply_message",
        "Send a quoted reply to a MaiChat message through the host. Requires user approval.",
        R"({"type":"object","properties":{"peer_id":{"type":"string"},"message_id":{"type":"string"},"text":{"type":"string"}},"required":["peer_id","message_id","text"]})",
        true);
    add("maichat_broadcast_text",
        "Send the same text to multiple MaiChat contacts through the host. The complete recipient list and text require user approval for every call.",
        R"({"type":"object","properties":{"peer_ids":{"type":"array","items":{"type":"string"},"minItems":1,"maxItems":200},"text":{"type":"string"}},"required":["peer_ids","text"]})",
        true);
}

void registerMobilePhotoTools(MaiToolRegistry& tools,
                              const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher) {
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_list_photos",
        "List photos visible to this app in the system photo library after OS authorization. "
        "Use offset and limit to page; a limited grant exposes only the user's selected photos.",
        R"({"type":"object","properties":{"offset":{"type":"integer","minimum":0},"limit":{"type":"integer","minimum":1,"maximum":100},"album_id":{"type":"string"}}})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_list_albums",
        "List system photo albums available under the current OS photo-library permission.",
        R"({"type":"object","properties":{}})", dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_read_photo",
        "Copy one photo by ID into the Agent working directory so view_image can inspect it. "
        "Only images authorized by the OS photo-library permission can be read.",
        R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_export_photo_original",
        "Copy a full-resolution photo by ID from the authorized system library into the Agent "
        "working directory for image creation. Preserve its original format and metadata; "
        "do not modify the library photo. Files larger than 100 MB are not supported.",
        R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_save_image",
        "Save an image from the Agent working directory as a NEW photo in the system library. "
        "The source file and all existing library photos remain unchanged. Pass a relative path "
        "inside the Agent working directory.",
        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})",
        dispatcher, false));
#if defined(__APPLE__)
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_photos_add_to_album",
        "Create or reuse an iOS Photos album and add existing photo IDs to it without duplicating "
        "or removing originals.",
        R"({"type":"object","properties":{"album_name":{"type":"string"},"photo_ids":{"type":"array","items":{"type":"string"},"minItems":1,"maxItems":50}},"required":["album_name","photo_ids"]})",
        dispatcher, false));
#elif defined(__ANDROID__)
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_photos_copy_to_album",
        "Android galleries use folders as albums. Copy the selected existing photos into "
        "Pictures/MaiChat/<album_name>; originals remain unchanged, so gallery duplicates "
        "will be visible.",
        R"({"type":"object","properties":{"album_name":{"type":"string"},"photo_ids":{"type":"array","items":{"type":"string"},"minItems":1,"maxItems":10}},"required":["album_name","photo_ids"]})",
        dispatcher, false));
#endif
}

}  // namespace

// 这是移动端适配器，JSON 只在语言边界，MaiAgent 的公开接口仍是领域对象。
struct MaiMobileAgent {
    std::mutex mutex;
    bool dirty = true;
    bool structureChanged = true;
    std::string cachedSession;
    std::vector<MaiMessage> cachedMessages;
    std::unordered_map<std::string, std::string> pendingDeltas;
    std::unordered_map<std::string, std::string> liveParts;
    std::unordered_map<std::string, std::string> errors;
    std::string workspace;
    std::string model;
    MaiModelConfig suggestionConfig;
    bool configured = false;
    std::shared_ptr<MaiMobileHostDispatcher> hostTools =
        std::make_shared<MaiMobileHostDispatcher>();
    // 最后销毁 agent：回调捕获的字段必须活到工作线程退出。
    std::unique_ptr<MaiAgent> agent;

    ~MaiMobileAgent() {
        agent.reset();
        hostTools->clear();
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
        suggestionConfig = config;
        auto tools = std::make_unique<MaiToolRegistry>();
        // 两个移动端只提供真实可用的本地文件和网络工具，不暴露桌面 shell。
        tools->add(makeMaiReadTool());
        tools->add(makeMaiWriteTool());
        tools->add(makeMaiEditTool());
        tools->add(makeMaiApplyPatchTool());
        tools->add(makeMaiGlobTool());
        tools->add(makeMaiGrepTool());
        tools->add(makeMaiWebFetchTool(config.caBundlePath));
        tools->add(makeMaiQuestionTool());
        tools->add(makeMaiCurrentTimeTool());
        tools->add(makeMaiTodoWriteTool());
        tools->add(makeMaiViewImageTool());
        registerMaiChatHostTools(*tools, hostTools);
        registerMobilePhotoTools(*tools, hostTools);
        MaiAgent::Options options;
        options.defaultModel = request.at("model").get<std::string>();
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

    Json snapshot(const std::string& selected, bool force) {
        std::unordered_map<std::string, std::string> errorCopy;
        bool reload = force || cachedSession != selected;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!dirty && !force) return {{"ok", true}, {"changed", false}};
            dirty = false;
            reload |= structureChanged;
            structureChanged = false;
            for (auto& delta : pendingDeltas) liveParts[delta.first] += delta.second;
            pendingDeltas.clear();
            errorCopy = errors;
        }
        Json sessions = Json::array(), messages = Json::array();
        bool busy = false;
        for (const auto& session : agent->listSessions()) {
            if (!session.isRoot()) continue;
            const bool active = agent->isBusy(session.id);
            busy |= active;
            sessions.push_back({{"id", session.id}, {"title", session.title}, {"busy", active}});
        }
        // 增量期间复用消息结构，避免每个批次重新读整段 SQLite 历史。
        if (reload) {
            cachedMessages = agent->listMessages(selected);
            cachedSession = selected;
        }
        const bool sessionBusy = agent->isBusy(selected);
        for (const auto& message : cachedMessages) {
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
                                               message.id == cachedMessages.back().id},
                                {"parts", parts}});
        }
        Json permissions = Json::array(), questions = Json::array();
        for (const auto& p : agent->listPendingPermissions())
            if (p.sessionId == selected)
                permissions.push_back({{"id", p.id},
                                       {"tool", p.toolName},
                                       {"input", p.arguments},
                                       {"allowForSession", p.allowForSession}});
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
        if (op == "snapshot") return snapshot(session, r.value("force", false));
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
            id = result(agent->submit(MaiSendPrompt{session, r.at("text"), std::move(images)}));
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
    return mobile->hostTools->set(context, handler, responseFree, contextRelease) ? 1 : 0;
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
    char* result = static_cast<char*>(std::malloc(response.size() + 1));
    if (result) std::memcpy(result, response.c_str(), response.size() + 1);
    return result;
}
void maiMobileAgentFree(char* response) {
    std::free(response);
}
