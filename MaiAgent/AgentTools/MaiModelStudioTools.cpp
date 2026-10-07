#include "MaiModelStudioTools.h"

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

bool isPaidGenerationAction(const std::string& raw) {
    const Json args = Json::parse(raw, nullptr, false);
    if (!args.is_object() || !args.value("action", Json{}).is_string()) return true;
    const std::string action = args["action"].get<std::string>();
    return action != "discover" && action != "diagnose" && action != "continue";
}

constexpr char kVideoEditModel[] = "wan2.7-videoedit";
constexpr char kWanVideoModel[] = "wan3.0-video";
constexpr char kQwenImageModel[] = "qwen-image-3.0-pro";
constexpr std::size_t kMaxResponseBytes = 2 * 1024 * 1024;
constexpr std::uint64_t kMaxVideoBytes = 100'000'000;
constexpr std::uint64_t kMaxImageBytes = 20'000'000;

std::string value(const Json& object, const char* field) {
    if (!object.is_object() || !object.contains(field) || !object[field].is_string()) return {};
    return object[field].get<std::string>();
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
               return std::isalnum(character) || character == '-' || character == '_';
           });
}

bool validWorkspaceId(const std::string& id) {
    const std::size_t prefix = id.compare(0, 3, "ws-") == 0    ? 3
                               : id.compare(0, 4, "llm-") == 0 ? 4
                                                               : 0;
    return prefix != 0 && id.size() > prefix && validId(id);
}

std::string baseName(const std::string& path) {
    return MaiFilePath::fromUtf8(path).baseName().toUtf8();
}

std::string extension(const std::string& path) {
    const std::string name = baseName(path);
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string result = name.substr(dot + 1);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

bool httpsHost(const std::string& url, const std::string& suffix) {
    if (url.compare(0, 8, "https://") != 0) return false;
    CURLU* parsed = curl_url();
    if (parsed == nullptr) return false;
    char* host = nullptr;
    char* user = nullptr;
    const bool parsedUrl = curl_url_set(parsed, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK;
    const bool hasHost = parsedUrl && curl_url_get(parsed, CURLUPART_HOST, &host, 0) == CURLUE_OK;
    const bool hasUser = parsedUrl && curl_url_get(parsed, CURLUPART_USER, &user, 0) == CURLUE_OK;
    const std::string name = hasHost ? host : "";
    const bool allowed =
        hasHost && !hasUser && !name.empty() &&
        (suffix.empty() || (name.size() > suffix.size() &&
                            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0));
    if (host != nullptr) curl_free(host);
    if (user != nullptr) curl_free(user);
    curl_url_cleanup(parsed);
    return allowed;
}

struct ResponseBuffer {
    std::string bytes;
    bool exceeded = false;
};

std::size_t receive(char* data, std::size_t size, std::size_t count, void* user) {
    auto& buffer = *static_cast<ResponseBuffer*>(user);
    if (count != 0 && size > SIZE_MAX / count) return 0;
    const std::size_t bytes = size * count;
    if (bytes > kMaxResponseBytes - buffer.bytes.size()) {
        buffer.exceeded = true;
        return 0;
    }
    buffer.bytes.append(data, bytes);
    return bytes;
}

int checkCanceled(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<const MaiToolContext*>(user)->isCanceled() ? 1 : 0;
}

struct HttpResponse {
    Json body;
    std::optional<MaiToolResult> error;
    bool transferFailed = false;
};

HttpResponse requestJson(const std::string& url, const std::string& key,
                         const std::string& caBundle, const Json* body, bool async, bool resolveOss,
                         const MaiToolContext& context, long timeoutSeconds = 180L) {
    maiAssertBlockingAllowed("wan_video_edit");
    CURL* curl = curl_easy_init();
    if (curl == nullptr)
        return {{}, failure(MaiErrorCode::Internal, "internal", "Could not initialize HTTP")};
    ResponseBuffer response;
    const std::string payload = body != nullptr ? body->dump() : std::string{};
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: Bearer " + key).c_str());
    headers = curl_slist_append(headers, "Accept: application/json");
    if (body != nullptr) headers = curl_slist_append(headers, "Content-Type: application/json");
    if (async) headers = curl_slist_append(headers, "X-DashScope-Async: enable");
    if (resolveOss) headers = curl_slist_append(headers, "X-DashScope-OssResourceResolve: enable");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, body == nullptr ? 60L : timeoutSeconds);
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
        return {{},
                failure(MaiErrorCode::Protocol, "protocol", "Response exceeded 2 MB"),
                async && body != nullptr && status >= 200 && status < 300};
    if (result != CURLE_OK)
        return {{}, failure(MaiErrorCode::Network, "network", curl_easy_strerror(result)), true};
    Json parsed = Json::parse(response.bytes, nullptr, false);
    if (!parsed.is_object())
        return {{},
                failure(MaiErrorCode::Protocol, "protocol", "Model Studio returned invalid JSON"),
                async && body != nullptr && status >= 200 && status < 300};
    if (status < 200 || status >= 300) {
        const std::string providerMessage = value(parsed, "message");
        const Json detail = parsed.value("error", Json::object());
        const std::string message =
            providerMessage.empty() ? value(detail, "message") : providerMessage;
        const std::string providerCode =
            value(parsed, "code").empty() ? value(detail, "code") : value(parsed, "code");
        return {{},
                MaiToolResult::failure(
                    MaiErrorCode::Network,
                    Json{{"code", "provider_error"},
                         {"provider_code", providerCode},
                         {"http_status", status},
                         {"request_id", value(parsed, "request_id")},
                         {"message", message.empty() ? "Model Studio request failed (HTTP " +
                                                           std::to_string(status) + ")"
                                                     : message}}
                        .dump())};
    }
    return {std::move(parsed), std::nullopt};
}

std::optional<MaiToolResult> readMedia(const std::string& candidate, const MaiToolContext& context,
                                       std::uint64_t limit, std::string& bytes, std::string& path) {
    path = context.resolvePath(candidate);
    if (path.empty()) return invalid("Media path is not accessible");
    std::uint64_t size = 0;
    if (!MaiFileSystem::fileSize(MaiFilePath::fromUtf8(path), size) || size == 0 || size > limit)
        return invalid("Media file is empty or exceeds the model size limit");
    bool truncated = false;
    const MaiError error =
        MaiFileSystem::readFile(MaiFilePath::fromUtf8(path), bytes, limit + 1, &truncated);
    if (error) return failure(MaiErrorCode::Internal, "io_error", error.message());
    if (truncated || bytes.size() > limit)
        return invalid("Media file exceeds the model size limit");
    return std::nullopt;
}

std::string encodeBase64(const std::string& bytes) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const auto a = static_cast<unsigned char>(bytes[index]);
        const auto b = index + 1 < bytes.size() ? static_cast<unsigned char>(bytes[index + 1]) : 0;
        const auto c = index + 2 < bytes.size() ? static_cast<unsigned char>(bytes[index + 2]) : 0;
        encoded.push_back(alphabet[a >> 2]);
        encoded.push_back(alphabet[((a & 3) << 4) | (b >> 4)]);
        encoded.push_back(index + 1 < bytes.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=');
        encoded.push_back(index + 2 < bytes.size() ? alphabet[c & 63] : '=');
    }
    return encoded;
}

std::string modelStudioBase(const MaiWanCredentials& credentials) {
    return "https://" + credentials.workspaceId + ".cn-beijing.maas.aliyuncs.com/api/v1";
}

bool hasCredentials(const MaiWanCredentials& credentials) {
    return !credentials.apiKey.empty() && validWorkspaceId(credentials.workspaceId);
}

MaiToolResult diagnoseModel(const MaiWanCredentials& credentials, const char* model,
                            const std::string& caBundle, const MaiToolContext& context) {
    HttpResponse response =
        requestJson(modelStudioBase(credentials) + "/models/limits?model=" + model,
                    credentials.apiKey, caBundle, nullptr, false, false, context);
    if (response.error) return *response.error;
    const Json output = response.body.value("output", Json::object());
    const Json quotas = output.value("quotas", Json::array());
    if (!quotas.is_array())
        return failure(MaiErrorCode::Protocol, "protocol", "Invalid model limit response");
    for (const Json& entry : quotas) {
        if (value(entry, "model") != model ||
            value(entry, "workspace_id") != credentials.workspaceId)
            continue;
        const Json limit = entry.value("model_limit", Json::object());
        return MaiToolResult::success(
            Json{{"model", model},
                 {"model_visible", true},
                 {"workspace_matches", true},
                 {"request_limit", limit.value("request_limit", Json(nullptr))},
                 {"usage_limit", limit.value("usage_limit", Json(nullptr))},
                 {"free_balance_checked", false},
                 {"note",
                  "Model visibility and rate limits do not prove billing balance or "
                  "permission for every generation request."}}
                .dump());
    }
    return MaiToolResult::success(
        Json{{"model", model},
             {"model_visible", false},
             {"workspace_matches", false},
             {"free_balance_checked", false},
             {"note", "This key cannot see the model in this workspace's limit list."}}
            .dump());
}

std::optional<MaiToolResult> imageDataUrl(const std::string& candidate,
                                          const MaiToolContext& context, std::uint64_t maxBytes,
                                          std::string& url) {
    std::string bytes;
    std::string path;
    if (auto error = readMedia(candidate, context, maxBytes, bytes, path)) return error;
    const std::string format = extension(path);
    const std::string mime = (format == "jpg" || format == "jpeg") ? "image/jpeg"
                             : format == "png"                     ? "image/png"
                             : format == "webp"                    ? "image/webp"
                                                                   : "";
    if (mime.empty()) return invalid("Image must be JPEG, PNG, or WebP");
    url = "data:" + mime + ";base64," + encodeBase64(bytes);
    return std::nullopt;
}

std::optional<MaiToolResult> uploadVideo(const std::string& candidate,
                                         const MaiWanCredentials& credentials,
                                         const std::string& caBundle, const char* model,
                                         const MaiToolContext& context, std::string& ossUrl) {
    std::string bytes;
    std::string path;
    if (auto error = readMedia(candidate, context, kMaxVideoBytes, bytes, path)) return error;
    const std::string format = extension(path);
    if (format != "mp4" && format != "mov") return invalid("video_path must be .mp4 or .mov");
    const std::string policyUrl =
        "https://dashscope.aliyuncs.com/api/v1/uploads?action=getPolicy&model=" +
        std::string(model);
    HttpResponse policy =
        requestJson(policyUrl, credentials.apiKey, caBundle, nullptr, false, false, context);
    if (policy.error) return policy.error;
    const Json details = policy.body.value("data", Json::object());
    const std::string host = value(details, "upload_host");
    const std::string directory = value(details, "upload_dir");
    if (!httpsHost(host, ".aliyuncs.com") || directory.compare(0, 18, "dashscope-instant/") != 0 ||
        directory.find("..") != std::string::npos || value(details, "oss_access_key_id").empty() ||
        value(details, "signature").empty() || value(details, "policy").empty() ||
        value(details, "x_oss_object_acl") != "private")
        return failure(MaiErrorCode::Protocol, "protocol", "Invalid private upload policy");
    const std::string objectKey =
        directory + "/" + MaiIdGenerator::generate("mai_wan_") + "." + format;
    CURL* curl = curl_easy_init();
    if (curl == nullptr)
        return failure(MaiErrorCode::Internal, "internal", "Could not initialize upload");
    curl_mime* mime = curl_mime_init(curl);
    if (mime == nullptr) {
        curl_easy_cleanup(curl);
        return failure(MaiErrorCode::Internal, "internal", "Could not initialize upload form");
    }
    const auto addText = [&](const char* name, const std::string& text) {
        curl_mimepart* part = curl_mime_addpart(mime);
        return part != nullptr && curl_mime_name(part, name) == CURLE_OK &&
               curl_mime_data(part, text.c_str(), CURL_ZERO_TERMINATED) == CURLE_OK;
    };
    const bool validForm =
        addText("OSSAccessKeyId", value(details, "oss_access_key_id")) &&
        addText("Signature", value(details, "signature")) &&
        addText("policy", value(details, "policy")) &&
        addText("x-oss-object-acl", value(details, "x_oss_object_acl")) &&
        addText("x-oss-forbid-overwrite", value(details, "x_oss_forbid_overwrite")) &&
        addText("key", objectKey) && addText("success_action_status", "200");
    curl_mimepart* file = validForm ? curl_mime_addpart(mime) : nullptr;
    const bool validFile =
        file != nullptr && curl_mime_name(file, "file") == CURLE_OK &&
        curl_mime_filename(file, baseName(path).c_str()) == CURLE_OK &&
        curl_mime_type(file, format == "mp4" ? "video/mp4" : "video/quicktime") == CURLE_OK &&
        curl_mime_data(file, bytes.data(), bytes.size()) == CURLE_OK;
    if (!validFile) {
        curl_mime_free(mime);
        curl_easy_cleanup(curl);
        return failure(MaiErrorCode::Internal, "internal", "Could not construct upload form");
    }
    ResponseBuffer response;
    curl_easy_setopt(curl, CURLOPT_URL, host.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, checkCanceled);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    if (!caBundle.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
#endif
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    if (context.isCanceled())
        return failure(MaiErrorCode::Canceled, "canceled", "Upload was canceled");
    if (response.exceeded || result != CURLE_OK || status != 200)
        return failure(MaiErrorCode::Network, "upload_failed",
                       "Private video upload failed (HTTP " + std::to_string(status) + ")");
    ossUrl = "oss://" + objectKey;
    return std::nullopt;
}

MaiToolResult downloadMedia(const std::string& url, const std::string& relative,
                            const MaiToolContext& context, bool png) {
    if (!httpsHost(url, ""))
        return failure(MaiErrorCode::Protocol, "protocol", "No HTTPS media output URL");
    auto downloader = makeMaiDownloadFileTool();
    const MaiToolResult result = downloader->execute(
        Json{{"url", url}, {"output_path", relative}, {"max_size_mb", 500}, {"timeout_s", 300}}
            .dump(),
        context);
    if (result.hasError()) return result;
    Json output = Json::parse(result.output(), nullptr, false);
    const std::string path = value(output, "path");
    std::string prefix;
    const MaiError readError = MaiFileSystem::readFile(MaiFilePath::fromUtf8(path), prefix, 12);
    const bool valid =
        !readError && (png ? prefix.size() >= 8 && prefix.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0
                           : prefix.size() >= 8 && prefix.compare(4, 4, "ftyp") == 0);
    if (!valid) {
        if (!path.empty()) (void)MaiFileSystem::removeFile(MaiFilePath::fromUtf8(path));
        return failure(MaiErrorCode::Protocol, "protocol", "Output has an invalid media format");
    }
    output["mime_type"] = png ? "image/png" : "video/mp4";
    return MaiToolResult::success(output.dump());
}

class MaiWanVideoEditTool final : public MaiTool {
public:
    MaiWanVideoEditTool(MaiWanCredentialsProvider credentials, std::string caBundle)
        : mCredentials(std::move(credentials)), mCaBundle(std::move(caBundle)) {}

    std::string name() const override {
        return "wan_video_edit";
    }
    bool requiresApproval(const std::string& raw) const override {
        return isPaidGenerationAction(raw);
    }
    bool requiresPerCallApproval(const std::string& raw) const override {
        return isPaidGenerationAction(raw);
    }

    std::optional<MaiSpecialistInfo> specialistInfo() const override {
        const MaiWanCredentials credentials = mCredentials ? mCredentials() : MaiWanCredentials{};
        const bool configured =
            !credentials.apiKey.empty() && validWorkspaceId(credentials.workspaceId);
        const auto status = configured ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                       : MaiSpecialistCapabilityStatus::NotConfigured;
        return MaiSpecialistInfo{
            name(),
            kVideoEditModel,
            configured,
            {{"instruction_video_edit", true, true, status,
              "Input video: 2-10 seconds; output normally keeps the source duration"},
             {"reference_image_video_edit", true, true, status,
              "Up to four JPEG, PNG, or WebP images"},
             {"local_video_upload", true, true, status,
              "Private temporary upload; development and testing only"}}};
    }

    std::string description() const override {
        return "Wan2.7 video editing specialist. Use discover for capabilities and diagnose "
               "for a read-only model access check. "
               "Delegate one local 2-10 second MP4/MOV and a clear editing goal, or revise a "
               "completed task in the same AI conversation. The app reports completion "
               "automatically; continue is for manual diagnostics only. "
               "Optional local reference images guide clothes or objects. Uploads use private "
               "temporary Model Studio storage; this upload route is for development and "
               "testing, not production. The source file stays unchanged. Use agent_send_media "
               "to deliver completed output.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("action":{"type":"string","enum":["discover","diagnose","delegate","continue","revise"]},)"
               R"("message":{"type":"string"},"context":{"type":"string"},)"
               R"("conversation_id":{"type":"string"},"video_path":{"type":"string"},)"
               R"("reference_image_paths":{"type":"array","maxItems":4,"items":{"type":"string"}},)"
               R"("resolution":{"type":"string","enum":["720P","1080P"]},)"
               R"("audio_setting":{"type":"string","enum":["auto","origin"]},)"
               R"("prompt_extend":{"type":"boolean"},"watermark":{"type":"boolean"}},)"
               R"("required":["action"],"additionalProperties":false})";
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object()) return invalid("arguments must be an object");
        for (const char* field : {"action", "message", "context", "conversation_id", "video_path",
                                  "resolution", "audio_setting"}) {
            if (args.contains(field) && !args[field].is_string())
                return invalid(std::string(field) + " must be a string");
        }
        const std::string action = value(args, "action");
        const std::string message = value(args, "message");
        if (message.empty() && (action == "delegate" || action == "revise"))
            return invalid("message is required to start or revise a task");
        if (action == "discover") {
            const MaiSpecialistInfo info = *specialistInfo();
            const MaiWanCredentials credentials =
                mCredentials ? mCredentials() : MaiWanCredentials{};
            Json missing = Json::array();
            if (credentials.apiKey.empty()) missing.push_back("api_key");
            if (!validWorkspaceId(credentials.workspaceId)) missing.push_back("workspace_id");
            Json capabilities = Json::array();
            for (const auto& item : info.capabilities)
                capabilities.push_back(
                    Json{{"id", item.id},
                         {"tool_status", maiSpecialistCapabilityStatusToString(item.status)},
                         {"limitation", item.limitation}});
            return MaiToolResult::success(Json{{"tool_kind", "model_backed"},
                                               {"bound_model", kVideoEditModel},
                                               {"configured", info.configured},
                                               {"missing_configuration", missing},
                                               {"capabilities", capabilities},
                                               {"reply",
                                                "I edit an existing short video using instructions "
                                                "and optional reference images."}}
                                              .dump());
        }
        const MaiWanCredentials credentials = mCredentials ? mCredentials() : MaiWanCredentials{};
        if (credentials.apiKey.empty() || !validWorkspaceId(credentials.workspaceId))
            return failure(MaiErrorCode::NotConfigured, "not_configured",
                           "Configure a Beijing Model Studio API key and Workspace ID");
        if (action == "diagnose")
            return diagnoseModel(credentials, kVideoEditModel, mCaBundle, context);
        if (action == "delegate") return delegate(args, credentials, context);
        if (action == "continue") return continueTask(args, credentials, context);
        if (action == "revise") {
            MaiSpecialistTask previous;
            const std::string id = value(args, "conversation_id");
            if (context.specialistTasks == nullptr ||
                !context.specialistTasks->getSpecialistTask(id, context.sessionId, previous) ||
                previous.specialistName != name() || !validId(previous.providerTaskId))
                return failure(MaiErrorCode::NotFound, "not_found",
                               "Previous Wan edit was not found in this AI conversation");
            if (!value(args, "video_path").empty())
                return invalid("revise uses the completed previous video; do not set video_path");
            const std::string url = taskVideoUrl(previous.providerTaskId, credentials, context);
            if (url.empty())
                return failure(MaiErrorCode::NotFound, "not_found",
                               "Previous output URL is unavailable; it may have expired");
            Json revision = args;
            revision["source_url"] = url;
            revision["parent_task_id"] = previous.id;
            std::string prior = previous.contextSummary;
            if (!prior.empty()) prior += "\n";
            prior += "Previous goal: " + previous.intent;
            const std::string current = value(args, "context");
            if (!current.empty()) prior += "\nCurrent feedback: " + current;
            if (prior.size() > 4000) return invalid("Revision context is too long");
            revision["context"] = std::move(prior);
            return delegate(revision, credentials, context);
        }
        return invalid("action must be discover, diagnose, delegate, continue, or revise");
    }

private:
    std::string apiBase(const MaiWanCredentials& credentials) const {
        return "https://" + credentials.workspaceId + ".cn-beijing.maas.aliyuncs.com/api/v1";
    }

    std::string taskVideoUrl(const std::string& taskId, const MaiWanCredentials& credentials,
                             const MaiToolContext& context) const {
        HttpResponse response =
            requestJson(apiBase(credentials) + "/tasks/" + taskId, credentials.apiKey, mCaBundle,
                        nullptr, false, false, context);
        if (response.error) return {};
        const Json output = response.body.value("output", Json::object());
        if (value(output, "task_status") != "SUCCEEDED") return {};
        const std::string url = value(output, "video_url");
        return httpsHost(url, "") ? url : std::string{};
    }

    MaiToolResult delegate(const Json& args, const MaiWanCredentials& credentials,
                           const MaiToolContext& context) const {
        const std::string message = value(args, "message");
        const std::string extra = value(args, "context");
        if (message.empty() || message.size() > 5000 || extra.size() > 4000)
            return invalid("message or context is too long");
        if (!args.contains("resolution"))
            return invalid("confirm 720P or 1080P resolution before video editing");
        const std::string resolution =
            value(args, "resolution").empty() ? "720P" : value(args, "resolution");
        const std::string audio =
            value(args, "audio_setting").empty() ? "origin" : value(args, "audio_setting");
        if ((resolution != "720P" && resolution != "1080P") ||
            (audio != "auto" && audio != "origin"))
            return invalid("Unsupported resolution or audio_setting");
        for (const char* field : {"prompt_extend", "watermark"}) {
            if (args.contains(field) && !args[field].is_boolean())
                return invalid(std::string(field) + " must be boolean");
        }
        if (args.contains("reference_image_paths") && !args["reference_image_paths"].is_array())
            return invalid("reference_image_paths must be an array");
        const Json imagePaths = args.value("reference_image_paths", Json::array());
        if (imagePaths.size() > 4) return invalid("At most four reference images are supported");
        const std::string sourceUrl = value(args, "source_url");
        const std::string videoPath = value(args, "video_path");
        if (sourceUrl.empty() == videoPath.empty())
            return invalid("Provide exactly one local video source");
        Json media = Json::array();
        for (const Json& entry : imagePaths) {
            if (!entry.is_string()) return invalid("Each reference image path must be a string");
            std::string bytes;
            std::string path;
            if (auto error =
                    readMedia(entry.get<std::string>(), context, kMaxImageBytes, bytes, path))
                return *error;
            const std::string format = extension(path);
            const std::string mime = (format == "jpg" || format == "jpeg") ? "image/jpeg"
                                     : format == "png"                     ? "image/png"
                                     : format == "webp"                    ? "image/webp"
                                                                           : "";
            if (mime.empty()) return invalid("Reference image must be JPEG, PNG, or WebP");
            media.push_back(Json{{"type", "reference_image"},
                                 {"url", "data:" + mime + ";base64," + encodeBase64(bytes)}});
        }
        std::string prompt = message;
        if (!extra.empty()) prompt += "\nRelevant context: " + extra;
        if (prompt.size() > 5000) return invalid("Combined prompt exceeds 5000 characters");
        std::string videoUrl = sourceUrl;
        if (videoUrl.empty()) {
            if (auto error = uploadVideo(videoPath, credentials, mCaBundle, kVideoEditModel,
                                         context, videoUrl))
                return *error;
        } else if (!httpsHost(videoUrl, "")) {
            return invalid("Revision source is not an HTTPS video URL");
        }
        media.insert(media.begin(), Json{{"type", "video"}, {"url", videoUrl}});
        const Json body = {{"model", kVideoEditModel},
                           {"input", {{"prompt", prompt}, {"media", media}}},
                           {"parameters",
                            {{"resolution", resolution},
                             {"audio_setting", audio},
                             {"prompt_extend", args.value("prompt_extend", false)},
                             {"watermark", args.value("watermark", true)}}}};
        HttpResponse response =
            requestJson(apiBase(credentials) + "/services/aigc/video-generation/video-synthesis",
                        credentials.apiKey, mCaBundle, &body, true,
                        videoUrl.compare(0, 6, "oss://") == 0, context);
        if (response.error) {
            if (response.transferFailed)
                return failure(MaiErrorCode::Network, "submission_unknown",
                               "The edit request may have reached Model Studio. Do not submit "
                               "it again automatically; inspect the provider task list first.");
            return *response.error;
        }
        const Json output = response.body.value("output", Json::object());
        const std::string taskId = value(output, "task_id");
        if (!validId(taskId))
            return failure(MaiErrorCode::Protocol, "submission_unknown",
                           "Model Studio accepted the edit but returned no task ID. Do not "
                           "submit it again automatically; inspect the provider task list.");
        Json result = {{"task_id", taskId},
                       {"conversation_id", taskId},
                       {"status", value(output, "task_status")},
                       {"bound_model", kVideoEditModel},
                       {"reply", "Video edit submitted. The app will report its result."}};
        if (context.specialistTasks != nullptr && !context.sessionId.empty()) {
            MaiSpecialistTask task;
            task.id = MaiIdGenerator::generate("spt_");
            task.ownerSessionId = context.sessionId;
            task.specialistName = name();
            task.parentTaskId = value(args, "parent_task_id");
            task.providerTaskId = taskId;
            task.intent = message;
            task.contextSummary = extra;
            task.inputReference = videoPath.empty() ? task.parentTaskId : videoPath;
            task.created = MaiTime::getCurrentTime();
            const MaiError stored = context.specialistTasks->insertSpecialistTask(task);
            if (stored) {
                result["context_persistence_warning"] = stored.message();
                result["reply"] =
                    "Paid edit was submitted, but context was not saved. Do not resubmit.";
            } else {
                result["conversation_id"] = task.id;
                result["specialist_task_id"] = task.id;
                if (!task.parentTaskId.empty()) result["revision_of"] = task.parentTaskId;
            }
        }
        return MaiToolResult::success(result.dump());
    }

    MaiToolResult continueTask(const Json& args, const MaiWanCredentials& credentials,
                               const MaiToolContext& context) const {
        const std::string conversationId = value(args, "conversation_id");
        if (!validId(conversationId)) return invalid("A valid conversation_id is required");
        std::string taskId = conversationId;
        if (conversationId.compare(0, 4, "spt_") == 0) {
            MaiSpecialistTask previous;
            if (context.specialistTasks == nullptr ||
                !context.specialistTasks->getSpecialistTask(conversationId, context.sessionId,
                                                            previous) ||
                previous.specialistName != name())
                return failure(MaiErrorCode::NotFound, "not_found",
                               "Wan edit was not found in this AI conversation");
            taskId = previous.providerTaskId;
        }
        if (!validId(taskId)) return invalid("Invalid provider task ID");
        HttpResponse response =
            requestJson(apiBase(credentials) + "/tasks/" + taskId, credentials.apiKey, mCaBundle,
                        nullptr, false, false, context);
        if (response.error) return *response.error;
        const Json output = response.body.value("output", Json::object());
        const std::string status = value(output, "task_status");
        if (status == "FAILED" || status == "CANCELED" || status == "UNKNOWN")
            return failure(MaiErrorCode::Network, "task_failed",
                           value(response.body, "message").empty()
                               ? "Wan edit task ended with status " + status
                               : value(response.body, "message"));
        if (status == "PENDING" || status == "RUNNING")
            return MaiToolResult::success(Json{{"conversation_id", conversationId},
                                               {"task_id", taskId},
                                               {"status", status},
                                               {"reply", "Video editing is still in progress."}}
                                              .dump());
        if (status != "SUCCEEDED")
            return failure(MaiErrorCode::Protocol, "protocol", "Unknown Wan task status");
        const std::string relative = "wan-videoedit-" + taskId + ".mp4";
        if (context.root.empty() || maiResolvePathWithinRoot(context.root, relative).empty())
            return invalid("An Agent workspace is required to save the video");
        const std::string path = maiResolvePathWithinRoot(context.root, relative);
        std::uint64_t size = 0;
        if (MaiFileSystem::fileSize(MaiFilePath::fromUtf8(path), size) && size > 0)
            return MaiToolResult::success(Json{{"conversation_id", conversationId},
                                               {"task_id", taskId},
                                               {"status", status},
                                               {"path", path},
                                               {"bytes", size},
                                               {"mime_type", "video/mp4"},
                                               {"reply", "Edited video saved locally."}}
                                              .dump());
        MaiToolResult downloaded =
            downloadMedia(value(output, "video_url"), relative, context, false);
        if (downloaded.hasError()) return downloaded;
        Json result = Json::parse(downloaded.output());
        result.update(Json{{"conversation_id", conversationId},
                           {"task_id", taskId},
                           {"status", status},
                           {"reply", "Edited video saved locally."}});
        return MaiToolResult::success(result.dump());
    }

    MaiWanCredentialsProvider mCredentials;
    std::string mCaBundle;
};

class MaiWanVideoTool final : public MaiTool {
public:
    MaiWanVideoTool(MaiWanCredentialsProvider credentials, std::string caBundle)
        : mCredentials(std::move(credentials)), mCaBundle(std::move(caBundle)) {}

    std::string name() const override {
        return "wan_video";
    }
    bool requiresApproval(const std::string& raw) const override {
        return isPaidGenerationAction(raw);
    }
    bool requiresPerCallApproval(const std::string& raw) const override {
        return isPaidGenerationAction(raw);
    }

    std::optional<MaiSpecialistInfo> specialistInfo() const override {
        const MaiWanCredentials credentials = mCredentials ? mCredentials() : MaiWanCredentials{};
        const bool configured = hasCredentials(credentials);
        const auto status = configured ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                       : MaiSpecialistCapabilityStatus::NotConfigured;
        return MaiSpecialistInfo{
            name(),
            kWanVideoModel,
            configured,
            {{"text_to_video", true, true, status,
              "Creates a 2-30 second video from text; video input and output must total at most 30 "
              "seconds"},
             {"first_frame_to_video", true, true, status, "Accepts a local first-frame image"},
             {"first_last_frame_to_video", true, true, status,
              "Accepts local first and last frame images"},
             {"reference_video_edit", true, true, status,
              "Uploads one local MP4/MOV as Video 1 for editing or extension"},
             {"reference_images", true, true, status, "Accepts up to ten local images"},
             {"multiple_reference_videos", true, true,
              MaiSpecialistCapabilityStatus::NotImplemented, "Only one video reference is wired"}}};
    }

    std::string description() const override {
        return "Wan3.0 model-backed video specialist supporting 2-30 second output. Use discover "
               "for capabilities and diagnose "
               "for a read-only model access check. "
               "Delegate a text goal to create a video, optionally with first/last frames; "
               "delegate an edit or extend goal with video_path to reference Video 1. A reference "
               "goal can combine one local video and up to ten images. The app checks tasks and "
               "reports completion automatically; continue is for manual diagnostics only. "
               "Revise a completed task in the same AI "
               "conversation with new feedback. Cloud moderation can still reject a request.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("action":{"type":"string","enum":["discover","diagnose","delegate","continue","revise"]},)"
               R"("message":{"type":"string"},"context":{"type":"string"},)"
               R"("conversation_id":{"type":"string"},"mode":{"type":"string",)"
               R"("enum":["create","edit","extend","reference"]},)"
               R"("video_path":{"type":"string"},"first_frame_path":{"type":"string"},)"
               R"("last_frame_path":{"type":"string"},"reference_image_paths":{"type":"array",)"
               R"("maxItems":10,"items":{"type":"string"}},)"
               R"("resolution":{"type":"string","enum":["480P","720P","1080P"]},)"
               R"("ratio":{"type":"string","enum":["adaptive","21:9","16:9","4:3","1:1","3:4","9:16"]},)"
               R"("duration":{"type":"integer"},"audio":{"type":"boolean"},)"
               R"("prompt_extend":{"type":"boolean"},"watermark":{"type":"boolean"}},)"
               R"("required":["action"],"additionalProperties":false})";
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object()) return invalid("arguments must be an object");
        for (const char* field :
             {"action", "message", "context", "conversation_id", "mode", "video_path",
              "first_frame_path", "last_frame_path", "resolution", "ratio"}) {
            if (args.contains(field) && !args[field].is_string())
                return invalid(std::string(field) + " must be a string");
        }
        const std::string action = value(args, "action");
        const std::string message = value(args, "message");
        if (message.empty() && (action == "delegate" || action == "revise"))
            return invalid("message is required to start or revise a task");
        const MaiWanCredentials credentials = mCredentials ? mCredentials() : MaiWanCredentials{};
        if (action == "discover") {
            const MaiSpecialistInfo info = *specialistInfo();
            Json capabilities = Json::array();
            for (const auto& capability : info.capabilities)
                capabilities.push_back(
                    Json{{"id", capability.id},
                         {"tool_status", maiSpecialistCapabilityStatusToString(capability.status)},
                         {"limitation", capability.limitation}});
            Json missing = Json::array();
            if (credentials.apiKey.empty()) missing.push_back("api_key");
            if (!validWorkspaceId(credentials.workspaceId)) missing.push_back("workspace_id");
            return MaiToolResult::success(
                Json{{"tool_kind", "model_backed"},
                     {"bound_model", kWanVideoModel},
                     {"configured", info.configured},
                     {"missing_configuration", missing},
                     {"capabilities", capabilities},
                     {"reply", "I can create or edit a video using Wan3.0."}}
                    .dump());
        }
        if (!hasCredentials(credentials))
            return failure(MaiErrorCode::NotConfigured, "not_configured",
                           "Configure a Model Studio API key and Workspace ID");
        if (action == "diagnose")
            return diagnoseModel(credentials, kWanVideoModel, mCaBundle, context);
        if (action == "delegate") return delegate(args, credentials, context);
        if (action == "continue") return continueTask(args, credentials, context);
        if (action == "revise") {
            MaiSpecialistTask previous;
            const std::string id = value(args, "conversation_id");
            if (context.specialistTasks == nullptr ||
                !context.specialistTasks->getSpecialistTask(id, context.sessionId, previous) ||
                previous.specialistName != name() || !validId(previous.providerTaskId))
                return failure(MaiErrorCode::NotFound, "not_found",
                               "Previous Wan3 task was not found in this AI conversation");
            if (!value(args, "video_path").empty())
                return invalid("revise uses the previous output; do not set video_path");
            HttpResponse completed =
                requestJson(modelStudioBase(credentials) + "/tasks/" + previous.providerTaskId,
                            credentials.apiKey, mCaBundle, nullptr, false, false, context);
            if (completed.error) return *completed.error;
            const Json output = completed.body.value("output", Json::object());
            const std::string sourceUrl = value(output, "video_url");
            if (value(output, "task_status") != "SUCCEEDED" || !httpsHost(sourceUrl, ""))
                return failure(MaiErrorCode::NotFound, "not_found",
                               "Previous output is unfinished or its URL has expired");
            Json revision = args;
            revision["mode"] = "edit";
            revision["source_url"] = sourceUrl;
            revision["parent_task_id"] = previous.id;
            std::string prior = previous.contextSummary;
            if (!prior.empty()) prior += "\n";
            prior += "Previous goal: " + previous.intent;
            const std::string current = value(args, "context");
            if (!current.empty()) prior += "\nCurrent feedback: " + current;
            if (prior.size() > 4000) return invalid("Revision context is too long");
            revision["context"] = std::move(prior);
            return delegate(revision, credentials, context);
        }
        return invalid("action must be discover, diagnose, delegate, continue, or revise");
    }

private:
    MaiToolResult delegate(const Json& args, const MaiWanCredentials& credentials,
                           const MaiToolContext& context) const {
        const std::string message = value(args, "message");
        const std::string extra = value(args, "context");
        if (message.size() > 10000 || extra.size() > 4000)
            return invalid("message or context is too long");
        const std::string mode = value(args, "mode").empty() ? "create" : value(args, "mode");
        if (mode != "create" && mode != "edit" && mode != "extend" && mode != "reference")
            return invalid("Unsupported Wan3 mode");
        const std::string videoPath = value(args, "video_path");
        const std::string sourceUrl = value(args, "source_url");
        const std::string first = value(args, "first_frame_path");
        const std::string last = value(args, "last_frame_path");
        if (!videoPath.empty() && !sourceUrl.empty())
            return invalid("Provide one video source, not two");
        if ((mode == "create" && (!videoPath.empty() || !sourceUrl.empty())) ||
            ((mode == "edit" || mode == "extend") && videoPath.empty() && sourceUrl.empty()))
            return invalid("Edit and extend require one video; create cannot use video_path");
        if (!last.empty() && first.empty())
            return invalid("last_frame_path requires first_frame_path");
        if ((!first.empty() || !last.empty()) && mode != "create")
            return invalid("First/last frames are only valid for create mode");
        if (args.contains("reference_image_paths") && !args["reference_image_paths"].is_array())
            return invalid("reference_image_paths must be an array");
        const Json imagePaths = args.value("reference_image_paths", Json::array());
        if (imagePaths.size() > 10 || (!first.empty() && !imagePaths.empty()))
            return invalid("Use up to ten references or first/last frames, not both");
        if (mode == "reference" && videoPath.empty() && sourceUrl.empty() && imagePaths.empty())
            return invalid("Reference mode requires an image or video");
        if ((mode == "create" || mode == "reference") &&
            (!args.contains("duration") || !args.contains("resolution") || !args.contains("ratio")))
            return invalid("confirm duration, resolution, and ratio before video generation");
        const std::string resolution =
            value(args, "resolution").empty() ? "720P" : value(args, "resolution");
        const std::string ratio = value(args, "ratio").empty() ? "adaptive" : value(args, "ratio");
        if (resolution != "480P" && resolution != "720P" && resolution != "1080P")
            return invalid("resolution must be 480P, 720P, or 1080P");
        if (ratio != "adaptive" && ratio != "21:9" && ratio != "16:9" && ratio != "4:3" &&
            ratio != "1:1" && ratio != "3:4" && ratio != "9:16")
            return invalid("Unsupported Wan3 ratio");
        if (args.contains("duration") && !args["duration"].is_number_integer())
            return invalid("duration must be an integer");
        const int duration = args.value("duration", mode == "edit" ? -1 : 5);
        if (duration != -1 && (duration < 2 || duration > 30))
            return invalid("duration must be -1 or 2 to 30 seconds");
        for (const char* field : {"audio", "prompt_extend", "watermark"}) {
            if (args.contains(field) && !args[field].is_boolean())
                return invalid(std::string(field) + " must be boolean");
        }
        Json media = Json::array();
        if (!first.empty()) {
            std::string image;
            if (auto error = imageDataUrl(first, context, kMaxImageBytes, image)) return *error;
            media.push_back(Json{{"type", "first_frame"}, {"url", image}});
        }
        if (!last.empty()) {
            std::string image;
            if (auto error = imageDataUrl(last, context, kMaxImageBytes, image)) return *error;
            media.push_back(Json{{"type", "last_frame"}, {"url", image}});
        }
        for (const Json& item : imagePaths) {
            if (!item.is_string()) return invalid("Reference image path must be a string");
            std::string image;
            if (auto error = imageDataUrl(item.get<std::string>(), context, kMaxImageBytes, image))
                return *error;
            media.push_back(Json{{"type", "reference_image"}, {"url", image}});
        }
        std::string prompt = message;
        if (!extra.empty()) prompt += "\nRelevant context: " + extra;
        if (mode == "edit") prompt = "Edit video 1. Preserve unmentioned content. " + prompt;
        if (mode == "extend") prompt = "Extend video 1 consistently. " + prompt;
        if (prompt.size() > 20000) return invalid("Combined prompt exceeds 20000 characters");
        std::string videoUrl = sourceUrl;
        if (!videoPath.empty()) {
            if (auto error = uploadVideo(videoPath, credentials, mCaBundle, kWanVideoModel, context,
                                         videoUrl))
                return *error;
        }
        if (!videoUrl.empty())
            media.push_back(Json{{"type", "reference_video"}, {"url", videoUrl}});
        Json input = {{"prompt", prompt}};
        if (!media.empty()) input["media"] = std::move(media);
        const Json body = {{"model", kWanVideoModel},
                           {"input", input},
                           {"parameters",
                            {{"resolution", resolution},
                             {"ratio", ratio},
                             {"duration", duration},
                             {"audio", args.value("audio", true)},
                             {"prompt_extend", args.value("prompt_extend", false)},
                             {"watermark", args.value("watermark", true)}}}};
        HttpResponse response = requestJson(
            modelStudioBase(credentials) + "/services/aigc/video-generation/video-synthesis",
            credentials.apiKey, mCaBundle, &body, true, videoUrl.compare(0, 6, "oss://") == 0,
            context);
        if (response.error) {
            if (response.transferFailed)
                return failure(
                    MaiErrorCode::Network, "submission_unknown",
                    "Wan3 request may have been accepted. Do not resubmit automatically.");
            return *response.error;
        }
        const Json output = response.body.value("output", Json::object());
        const std::string taskId = value(output, "task_id");
        if (!validId(taskId))
            return failure(MaiErrorCode::Protocol, "submission_unknown",
                           "Wan3 returned no task ID; do not resubmit automatically");
        Json result = {{"task_id", taskId},
                       {"conversation_id", taskId},
                       {"status", value(output, "task_status")},
                       {"bound_model", kWanVideoModel},
                       {"reply", "Wan3 video task submitted. The app will report its result."}};
        if (context.specialistTasks != nullptr && !context.sessionId.empty()) {
            MaiSpecialistTask task;
            task.id = MaiIdGenerator::generate("spt_");
            task.ownerSessionId = context.sessionId;
            task.specialistName = name();
            task.parentTaskId = value(args, "parent_task_id");
            task.providerTaskId = taskId;
            task.intent = message;
            task.contextSummary = extra;
            task.inputReference = videoPath.empty() ? first : videoPath;
            task.created = MaiTime::getCurrentTime();
            const MaiError stored = context.specialistTasks->insertSpecialistTask(task);
            if (stored)
                result["context_persistence_warning"] = stored.message();
            else {
                result["conversation_id"] = task.id;
                result["specialist_task_id"] = task.id;
                if (!task.parentTaskId.empty()) result["revision_of"] = task.parentTaskId;
            }
        }
        return MaiToolResult::success(result.dump());
    }

    MaiToolResult continueTask(const Json& args, const MaiWanCredentials& credentials,
                               const MaiToolContext& context) const {
        const std::string conversationId = value(args, "conversation_id");
        if (!validId(conversationId)) return invalid("A valid conversation_id is required");
        std::string taskId = conversationId;
        if (conversationId.compare(0, 4, "spt_") == 0) {
            MaiSpecialistTask previous;
            if (context.specialistTasks == nullptr ||
                !context.specialistTasks->getSpecialistTask(conversationId, context.sessionId,
                                                            previous) ||
                previous.specialistName != name())
                return failure(MaiErrorCode::NotFound, "not_found",
                               "Wan3 task was not found in this AI conversation");
            taskId = previous.providerTaskId;
        }
        if (!validId(taskId)) return invalid("Invalid provider task ID");
        HttpResponse response =
            requestJson(modelStudioBase(credentials) + "/tasks/" + taskId, credentials.apiKey,
                        mCaBundle, nullptr, false, false, context);
        if (response.error) return *response.error;
        const Json output = response.body.value("output", Json::object());
        const std::string status = value(output, "task_status");
        if (status == "FAILED" || status == "CANCELED" || status == "UNKNOWN")
            return failure(MaiErrorCode::Network, "task_failed",
                           value(response.body, "message").empty()
                               ? "Wan3 task ended with status " + status
                               : value(response.body, "message"));
        if (status == "PENDING" || status == "RUNNING")
            return MaiToolResult::success(Json{{"conversation_id", conversationId},
                                               {"task_id", taskId},
                                               {"status", status},
                                               {"reply", "Wan3 video is still in progress."}}
                                              .dump());
        if (status != "SUCCEEDED")
            return failure(MaiErrorCode::Protocol, "protocol", "Unknown Wan3 task status");
        const std::string relative = "wan3-" + taskId + ".mp4";
        if (context.root.empty() || maiResolvePathWithinRoot(context.root, relative).empty())
            return invalid("An Agent workspace is required to save the video");
        const std::string path = maiResolvePathWithinRoot(context.root, relative);
        std::uint64_t size = 0;
        if (MaiFileSystem::fileSize(MaiFilePath::fromUtf8(path), size) && size > 0)
            return MaiToolResult::success(Json{{"conversation_id", conversationId},
                                               {"task_id", taskId},
                                               {"status", status},
                                               {"path", path},
                                               {"bytes", size},
                                               {"mime_type", "video/mp4"},
                                               {"reply", "Wan3 video saved locally."}}
                                              .dump());
        MaiToolResult downloaded =
            downloadMedia(value(output, "video_url"), relative, context, false);
        if (downloaded.hasError()) return downloaded;
        Json result = Json::parse(downloaded.output());
        result.update(Json{{"conversation_id", conversationId},
                           {"task_id", taskId},
                           {"status", status},
                           {"reply", "Wan3 video saved locally."}});
        return MaiToolResult::success(result.dump());
    }

    MaiWanCredentialsProvider mCredentials;
    std::string mCaBundle;
};

class MaiQwenImageTool final : public MaiTool {
public:
    MaiQwenImageTool(MaiWanCredentialsProvider credentials, std::string caBundle)
        : mCredentials(std::move(credentials)), mCaBundle(std::move(caBundle)) {}

    std::string name() const override {
        return "qwen_image";
    }
    bool requiresApproval(const std::string& raw) const override {
        return isPaidGenerationAction(raw);
    }
    bool requiresPerCallApproval(const std::string& raw) const override {
        return isPaidGenerationAction(raw);
    }

    std::optional<MaiSpecialistInfo> specialistInfo() const override {
        const MaiWanCredentials credentials = mCredentials ? mCredentials() : MaiWanCredentials{};
        const bool configured = hasCredentials(credentials);
        const auto status = configured ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                       : MaiSpecialistCapabilityStatus::NotConfigured;
        return MaiSpecialistInfo{
            name(),
            kQwenImageModel,
            configured,
            {{"text_to_image", true, true, status, "Creates one PNG from a text goal"},
             {"image_edit", true, true, status,
              "Edits one to three local JPEG, PNG, or WebP inputs"},
             {"image_revision", true, true, status,
              "Reuses the previous PNG in the same AI conversation"}}};
    }

    std::string description() const override {
        return "Qwen-Image-3.0-Pro model-backed image specialist. Use discover for capabilities "
               "and diagnose for a read-only model access check. Delegate a text goal with no "
               "input for a new PNG, or pass one to "
               "three local image_paths to edit or combine images. Revise a previous result by "
               "conversation_id and feedback. Input files are unchanged; the new PNG is saved "
               "inside the Agent workspace. Use agent_send_media to deliver it.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("action":{"type":"string","enum":["discover","diagnose","delegate","revise"]},)"
               R"("message":{"type":"string"},"context":{"type":"string"},)"
               R"("conversation_id":{"type":"string"},"image_path":{"type":"string"},)"
               R"("image_paths":{"type":"array","maxItems":3,"items":{"type":"string"}},)"
               R"("output_path":{"type":"string"},"size":{"type":"string"},)"
               R"("negative_prompt":{"type":"string"},"prompt_extend":{"type":"boolean"},)"
               R"("watermark":{"type":"boolean"}},"required":["action"],)"
               R"("additionalProperties":false})";
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object()) return invalid("arguments must be an object");
        for (const char* field : {"action", "message", "context", "conversation_id", "image_path",
                                  "output_path", "size", "negative_prompt"}) {
            if (args.contains(field) && !args[field].is_string())
                return invalid(std::string(field) + " must be a string");
        }
        const std::string action = value(args, "action");
        const std::string message = value(args, "message");
        if (message.empty() && (action == "delegate" || action == "revise"))
            return invalid("message is required to start or revise a task");
        const MaiWanCredentials credentials = mCredentials ? mCredentials() : MaiWanCredentials{};
        if (action == "discover") {
            const MaiSpecialistInfo info = *specialistInfo();
            Json capabilities = Json::array();
            for (const auto& capability : info.capabilities)
                capabilities.push_back(
                    Json{{"id", capability.id},
                         {"tool_status", maiSpecialistCapabilityStatusToString(capability.status)},
                         {"limitation", capability.limitation}});
            Json missing = Json::array();
            if (credentials.apiKey.empty()) missing.push_back("api_key");
            if (!validWorkspaceId(credentials.workspaceId)) missing.push_back("workspace_id");
            return MaiToolResult::success(
                Json{{"tool_kind", "model_backed"},
                     {"bound_model", kQwenImageModel},
                     {"configured", info.configured},
                     {"missing_configuration", missing},
                     {"capabilities", capabilities},
                     {"reply", "I create or edit PNG images with Qwen-Image-3.0-Pro."}}
                    .dump());
        }
        if (!hasCredentials(credentials))
            return failure(MaiErrorCode::NotConfigured, "not_configured",
                           "Configure a Model Studio API key and Workspace ID");
        if (action == "diagnose")
            return diagnoseModel(credentials, kQwenImageModel, mCaBundle, context);
        if (action == "revise") {
            MaiSpecialistTask previous;
            const std::string id = value(args, "conversation_id");
            if (context.specialistTasks == nullptr ||
                !context.specialistTasks->getSpecialistTask(id, context.sessionId, previous) ||
                previous.specialistName != name() || previous.outputPath.empty())
                return failure(MaiErrorCode::NotFound, "not_found",
                               "Previous Qwen image was not found in this AI conversation");
            if (!value(args, "image_path").empty() || args.contains("image_paths"))
                return invalid("revise uses the previous PNG; do not override its input");
            args["image_path"] = previous.outputPath;
            args["parent_task_id"] = previous.id;
            std::string prior = previous.contextSummary;
            if (!prior.empty()) prior += "\n";
            prior += "Previous goal: " + previous.intent;
            const std::string current = value(args, "context");
            if (!current.empty()) prior += "\nCurrent feedback: " + current;
            if (prior.size() > 4000) return invalid("Revision context is too long");
            args["context"] = std::move(prior);
            return delegate(args, credentials, context);
        }
        if (action == "delegate") return delegate(args, credentials, context);
        return invalid("action must be discover, diagnose, delegate, or revise");
    }

private:
    MaiToolResult delegate(const Json& args, const MaiWanCredentials& credentials,
                           const MaiToolContext& context) const {
        const std::string message = value(args, "message");
        const std::string extra = value(args, "context");
        std::string prompt = message;
        if (!extra.empty()) prompt += "\nRelevant context: " + extra;
        if (prompt.size() > 12000) return invalid("Image prompt is too long");
        const std::string negative = value(args, "negative_prompt");
        if (negative.size() > 3000) return invalid("negative_prompt is too long");
        for (const char* field : {"prompt_extend", "watermark"}) {
            if (args.contains(field) && !args[field].is_boolean())
                return invalid(std::string(field) + " must be boolean");
        }
        const std::string singleImage = value(args, "image_path");
        if (args.contains("image_paths") && !args["image_paths"].is_array())
            return invalid("image_paths must be an array");
        const Json imagePaths = args.value("image_paths", Json::array());
        if ((!singleImage.empty() && !imagePaths.empty()) || imagePaths.size() > 3)
            return invalid("Provide one image_path or up to three image_paths");
        Json images = Json::array();
        if (!singleImage.empty()) {
            std::string image;
            if (auto error = imageDataUrl(singleImage, context, 10'000'000, image)) return *error;
            images.push_back(image);
        }
        for (const Json& item : imagePaths) {
            if (!item.is_string()) return invalid("Each image path must be a string");
            std::string image;
            if (auto error = imageDataUrl(item.get<std::string>(), context, 10'000'000, image))
                return *error;
            images.push_back(image);
        }
        const std::string size = value(args, "size");
        if (!size.empty() && size != "auto") {
            const std::size_t separator = size.find('x');
            if (separator == std::string::npos || separator == 0 || separator > 5 ||
                separator + 1 >= size.size() || size.size() - separator - 1 > 5)
                return invalid("size must be auto or WIDTHxHEIGHT");
            const auto digits = [](unsigned char character) {
                return std::isdigit(character) != 0;
            };
            if (!std::all_of(size.begin(), size.begin() + static_cast<std::ptrdiff_t>(separator),
                             digits) ||
                !std::all_of(size.begin() + static_cast<std::ptrdiff_t>(separator + 1), size.end(),
                             digits))
                return invalid("size must be auto or WIDTHxHEIGHT");
            const int width = std::stoi(size.substr(0, separator));
            const int height = std::stoi(size.substr(separator + 1));
            const std::int64_t area = static_cast<std::int64_t>(width) * height;
            if (width < 1 || height < 1 || area < 512LL * 512 || area > 2048LL * 2048 ||
                width > height * 8 || height > width * 8)
                return invalid("size is outside Qwen-Image-3.0-Pro limits");
        }
        std::string relative = value(args, "output_path");
        if (relative.empty()) relative = MaiIdGenerator::generate("qwen_") + ".png";
        if (context.root.empty() || MaiFilePath::fromUtf8(relative).isAbsolute() ||
            extension(relative) != "png" ||
            maiResolvePathWithinRoot(context.root, relative).empty())
            return invalid("output_path must be a relative .png path in the Agent workspace");
        const std::string path = maiResolvePathWithinRoot(context.root, relative);
        if (MaiFileSystem::exists(MaiFilePath::fromUtf8(path)))
            return invalid("output_path already exists");
        Json body = {{"model", kQwenImageModel},
                     {"prompt", prompt},
                     {"n", 1},
                     {"prompt_extend", args.value("prompt_extend", true)},
                     {"watermark", args.value("watermark", false)}};
        if (!negative.empty()) body["negative_prompt"] = negative;
        if (!size.empty()) body["size"] = size;
        if (images.size() == 1) body["image"] = images[0];
        if (images.size() > 1) body["image"] = images;
        HttpResponse response =
            requestJson("https://" + credentials.workspaceId +
                            ".cn-beijing.maas.aliyuncs.com/compatible-mode/v1/images/generations",
                        credentials.apiKey, mCaBundle, &body, false, false, context, 600L);
        if (response.error) {
            if (response.transferFailed)
                return failure(
                    MaiErrorCode::Network, "submission_unknown",
                    "Qwen image request may have completed. Do not retry automatically.");
            return *response.error;
        }
        const Json data = response.body.value("data", Json::array());
        if (!data.is_array() || data.empty())
            return failure(MaiErrorCode::Protocol, "protocol", "Qwen returned no image");
        const std::string url = value(data[0], "url");
        MaiToolResult downloaded = downloadMedia(url, relative, context, true);
        if (downloaded.hasError()) return downloaded;
        Json output = Json::parse(downloaded.output());
        output["bound_model"] = kQwenImageModel;
        output["reply"] = "Qwen image saved locally.";
        if (context.specialistTasks != nullptr && !context.sessionId.empty()) {
            MaiSpecialistTask task;
            task.id = MaiIdGenerator::generate("spt_");
            task.ownerSessionId = context.sessionId;
            task.specialistName = name();
            task.parentTaskId = value(args, "parent_task_id");
            task.intent = message;
            task.contextSummary = extra;
            task.inputReference = singleImage.empty() ? "text_or_multiple_images" : singleImage;
            task.outputPath = value(output, "path");
            task.created = MaiTime::getCurrentTime();
            const MaiError stored = context.specialistTasks->insertSpecialistTask(task);
            if (stored)
                output["context_persistence_warning"] = stored.message();
            else {
                output["conversation_id"] = task.id;
                output["specialist_task_id"] = task.id;
                if (!task.parentTaskId.empty()) output["revision_of"] = task.parentTaskId;
            }
        }
        return MaiToolResult::success(output.dump());
    }

    MaiWanCredentialsProvider mCredentials;
    std::string mCaBundle;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiWanVideoEditTool(MaiWanCredentialsProvider credentials,
                                                 std::string caBundlePath) {
    return std::make_unique<MaiWanVideoEditTool>(std::move(credentials), std::move(caBundlePath));
}

std::unique_ptr<MaiTool> makeMaiWanVideoTool(MaiWanCredentialsProvider credentials,
                                             std::string caBundlePath) {
    return std::make_unique<MaiWanVideoTool>(std::move(credentials), std::move(caBundlePath));
}

std::unique_ptr<MaiTool> makeMaiQwenImageTool(MaiWanCredentialsProvider credentials,
                                              std::string caBundlePath) {
    return std::make_unique<MaiQwenImageTool>(std::move(credentials), std::move(caBundlePath));
}
