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

constexpr char kVideoModel[] = "cogvideox-3";
constexpr char kImageModel[] = "glm-image";
constexpr char kVideoUrl[] = "https://open.bigmodel.cn/api/paas/v4/videos/generations";
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

std::string describeGlmVideoSchema(const std::string& raw) {
    Json schema = Json::parse(raw);
    Json& fields = schema["properties"];
    // 以下英文会随 JSON Schema 直接交给主模型；中文解释每个关键参数的实际含义。
    // 视频和图片工具共用类，但绝不能把图片工具的参数拿来构造视频请求。
    // action：discover 查询；delegate 付费提交；continue 查旧任务；
    // delete 仅删除已交付本地索引，不向 GLM 发云端删除请求。
    fields["action"]["description"] =
        "discover lists actual tool capabilities; delegate is paid; continue checks the "
        "existing task; delete removes only a delivered local record";
    // message 是视频动作说明，context 只放必要背景，不传完整聊天历史或媒体字节。
    fields["message"]["description"] = "Video scene and motion instruction";
    fields["context"]["description"] =
        "Only task-relevant background; do not place media bytes or full chat history here";
    // image_path 只是一张严格首帧，本地 JPG/PNG 不超过 5 MB，先上传私有 OSS。
    fields["image_path"]["description"] =
        "Optional single local JPG/PNG first frame, at most 5 MB, uploaded to private OSS";
    // last_frame_path 必须配合 image_path 才是严格首尾帧；
    // 这不等于模型支持任意张图片共同参考。
    fields["last_frame_path"]["description"] =
        "Optional local strict last frame; requires image_path. Arbitrary multi-image "
        "references are unsupported";
    // duration 只可选 5/10 秒；size 必须从枚举里选，不能自行拼一个分辨率。
    fields["duration"]["description"] = "Output duration is exactly 5 or 10 seconds";
    fields["size"]["description"] =
        "Choose one schema-listed output size; unsupported sizes fail before paid submission";
    // fps 为 30/60；quality 是 speed/quality 档；with_audio 是声音开关。
    fields["fps"]["description"] = "Output frames per second: 30 or 60";
    fields["quality"]["description"] = "Generation quality mode: speed or quality";
    fields["with_audio"]["description"] = "Whether generated video includes audio";
    // conversation_id 只引用本会话的旧 spt_ 任务；poll_once 仅做单次排障查询。
    // 正常任务完成由 App 后台自动跟进并交付。
    fields["conversation_id"]["description"] =
        "Saved spt_ task ID for continue or local-record delete in this AI conversation";
    // parent_task_id 只建立本地修订链关系，不会自动沿用上次图片或视频素材。
    fields["parent_task_id"]["description"] =
        "Optional prior local task ID for revision lineage; inputs are not reused automatically";
    fields["poll_once"]["description"] =
        "Query status once; the App normally polls and delivers the result automatically";
    return schema.dump();
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

std::size_t utf8CharacterCount(const std::string& text) {
    return static_cast<std::size_t>(std::count_if(
        text.begin(), text.end(), [](unsigned char byte) { return (byte & 0xc0) != 0x80; }));
}

MaiToolResult downloadMedia(const std::string& url, const std::string& relative, bool video,
                            const MaiToolContext& context, const std::string& caBundle) {
    if (!httpsUrl(url))
        return failure(MaiErrorCode::Protocol, "protocol", "GLM returned no HTTPS media URL");
    const std::string temporary = relative + "." + MaiIdGenerator::generate("part_") + ".download";
    auto downloader = makeMaiDownloadFileTool(caBundle);
    const MaiToolResult fetched = downloader->execute(Json{{"url", url},
                                                           {"output_path", temporary},
                                                           {"max_size_mb", video ? 500 : 50},
                                                           {"timeout_s", 300}}
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
    const bool mp4 = prefix.size() >= 8 && prefix.compare(4, 4, "ftyp") == 0;
    const bool png = prefix.size() >= 8 && prefix.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0;
    const bool jpeg = prefix.size() >= 3 && static_cast<unsigned char>(prefix[0]) == 0xff &&
                      static_cast<unsigned char>(prefix[1]) == 0xd8 &&
                      static_cast<unsigned char>(prefix[2]) == 0xff;
    const bool webp = prefix.size() >= 12 && prefix.compare(0, 4, "RIFF") == 0 &&
                      prefix.compare(8, 4, "WEBP") == 0;
    if (video ? !mp4 : !png && !jpeg && !webp) {
        (void)MaiFileSystem::removeFile(MaiFilePath::fromUtf8(source));
        return failure(MaiErrorCode::Protocol, "protocol", "GLM output is not expected media");
    }
    const std::string extension = video ? ".mp4" : png ? ".png" : jpeg ? ".jpg" : ".webp";
    const std::string target = maiResolvePathWithinRoot(context.root, relative + extension);
    if (target.empty() || MaiFileSystem::exists(MaiFilePath::fromUtf8(target)) ||
        MaiFileSystem::publishNewFile(MaiFilePath::fromUtf8(source),
                                      MaiFilePath::fromUtf8(target))) {
        (void)MaiFileSystem::removeFile(MaiFilePath::fromUtf8(source));
        return failure(MaiErrorCode::Internal, "io_error", "Could not publish GLM media");
    }
    return MaiToolResult::success(Json{{"path", target},
                                       {"mime_type", video  ? "video/mp4"
                                                     : png  ? "image/png"
                                                     : jpeg ? "image/jpeg"
                                                            : "image/webp"}}
                                      .dump());
}

class MaiGlmMediaTool final : public MaiTool {
public:
    MaiGlmMediaTool(bool video, MaiGlmApiKeyProvider key, std::string caBundle,
                    MaiCreativeMediaUploadProvider uploadMedia = {})
        : mVideo(video),
          mKey(std::move(key)),
          mCaBundle(std::move(caBundle)),
          mUploadMedia(std::move(uploadMedia)) {}

    std::string name() const override {
        return mVideo ? "glm_video" : "glm_image";
    }
    std::string description() const override {
        // 视频分支英文描述的对应含义：CogVideoX-3 只接文生视频、单首帧或严格
        // 首尾帧，输出时长为 5/10 秒。图片先经私有 OSS，帧率可选 30/60，
        // 画质可选 speed/quality，可决定是否生成声音；实际尺寸只能取 schema 列表。
        // discover 用于选模型前查真实能力；delegate 才付费，continue 仅查询。
        // 不支持已有视频编辑或任意多图参考；delete 只删除已交付的本地任务记录，
        // 没有已核实的云端取消接口。GLM Coding Plan Key 不代表视频权益已开通。
        // 图片分支是付费文生图，不提供已有图片编辑；动作和结果交接规则相同。
        return mVideo
                   ? "GLM CogVideoX-3 video specialist. Call discover before choosing it. "
                     "This tool creates 5 or 10 second video from text, one first-frame "
                     "image_path, or exactly two ordered first/last frames. Local JPG/PNG inputs "
                     "go through private OSS. Choose one supported size, 30/60 fps, "
                     "quality=speed/quality, and optional with_audio. It does not accept a "
                     "source video, arbitrary multi-image references, or video editing. "
                     "Delegate creates a paid task; continue checks it; delete removes only "
                     "a delivered local record. No verified cloud cancel API is wired. "
                     "Do not claim the GLM Coding Plan Key includes video model entitlement."
                   : "GLM-Image specialist for paid text-to-image. Use action=discover to inspect "
                     "capabilities, delegate to submit after confirmation, continue to check a "
                     "task. Existing-image editing is not supported. Completion is reported to "
                     "the main Agent automatically.";
    }
    std::string parametersSchema() const override {
        // 视频和图片共用工具类，但两套参数不同：视频时长只能是 5 或 10 秒。
        // image_path 是首帧；last_frame_path 只有与首帧同时提供才构成首尾帧任务。
        // quality、fps、size、with_audio 是供应商参数，提交前仍要做组合校验。
        return mVideo
                   ? describeGlmVideoSchema(
                         R"({"type":"object","properties":{"action":{"type":"string","enum":["discover","delegate","continue","delete"]},"message":{"type":"string"},"context":{"type":"string"},"image_path":{"type":"string"},"last_frame_path":{"type":"string"},"duration":{"type":"integer","enum":[5,10]},"size":{"type":"string","enum":["1280x720","720x1280","1024x1024","1920x1080","1080x1920","2048x1080","3840x2160"]},"fps":{"type":"integer","enum":[30,60]},"quality":{"type":"string","enum":["speed","quality"]},"with_audio":{"type":"boolean"},"conversation_id":{"type":"string"},"parent_task_id":{"type":"string"},"poll_once":{"type":"boolean"}},"required":["action"]})")
                   : R"({"type":"object","properties":{"action":{"type":"string","enum":["discover","delegate","continue"]},"message":{"type":"string"},"context":{"type":"string"},"size":{"type":"string"},"conversation_id":{"type":"string"},"parent_task_id":{"type":"string"},"poll_once":{"type":"boolean"}},"required":["action"]})";
    }
    std::optional<MaiSpecialistInfo> specialistInfo() const override {
        const bool configured = mKey && !mKey().empty();
        const auto ready = configured ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                      : MaiSpecialistCapabilityStatus::NotConfigured;
        const auto mediaReady = !configured ? MaiSpecialistCapabilityStatus::NotConfigured
                                : mUploadMedia
                                    ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                    : MaiSpecialistCapabilityStatus::UploadNotConfigured;
        MaiSpecialistInfo info;
        info.toolName = name();
        info.modelId = mVideo ? kVideoModel : kImageModel;
        info.configured = configured;
        // text_to_video：视频只生成 5 或 10 秒；text_to_image：图片由文本生成。
        // 两条能力都还需用当前账号做实盘权益验证，配置 Key 不代表已验证。
        info.capabilities.push_back(
            {mVideo ? "text_to_video" : "text_to_image", true, true, ready,
             mVideo ? "CogVideoX-3 generates exactly 5 or 10 seconds, with 30 or 60 fps, "
                      "quality=speed/quality, optional with_audio, and only schema-listed "
                      "sizes. Video entitlement still needs a live call"
                    : "Provider entitlement must be checked with a live call"});
        if (mVideo) {
            // image_to_video：输入 PNG/JPEG，每张最多 5 MB。
            info.capabilities.push_back(
                {"image_to_video", true, true, mediaReady,
                 "Exactly one JPG/PNG first-frame image_path via private OSS, maximum 5 MB; "
                 "the prompt should describe motion of the depicted subject"});
            // first_last_frame_video：image_url 仅接顺序确定的两张图，先首帧再尾帧。
            info.capabilities.push_back(
                {"first_last_frame_video", true, true, mediaReady,
                 "Exactly two ordered JPG/PNG inputs: image_path first and last_frame_path "
                 "second. This is strict frame control, not arbitrary multi-image reference"});
            // multi_reference_video：视频 API 没开放任意多图参考；只有单首帧或严格两帧。
            info.capabilities.push_back(
                {"multi_reference_video", false, false,
                 MaiSpecialistCapabilityStatus::NotImplemented,
                 "CogVideoX-3 video API accepts one first frame or exactly two first/last frames; "
                 "arbitrary multi-image reference is not exposed"});
            // existing_video_edit：生成 API 没有源视频输入，因此不能编辑已有视频。
            info.capabilities.push_back({"existing_video_edit", false, false,
                                         MaiSpecialistCapabilityStatus::NotImplemented,
                                         "CogVideoX-3 generation API has no source-video input"});
            // 当前没有经过供应商验证的取消接口，删除仅整理本地已交付记录。
            info.capabilities.push_back({"cloud_task_cancel", false, false,
                                         MaiSpecialistCapabilityStatus::NotImplemented,
                                         "No verified provider cancellation API is wired"});
            // 本地删除只适用于已交付的本会话记录，保留云端结果和下载的视频。
            info.capabilities.push_back(
                {"local_record_delete", false, false,
                 MaiSpecialistCapabilityStatus::ImplementedUnverified,
                 "delete requires a delivered task in this AI conversation; cloud output "
                 "and downloaded video remain"});
        } else {
            // existing_image_edit：GLM-Image 的这条异步生成 API 只接受文本。
            info.capabilities.push_back({"existing_image_edit", false, false,
                                         MaiSpecialistCapabilityStatus::NotImplemented,
                                         "GLM-Image async generation API is text-only"});
        }
        return info;
    }
    bool requiresApproval(const std::string& argumentsJson) const override {
        return requiresPerCallApproval(argumentsJson);
    }
    bool requiresPerCallApproval(const std::string& argumentsJson) const override {
        const Json args = Json::parse(argumentsJson, nullptr, false);
        const std::string action = value(args, "action");
        return action != "discover" && action != "continue";
    }
    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        // 执行顺序：解析参数与输入图 → 检查型号规格 → 付费提交 → 保存任务 ID。
        // continue 仅查询原任务；失败时保留上游错误，不把首查异常当成生成失败。
        const Json args = Json::parse(argumentsJson, nullptr, false);
        if (!args.is_object()) return invalid("arguments must be a JSON object");
        for (const char* field : {"action", "message", "context", "image_path", "last_frame_path",
                                  "quality", "size", "conversation_id", "parent_task_id"}) {
            if (args.contains(field) && !args[field].is_string())
                return invalid(std::string(field) + " must be a string");
        }
        for (const char* field : {"duration", "fps"}) {
            if (args.contains(field) && !args[field].is_number_integer())
                return invalid(std::string(field) + " must be an integer");
        }
        for (const char* field : {"with_audio", "poll_once"}) {
            if (args.contains(field) && !args[field].is_boolean())
                return invalid(std::string(field) + " must be boolean");
        }
        if (args.contains("video_path") || args.contains("video_url"))
            return invalid("This GLM generation API cannot edit an existing video");
        if (!mVideo && (args.contains("image_path") || args.contains("last_frame_path")))
            return invalid("GLM-Image generation does not accept an input image");
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
                                               {"bound_model", info.modelId},
                                               {"configured", info.configured},
                                               {"capabilities", std::move(capabilities)},
                                               {"reply", "GLM generation capabilities listed"}}
                                              .dump());
        }
        if (mVideo && action == "delete") {
            // 不存在已核实的云端取消/删除接口：这里只删除已交付的本地任务索引。
            const std::string id = value(args, "conversation_id");
            MaiSpecialistTask task;
            if (id.compare(0, 4, "spt_") != 0 || context.specialistTasks == nullptr ||
                !context.specialistTasks->getSpecialistTask(id, context.sessionId, task) ||
                task.specialistName != name())
                return failure(MaiErrorCode::NotFound, "not_found",
                               "GLM video task was not found in this AI conversation");
            const MaiError removed =
                context.specialistTasks->deleteCompletedSpecialistTask(id, context.sessionId);
            if (removed) return failure(removed.code(), "delete_unavailable", removed.message());
            return MaiToolResult::success(
                Json{{"conversation_id", id},
                     {"task_id", task.providerTaskId},
                     {"status", "local_record_deleted"},
                     {"provider_deleted", false},
                     {"reply", "Local GLM task record deleted; cloud job and output remain."}}
                    .dump());
        }
        if (action != "delegate" && action != "continue")
            return invalid("action must be discover, delegate, continue, or delete for video");
        const std::string key = mKey ? mKey() : std::string{};
        if (key.empty())
            return failure(MaiErrorCode::NotConfigured, "not_configured",
                           "GLM API key is missing. Select a GLM main model or configure the "
                           "same platform API key in the host.");
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
        Json body = {{"model", mVideo ? kVideoModel : kImageModel},
                     {"prompt", prompt},
                     {"watermark_enabled", true}};
        std::string inputReference;
        if (mVideo) {
            if (!args.contains("duration") || !args["duration"].is_number_integer() ||
                !args.contains("size") || !args["size"].is_string())
                return invalid("Confirm duration and size before paid video generation");
            if (args["duration"] != 5 && args["duration"] != 10)
                return invalid("duration must be 5 or 10 seconds");
            const int duration = args["duration"].get<int>();
            const std::string size = args["size"].get<std::string>();
            if (size != "1280x720" && size != "720x1280" && size != "1024x1024" &&
                size != "1920x1080" && size != "1080x1920" && size != "2048x1080" &&
                size != "3840x2160")
                return invalid("Unsupported video size");
            if (utf8CharacterCount(prompt) > 512)
                return invalid("CogVideoX-3 prompt must be at most 512 characters");
            body["duration"] = duration;
            body["size"] = size;
            const std::string imagePath = value(args, "image_path");
            const std::string lastFramePath = value(args, "last_frame_path");
            if (!lastFramePath.empty() && imagePath.empty())
                return invalid("last_frame_path requires image_path");
            if (!imagePath.empty()) {
                std::string first;
                const std::string path = context.resolvePath(imagePath);
                std::uint64_t imageSize = 0;
                if (path.empty() ||
                    !MaiFileSystem::fileSize(MaiFilePath::fromUtf8(path), imageSize) ||
                    imageSize == 0 || imageSize > 5'000'000)
                    return invalid("image_path must contain 1 to 5000000 bytes");
                if (!mUploadMedia)
                    return failure(MaiErrorCode::NotConfigured, "upload_not_configured",
                                   "Private OSS media upload is not configured");
                const auto uploaded = mUploadMedia(path, context);
                if (!uploaded)
                    return failure(uploaded.error().code(), "upload_failed",
                                   uploaded.error().message());
                first = uploaded.value();
                if (!httpsUrl(first))
                    return failure(MaiErrorCode::Protocol, "upload_failed",
                                   "OSS returned no HTTPS image URL");
                inputReference = imagePath;
                if (lastFramePath.empty()) {
                    body["image_url"] = std::move(first);
                } else {
                    std::string last;
                    const std::string lastPath = context.resolvePath(lastFramePath);
                    std::uint64_t lastSize = 0;
                    if (lastPath.empty() ||
                        !MaiFileSystem::fileSize(MaiFilePath::fromUtf8(lastPath), lastSize) ||
                        lastSize == 0 || lastSize > 5'000'000)
                        return invalid("last_frame_path must contain 1 to 5000000 bytes");
                    const auto uploadedLast = mUploadMedia(lastPath, context);
                    if (!uploadedLast)
                        return failure(uploadedLast.error().code(), "upload_failed",
                                       uploadedLast.error().message());
                    last = uploadedLast.value();
                    if (!httpsUrl(last))
                        return failure(MaiErrorCode::Protocol, "upload_failed",
                                       "OSS returned no HTTPS image URL");
                    body["image_url"] = Json::array({std::move(first), std::move(last)});
                }
            }
            if (args.contains("fps") && args["fps"] != 30 && args["fps"] != 60)
                return invalid("fps must be 30 or 60");
            const int fps = args.value("fps", 30);
            body["fps"] = fps;
            const std::string quality =
                value(args, "quality").empty() ? "quality" : value(args, "quality");
            if (quality != "speed" && quality != "quality")
                return invalid("quality must be speed or quality");
            body["quality"] = quality;
            if (args.contains("with_audio") && !args["with_audio"].is_boolean())
                return invalid("with_audio must be boolean");
            body["with_audio"] = args.value("with_audio", false);
        } else {
            const std::string size =
                value(args, "size").empty() ? "1280x1280" : value(args, "size");
            if (!validImageSize(size))
                return invalid("image size must be a supported 32-pixel multiple");
            body["quality"] = "hd";
            body["size"] = size;
        }
        if (context.root.empty()) return invalid("Agent workspace is required");
        const HttpResult submitted =
            requestJson(mVideo ? kVideoUrl : kImageUrl, key, mCaBundle, &body, context);
        if (submitted.error) return *submitted.error;
        const std::string providerId = value(submitted.data, "id");
        if (!validId(providerId))
            return failure(MaiErrorCode::Protocol, "protocol", "GLM returned no task ID");
        Json output = {{"task_id", providerId},
                       {"conversation_id", providerId},
                       {"status", "submitted"},
                       {"bound_model", mVideo ? kVideoModel : kImageModel},
                       {"reply", "The GLM task has started. The app will report its result."}};
        if (context.specialistTasks != nullptr && !context.sessionId.empty()) {
            MaiSpecialistTask task;
            task.id = MaiIdGenerator::generate("spt_");
            task.ownerSessionId = context.sessionId;
            task.specialistName = name();
            task.parentTaskId = value(args, "parent_task_id");
            task.providerTaskId = providerId;
            task.intent = message;
            task.contextSummary = extra;
            task.inputReference = inputReference;
            task.created = MaiTime::getCurrentTime();
            const MaiError stored = context.specialistTasks->insertSpecialistTask(task);
            if (stored) {
                output["context_persistence_warning"] = stored.message();
                output["reply"] =
                    "The task was submitted but automatic notification could not "
                    "be scheduled. Continue using its provider task ID.";
            } else {
                output["conversation_id"] = task.id;
                output["specialist_task_id"] = task.id;
            }
        }
        return MaiToolResult::success(output.dump());
    }

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
                               "GLM task was not found in this conversation");
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
                                               {"reply", "The GLM task is still processing."}}
                                              .dump());
        if (status == "FAIL") {
            const Json detail = result.data.value("error", Json::object());
            const std::string message = value(detail, "message");
            return failure(MaiErrorCode::Network, "task_failed",
                           message.empty() ? "GLM generation failed" : message);
        }
        if (status != "SUCCESS")
            return failure(MaiErrorCode::Protocol, "protocol", "GLM returned unknown task status");
        const char* field = mVideo ? "video_result" : "image_result";
        if (!result.data.contains(field) || !result.data[field].is_array() ||
            result.data[field].empty())
            return failure(MaiErrorCode::Protocol, "protocol", "GLM returned no media result");
        const std::string url = value(result.data[field][0], "url");
        const std::string relative = "glm-" + MaiIdGenerator::generate("file_");
        MaiToolResult media = downloadMedia(url, relative, mVideo, context, mCaBundle);
        if (media.hasError()) return media;
        Json output = Json::parse(media.output());
        output["conversation_id"] = conversationId;
        output["task_id"] = providerId;
        output["status"] = "succeeded";
        output["bound_model"] = mVideo ? kVideoModel : kImageModel;
        output["reply"] = mVideo ? "GLM video is ready for review at the returned path."
                                 : "GLM image is ready for review at the returned path.";
        return MaiToolResult::success(output.dump());
    }

    bool mVideo;
    MaiGlmApiKeyProvider mKey;
    std::string mCaBundle;
    MaiCreativeMediaUploadProvider mUploadMedia;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiGlmVideoTool(MaiGlmApiKeyProvider apiKey, std::string caBundlePath,
                                             MaiCreativeMediaUploadProvider uploadMedia) {
    return std::make_unique<MaiGlmMediaTool>(true, std::move(apiKey), std::move(caBundlePath),
                                             std::move(uploadMedia));
}

std::unique_ptr<MaiTool> makeMaiGlmImageTool(MaiGlmApiKeyProvider apiKey,
                                             std::string caBundlePath) {
    return std::make_unique<MaiGlmMediaTool>(false, std::move(apiKey), std::move(caBundlePath));
}
