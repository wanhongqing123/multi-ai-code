// 文件作用：封装智谱图片媒体能力的请求与结果处理。
// 直接依赖：MaiBlockingCheck.h、MaiDownloadFileTool.h、MaiFilePath.h；本文件在这些接口之间完成调用与结果转换。
#include "MaiGlmMediaTools.h"

#include <curl/curl.h>
#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "MaiBlockingCheck.h"
#include "MaiDownloadFileTool.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiPathGuard.h"

namespace {

using Json = nlohmann::json;

constexpr char kImageModel[] = "glm-image";
constexpr char kImageUrl[] = "https://open.bigmodel.cn/api/paas/v4/async/images/generations";
constexpr char kResultUrl[] = "https://open.bigmodel.cn/api/paas/v4/async-result/";
constexpr std::size_t kMaxResponseBytes = 2 * 1024 * 1024;

std::string value(const Json& data, const char* field) {
    return data.is_object() && data.contains(field) && data[field].is_string()
               ? data[field].get<std::string>()
               : std::string{};
}

MaiToolResult failure(MaiErrorCode error, const char* code, const std::string& message) {
    return MaiToolResult::failure(error, Json{{"code", code}, {"message", message}}.dump());
}

MaiToolResult invalid(const std::string& message) {
    return failure(MaiErrorCode::InvalidInput, "invalid_input", message);
}

bool validId(const std::string& id) {
    return !id.empty() && id.size() <= 128 &&
           std::all_of(id.begin(), id.end(), [](unsigned char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= 'a' && character <= 'z') || character == '-' ||
                      character == '_';
           });
}

bool httpsUrl(const std::string& url) {
    if (url.compare(0, 8, "https://") != 0) return false;
    CURLU* parsed = curl_url();
    if (parsed == nullptr) return false;
    char* host = nullptr;
    char* user = nullptr;
    const bool parsedUrl = curl_url_set(parsed, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK;
    const bool hasHost = parsedUrl && curl_url_get(parsed, CURLUPART_HOST, &host, 0) == CURLUE_OK;
    const bool hasUser = parsedUrl && curl_url_get(parsed, CURLUPART_USER, &user, 0) == CURLUE_OK;
    const bool valid = hasHost && !hasUser && host != nullptr && *host != '\0';
    if (host != nullptr) curl_free(host);
    if (user != nullptr) curl_free(user);
    curl_url_cleanup(parsed);
    return valid;
}

struct ResponseBuffer {
    std::string bytes;
    bool exceeded = false;
};

std::size_t receive(char* data, std::size_t size, std::size_t count, void* user) {
    auto& response = *static_cast<ResponseBuffer*>(user);
    if (count != 0 && size > SIZE_MAX / count) return 0;
    const std::size_t bytes = size * count;
    if (bytes > kMaxResponseBytes - response.bytes.size()) {
        response.exceeded = true;
        return 0;
    }
    response.bytes.append(data, bytes);
    return bytes;
}

int checkCanceled(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<const MaiToolContext*>(user)->isCanceled() ? 1 : 0;
}

struct HttpResult {
    Json data;
    std::optional<MaiToolResult> error;
};

// 向智谱媒体接口发送 JSON 请求并收集 HTTP 状态、响应体和网络错误。
// 传输失败不伪造成媒体任务失败，调用方据此决定是否继续查询原任务。
HttpResult requestJson(const std::string& url, const std::string& key, const std::string& caBundle,
                       const Json* body, const MaiToolContext& context) {
    maiAssertBlockingAllowed("glm_media_tool");
    CURL* curl = curl_easy_init();
    if (curl == nullptr)
        return {{}, failure(MaiErrorCode::Internal, "internal", "Could not initialize HTTP")};
    ResponseBuffer response;
    const std::string payload = body == nullptr ? std::string{} : body->dump();
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: Bearer " + key).c_str());
    headers = curl_slist_append(headers, "Accept: application/json");
    if (body != nullptr) headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, body == nullptr ? 60L : 180L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, checkCanceled);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    if (!caBundle.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
    if (body != nullptr) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(payload.size()));
    }
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
#endif
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (context.isCanceled())
        return {{}, failure(MaiErrorCode::Canceled, "canceled", "Task was canceled")};
    if (response.exceeded)
        return {{}, failure(MaiErrorCode::Protocol, "protocol", "GLM response exceeded 2 MB")};
    if (result != CURLE_OK)
        return {{}, failure(MaiErrorCode::Network, "network", curl_easy_strerror(result))};
    Json data = Json::parse(response.bytes, nullptr, false);
    if (!data.is_object())
        return {{}, failure(MaiErrorCode::Protocol, "protocol", "GLM returned invalid JSON")};
    if (status < 200 || status >= 300) {
        const Json detail = data.value("error", Json::object());
        const std::string code =
            value(detail, "code").empty() ? value(data, "code") : value(detail, "code");
        const std::string message =
            value(detail, "message").empty() ? value(data, "message") : value(detail, "message");
        return {{},
                MaiToolResult::failure(
                    MaiErrorCode::Network,
                    Json{{"code", "provider_error"},
                         {"provider_code", code},
                         {"http_status", status},
                         {"request_id", value(data, "request_id")},
                         {"message", message.empty() ? "GLM request failed (HTTP " +
                                                           std::to_string(status) + ")"
                                                     : message}}
                        .dump())};
    }
    return {std::move(data), std::nullopt};
}

bool validImageSize(const std::string& size) {
    const std::size_t x = size.find('x');
    if (x == std::string::npos || size.find('x', x + 1) != std::string::npos) return false;
    const std::string width = size.substr(0, x);
    const std::string height = size.substr(x + 1);
    const auto digit = [](unsigned char byte) { return byte >= '0' && byte <= '9'; };
    if (width.empty() || height.empty() || width.size() > 4 || height.size() > 4 ||
        !std::all_of(width.begin(), width.end(), digit) ||
        !std::all_of(height.begin(), height.end(), digit))
        return false;
    const int w = std::stoi(width);
    const int h = std::stoi(height);
    return w >= 1024 && w <= 2048 && h >= 1024 && h <= 2048 && w % 32 == 0 && h % 32 == 0 &&
           w * h <= 4'194'304;
}

MaiToolResult downloadImage(const std::string& url, const std::string& relative,
                            const MaiToolContext& context, const std::string& caBundle) {
    if (!httpsUrl(url))
        return failure(MaiErrorCode::Protocol, "protocol", "GLM returned no HTTPS media URL");
    const std::string temporary = relative + "." + MaiIdGenerator::generate("part_") + ".download";
    auto downloader = makeMaiDownloadFileTool(caBundle);
    const MaiToolResult fetched = downloader->execute(
        Json{{"url", url}, {"output_path", temporary}, {"max_size_mb", 50}, {"timeout_s", 300}}
            .dump(),
        context);
    if (fetched.hasError()) return fetched;
    const Json downloaded = Json::parse(fetched.output(), nullptr, false);
    const std::string source = value(downloaded, "path");
    std::string prefix;
    if (source.empty())
        return failure(MaiErrorCode::Protocol, "protocol", "GLM media download is unreadable");
    if (MaiFileSystem::readFile(MaiFilePath::fromUtf8(source), prefix, 16)) {
        (void)MaiFileSystem::removeFile(MaiFilePath::fromUtf8(source));
        return failure(MaiErrorCode::Protocol, "protocol", "GLM media download is unreadable");
    }
    const bool png = prefix.size() >= 8 && prefix.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0;
    const bool jpeg = prefix.size() >= 3 && static_cast<unsigned char>(prefix[0]) == 0xff &&
                      static_cast<unsigned char>(prefix[1]) == 0xd8 &&
                      static_cast<unsigned char>(prefix[2]) == 0xff;
    const bool webp = prefix.size() >= 12 && prefix.compare(0, 4, "RIFF") == 0 &&
                      prefix.compare(8, 4, "WEBP") == 0;
    if (!png && !jpeg && !webp) {
        (void)MaiFileSystem::removeFile(MaiFilePath::fromUtf8(source));
        return failure(MaiErrorCode::Protocol, "protocol", "GLM output is not expected media");
    }
    const std::string extension = png ? ".png" : jpeg ? ".jpg" : ".webp";
    const std::string target = maiResolvePathWithinRoot(context.root, relative + extension);
    if (target.empty() || MaiFileSystem::exists(MaiFilePath::fromUtf8(target)) ||
        MaiFileSystem::publishNewFile(MaiFilePath::fromUtf8(source),
                                      MaiFilePath::fromUtf8(target))) {
        (void)MaiFileSystem::removeFile(MaiFilePath::fromUtf8(source));
        return failure(MaiErrorCode::Internal, "io_error", "Could not publish GLM media");
    }
    return MaiToolResult::success(Json{{"path", target},
                                       {"mime_type", png    ? "image/png"
                                                     : jpeg ? "image/jpeg"
                                                            : "image/webp"}}
                                      .dump());
}

class MaiGlmImageTool final : public MaiTool {
public:
    MaiGlmImageTool(MaiGlmApiKeyProvider key, std::string caBundle)
        : mKey(std::move(key)), mCaBundle(std::move(caBundle)) {}

    std::string name() const override {
        return "glm_image";
    }
    std::string description() const override {
        // 仅保留 glm-image 文生图。该异步接口不接入已有图片，也不能编辑已有视频；
        // discover 查真实能力，delegate 付费提交，continue 查询并下载结果。
        return "GLM-Image specialist for paid text-to-image. Use discover to inspect "
               "capabilities, delegate after confirmation, and continue to check a saved task. "
               "Existing-image editing and video generation are not provided by this tool. "
               "The App automatically reports and downloads a completed image.";
    }
    std::string parametersSchema() const override {
        const std::string raw =
            R"({"type":"object","properties":{"action":{"type":"string","enum":["discover","delegate","continue"]},"message":{"type":"string"},"context":{"type":"string"},"size":{"type":"string"},"conversation_id":{"type":"string"},"parent_task_id":{"type":"string"},"poll_once":{"type":"boolean"}},"required":["action"],"additionalProperties":false})";
        Json schema = Json::parse(raw);
        Json& fields = schema["properties"];
        // 这些英文描述会直接给主模型。图片生成只使用文字，不接受 image_path；
        // 不要因 GLM 同平台还有视频型号，就推断此工具能生成视频。
        fields["action"]["description"] =
            "discover reads capabilities; delegate starts a paid image task; continue checks it";
        fields["message"]["description"] = "Required description of the new image";
        fields["context"]["description"] = "Only relevant visual constraints";
        fields["size"]["description"] =
            "Output width x height, each 1024-2048 and a 32-pixel multiple; default 1280x1280";
        fields["conversation_id"]["description"] =
            "Saved spt_ task ID for continue in the current AI conversation";
        fields["parent_task_id"]["description"] =
            "Optional previous local task ID for history only; no media input is reused";
        fields["poll_once"]["description"] =
            "Query once; the App normally polls and reports the result automatically";
        return schema.dump();
    }
    std::optional<MaiSpecialistInfo> specialistInfo() const override {
        const bool configured = mKey && !mKey().empty();
        const auto status = configured ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                       : MaiSpecialistCapabilityStatus::NotConfigured;
        MaiSpecialistInfo info;
        info.toolName = name();
        info.modelId = kImageModel;
        info.configured = configured;
        info.capabilities.push_back({"text_to_image", true, true, status,
                                     "Provider entitlement requires a live API response"});
        info.capabilities.push_back(
            {"existing_image_edit", false, false, MaiSpecialistCapabilityStatus::NotImplemented,
             "GLM-Image async generation accepts text, not a source image"});
        return info;
    }
    bool requiresApproval(const std::string& raw) const override {
        return requiresPerCallApproval(raw);
    }
    bool requiresPerCallApproval(const std::string& raw) const override {
        const Json args = Json::parse(raw, nullptr, false);
        const std::string action = value(args, "action");
        return action != "discover" && action != "continue";
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object()) return invalid("arguments must be a JSON object");
        const Json schema = Json::parse(parametersSchema());
        for (auto field = args.begin(); field != args.end(); ++field) {
            if (!schema["properties"].contains(field.key()))
                return invalid("unsupported GLM image parameter: " + field.key());
        }
        for (const char* field :
             {"action", "message", "context", "size", "conversation_id", "parent_task_id"}) {
            if (args.contains(field) && !args[field].is_string())
                return invalid(std::string(field) + " must be a string");
        }
        if (args.contains("poll_once") && !args["poll_once"].is_boolean())
            return invalid("poll_once must be boolean");
        const std::string action = value(args, "action");
        if (action == "discover") {
            const MaiSpecialistInfo info = *specialistInfo();
            Json capabilities = Json::array();
            for (const MaiSpecialistCapability& capability : info.capabilities)
                capabilities.push_back(
                    {{"id", capability.id},
                     {"model_support", capability.modelSupported},
                     {"api_support", capability.apiSupported},
                     {"tool_status", maiSpecialistCapabilityStatusToString(capability.status)},
                     {"limitation", capability.limitation}});
            return MaiToolResult::success(Json{{"tool_kind", "model_backed"},
                                               {"bound_model", kImageModel},
                                               {"configured", info.configured},
                                               {"capabilities", std::move(capabilities)},
                                               {"reply", "GLM image capabilities listed"}}
                                              .dump());
        }
        if (action != "delegate" && action != "continue")
            return invalid("action must be discover, delegate, or continue");
        const std::string key = mKey ? mKey() : std::string{};
        if (key.empty())
            return failure(MaiErrorCode::NotConfigured, "not_configured",
                           "GLM API key is missing in the host configuration");
        return action == "delegate" ? delegate(args, key, context)
                                    : continueTask(args, key, context);
    }

private:
    MaiToolResult delegate(const Json& args, const std::string& key,
                           const MaiToolContext& context) const {
        const std::string message = value(args, "message");
        const std::string extra = value(args, "context");
        if (message.empty() || message.size() > 4000 || extra.size() > 4000)
            return invalid("message is required and message/context must be at most 4000 bytes");
        std::string prompt = message;
        if (!extra.empty()) prompt += "\nRelevant context: " + extra;
        const std::string size = value(args, "size").empty() ? "1280x1280" : value(args, "size");
        if (!validImageSize(size))
            return invalid("image size must be a supported 32-pixel multiple");
        if (context.root.empty()) return invalid("Agent workspace is required");
        const Json body = {{"model", kImageModel},
                           {"prompt", prompt},
                           {"watermark_enabled", true},
                           {"quality", "hd"},
                           {"size", size}};
        const HttpResult submitted = requestJson(kImageUrl, key, mCaBundle, &body, context);
        if (submitted.error) return *submitted.error;
        const std::string providerId = value(submitted.data, "id");
        if (!validId(providerId))
            return failure(MaiErrorCode::Protocol, "protocol", "GLM returned no task ID");
        Json output = {
            {"task_id", providerId},
            {"conversation_id", providerId},
            {"status", "submitted"},
            {"bound_model", kImageModel},
            {"reply", "The GLM image task has started. The App will report its result."}};
        if (context.specialistTasks != nullptr && !context.sessionId.empty()) {
            MaiSpecialistTask task;
            task.id = MaiIdGenerator::generate("spt_");
            task.ownerSessionId = context.sessionId;
            task.specialistName = name();
            task.parentTaskId = value(args, "parent_task_id");
            task.providerTaskId = providerId;
            task.intent = message;
            task.contextSummary = extra;
            task.created = MaiTime::getCurrentTime();
            const MaiError stored = context.specialistTasks->insertSpecialistTask(task);
            if (stored) {
                output["context_persistence_warning"] = stored.message();
                output["reply"] = "Task submitted, but automatic notification was not scheduled.";
            } else {
                output["conversation_id"] = task.id;
                output["specialist_task_id"] = task.id;
            }
        }
        return MaiToolResult::success(output.dump());
    }

    // 查询已提交的智谱媒体任务并在完成后下载产物。
    // 云任务标识保持不变；查询失败与生成失败以不同状态回报。
    MaiToolResult continueTask(const Json& args, const std::string& key,
                               const MaiToolContext& context) const {
        const std::string conversationId = value(args, "conversation_id");
        if (!validId(conversationId)) return invalid("conversation_id must be a valid task ID");
        std::string providerId = conversationId;
        if (conversationId.compare(0, 4, "spt_") == 0) {
            MaiSpecialistTask previous;
            if (context.specialistTasks == nullptr ||
                !context.specialistTasks->getSpecialistTask(conversationId, context.sessionId,
                                                            previous) ||
                previous.specialistName != name())
                return failure(MaiErrorCode::NotFound, "not_found",
                               "GLM image task was not found in this conversation");
            if (previous.status == MaiSpecialistTaskStatus::Succeeded &&
                !previous.outputPath.empty())
                return MaiToolResult::success(Json{
                    {"conversation_id", conversationId},
                    {"status", "succeeded"},
                    {"path", previous.outputPath},
                    {"reply", previous.finalText}}.dump());
            providerId = previous.providerTaskId;
        }
        if (!validId(providerId)) return invalid("GLM provider task ID is invalid");
        const HttpResult result =
            requestJson(std::string(kResultUrl) + providerId, key, mCaBundle, nullptr, context);
        if (result.error) return *result.error;
        const std::string status = value(result.data, "task_status");
        if (status == "PROCESSING")
            return MaiToolResult::success(Json{{"conversation_id", conversationId},
                                               {"task_id", providerId},
                                               {"status", "running"},
                                               {"reply", "The GLM image task is still processing."}}
                                              .dump());
        if (status == "FAIL") {
            const Json detail = result.data.value("error", Json::object());
            const std::string message = value(detail, "message");
            return failure(MaiErrorCode::Network, "task_failed",
                           message.empty() ? "GLM image generation failed" : message);
        }
        if (status != "SUCCESS")
            return failure(MaiErrorCode::Protocol, "protocol", "GLM returned unknown task status");
        if (!result.data.contains("image_result") || !result.data["image_result"].is_array() ||
            result.data["image_result"].empty())
            return failure(MaiErrorCode::Protocol, "protocol", "GLM returned no image result");
        const std::string url = value(result.data["image_result"][0], "url");
        const std::string relative = "glm-" + MaiIdGenerator::generate("file_");
        MaiToolResult media = downloadImage(url, relative, context, mCaBundle);
        if (media.hasError()) return media;
        Json output = Json::parse(media.output());
        output["conversation_id"] = conversationId;
        output["task_id"] = providerId;
        output["status"] = "succeeded";
        output["bound_model"] = kImageModel;
        output["reply"] = "GLM image is ready for review at the returned path.";
        return MaiToolResult::success(output.dump());
    }

    MaiGlmApiKeyProvider mKey;
    std::string mCaBundle;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiGlmImageTool(MaiGlmApiKeyProvider apiKey,
                                             std::string caBundlePath) {
    return std::make_unique<MaiGlmImageTool>(std::move(apiKey), std::move(caBundlePath));
}
