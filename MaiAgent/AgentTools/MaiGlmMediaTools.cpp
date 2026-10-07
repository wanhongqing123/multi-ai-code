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

std::optional<MaiToolResult> imageData(const std::string& candidate, const MaiToolContext& context,
                                       std::string& encoded) {
    const std::string path = context.resolvePath(candidate);
    if (path.empty()) return invalid("image_path is not accessible");
    std::uint64_t size = 0;
    if (!MaiFileSystem::fileSize(MaiFilePath::fromUtf8(path), size) || size == 0 ||
        size > 5'000'000)
        return invalid("image_path must contain 1 to 5000000 bytes");
    std::string bytes;
    bool truncated = false;
    const MaiError error =
        MaiFileSystem::readFile(MaiFilePath::fromUtf8(path), bytes, 5'000'001, &truncated);
    if (error || truncated) return invalid("image_path could not be read within 5 MB");
    const bool png = bytes.size() >= 8 && bytes.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0;
    const bool jpeg = bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xff &&
                      static_cast<unsigned char>(bytes[1]) == 0xd8 &&
                      static_cast<unsigned char>(bytes[2]) == 0xff;
    if (!png && !jpeg) return invalid("image_path must contain PNG or JPEG image bytes");
    encoded = std::string("data:") + (png ? "image/png" : "image/jpeg") + ";base64," +
              encodeBase64(bytes);
    return std::nullopt;
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
    MaiGlmMediaTool(bool video, MaiGlmApiKeyProvider key, std::string caBundle)
        : mVideo(video), mKey(std::move(key)), mCaBundle(std::move(caBundle)) {}

    std::string name() const override {
        return mVideo ? "glm_video" : "glm_image";
    }
    std::string description() const override {
        return mVideo
                   ? "GLM CogVideoX-3 specialist for paid 5 or 10 second text-to-video and "
                     "image-to-video. "
                     "When image_path is present, describe how the existing subject moves. "
                     "For face fidelity, choose quality=quality and a 1080P-or-higher size; "
                     "quality=speed and 720P are lower-quality previews. Quality defaults to "
                     "quality. Use action=discover to "
                     "inspect capabilities, delegate to submit after "
                     "confirmation, continue to check a task. Existing-video editing is not "
                     "supported. Completion is reported to the main Agent automatically."
                   : "GLM-Image specialist for paid text-to-image. Use action=discover to inspect "
                     "capabilities, delegate to submit after confirmation, continue to check a "
                     "task. Existing-image editing is not supported. Completion is reported to "
                     "the main Agent automatically.";
    }
    std::string parametersSchema() const override {
        return mVideo
                   ? R"({"type":"object","properties":{"action":{"type":"string","enum":["discover","delegate","continue"]},"message":{"type":"string"},"context":{"type":"string"},"image_path":{"type":"string"},"last_frame_path":{"type":"string"},"duration":{"type":"integer","enum":[5,10]},"size":{"type":"string","enum":["1280x720","720x1280","1024x1024","1920x1080","1080x1920","2048x1080","3840x2160"]},"fps":{"type":"integer","enum":[30,60]},"quality":{"type":"string","enum":["speed","quality"]},"with_audio":{"type":"boolean"},"conversation_id":{"type":"string"},"parent_task_id":{"type":"string"},"poll_once":{"type":"boolean"}},"required":["action"]})"
                   : R"({"type":"object","properties":{"action":{"type":"string","enum":["discover","delegate","continue"]},"message":{"type":"string"},"context":{"type":"string"},"size":{"type":"string"},"conversation_id":{"type":"string"},"parent_task_id":{"type":"string"},"poll_once":{"type":"boolean"}},"required":["action"]})";
    }
    std::optional<MaiSpecialistInfo> specialistInfo() const override {
        const bool configured = mKey && !mKey().empty();
        const auto ready = configured ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                      : MaiSpecialistCapabilityStatus::NotConfigured;
        MaiSpecialistInfo info;
        info.toolName = name();
        info.modelId = mVideo ? kVideoModel : kImageModel;
        info.configured = configured;
        info.capabilities.push_back(
            {mVideo ? "text_to_video" : "text_to_image", true, true, ready,
             mVideo ? "Output duration is 5 or 10 seconds; provider entitlement must be checked "
                      "with a live call"
                    : "Provider entitlement must be checked with a live call"});
        if (mVideo) {
            info.capabilities.push_back(
                {"image_to_video", true, true, ready, "PNG or JPEG input, maximum 5 MB per image"});
            info.capabilities.push_back(
                {"first_last_frame_video", true, true, ready,
                 "image_url accepts exactly two ordered images: first frame, then last frame"});
            info.capabilities.push_back(
                {"multi_reference_video", false, false,
                 MaiSpecialistCapabilityStatus::NotImplemented,
                 "CogVideoX-3 video API accepts one first frame or exactly two first/last frames; "
                 "arbitrary multi-image reference is not exposed"});
            info.capabilities.push_back({"existing_video_edit", false, false,
                                         MaiSpecialistCapabilityStatus::NotImplemented,
                                         "CogVideoX-3 generation API has no source-video input"});
        } else {
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
                     {"tool_status", maiSpecialistCapabilityStatusToString(capability.status)},
                     {"limitation", capability.limitation}});
            return MaiToolResult::success(Json{{"tool_kind", "model_backed"},
                                               {"bound_model", info.modelId},
                                               {"configured", info.configured},
                                               {"capabilities", std::move(capabilities)},
                                               {"reply", "GLM generation capabilities listed"}}
                                              .dump());
        }
        if (action != "delegate" && action != "continue")
            return invalid("action must be discover, delegate, or continue");
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
                if (auto error = imageData(imagePath, context, first)) return *error;
                inputReference = imagePath;
                if (lastFramePath.empty()) {
                    body["image_url"] = std::move(first);
                } else {
                    std::string last;
                    if (auto error = imageData(lastFramePath, context, last)) return *error;
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
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiGlmVideoTool(MaiGlmApiKeyProvider apiKey,
                                             std::string caBundlePath) {
    return std::make_unique<MaiGlmMediaTool>(true, std::move(apiKey), std::move(caBundlePath));
}

std::unique_ptr<MaiTool> makeMaiGlmImageTool(MaiGlmApiKeyProvider apiKey,
                                             std::string caBundlePath) {
    return std::make_unique<MaiGlmMediaTool>(false, std::move(apiKey), std::move(caBundlePath));
}
