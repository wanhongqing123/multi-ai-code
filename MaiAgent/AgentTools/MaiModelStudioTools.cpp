#include "MaiModelStudioTools.h"

#include <curl/curl.h>
#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
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
    // delegate/revise 可能扣费；cancel/delete 虽不发起新生成，也会改变云端或本地
    // 任务状态，因此继续走每次确认。discover/diagnose/continue 才是纯只读。
    const Json args = Json::parse(raw, nullptr, false);
    if (!args.is_object() || !args.value("action", Json{}).is_string()) return true;
    const std::string action = args["action"].get<std::string>();
    return action != "discover" && action != "diagnose" && action != "continue";
}

constexpr char kWanVideoModel[] = "wan3.0-video";
constexpr char kBailianKlingTurboModel[] = "kling/kling-v3-turbo-video-generation";
constexpr char kBailianKlingStandardModel[] = "kling/kling-v3-video-generation";
constexpr char kBailianKlingOmniModel[] = "kling/kling-v3-omni-video-generation";
constexpr char kWanImageModel[] = "wan2.7-image";
constexpr char kBailianKlingImageModel[] = "kling/kling-v3-image-generation";
constexpr char kBailianKlingOmniImageModel[] = "kling/kling-v3-omni-image-generation";
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

std::optional<MaiToolResult> rejectUnknownFields(const Json& args, const std::string& rawSchema) {
    // Schema 拒绝旧字段只是模型侧提示；这里在付费执行前再核对一次，防止静默丢素材。
    const Json schema = Json::parse(rawSchema, nullptr, false);
    if (!schema.is_object() || !schema.value("properties", Json{}).is_object())
        return failure(MaiErrorCode::Internal, "internal", "Invalid specialist schema");
    for (auto field = args.begin(); field != args.end(); ++field) {
        if (!schema["properties"].contains(field.key()))
            return invalid("unsupported media parameter: " + field.key());
    }
    return std::nullopt;
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

bool supportedWanDocument(const std::string& path) {
    const std::string suffix = extension(path);
    return suffix == "docx" || suffix == "doc" || suffix == "xlsx" || suffix == "xls" ||
           suffix == "pptx" || suffix == "ppt" || suffix == "pdf" || suffix == "txt" ||
           suffix == "key" || suffix == "pages" || suffix == "numbers" || suffix == "md";
}

bool httpsHost(const std::string& url, const std::string& suffix);

bool supportedWanDocumentUrl(const std::string& url) {
    if (!httpsHost(url, "")) return false;
    CURLU* parsed = curl_url();
    if (parsed == nullptr) return false;
    char* path = nullptr;
    const bool valid = curl_url_set(parsed, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK &&
                       curl_url_get(parsed, CURLUPART_PATH, &path, 0) == CURLUE_OK &&
                       path != nullptr && supportedWanDocument(path);
    if (path != nullptr) curl_free(path);
    curl_url_cleanup(parsed);
    return valid;
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
                         const MaiToolContext& context, long timeoutSeconds = 180L,
                         bool cancelRequest = false) {
    maiAssertBlockingAllowed("model_studio_request");
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
    char curlError[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlError);
    if (!caBundle.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
    if (body != nullptr) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(payload.size()));
    }
    // 百炼取消是无正文 POST，不能因 body 为空就误发 GET。
    if (cancelRequest) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(0));
    }
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
#endif
    CURLcode result = CURLE_OK;
    long status = 0;
    int attempts = 0;
    // 只读状态查询遇到短暂网络故障或限流可重试；付费 POST 绝不自动重提。
    for (;;) {
        ++attempts;
        response.bytes.clear();
        response.exceeded = false;
        curlError[0] = '\0';
        result = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        const bool transientCurl =
            result == CURLE_COULDNT_RESOLVE_HOST || result == CURLE_COULDNT_CONNECT ||
            result == CURLE_OPERATION_TIMEDOUT || result == CURLE_SEND_ERROR ||
            result == CURLE_RECV_ERROR || result == CURLE_GOT_NOTHING;
        const bool transientHttp =
            result == CURLE_OK && (status == 408 || status == 429 || status == 500 ||
                                   status == 502 || status == 503 || status == 504);
        if (body != nullptr || cancelRequest || attempts >= 3 || context.isCanceled() ||
            response.exceeded || (!transientCurl && !transientHttp))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(attempts * 250));
    }
    char* primaryIp = nullptr;
    curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &primaryIp);
    const std::string connectedIp = primaryIp != nullptr ? std::string(primaryIp) : std::string{};
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (context.isCanceled())
        return {{}, failure(MaiErrorCode::Canceled, "canceled", "Task was canceled")};
    if (response.exceeded)
        return {{},
                failure(MaiErrorCode::Protocol, "protocol", "Response exceeded 2 MB"),
                async && body != nullptr && status >= 200 && status < 300};
    if (result != CURLE_OK)
        return {
            {},
            MaiToolResult::failure(
                MaiErrorCode::Network,
                Json{{"code", "network_error"},
                     {"stage", result == CURLE_PEER_FAILED_VERIFICATION ? "tls_verify"
                               : result == CURLE_COULDNT_RESOLVE_HOST   ? "dns"
                                                                        : "transport"},
                     {"method", body == nullptr && !cancelRequest ? "GET" : "POST"},
                     {"request_url", url.substr(0, url.find('?'))},
                     {"connected_ip", connectedIp},
                     {"curl_code", static_cast<int>(result)},
                     {"curl_error", curlError[0] != '\0' ? curlError : curl_easy_strerror(result)},
                     {"http_status", status},
                     {"attempts", attempts},
                     {"request_id", nullptr},
                     {"message", curl_easy_strerror(result)}}
                    .dump()),
            body != nullptr};
    Json parsed = Json::parse(response.bytes, nullptr, false);
    // 百炼取消接口有的地域返回对象，有的实现返回纯 request_id 字符串。
    if (cancelRequest && parsed.is_string())
        parsed = Json{{"request_id", parsed.get<std::string>()}};
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
                         {"request_url", url.substr(0, url.find('?'))},
                         {"attempts", attempts},
                         {"message", message.empty() ? "Model Studio request failed (HTTP " +
                                                           std::to_string(status) + ")"
                                                     : message}}
                        .dump())};
    }
    return {std::move(parsed), std::nullopt};
}

std::string modelStudioBase(const MaiWanCredentials& credentials) {
    return "https://" + credentials.workspaceId + ".cn-beijing.maas.aliyuncs.com/api/v1";
}

bool hasCredentials(const MaiWanCredentials& credentials) {
    return !credentials.apiKey.empty() && validWorkspaceId(credentials.workspaceId);
}

std::string describeKlingVideoSchema(const std::string& raw) {
    Json schema = Json::parse(raw);
    Json& fields = schema["properties"];
    // 这里补充的是主模型直接可见的字段含义；Schema 只负责说明参数，
    // Omni 的数量与组合仍由 delegate 在付费提交前严格校验。
    // action：discover/diagnose 只读；delegate 新建付费任务；continue 查旧任务；
    // cancel 仅取消排队任务；delete 只删已交付的本地索引，不删除百炼云端记录。
    fields["action"]["description"] =
        "discover/diagnose are read-only; delegate is paid; continue checks an existing task; "
        "cancel stops only PENDING; delete removes only a delivered local record";
    // message 是本次要生成或编辑的视频指令；不能把模式选择只藏在自然语言里，
    // 还要同时明确 task_mode 和模型型号。
    fields["message"]["description"] =
        "Video creation or edit instruction, up to the provider prompt limit";
    // model：Turbo 只支持文字或一张首帧，分辨率 std/pro；标准版增加尾帧和 4k；
    // Omni 再增加多图参考、单个特征视频参考和源视频编辑。
    fields["model"]["description"] =
        "Turbo: text/first frame, std/pro. Standard: adds last frame and 4k. Omni: adds "
        "reference images, feature video, base-video edit, and 4k";
    // task_mode：create 生成；reference 用 Omni 的图片/特征视频引导；
    // edit 用 Omni 改一段已有视频，不能把 reference 与 edit 混为一谈。
    fields["task_mode"]["description"] =
        "create uses text or strict frame input; reference uses Omni images and/or one feature "
        "video; edit uses Omni and exactly one base video";
    // local_image_paths：图片先上传私有 OSS；无视频时最多七张，带视频时最多四张。
    // 它们是任意参考图，不是首帧或尾帧的严格控制输入。
    fields["local_image_paths"]["description"] =
        "Omni reference images via private OSS: up to seven without video, up to four with "
        "one feature/base video; not strict frame input";
    // image_path 与 last_frame_path：严格首尾帧。尾帧必须有首帧，
    // 且不能选只支持首帧的 Turbo。
    fields["image_path"]["description"] = "One strict first-frame JPEG/PNG uploaded to private OSS";
    fields["last_frame_path"]["description"] =
        "One strict last frame; requires image_path and Standard/Omni create mode";
    // video_path 是本地 MP4/MOV 上传源，source_video_url 是已有 HTTPS 外链，
    // 两者只能给一个；视频应为 3–15.5 秒且不超过 200 MB。
    fields["video_path"]["description"] =
        "One local MP4/MOV uploaded to private OSS as Omni feature or base video; 3-15.5 "
        "seconds, up to 200 MB";
    fields["source_video_url"]["description"] =
        "Accessible HTTPS source video URL, mutually exclusive with video_path";
    // duration：普通生成可选 3–15 秒；特征视频参考生成最多 10 秒；
    // 视频编辑不把 duration 当新的输出时长，而是跟随源视频时长。
    fields["duration"]["description"] =
        "Output 3-15 seconds for pure generation, 3-10 with a feature video; Omni edit "
        "follows source duration";
    // resolution：std=720P，pro=1080P；只有标准版和 Omni 可用 4k。
    fields["resolution"]["description"] = "std=720P, pro=1080P; 4k only with Standard or Omni";
    // ratio：需要画幅的模式只能给 16:9、9:16 或 1:1；不能借用 Wan 的 adaptive。
    fields["ratio"]["description"] =
        "Output 16:9, 9:16, or 1:1 when ratio is required by the chosen mode";
    // audio：这里控制生成声音，不是上传参考音频。Turbo 即使传 false 也会出声；
    // 当前工具没有暴露保留源视频原声的 keep_original_sound 参数。
    fields["audio"]["description"] =
        "Output audio switch; Turbo always produces audio, and this tool cannot preserve "
        "original source audio in a source-video request";
    // watermark 控制输出水印；poll_once 是只查一次旧任务，
    // 正常情况下 App 自己查询并交付，不需要主模型反复手动轮询。
    fields["watermark"]["description"] = "Whether the generated video has a watermark";
    fields["poll_once"]["description"] =
        "Query status once; the App normally polls and delivers the result automatically";
    // conversation_id：继续查询、取消或删除时引用本次 AI 会话保存的 spt_ 任务，
    // 不接受一个任意供应商 ID 来修改别人的任务。
    fields["conversation_id"]["description"] =
        "Saved spt_ task ID for continue, cancel, or delete in this AI conversation";
    return schema.dump();
}

MaiToolResult manageVideoTask(const Json& args, const char* specialistName,
                              const MaiWanCredentials& credentials, const std::string& caBundle,
                              const MaiToolContext& context) {
    // 管理操作只接受本会话持久化的任务 ID，不能拿任意供应商 ID 去取消或清理。
    const std::string conversationId = value(args, "conversation_id");
    MaiSpecialistTask task;
    if (conversationId.compare(0, 4, "spt_") != 0 || context.specialistTasks == nullptr ||
        !context.specialistTasks->getSpecialistTask(conversationId, context.sessionId, task) ||
        task.specialistName != specialistName || !validId(task.providerTaskId))
        return failure(MaiErrorCode::NotFound, "not_found",
                       "Video task was not found in this AI conversation");
    const std::string action = value(args, "action");
    if (action == "delete") {
        // 百炼没有视频任务云端删除 API。只移除已交付的本地索引，不碰云端或视频文件。
        const MaiError removed = context.specialistTasks->deleteCompletedSpecialistTask(
            conversationId, context.sessionId);
        if (removed) return failure(removed.code(), "delete_unavailable", removed.message());
        return MaiToolResult::success(
            Json{{"conversation_id", conversationId},
                 {"task_id", task.providerTaskId},
                 {"status", "local_record_deleted"},
                 {"provider_deleted", false},
                 {"reply", "Local task record deleted; cloud task and downloaded media remain."}}
                .dump());
    }
    if (task.status != MaiSpecialistTaskStatus::Submitted &&
        task.status != MaiSpecialistTaskStatus::Running)
        return invalid("Only an active queued video task can be canceled");
    const std::string url = modelStudioBase(credentials) + "/tasks/" + task.providerTaskId;
    HttpResponse current =
        requestJson(url, credentials.apiKey, caBundle, nullptr, false, false, context);
    if (current.error) return *current.error;
    if (value(current.body.value("output", Json::object()), "task_status") != "PENDING")
        return invalid(
            "Model Studio can cancel only PENDING tasks; this task has started or ended");
    // 成功响应才更新本地状态；网络失败时继续保留轮询，防止把运行任务误报为取消。
    HttpResponse canceled = requestJson(url + "/cancel", credentials.apiKey, caBundle, nullptr,
                                        false, false, context, 60L, true);
    if (canceled.error) return *canceled.error;
    const MaiError saved = context.specialistTasks->finishSpecialistTask(
        conversationId, context.sessionId, MaiSpecialistTaskStatus::Canceled,
        "The queued video task was canceled by the provider.", {}, MaiTime::getCurrentTime());
    Json result = {{"conversation_id", conversationId},
                   {"task_id", task.providerTaskId},
                   {"status", "CANCELED"},
                   {"reply", "The queued cloud video task was canceled."}};
    if (saved) result["local_state_warning"] = saved.message();
    return MaiToolResult::success(result.dump());
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

std::optional<MaiToolResult> uploadOssMedia(const std::string& candidate,
                                            const MaiToolContext& context,
                                            const MaiCreativeMediaUploadProvider& uploadMedia,
                                            std::uint64_t maxBytes, bool video, std::string& url) {
    const std::string path = context.resolvePath(candidate);
    if (path.empty()) return invalid("Media path is not accessible");
    std::uint64_t size = 0;
    if (!MaiFileSystem::fileSize(MaiFilePath::fromUtf8(path), size) || size == 0 || size > maxBytes)
        return invalid("Media file is empty or exceeds the model size limit");
    const std::string format = extension(path);
    if (video ? (format != "mp4" && format != "mov")
              : (format != "jpg" && format != "jpeg" && format != "png"))
        return invalid(video ? "Video must be MP4 or MOV" : "Image must be JPEG or PNG");
    // Wan 与百炼可灵均以公网 HTTPS 引用 OSS 私有素材，不向模型发送 Base64。
    if (!uploadMedia)
        return failure(MaiErrorCode::NotConfigured, "upload_not_configured",
                       "Private OSS media upload is not configured");
    const auto uploaded = uploadMedia(path, context);
    if (!uploaded)
        return failure(uploaded.error().code(), "upload_failed", uploaded.error().message());
    if (!httpsHost(uploaded.value(), ""))
        return failure(MaiErrorCode::Protocol, "upload_failed", "OSS returned no HTTPS URL");
    url = uploaded.value();
    return std::nullopt;
}

std::optional<MaiToolResult> uploadOssDocument(const std::string& candidate,
                                               const MaiToolContext& context,
                                               const MaiCreativeMediaUploadProvider& uploadMedia,
                                               std::string& url) {
    const std::string path = context.resolvePath(candidate);
    std::uint64_t size = 0;
    if (path.empty() || !supportedWanDocument(path) ||
        !MaiFileSystem::fileSize(MaiFilePath::fromUtf8(path), size) || size == 0 ||
        size > 100'000'000)
        return invalid("file_path must be a supported document under 100 MB");
    if (!uploadMedia)
        return failure(MaiErrorCode::NotConfigured, "upload_not_configured",
                       "Private OSS document upload is not configured");
    const auto uploaded = uploadMedia(path, context);
    if (!uploaded)
        return failure(uploaded.error().code(), "upload_failed", uploaded.error().message());
    if (!httpsHost(uploaded.value(), ""))
        return failure(MaiErrorCode::Protocol, "upload_failed", "OSS returned no HTTPS file URL");
    url = uploaded.value();
    return std::nullopt;
}

MaiToolResult downloadMedia(const std::string& url, const std::string& relative,
                            const std::string& caBundle, const MaiToolContext& context, bool png) {
    if (!httpsHost(url, ""))
        return failure(MaiErrorCode::Protocol, "media_download_failed",
                       "Provider returned no HTTPS media output URL");
    // 状态查询与成片下载必须共用宿主提供的 CA；iOS 内嵌 OpenSSL 没有系统默认根证书。
    auto downloader = makeMaiDownloadFileTool(caBundle);
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
        return failure(MaiErrorCode::Protocol, "media_download_failed",
                       "Downloaded media has an invalid format");
    }
    output["mime_type"] = png ? "image/png" : "video/mp4";
    return MaiToolResult::success(output.dump());
}

std::string firstImageUrl(const Json& output) {
    const Json choices = output.value("choices", Json::array());
    if (!choices.is_array() || choices.empty() || !choices[0].is_object()) return {};
    const Json message = choices[0].value("message", Json::object());
    const Json content = message.value("content", Json::array());
    if (!content.is_array()) return {};
    for (const Json& item : content) {
        if (value(item, "type") == "image" && httpsHost(value(item, "image"), ""))
            return value(item, "image");
    }
    return {};
}

class MaiWanVideoTool final : public MaiTool {
public:
    MaiWanVideoTool(MaiWanCredentialsProvider credentials, std::string caBundle,
                    MaiCreativeMediaUploadProvider uploadMedia)
        : mCredentials(std::move(credentials)),
          mCaBundle(std::move(caBundle)),
          mUploadMedia(std::move(uploadMedia)) {}

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
        const auto mediaStatus = !configured ? MaiSpecialistCapabilityStatus::NotConfigured
                                 : mUploadMedia
                                     ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                     : MaiSpecialistCapabilityStatus::UploadNotConfigured;
        return MaiSpecialistInfo{
            name(),
            kWanVideoModel,
            configured,
            // text_to_video：从文字生成 2–30 秒；输入视频和输出时长相加不能超过 30 秒。
            {{"text_to_video", true, true, status,
              "mode=create creates 2-30 seconds from text, or duration=-1 for smart duration. "
              "Output is 480P/720P/1080P MP4 at 30 fps; audio can be disabled"},
             // first_frame_to_video：接受一张本地首帧图。
             {"first_frame_to_video", true, true, mediaStatus,
              "mode=create accepts exactly one first_frame_path uploaded to private OSS; "
              "the input frame controls the opening image"},
             // first_last_frame_to_video：接受本地首帧和尾帧两张图。
             {"first_last_frame_to_video", true, true, mediaStatus,
              "mode=create accepts first_frame_path plus last_frame_path; strict frame "
              "control cannot mix with arbitrary references, documents, or web links"},
             // reference_video_edit：上传一个本地 MP4/MOV，作为 Video 1 编辑或续写。
             {"reference_video_edit", true, true, mediaStatus,
              "mode=edit or extend accepts one local MP4/MOV video_path, uploaded to private "
              "OSS, or the previous completed output through revise. The sum of input and "
              "output video durations must not exceed 30 seconds"},
             // reference_images：最多十张本地参考图，超出需先处理中间输入。
             {"reference_images", true, true, mediaStatus,
              "mode=reference accepts up to ten local_image_paths through private OSS, "
              "optionally with one reference video; not with strict first/last frames"},
             // reference_file：单个文档可用 HTTPS 外链；本地文件须先直传私有 OSS。
             {"reference_file", true, true, status,
              "One public HTTPS file_url, or one local file_path if private OSS upload is "
              "configured. file and link are mutually exclusive; prompt_extend must be true. "
              "Local documents are capped at 100 MB; provider page limits still apply"},
             // reference_link：单个公开网页 URL 与文件二选一，可与参考图/视频组合。
             {"reference_link", true, true, status,
              "One public HTTPS link_url in create/reference mode; cannot mix with a "
              "document or strict first/last frames, and prompt_extend must be true"},
             // 官方支持音频参考，但当前工具没有 reference_audio 参数及上传链路。
             {"reference_audio", true, true, MaiSpecialistCapabilityStatus::NotImplemented,
              "Wan3 API accepts reference audio, but this tool has no audio input parameter"},
             // multiple_reference_videos：目前只接一个视频参考，多视频未实现。
             {"multiple_reference_videos", true, true,
              MaiSpecialistCapabilityStatus::NotImplemented,
              "Wan3 API allows several reference videos; this tool wires only one video_path"},
             // 百炼通用任务接口只能取消排队任务；本地记录删除不属于模型/API 能力。
             {"cloud_task_cancel", false, true, status,
              "cancel works only while PENDING; a RUNNING cloud job cannot be canceled"},
             // cloud_task_delete：百炼没有当前视频任务的云端删除接口，
             // 即使本地可隐藏记录，也不能向用户声称云端任务已经删除。
             {"cloud_task_delete", false, false, MaiSpecialistCapabilityStatus::NotImplemented,
              "Model Studio has no cloud video-task deletion API in this adapter"},
             // local_record_delete：仅删除已交付任务的 App 索引，不删百炼任务和 MP4。
             {"local_record_delete", false, false,
              MaiSpecialistCapabilityStatus::ImplementedUnverified,
              "delete removes only an already delivered local record; cloud output and saved "
              "MP4 remain"}}};
    }

    std::string description() const override {
        // Wan3.0 英文描述的对应含义：输出时长 2–30 秒或 -1 智能决定，
        // 输入视频与输出时长总计不能超过 30 秒；支持 480P/720P/1080P 与声音开关。
        // discover 查能力，diagnose 只读校验访问权限。delegate 可用文本或首尾帧
        // 创建，也能在 edit/extend 模式传一个 video_path；reference 可把一个
        // 本地视频与最多十张图结合。供应商支持的多视频/参考音频目前没接入工具。
        // file_path 走私有 OSS，file_url 是公开文档外链；link_url 是公开网页。
        // App 自动查询并报告结果，continue 只用于手动排障；revise 在同一会话
        // 对完成任务追加反馈。cancel 只取消 PENDING，delete 只移除已交付本地记录。
        // 云端仍可能审核拒绝，不能把已提交当作已完成。
        return "Wan3.0 video through Beijing Model Studio. Call discover before choosing it; "
               "diagnose checks model visibility without generating. Output: 2-30 seconds or "
               "duration=-1, 480P/720P/1080P, adaptive or explicit ratio, optional audio. "
               "Modes: create=text or one strict first frame or first+last frame; "
               "reference=up to ten local images and optionally one video; edit=one source "
               "video; extend=one source video. A source video plus output may total at most "
               "30 seconds. One document file_path/file_url or one public link_url can guide "
               "create/reference; it requires prompt_extend=true and cannot mix with strict "
               "frames. All local image, video, and document inputs go through private OSS. "
               "The provider supports more videos and reference audio, but this tool does not. "
               "Delegate creates a paid job; continue only checks an existing job; revise "
               "edits the prior completed output in this AI conversation. Cancel works only "
               "while PENDING; delete removes only a delivered local task record, not the "
               "cloud job or downloaded MP4. Cloud moderation may still reject inputs.";
    }

    std::string parametersSchema() const override {
        // Wan3.0：mode 决定 create/edit/extend/reference；首尾帧和多图参考是两种
        // 不同的控制方式。video_path 最多一个，local_image_paths 最多十张。
        // file_path/file_url 互斥，link_url 与文件互斥；文档/网页不能混严格首尾帧。
        const std::string raw =
            R"({"type":"object","properties":{)"
            R"("action":{"type":"string","enum":["discover","diagnose","delegate","continue","cancel","delete","revise"]},)"
            R"("message":{"type":"string"},"context":{"type":"string"},)"
            R"("conversation_id":{"type":"string"},"mode":{"type":"string",)"
            R"("enum":["create","edit","extend","reference"]},)"
            R"("video_path":{"type":"string"},"file_path":{"type":"string"},)"
            R"("file_url":{"type":"string"},"link_url":{"type":"string"},)"
            R"("first_frame_path":{"type":"string"},)"
            R"("last_frame_path":{"type":"string"},"local_image_paths":{"type":"array",)"
            R"("maxItems":10,"items":{"type":"string"}},)"
            R"("resolution":{"type":"string","enum":["480P","720P","1080P"]},)"
            R"("ratio":{"type":"string","enum":["adaptive","21:9","16:9","4:3","1:1","3:4","9:16"]},)"
            R"("duration":{"type":"integer"},"audio":{"type":"boolean"},)"
            R"("prompt_extend":{"type":"boolean"},"watermark":{"type":"boolean"},)"
            R"("poll_once":{"type":"boolean"}},)"
            R"("required":["action"],"additionalProperties":false})";
        Json schema = Json::parse(raw);
        Json& fields = schema["properties"];
        // 以下字段描述会与 description 一起直接送到主模型；
        // 参数之间的互斥、素材可访问性及成本条件仍由 delegate 复核。
        // action：discover/diagnose 只读，delegate/revise 会新建付费任务，
        // continue 仅查询；cancel 只取消排队任务，delete 只清理本地已交付记录。
        fields["action"]["description"] =
            "discover/diagnose are read-only; delegate/revise are paid; continue is read-only; "
            "cancel stops only PENDING; delete removes only a delivered local record";
        // message 是本次视频目标或修改指令；context 只保留当前任务相关条件，
        // 不应把整段聊天历史原样塞入供应商提示词。
        fields["message"]["description"] = "Video creation or edit instruction for this task";
        fields["context"]["description"] =
            "Only relevant background and constraints; not the complete conversation";
        // mode：create 是文生或严格首尾帧；reference 是最多十图加可选单视频；
        // edit/extend 必须有且仅有一段源视频，不接受文档和网页。
        fields["mode"]["description"] =
            "create=text or strict first/last frames; reference=up to ten images plus at most "
            "one video; edit/extend=exactly one source video";
        // duration：输出可以明确选 2–30 秒，也可以用 -1 智能时长；
        // 一旦有视频输入，输入与输出相加不能超过 30 秒。
        fields["duration"]["description"] =
            "Output seconds 2-30, or -1 for smart duration; with a source video, input plus "
            "output duration must total at most 30 seconds";
        // resolution/ratio：输出只有 480P、720P、1080P 和列出的比例；
        // 编辑原视频希望保持构图时应考虑 adaptive，而不是擅自裁成固定画幅。
        fields["resolution"]["description"] = "Output 480P, 720P, or 1080P at 30 fps";
        fields["ratio"]["description"] =
            "Output ratio or adaptive; use adaptive with source-video editing when keeping "
            "the original framing";
        // first_frame_path 与 last_frame_path：本地图仅作 OSS 上传源；
        // 尾帧必须配首帧，严格首尾帧不能混任意参考图、文档或网页。
        fields["first_frame_path"]["description"] =
            "One local strict first frame uploaded to private OSS; create mode only";
        fields["last_frame_path"]["description"] =
            "One local strict last frame; requires first_frame_path and cannot mix with "
            "reference images, files, or links";
        // local_image_paths：最多十张参考图，与严格首尾帧互斥。
        // video_path：当前工具最多一个源/参考视频，不等于官方 API 的多视频上限。
        fields["local_image_paths"]["description"] =
            "At most ten local reference images uploaded to private OSS; cannot mix with "
            "strict first/last frames";
        fields["video_path"]["description"] =
            "At most one local MP4/MOV uploaded to private OSS as source/reference video";
        // file_path/file_url/link_url：本地文档经私有 OSS；file_url 为公网文档；
        // link_url 为公网网页。文档与网页二选一，并需要 prompt_extend=true。
        // 本地文档上限 100 MB，还要服从供应商页数限制。
        fields["file_path"]["description"] =
            "One local supported document (up to 100 MB) uploaded to private OSS; cannot mix "
            "with file_url or link_url and requires prompt_extend=true";
        fields["file_url"]["description"] =
            "One public HTTPS supported document URL; cannot mix with file_path or link_url";
        fields["link_url"]["description"] =
            "One public HTTPS web page; cannot mix with a document or strict frames";
        // audio 是生成视频时是否输出声音，不是参考音频输入；后者尚未接入。
        fields["audio"]["description"] =
            "Whether Wan generates audio; this is not a reference-audio input";
        // prompt_extend 是提示词扩展，文档/网页参考时必须为 true；
        // watermark 控制成片水印；poll_once 仅查询一次，正常交付由 App 后台完成。
        fields["prompt_extend"]["description"] =
            "Prompt extension; must be true when file_path, file_url, or link_url is used";
        fields["watermark"]["description"] = "Whether the generated video includes a watermark";
        fields["poll_once"]["description"] =
            "Query status once; the App normally polls and delivers the result automatically";
        // conversation_id 用于同一 AI 会话的继续、修订、取消与本地记录删除；
        // 工具会校验该任务归属，不能只凭供应商 task_id 执行管理动作。
        fields["conversation_id"]["description"] =
            "Saved spt_ task ID for continue, revise, cancel, or delete in this AI conversation";
        return schema.dump();
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        // Wan3.0 按 mode 选择创建、编辑、延长或参考；图片/视频都在执行层校验
        // 数量和可访问路径，云端受理后保存任务 ID 等待异步交付。
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object()) return invalid("arguments must be an object");
        if (auto error = rejectUnknownFields(args, parametersSchema())) return *error;
        if (args.contains("poll_once") && !args["poll_once"].is_boolean())
            return invalid("poll_once must be boolean");
        for (const char* field : {"action", "message", "context", "conversation_id", "mode",
                                  "video_path", "file_path", "file_url", "link_url",
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
                         {"model_support", capability.modelSupported},
                         {"api_support", capability.apiSupported},
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
        if (action == "delete")
            return manageVideoTask(args, name().c_str(), credentials, mCaBundle, context);
        if (!hasCredentials(credentials))
            return failure(MaiErrorCode::NotConfigured, "not_configured",
                           "Configure a Model Studio API key and Workspace ID");
        if (action == "diagnose")
            return diagnoseModel(credentials, kWanVideoModel, mCaBundle, context);
        if (action == "delegate") return delegate(args, credentials, context);
        if (action == "continue") return continueTask(args, credentials, context);
        if (action == "cancel")
            return manageVideoTask(args, name().c_str(), credentials, mCaBundle, context);
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
        return invalid(
            "action must be discover, diagnose, delegate, continue, cancel, delete, or revise");
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
        const std::string filePath = value(args, "file_path");
        const std::string fileUrl = value(args, "file_url");
        const std::string linkUrl = value(args, "link_url");
        const std::string first = value(args, "first_frame_path");
        const std::string last = value(args, "last_frame_path");
        const bool hasFile = !filePath.empty() || !fileUrl.empty();
        if ((!filePath.empty() && !fileUrl.empty()) || (hasFile && !linkUrl.empty()))
            return invalid("Choose one document path or URL, or one web link");
        if ((!first.empty() || !last.empty()) && (hasFile || !linkUrl.empty()))
            return invalid("Documents and web links cannot mix with strict first/last frames");
        if ((mode == "edit" || mode == "extend") && (hasFile || !linkUrl.empty()))
            return invalid("Document and web-link references require create or reference mode");
        if (!fileUrl.empty() && !supportedWanDocumentUrl(fileUrl))
            return invalid("file_url must be HTTPS and end in a supported document extension");
        if (!linkUrl.empty() && !httpsHost(linkUrl, ""))
            return invalid("link_url must be a public HTTPS web page");
        if (!videoPath.empty() && !sourceUrl.empty())
            return invalid("Provide one video source, not two");
        if ((mode == "create" && (!videoPath.empty() || !sourceUrl.empty())) ||
            ((mode == "edit" || mode == "extend") && videoPath.empty() && sourceUrl.empty()))
            return invalid("Edit and extend require one video; create cannot use video_path");
        if (!last.empty() && first.empty())
            return invalid("last_frame_path requires first_frame_path");
        if ((!first.empty() || !last.empty()) && mode != "create")
            return invalid("First/last frames are only valid for create mode");
        if (args.contains("local_image_paths") && !args["local_image_paths"].is_array())
            return invalid("local_image_paths must be an array");
        const Json imagePaths = args.value("local_image_paths", Json::array());
        if (imagePaths.size() > 10 || (!first.empty() && !imagePaths.empty()))
            return invalid("Use up to ten references or first/last frames, not both");
        if (mode == "reference" && videoPath.empty() && sourceUrl.empty() && imagePaths.empty() &&
            !hasFile && linkUrl.empty())
            return invalid("Reference mode requires an image, video, document, or web link");
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
        if (hasFile) {
            std::string url = fileUrl;
            if (!filePath.empty()) {
                if (auto error = uploadOssDocument(filePath, context, mUploadMedia, url))
                    return *error;
            }
            media.push_back(Json{{"type", "file"}, {"url", url}});
        }
        if (!linkUrl.empty()) media.push_back(Json{{"type", "link"}, {"url", linkUrl}});
        if (!first.empty()) {
            std::string image;
            if (auto error =
                    uploadOssMedia(first, context, mUploadMedia, kMaxImageBytes, false, image))
                return *error;
            media.push_back(Json{{"type", "first_frame"}, {"url", image}});
        }
        if (!last.empty()) {
            std::string image;
            if (auto error =
                    uploadOssMedia(last, context, mUploadMedia, kMaxImageBytes, false, image))
                return *error;
            media.push_back(Json{{"type", "last_frame"}, {"url", image}});
        }
        for (const Json& item : imagePaths) {
            if (!item.is_string()) return invalid("Reference image path must be a string");
            std::string image;
            if (auto error = uploadOssMedia(item.get<std::string>(), context, mUploadMedia,
                                            kMaxImageBytes, false, image))
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
            if (auto error = uploadOssMedia(videoPath, context, mUploadMedia, kMaxVideoBytes, true,
                                            videoUrl))
                return *error;
        }
        if (!videoUrl.empty())
            media.push_back(Json{{"type", "reference_video"}, {"url", videoUrl}});
        Json input = {{"prompt", prompt}};
        if (!media.empty()) input["media"] = std::move(media);
        const bool promptExtend = args.value("prompt_extend", true);
        if ((hasFile || !linkUrl.empty()) && !promptExtend)
            return invalid("prompt_extend must be true for document or web-link input");
        const Json body = {{"model", kWanVideoModel},
                           {"input", input},
                           {"parameters",
                            {{"resolution", resolution},
                             {"ratio", ratio},
                             {"duration", duration},
                             {"audio", args.value("audio", true)},
                             {"prompt_extend", promptExtend},
                             {"watermark", args.value("watermark", false)}}}};
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
        if (status == "CANCELED")
            return MaiToolResult::success(Json{{"conversation_id", conversationId},
                                               {"task_id", taskId},
                                               {"status", status},
                                               {"reply", "The cloud video task was canceled."}}
                                              .dump());
        if (status == "FAILED" || status == "UNKNOWN")
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
            downloadMedia(value(output, "video_url"), relative, mCaBundle, context, false);
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
    MaiCreativeMediaUploadProvider mUploadMedia;
};

class MaiBailianKlingVideoTool final : public MaiTool {
public:
    MaiBailianKlingVideoTool(MaiWanCredentialsProvider credentials, std::string caBundle,
                             MaiCreativeMediaUploadProvider uploadMedia)
        : mCredentials(std::move(credentials)),
          mCaBundle(std::move(caBundle)),
          mUploadMedia(std::move(uploadMedia)) {}

    std::string name() const override {
        return "kling_bailian_video";
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
        const auto ready = configured ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                      : MaiSpecialistCapabilityStatus::NotConfigured;
        const auto media = !configured    ? MaiSpecialistCapabilityStatus::NotConfigured
                           : mUploadMedia ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                          : MaiSpecialistCapabilityStatus::UploadNotConfigured;
        return MaiSpecialistInfo{
            name(),
            kBailianKlingTurboModel,
            configured,
            // 百炼可灵仅北京地域；Key 与万相可共用，但该模型仍须单独开通。
            {{"text_to_video", true, true, ready,
              "Turbo/Standard/Omni each create 3-15 second MP4. Turbo supports std(720P) and "
              "pro(1080P); Standard/Omni additionally support 4k. Ratio is 16:9, 9:16, or "
              "1:1. Model access must be activated in Beijing Model Studio"},
             {"first_frame_to_video", true, true, media,
              "mode=create accepts one JPEG/PNG image_path through private OSS with Turbo, "
              "Standard, or Omni"},
             {"first_last_frame_to_video", true, true, media,
              "mode=create accepts image_path plus last_frame_path with Standard or Omni; "
              "Turbo has no last-frame input"},
             {"reference_video", true, true, media,
              "Omni mode=reference accepts one 3-15.5 second MP4/MOV feature video. The "
              "output with a feature video is limited to 3-10 seconds; at most four reference "
              "images may accompany it"},
             {"video_edit", true, true, media,
              "Omni mode=edit accepts one 3-15.5 second MP4/MOV base video and up to four "
              "reference images. The output follows source duration; this tool does not "
              "expose keep_original_sound"},
             {"reference_images", true, true, media,
              "Omni mode=reference accepts up to seven JPEG/PNG local_image_paths without "
              "video, or up to four with one feature video. This tool does not expose subject "
              "element_list"},
             // 可灵多镜头、主体元素和保留源声音均未进入当前参数 schema。
             {"advanced_omni_controls", true, true, MaiSpecialistCapabilityStatus::NotImplemented,
              "Provider multi-shot, element_list, and keep_original_sound controls are not "
              "exposed by this tool"},
             // 云端取消沿用百炼通用接口，只能处理 PENDING，RUNNING 无法取消。
             {"cloud_task_cancel", false, true, ready,
              "cancel works only for PENDING tasks; RUNNING jobs cannot be canceled"},
             // 供应商没有本适配器可用的云端删除；删除只清理本地已交付索引。
             {"cloud_task_delete", false, false, MaiSpecialistCapabilityStatus::NotImplemented,
              "Bailian has no cloud video-task deletion API in this adapter"},
             {"local_record_delete", false, false,
              MaiSpecialistCapabilityStatus::ImplementedUnverified,
              "delete removes only a delivered local record; cloud output and saved MP4 remain"}}};
    }
    std::string description() const override {
        // 可灵统一走阿里云百炼的北京地域接口，沿用万相凭据。
        // Turbo 只接文字/首帧且不支持 4k；标准版加首尾帧和 4k；Omni 加多图、
        // 单视频特征参考与源视频编辑。普通输出 3–15 秒；带特征视频时 3–10 秒。
        // 无视频时参考图最多七张，有视频时最多四张；尚未接主体元素、多镜头、
        // 原声保留控制。所有本地素材先直传私有 OSS，模型只接收短期 HTTPS 链接。
        // 付费前确认型号、时长、分辨率和音频；continue 查询旧任务，cancel 仅排队，
        // delete 只清理本地已交付记录，不重复付费提交。
        return "Kling V3 video via Beijing Model Studio, using Wan regional credentials after "
               "Kling access is activated. Call discover before selecting Turbo, Standard, or "
               "Omni; diagnose is a read-only access check. Turbo supports text or one first "
               "frame at std(720P)/pro(1080P); Standard adds first+last frames and 4k; Omni "
               "adds reference images, one feature video, and base-video editing. Pure "
               "generation lasts 3-15 seconds; feature-video reference is 3-10 seconds; edit "
               "follows source duration. Omni takes up to seven reference images without "
               "video, or four with one video. Local JPEG/PNG and MP4/MOV are uploaded to "
               "private OSS. The tool does not expose subject elements, multi-shot control, "
               "or keep_original_sound. Turbo always produces audio even if audio=false. "
               "Confirm model, duration, resolution, ratio, and audio before paid delegate. "
               "Continue checks an existing job; cancel works only while PENDING; delete "
               "removes only a delivered local record and keeps cloud output and saved MP4.";
    }
    std::string parametersSchema() const override {
        // task_mode 区分生成、特征参考与源视频编辑；model 是百炼模型 ID，不是直连 ID。
        // 本地路径只是 OSS 上传源，source_video_url 是用户已有的可访问 HTTPS 外链。
        // action=discover/diagnose 只读；delegate 会付费；continue 只查已有任务。
        // image_path/last_frame_path 是严格首尾帧，local_image_paths 为 Omni 的
        // 多张参考图；video_path/source_video_url 两者只能选一个，前者上传私有 OSS。
        // duration、resolution、ratio、audio、watermark 均影响产物或费用，必须在
        // 付费前逐项确认。conversation_id 只用于 continue，不是源视频地址。
        return describeKlingVideoSchema(
            R"({"type":"object","properties":{"action":{"type":"string","enum":["discover","diagnose","delegate","continue","cancel","delete"]},"model":{"type":"string","enum":["kling/kling-v3-turbo-video-generation","kling/kling-v3-video-generation","kling/kling-v3-omni-video-generation"]},"task_mode":{"type":"string","enum":["create","reference","edit"]},"message":{"type":"string"},"image_path":{"type":"string"},"last_frame_path":{"type":"string"},"local_image_paths":{"type":"array","maxItems":7,"items":{"type":"string"}},"video_path":{"type":"string"},"source_video_url":{"type":"string"},"duration":{"type":"integer"},"resolution":{"type":"string","enum":["std","pro","4k"]},"ratio":{"type":"string","enum":["16:9","9:16","1:1"]},"audio":{"type":"boolean"},"watermark":{"type":"boolean"},"conversation_id":{"type":"string"},"poll_once":{"type":"boolean"}},"required":["action"],"additionalProperties":false})");
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object()) return invalid("arguments must be an object");
        if (auto error = rejectUnknownFields(args, parametersSchema())) return *error;
        if (args.contains("poll_once") && !args["poll_once"].is_boolean())
            return invalid("poll_once must be boolean");
        const std::string action = value(args, "action");
        const MaiWanCredentials credentials = mCredentials ? mCredentials() : MaiWanCredentials{};
        if (action == "discover") {
            const MaiSpecialistInfo info = *specialistInfo();
            Json capabilities = Json::array();
            for (const auto& capability : info.capabilities)
                capabilities.push_back(
                    Json{{"id", capability.id},
                         {"model_support", capability.modelSupported},
                         {"api_support", capability.apiSupported},
                         {"tool_status", maiSpecialistCapabilityStatusToString(capability.status)},
                         {"limitation", capability.limitation}});
            return MaiToolResult::success(Json{{"tool_kind", "model_backed"},
                                               {"bound_model", info.modelId},
                                               {"configured", info.configured},
                                               {"capabilities", capabilities},
                                               {"reply", "Kling via Beijing Model Studio."}}
                                              .dump());
        }
        if (action == "delete")
            return manageVideoTask(args, name().c_str(), credentials, mCaBundle, context);
        if (!hasCredentials(credentials))
            return failure(MaiErrorCode::NotConfigured, "not_configured",
                           "Configure a Beijing Model Studio API Key and Workspace ID");
        if (action == "diagnose") {
            const std::string model =
                value(args, "model").empty() ? kBailianKlingTurboModel : value(args, "model");
            if (model != kBailianKlingTurboModel && model != kBailianKlingStandardModel &&
                model != kBailianKlingOmniModel)
                return invalid("Unsupported Bailian Kling model");
            return diagnoseModel(credentials, model.c_str(), mCaBundle, context);
        }
        if (action == "continue") return continueTask(args, credentials, context);
        if (action == "cancel")
            return manageVideoTask(args, name().c_str(), credentials, mCaBundle, context);
        if (action == "delegate") return delegate(args, credentials, context);
        return invalid("action must be discover, diagnose, delegate, continue, cancel, or delete");
    }

private:
    MaiToolResult delegate(const Json& args, const MaiWanCredentials& credentials,
                           const MaiToolContext& context) const {
        // 先做型号与素材组合校验、上传 OSS，最后才发异步付费请求。
        for (const char* field : {"model", "task_mode", "message", "image_path", "last_frame_path",
                                  "video_path", "source_video_url", "resolution", "ratio"}) {
            if (args.contains(field) && !args[field].is_string())
                return invalid(std::string(field) + " must be a string");
        }
        if (args.contains("local_image_paths") && !args["local_image_paths"].is_array())
            return invalid("local_image_paths must be an array");
        for (const char* field : {"audio", "watermark"}) {
            if (args.contains(field) && !args[field].is_boolean())
                return invalid(std::string(field) + " must be boolean");
        }
        if (!args.contains("duration") || !args["duration"].is_number_integer())
            return invalid("Confirm duration before paid Kling generation");
        const int duration = args["duration"].get<int>();
        if (duration < 3 || duration > 15) return invalid("Kling duration must be 3 to 15 seconds");
        const std::string resolution = value(args, "resolution");
        if (resolution != "std" && resolution != "pro" && resolution != "4k")
            return invalid("Confirm resolution as std, pro, or 4k");
        const std::string message = value(args, "message");
        if (message.empty() || message.size() > 2500)
            return invalid("message must contain 1 to 2500 bytes");
        const std::string first = value(args, "image_path");
        const std::string last = value(args, "last_frame_path");
        const std::string videoPath = value(args, "video_path");
        const std::string videoUrl = value(args, "source_video_url");
        const Json referencePaths = args.value("local_image_paths", Json::array());
        if (referencePaths.size() > 7)
            return invalid("Kling Omni accepts at most seven reference images");
        const std::string taskMode =
            value(args, "task_mode").empty() ? "create" : value(args, "task_mode");
        if (taskMode != "create" && taskMode != "reference" && taskMode != "edit")
            return invalid("task_mode must be create, reference, or edit");
        const std::string requestedModel = value(args, "model");
        const std::string model =
            requestedModel.empty()
                ? (taskMode != "create" || !referencePaths.empty() ? kBailianKlingOmniModel
                   : !last.empty()                                 ? kBailianKlingStandardModel
                                                                   : kBailianKlingTurboModel)
                : requestedModel;
        const bool turbo = model == kBailianKlingTurboModel;
        const bool standard = model == kBailianKlingStandardModel;
        const bool omni = model == kBailianKlingOmniModel;
        if (!turbo && !standard && !omni) return invalid("Unsupported Bailian Kling model");
        if (turbo && resolution == "4k") return invalid("Kling Turbo does not support 4k");
        if ((taskMode != "create" || !referencePaths.empty() || !videoPath.empty() ||
             !videoUrl.empty()) &&
            !omni)
            return invalid("Kling reference and edit require the Omni model");
        if (!last.empty() && (first.empty() || turbo || taskMode != "create"))
            return invalid("A last frame requires first frame and Standard or Omni create mode");
        if (!videoPath.empty() && !videoUrl.empty())
            return invalid("Provide video_path or source_video_url, not both");
        const bool hasVideo = !videoPath.empty() || !videoUrl.empty();
        if ((taskMode == "create" && (hasVideo || !referencePaths.empty())) ||
            (taskMode == "reference" && !hasVideo && referencePaths.empty()) ||
            (taskMode == "edit" && !hasVideo))
            return invalid("The selected Kling task_mode requires a different media combination");
        if (taskMode == "edit" && (!first.empty() || !last.empty()))
            return invalid("Kling edit does not combine with strict first or last frames");
        if (taskMode == "reference" && !last.empty())
            return invalid("Kling feature reference does not combine with a last frame");
        if (hasVideo && !first.empty() && !referencePaths.empty())
            return invalid("Kling video reference accepts first frame or reference images");
        if (hasVideo && referencePaths.size() > 4)
            return invalid("Kling video plus images accepts at most four references");
        if (taskMode == "reference" && hasVideo && duration > 10)
            return invalid("Kling feature-video output is limited to 3 to 10 seconds");
        if (hasVideo && args.value("audio", false))
            return invalid("Kling with source video requires audio=false");
        const std::string ratio = value(args, "ratio");
        const bool needsRatio =
            (taskMode == "reference" && first.empty()) || (taskMode == "create" && first.empty());
        if (needsRatio && ratio != "16:9" && ratio != "9:16" && ratio != "1:1")
            return invalid("Confirm ratio as 16:9, 9:16, or 1:1");
        Json media = Json::array();
        if (!first.empty()) {
            std::string url;
            if (auto error = uploadOssMedia(first, context, mUploadMedia, 10'000'000, false, url))
                return *error;
            media.push_back(Json{{"type", "first_frame"}, {"url", url}});
        }
        if (!last.empty()) {
            std::string url;
            if (auto error = uploadOssMedia(last, context, mUploadMedia, 10'000'000, false, url))
                return *error;
            media.push_back(Json{{"type", "last_frame"}, {"url", url}});
        }
        for (const Json& item : referencePaths) {
            if (!item.is_string()) return invalid("Reference image path must be a string");
            std::string url;
            if (auto error = uploadOssMedia(item.get<std::string>(), context, mUploadMedia,
                                            10'000'000, false, url))
                return *error;
            media.push_back(Json{{"type", "refer"}, {"url", url}});
        }
        if (hasVideo) {
            std::string url = videoUrl;
            if (!videoPath.empty()) {
                if (auto error =
                        uploadOssMedia(videoPath, context, mUploadMedia, 200'000'000, true, url))
                    return *error;
            }
            if (!httpsHost(url, "")) return invalid("source_video_url must be HTTPS");
            media.insert(media.begin(),
                         Json{{"type", taskMode == "edit" ? "base" : "feature"}, {"url", url}});
        }
        Json input = {{"prompt", message}};
        if (!media.empty()) input["media"] = std::move(media);
        Json parameters = {{"mode", resolution},
                           {"audio", args.value("audio", false)},
                           {"watermark", args.value("watermark", false)}};
        // 编辑任务由输入视频决定输出时长；这里的 duration 是付费前核对的
        // 源片四舍五入秒数，不作为 API 的输出时长参数传递。
        if (taskMode != "edit") parameters["duration"] = duration;
        if (needsRatio) parameters["aspect_ratio"] = ratio;
        const Json body = {{"model", model}, {"input", input}, {"parameters", parameters}};
        HttpResponse response = requestJson(
            modelStudioBase(credentials) + "/services/aigc/video-generation/video-synthesis",
            credentials.apiKey, mCaBundle, &body, true, false, context);
        if (response.error) {
            if (response.transferFailed)
                return failure(
                    MaiErrorCode::Network, "submission_unknown",
                    "Kling request may have been accepted. Do not resubmit automatically.");
            return *response.error;
        }
        const Json output = response.body.value("output", Json::object());
        const std::string taskId = value(output, "task_id");
        if (!validId(taskId))
            return failure(MaiErrorCode::Protocol, "submission_unknown",
                           "Kling returned no task ID; do not resubmit automatically");
        Json result = {{"task_id", taskId},
                       {"conversation_id", taskId},
                       {"status", value(output, "task_status")},
                       {"bound_model", model},
                       {"reply", "Kling video task submitted through Model Studio."}};
        if (context.specialistTasks != nullptr && !context.sessionId.empty()) {
            MaiSpecialistTask task;
            task.id = MaiIdGenerator::generate("spt_");
            task.ownerSessionId = context.sessionId;
            task.specialistName = name();
            task.providerTaskId = taskId;
            task.intent = message;
            task.inputReference =
                hasVideo ? (!videoPath.empty() ? videoPath : "external_video") : first;
            task.created = MaiTime::getCurrentTime();
            const MaiError stored = context.specialistTasks->insertSpecialistTask(task);
            if (stored)
                result["context_persistence_warning"] = stored.message();
            else {
                result["conversation_id"] = task.id;
                result["specialist_task_id"] = task.id;
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
            MaiSpecialistTask task;
            if (context.specialistTasks == nullptr ||
                !context.specialistTasks->getSpecialistTask(conversationId, context.sessionId,
                                                            task) ||
                task.specialistName != name())
                return failure(MaiErrorCode::NotFound, "not_found",
                               "Kling task was not found in this AI conversation");
            taskId = task.providerTaskId;
        }
        if (!validId(taskId)) return invalid("Invalid provider task ID");
        HttpResponse response =
            requestJson(modelStudioBase(credentials) + "/tasks/" + taskId, credentials.apiKey,
                        mCaBundle, nullptr, false, false, context);
        if (response.error) return *response.error;
        const Json output = response.body.value("output", Json::object());
        const std::string status = value(output, "task_status");
        if (status == "CANCELED")
            return MaiToolResult::success(Json{{"conversation_id", conversationId},
                                               {"task_id", taskId},
                                               {"status", status},
                                               {"reply", "The cloud video task was canceled."}}
                                              .dump());
        if (status == "FAILED" || status == "UNKNOWN")
            return failure(MaiErrorCode::Network, "task_failed",
                           value(output, "message").empty() ? "Kling task ended with " + status
                                                            : value(output, "message"));
        if (status == "PENDING" || status == "RUNNING")
            return MaiToolResult::success(Json{{"conversation_id", conversationId},
                                               {"task_id", taskId},
                                               {"status", status},
                                               {"reply", "Kling video is still in progress."}}
                                              .dump());
        if (status != "SUCCEEDED")
            return failure(MaiErrorCode::Protocol, "protocol", "Unknown Kling task status");
        const std::string relative = "kling-bailian-" + taskId + ".mp4";
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
                                               {"reply", "Kling video saved locally."}}
                                              .dump());
        MaiToolResult downloaded =
            downloadMedia(value(output, "video_url"), relative, mCaBundle, context, false);
        if (downloaded.hasError()) return downloaded;
        Json result = Json::parse(downloaded.output());
        result.update(Json{{"conversation_id", conversationId},
                           {"task_id", taskId},
                           {"status", status},
                           {"reply", "Kling video saved locally."}});
        return MaiToolResult::success(result.dump());
    }

    MaiWanCredentialsProvider mCredentials;
    std::string mCaBundle;
    MaiCreativeMediaUploadProvider mUploadMedia;
};

class MaiWanImageTool final : public MaiTool {
public:
    MaiWanImageTool(MaiWanCredentialsProvider credentials, std::string caBundle,
                    MaiCreativeMediaUploadProvider uploadMedia)
        : mCredentials(std::move(credentials)),
          mCaBundle(std::move(caBundle)),
          mUploadMedia(std::move(uploadMedia)) {}

    std::string name() const override {
        return "wan_image";
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
        const auto mediaStatus = !configured ? MaiSpecialistCapabilityStatus::NotConfigured
                                 : mUploadMedia
                                     ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                     : MaiSpecialistCapabilityStatus::UploadNotConfigured;
        return MaiSpecialistInfo{
            name(),
            kWanImageModel,
            configured,
            // text_to_image：文字目标生成一张 PNG。
            {{"text_to_image", true, true, status, "Creates one PNG from a text goal"},
             // image_edit：编辑一到九张本地 JPEG/PNG 图片，上传后只传 OSS URL。
             {"image_edit", true, true, mediaStatus,
              "Edits one to nine local JPEG or PNG inputs via private OSS URLs"},
             // image_revision：同一 AI 会话中沿用上一版 PNG，根据新反馈修订。
             {"image_revision", true, true, mediaStatus,
              "Reuses the previous PNG in the same AI conversation"}}};
    }

    std::string description() const override {
        // Wan2.7-Image 英文描述的对应含义：discover 查能力，diagnose 只读
        // 检查访问权限。delegate 异步提交文生图或 1–9 张图片编辑，continue 查询并下载。
        // revise 需同一会话的 conversation_id 和新反馈。输入不变，新 PNG 存在
        // Agent 工作区，最后用 agent_send_media 交付。
        return "Wan2.7-Image model-backed image specialist. Use discover for capabilities "
               "and diagnose for a read-only model access check. Delegate a text goal with no "
               "input for a new PNG, or pass one to "
               "nine local image_paths to edit or combine images. Continue the async task with "
               "its conversation_id; revise a previous result with new feedback. Input files "
               "are unchanged; the new PNG is saved in the Agent workspace. Use "
               "agent_send_media to deliver it.";
    }

    std::string parametersSchema() const override {
        // 万相图像：无 image_paths 时文生图，有 1–9 张时编辑或融合；
        // conversation_id 用于同一会话的 revise，不是随意引用别人的旧任务。
        return R"({"type":"object","properties":{)"
               R"("action":{"type":"string","enum":["discover","diagnose","delegate","continue","revise"]},)"
               R"("message":{"type":"string"},"context":{"type":"string"},)"
               R"("conversation_id":{"type":"string"},"image_path":{"type":"string"},)"
               R"("image_paths":{"type":"array","maxItems":9,"items":{"type":"string"}},)"
               R"("size":{"type":"string"},)"
               R"("watermark":{"type":"boolean"},"poll_once":{"type":"boolean"}},)"
               R"("required":["action"],)"
               R"("additionalProperties":false})";
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        // 万相不带图时文生图，带图时编辑/融合；修订必须引用当前会话已有产物。
        // 新 PNG 成功发布后才向主模型回报完成，原输入保留。
        Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object()) return invalid("arguments must be an object");
        if (auto error = rejectUnknownFields(args, parametersSchema())) return *error;
        if (args.contains("poll_once") && !args["poll_once"].is_boolean())
            return invalid("poll_once must be boolean");
        for (const char* field :
             {"action", "message", "context", "conversation_id", "image_path", "size"}) {
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
                         {"model_support", capability.modelSupported},
                         {"api_support", capability.apiSupported},
                         {"tool_status", maiSpecialistCapabilityStatusToString(capability.status)},
                         {"limitation", capability.limitation}});
            Json missing = Json::array();
            if (credentials.apiKey.empty()) missing.push_back("api_key");
            if (!validWorkspaceId(credentials.workspaceId)) missing.push_back("workspace_id");
            return MaiToolResult::success(
                Json{{"tool_kind", "model_backed"},
                     {"bound_model", kWanImageModel},
                     {"configured", info.configured},
                     {"missing_configuration", missing},
                     {"capabilities", capabilities},
                     {"reply", "I create or edit PNG images with Wan2.7-Image."}}
                    .dump());
        }
        if (!hasCredentials(credentials))
            return failure(MaiErrorCode::NotConfigured, "not_configured",
                           "Configure a Model Studio API key and Workspace ID");
        if (action == "diagnose")
            return diagnoseModel(credentials, kWanImageModel, mCaBundle, context);
        if (action == "continue") return continueTask(args, credentials, context);
        if (action == "revise") {
            MaiSpecialistTask previous;
            const std::string id = value(args, "conversation_id");
            if (context.specialistTasks == nullptr ||
                !context.specialistTasks->getSpecialistTask(id, context.sessionId, previous) ||
                previous.specialistName != name() || previous.outputPath.empty())
                return failure(MaiErrorCode::NotFound, "not_found",
                               "Previous Wan image was not found in this AI conversation");
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
        return invalid("action must be discover, diagnose, delegate, continue, or revise");
    }

private:
    MaiToolResult delegate(const Json& args, const MaiWanCredentials& credentials,
                           const MaiToolContext& context) const {
        const std::string message = value(args, "message");
        const std::string extra = value(args, "context");
        std::string prompt = message;
        if (!extra.empty()) prompt += "\nRelevant context: " + extra;
        if (prompt.size() > 5000) return invalid("Wan image prompt exceeds 5000 bytes");
        if (args.contains("watermark") && !args["watermark"].is_boolean())
            return invalid("watermark must be boolean");
        const std::string singleImage = value(args, "image_path");
        if (args.contains("image_paths") && !args["image_paths"].is_array())
            return invalid("image_paths must be an array");
        const Json imagePaths = args.value("image_paths", Json::array());
        if ((!singleImage.empty() && !imagePaths.empty()) || imagePaths.size() > 9)
            return invalid("Provide one image_path or up to nine image_paths");
        Json images = Json::array();
        if (!singleImage.empty()) {
            std::string image;
            if (auto error =
                    uploadOssMedia(singleImage, context, mUploadMedia, 20'000'000, false, image))
                return *error;
            images.push_back(image);
        }
        for (const Json& item : imagePaths) {
            if (!item.is_string()) return invalid("Each image path must be a string");
            std::string image;
            if (auto error = uploadOssMedia(item.get<std::string>(), context, mUploadMedia,
                                            20'000'000, false, image))
                return *error;
            images.push_back(image);
        }
        std::string size = value(args, "size");
        if (size.empty()) size = "2K";
        if (size != "1K" && size != "2K") {
            const std::size_t separator = size.find_first_of("x*");
            if (separator == std::string::npos || separator == 0 || separator > 5 ||
                separator + 1 >= size.size() || size.size() - separator - 1 > 5)
                return invalid("size must be 1K, 2K, or WIDTH*HEIGHT");
            const auto digits = [](unsigned char character) {
                return std::isdigit(character) != 0;
            };
            if (!std::all_of(size.begin(), size.begin() + static_cast<std::ptrdiff_t>(separator),
                             digits) ||
                !std::all_of(size.begin() + static_cast<std::ptrdiff_t>(separator + 1), size.end(),
                             digits))
                return invalid("size must be 1K, 2K, or WIDTH*HEIGHT");
            const int width = std::stoi(size.substr(0, separator));
            const int height = std::stoi(size.substr(separator + 1));
            const std::int64_t area = static_cast<std::int64_t>(width) * height;
            if (width < 1 || height < 1 || area < 768LL * 768 || area > 2048LL * 2048 ||
                width > height * 8 || height > width * 8)
                return invalid("size is outside Wan2.7-Image limits");
            size[separator] = '*';
        }
        Json content = Json::array({Json{{"text", prompt}}});
        for (const Json& image : images) content.push_back(Json{{"image", image}});
        const Json body = {
            {"model", kWanImageModel},
            {"input", {{"messages", Json::array({Json{{"role", "user"}, {"content", content}}})}}},
            {"parameters",
             {{"size", size}, {"n", 1}, {"watermark", args.value("watermark", false)}}}};
        HttpResponse response =
            requestJson(modelStudioBase(credentials) + "/services/aigc/image-generation/generation",
                        credentials.apiKey, mCaBundle, &body, true, false, context);
        if (response.error) {
            if (response.transferFailed)
                return failure(MaiErrorCode::Network, "submission_unknown",
                               "Wan image request may have completed. Do not retry automatically.");
            return *response.error;
        }
        const Json output = response.body.value("output", Json::object());
        const std::string taskId = value(output, "task_id");
        if (!validId(taskId))
            return failure(MaiErrorCode::Protocol, "submission_unknown",
                           "Wan image returned no task ID; do not resubmit automatically");
        Json result = {{"task_id", taskId},
                       {"conversation_id", taskId},
                       {"status", value(output, "task_status")},
                       {"bound_model", kWanImageModel},
                       {"reply", "Wan image task submitted. The app will report its result."}};
        if (context.specialistTasks != nullptr && !context.sessionId.empty()) {
            MaiSpecialistTask task;
            task.id = MaiIdGenerator::generate("spt_");
            task.ownerSessionId = context.sessionId;
            task.specialistName = name();
            task.parentTaskId = value(args, "parent_task_id");
            task.providerTaskId = taskId;
            task.intent = message;
            task.contextSummary = extra;
            task.inputReference = singleImage.empty() ? "text_or_multiple_images" : singleImage;
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
            MaiSpecialistTask task;
            if (context.specialistTasks == nullptr ||
                !context.specialistTasks->getSpecialistTask(conversationId, context.sessionId,
                                                            task) ||
                task.specialistName != name())
                return failure(MaiErrorCode::NotFound, "not_found",
                               "Wan image task was not found in this AI conversation");
            taskId = task.providerTaskId;
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
                           value(output, "message").empty() ? "Wan image task ended with " + status
                                                            : value(output, "message"));
        if (status == "PENDING" || status == "RUNNING")
            return MaiToolResult::success(Json{{"conversation_id", conversationId},
                                               {"task_id", taskId},
                                               {"status", status},
                                               {"reply", "Wan image is still in progress."}}
                                              .dump());
        if (status != "SUCCEEDED")
            return failure(MaiErrorCode::Protocol, "protocol", "Unknown Wan image task status");
        const std::string relative = "wan-image-" + taskId + ".png";
        if (context.root.empty() || maiResolvePathWithinRoot(context.root, relative).empty())
            return invalid("An Agent workspace is required to save the image");
        const std::string path = maiResolvePathWithinRoot(context.root, relative);
        std::uint64_t size = 0;
        if (MaiFileSystem::fileSize(MaiFilePath::fromUtf8(path), size) && size > 0)
            return MaiToolResult::success(Json{{"conversation_id", conversationId},
                                               {"task_id", taskId},
                                               {"status", status},
                                               {"path", path},
                                               {"bytes", size},
                                               {"mime_type", "image/png"},
                                               {"reply", "Wan image saved locally."}}
                                              .dump());
        const std::string url = firstImageUrl(output);
        if (url.empty())
            return failure(MaiErrorCode::Protocol, "media_download_failed",
                           "Wan image task has no result URL yet");
        MaiToolResult downloaded = downloadMedia(url, relative, mCaBundle, context, true);
        if (downloaded.hasError()) return downloaded;
        Json result = Json::parse(downloaded.output());
        result.update(Json{{"conversation_id", conversationId},
                           {"task_id", taskId},
                           {"status", status},
                           {"reply", "Wan image saved locally."}});
        return MaiToolResult::success(result.dump());
    }

    MaiWanCredentialsProvider mCredentials;
    std::string mCaBundle;
    MaiCreativeMediaUploadProvider mUploadMedia;
};

class MaiBailianKlingImageTool final : public MaiTool {
public:
    MaiBailianKlingImageTool(MaiWanCredentialsProvider credentials, std::string caBundle,
                             MaiCreativeMediaUploadProvider uploadMedia)
        : mCredentials(std::move(credentials)),
          mCaBundle(std::move(caBundle)),
          mUploadMedia(std::move(uploadMedia)) {}

    std::string name() const override {
        return "kling_bailian_image";
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
        const auto ready = configured ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                      : MaiSpecialistCapabilityStatus::NotConfigured;
        const auto mediaReady = !configured ? MaiSpecialistCapabilityStatus::NotConfigured
                                : mUploadMedia
                                    ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                    : MaiSpecialistCapabilityStatus::UploadNotConfigured;
        return MaiSpecialistInfo{
            name(),
            kBailianKlingImageModel,
            configured,
            // 标准版可文生图或单图参考；Omni 可使用最多十张本地参考图。
            {{"text_to_image", true, true, ready,
              "Bailian Kling V3 or Omni; one PNG output at 1k/2k, Omni also supports 4k"},
             {"single_image_reference", true, true, mediaReady,
              "One JPEG/PNG reference via private OSS, maximum 10 MB"},
             {"multi_image_reference", true, true, mediaReady,
              "Omni accepts up to ten JPEG/PNG references via private OSS"},
             {"storyboard_series", true, true, MaiSpecialistCapabilityStatus::NotImplemented,
              "This tool currently produces one PNG; series_amount is not exposed"}}};
    }
    std::string description() const override {
        // 百炼可灵图片异步提交、自动查询、下载；仅保留一次一张 PNG 的付费边界。
        // 普通 V3 支持文生/单图，Omni 接多图；本地素材先上传 OSS，不传 Base64。
        return "Kling image generation through Beijing Alibaba Model Studio, using the Wan "
               "regional API Key and Workspace. Use discover, read-only diagnose, paid delegate, "
               "or continue an existing task. Standard V3 accepts text or one image; Omni "
               "accepts up to ten images. Local JPEG/PNG references are uploaded to private OSS, "
               "never embedded as Base64. Confirm model, 1k/2k/4k resolution, and ratio before "
               "paid submission. This tool generates one PNG, not a storyboard series. Deliver "
               "the completed image with agent_send_media.";
    }
    std::string parametersSchema() const override {
        // discover/diagnose 不扣费；delegate 提交异步付费任务；continue 仅查旧任务。
        // image_path 单图、image_paths 多图，均只作私有 OSS 上传源；模型看到 URL。
        // model 可选 V3 或 Omni；Omni 才支持多图与 4k，输出固定一张 PNG。
        return R"({"type":"object","properties":{"action":{"type":"string","enum":["discover","diagnose","delegate","continue"]},"model":{"type":"string","enum":["kling/kling-v3-image-generation","kling/kling-v3-omni-image-generation"]},"message":{"type":"string"},"context":{"type":"string"},"image_path":{"type":"string"},"image_paths":{"type":"array","maxItems":10,"items":{"type":"string"}},"ratio":{"type":"string","enum":["16:9","9:16","1:1"]},"resolution":{"type":"string","enum":["1k","2k","4k"]},"watermark":{"type":"boolean"},"conversation_id":{"type":"string"},"poll_once":{"type":"boolean"}},"required":["action"],"additionalProperties":false})";
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object()) return invalid("arguments must be an object");
        if (auto error = rejectUnknownFields(args, parametersSchema())) return *error;
        for (const char* field : {"action", "model", "message", "context", "image_path", "ratio",
                                  "resolution", "conversation_id"}) {
            if (args.contains(field) && !args[field].is_string())
                return invalid(std::string(field) + " must be a string");
        }
        if (args.contains("poll_once") && !args["poll_once"].is_boolean())
            return invalid("poll_once must be boolean");
        const std::string action = value(args, "action");
        const MaiWanCredentials credentials = mCredentials ? mCredentials() : MaiWanCredentials{};
        if (action == "discover") {
            const MaiSpecialistInfo info = *specialistInfo();
            Json capabilities = Json::array();
            for (const auto& capability : info.capabilities)
                capabilities.push_back(
                    Json{{"id", capability.id},
                         {"model_support", capability.modelSupported},
                         {"api_support", capability.apiSupported},
                         {"tool_status", maiSpecialistCapabilityStatusToString(capability.status)},
                         {"limitation", capability.limitation}});
            return MaiToolResult::success(
                Json{{"tool_kind", "model_backed"},
                     {"bound_model", info.modelId},
                     {"configured", info.configured},
                     {"capabilities", capabilities},
                     {"reply", "Kling image models are available through Bailian."}}
                    .dump());
        }
        if (!hasCredentials(credentials))
            return failure(MaiErrorCode::NotConfigured, "not_configured",
                           "Configure a Beijing Model Studio API Key and Workspace ID");
        if (action == "diagnose") {
            const std::string model =
                value(args, "model").empty() ? kBailianKlingImageModel : value(args, "model");
            if (model != kBailianKlingImageModel && model != kBailianKlingOmniImageModel)
                return invalid("Unsupported Bailian Kling image model");
            return diagnoseModel(credentials, model.c_str(), mCaBundle, context);
        }
        if (action == "delegate") return delegate(args, credentials, context);
        if (action == "continue") return continueTask(args, credentials, context);
        return invalid("action must be discover, diagnose, delegate, or continue");
    }

private:
    MaiToolResult delegate(const Json& args, const MaiWanCredentials& credentials,
                           const MaiToolContext& context) const {
        const std::string message = value(args, "message");
        const std::string extra = value(args, "context");
        std::string prompt = message;
        if (!extra.empty()) prompt += "\nRelevant context: " + extra;
        if (prompt.empty() || prompt.size() > 2500)
            return invalid("Kling image prompt must contain 1 to 2500 bytes");
        const std::string single = value(args, "image_path");
        if (args.contains("image_paths") && !args["image_paths"].is_array())
            return invalid("image_paths must be an array");
        const Json imagePaths = args.value("image_paths", Json::array());
        if ((!single.empty() && !imagePaths.empty()) || imagePaths.size() > 10)
            return invalid("Use one image_path or up to ten image_paths");
        const std::size_t imageCount =
            imagePaths.size() + static_cast<std::size_t>(!single.empty());
        const std::string requestedModel = value(args, "model");
        const std::string model =
            requestedModel.empty()
                ? imageCount > 1 ? kBailianKlingOmniImageModel : kBailianKlingImageModel
                : requestedModel;
        if (model != kBailianKlingImageModel && model != kBailianKlingOmniImageModel)
            return invalid("Unsupported Bailian Kling image model");
        if (imageCount > 1 && model != kBailianKlingOmniImageModel)
            return invalid("Multiple reference images require Kling Omni");
        const std::string ratio = value(args, "ratio");
        if (ratio != "16:9" && ratio != "9:16" && ratio != "1:1")
            return invalid("Confirm Kling image ratio as 16:9, 9:16, or 1:1");
        const std::string resolution = value(args, "resolution");
        if (resolution != "1k" && resolution != "2k" &&
            (resolution != "4k" || model != kBailianKlingOmniImageModel))
            return invalid("Confirm supported Kling image resolution and model");
        if (args.contains("watermark") && !args["watermark"].is_boolean())
            return invalid("watermark must be boolean");
        Json content = Json::array({Json{{"text", prompt}}});
        const auto appendImage = [&](const std::string& candidate) -> std::optional<MaiToolResult> {
            std::string url;
            if (auto error =
                    uploadOssMedia(candidate, context, mUploadMedia, 10'000'000, false, url))
                return error;
            content.push_back(Json{{"image", url}});
            return std::nullopt;
        };
        if (!single.empty()) {
            if (auto error = appendImage(single)) return *error;
        }
        for (const Json& item : imagePaths) {
            if (!item.is_string()) return invalid("Each image path must be a string");
            if (auto error = appendImage(item.get<std::string>())) return *error;
        }
        const Json body = {
            {"model", model},
            {"input", {{"messages", Json::array({Json{{"role", "user"}, {"content", content}}})}}},
            {"parameters",
             {{"n", 1},
              {"result_type", "single"},
              {"aspect_ratio", ratio},
              {"resolution", resolution},
              {"watermark", args.value("watermark", false)}}}};
        HttpResponse response =
            requestJson(modelStudioBase(credentials) + "/services/aigc/image-generation/generation",
                        credentials.apiKey, mCaBundle, &body, true, false, context);
        if (response.error) {
            if (response.transferFailed)
                return failure(
                    MaiErrorCode::Network, "submission_unknown",
                    "Kling image may have been submitted. Do not resubmit automatically.");
            return *response.error;
        }
        const Json output = response.body.value("output", Json::object());
        const std::string taskId = value(output, "task_id");
        if (!validId(taskId))
            return failure(MaiErrorCode::Protocol, "submission_unknown",
                           "Kling image returned no task ID; do not resubmit automatically");
        Json result = {{"task_id", taskId},
                       {"conversation_id", taskId},
                       {"status", value(output, "task_status")},
                       {"bound_model", model},
                       {"reply", "Kling image task submitted through Model Studio."}};
        if (context.specialistTasks != nullptr && !context.sessionId.empty()) {
            MaiSpecialistTask task;
            task.id = MaiIdGenerator::generate("spt_");
            task.ownerSessionId = context.sessionId;
            task.specialistName = name();
            task.providerTaskId = taskId;
            task.intent = message;
            task.inputReference = single.empty() ? "text_or_multiple_images" : single;
            task.created = MaiTime::getCurrentTime();
            const MaiError stored = context.specialistTasks->insertSpecialistTask(task);
            if (stored)
                result["context_persistence_warning"] = stored.message();
            else {
                result["conversation_id"] = task.id;
                result["specialist_task_id"] = task.id;
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
            MaiSpecialistTask task;
            if (context.specialistTasks == nullptr ||
                !context.specialistTasks->getSpecialistTask(conversationId, context.sessionId,
                                                            task) ||
                task.specialistName != name())
                return failure(MaiErrorCode::NotFound, "not_found",
                               "Kling image task was not found in this AI conversation");
            taskId = task.providerTaskId;
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
                           value(output, "message").empty()
                               ? "Kling image task ended with " + status
                               : value(output, "message"));
        if (status == "PENDING" || status == "RUNNING")
            return MaiToolResult::success(Json{{"conversation_id", conversationId},
                                               {"task_id", taskId},
                                               {"status", status},
                                               {"reply", "Kling image is still in progress."}}
                                              .dump());
        if (status != "SUCCEEDED")
            return failure(MaiErrorCode::Protocol, "protocol", "Unknown Kling image status");
        const std::string relative = "kling-image-" + taskId + ".png";
        if (context.root.empty() || maiResolvePathWithinRoot(context.root, relative).empty())
            return invalid("An Agent workspace is required to save the image");
        const std::string path = maiResolvePathWithinRoot(context.root, relative);
        std::uint64_t size = 0;
        if (MaiFileSystem::fileSize(MaiFilePath::fromUtf8(path), size) && size > 0)
            return MaiToolResult::success(Json{{"conversation_id", conversationId},
                                               {"task_id", taskId},
                                               {"status", status},
                                               {"path", path},
                                               {"bytes", size},
                                               {"mime_type", "image/png"},
                                               {"reply", "Kling image saved locally."}}
                                              .dump());
        const std::string url = firstImageUrl(output);
        if (url.empty())
            return failure(MaiErrorCode::Protocol, "media_download_failed",
                           "Kling image task has no result URL yet");
        MaiToolResult downloaded = downloadMedia(url, relative, mCaBundle, context, true);
        if (downloaded.hasError()) return downloaded;
        Json result = Json::parse(downloaded.output());
        result.update(Json{{"conversation_id", conversationId},
                           {"task_id", taskId},
                           {"status", status},
                           {"reply", "Kling image saved locally."}});
        return MaiToolResult::success(result.dump());
    }

    MaiWanCredentialsProvider mCredentials;
    std::string mCaBundle;
    MaiCreativeMediaUploadProvider mUploadMedia;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiWanVideoTool(MaiWanCredentialsProvider credentials,
                                             std::string caBundlePath,
                                             MaiCreativeMediaUploadProvider uploadMedia) {
    return std::make_unique<MaiWanVideoTool>(std::move(credentials), std::move(caBundlePath),
                                             std::move(uploadMedia));
}

std::unique_ptr<MaiTool> makeMaiBailianKlingVideoTool(MaiWanCredentialsProvider credentials,
                                                      std::string caBundlePath,
                                                      MaiCreativeMediaUploadProvider uploadMedia) {
    return std::make_unique<MaiBailianKlingVideoTool>(
        std::move(credentials), std::move(caBundlePath), std::move(uploadMedia));
}

std::unique_ptr<MaiTool> makeMaiWanImageTool(MaiWanCredentialsProvider credentials,
                                             std::string caBundlePath,
                                             MaiCreativeMediaUploadProvider uploadMedia) {
    return std::make_unique<MaiWanImageTool>(std::move(credentials), std::move(caBundlePath),
                                             std::move(uploadMedia));
}

std::unique_ptr<MaiTool> makeMaiBailianKlingImageTool(MaiWanCredentialsProvider credentials,
                                                      std::string caBundlePath,
                                                      MaiCreativeMediaUploadProvider uploadMedia) {
    return std::make_unique<MaiBailianKlingImageTool>(
        std::move(credentials), std::move(caBundlePath), std::move(uploadMedia));
}
