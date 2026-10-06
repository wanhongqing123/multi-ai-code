#include "MaiCreativeMediaTools.h"

#include <curl/curl.h>
#include <json.hpp>

#include <algorithm>
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

constexpr char kKlingBase[] = "https://api-beijing.klingai.com";
constexpr char kMiniMaxBase[] = "https://api.minimax.cn";
constexpr std::size_t kMaxResponseBytes = 2 * 1024 * 1024;

std::string stringValue(const Json& value, const char* field) {
    return value.is_object() && value.contains(field) && value[field].is_string()
               ? value[field].get<std::string>()
               : std::string{};
}

MaiToolResult fail(MaiErrorCode error, const char* code, const std::string& message) {
    return MaiToolResult::failure(error, Json{{"code", code}, {"message", message}}.dump());
}

MaiToolResult invalid(const std::string& message) {
    return fail(MaiErrorCode::InvalidInput, "invalid_input", message);
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

std::string taskId(const Json& value) {
    const std::string text = stringValue(value, "task_id");
    if (!text.empty()) return text;
    if (value.is_object() && value.contains("task_id") && value["task_id"].is_number_integer())
        return std::to_string(value["task_id"].get<std::int64_t>());
    return stringValue(value, "id");
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
    maiAssertBlockingAllowed("creative_media_tool");
    CURL* curl = curl_easy_init();
    if (curl == nullptr)
        return {{}, fail(MaiErrorCode::Internal, "internal", "Could not initialize HTTP")};
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
        return {{}, fail(MaiErrorCode::Canceled, "canceled", "Task was canceled")};
    if (response.exceeded)
        return {{}, fail(MaiErrorCode::Protocol, "protocol", "Provider response exceeded 2 MB")};
    if (result != CURLE_OK)
        return {{}, fail(MaiErrorCode::Network, "network", curl_easy_strerror(result))};
    Json data = Json::parse(response.bytes, nullptr, false);
    if (!data.is_object())
        return {{}, fail(MaiErrorCode::Protocol, "protocol", "Provider returned invalid JSON")};
    const Json base = data.value("base_resp", Json::object());
    const int miniMaxStatus =
        base.is_object() && base.value("status_code", Json{}).is_number_integer()
            ? base["status_code"].get<int>()
            : 0;
    const int klingStatus =
        data.value("code", Json{}).is_number_integer() ? data["code"].get<int>() : 0;
    if (status < 200 || status >= 300 || miniMaxStatus != 0 || klingStatus != 0) {
        const std::string message = !stringValue(data, "message").empty()
                                        ? stringValue(data, "message")
                                        : stringValue(base, "status_msg");
        return {{},
                MaiToolResult::failure(
                    MaiErrorCode::Network,
                    Json{{"code", "provider_error"},
                         {"provider_code", miniMaxStatus != 0 ? miniMaxStatus : klingStatus},
                         {"http_status", status},
                         {"request_id", stringValue(data, "request_id")},
                         {"message", message.empty() ? "Provider request failed" : message}}
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

std::optional<MaiToolResult> readImage(const std::string& candidate, const MaiToolContext& context,
                                       bool dataUrl, std::string& encoded) {
    const std::string path = context.resolvePath(candidate);
    if (path.empty()) return invalid("image_path is not accessible");
    std::uint64_t size = 0;
    if (!MaiFileSystem::fileSize(MaiFilePath::fromUtf8(path), size) || size == 0 ||
        size > 5'000'000)
        return invalid("image_path must contain 1 to 5000000 bytes");
    std::string bytes;
    bool truncated = false;
    if (MaiFileSystem::readFile(MaiFilePath::fromUtf8(path), bytes, 5'000'001, &truncated) ||
        truncated)
        return invalid("image_path could not be read within 5 MB");
    const bool png = bytes.size() >= 8 && bytes.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0;
    const bool jpeg = bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xff &&
                      static_cast<unsigned char>(bytes[1]) == 0xd8 &&
                      static_cast<unsigned char>(bytes[2]) == 0xff;
    if (!png && !jpeg) return invalid("image_path must be PNG or JPEG");
    encoded = (dataUrl ? std::string("data:") + (png ? "image/png" : "image/jpeg") + ";base64,"
                       : std::string{}) +
              encodeBase64(bytes);
    return std::nullopt;
}

MaiToolResult downloadMedia(const std::string& url, bool video, const std::string& provider,
                            const MaiToolContext& context, const std::string& caBundle) {
    if (url.compare(0, 8, "https://") != 0)
        return fail(MaiErrorCode::Protocol, "protocol", "Provider returned no HTTPS media URL");
    const std::string relative = provider + "-" + MaiIdGenerator::generate("file_");
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
    const std::string source = stringValue(downloaded, "path");
    std::string prefix;
    if (source.empty() || MaiFileSystem::readFile(MaiFilePath::fromUtf8(source), prefix, 16))
        return fail(MaiErrorCode::Protocol, "protocol", "Downloaded media is unreadable");
    const bool mp4 = prefix.size() >= 8 && prefix.compare(4, 4, "ftyp") == 0;
    const bool png = prefix.size() >= 8 && prefix.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0;
    const bool jpeg = prefix.size() >= 3 && static_cast<unsigned char>(prefix[0]) == 0xff &&
                      static_cast<unsigned char>(prefix[1]) == 0xd8 &&
                      static_cast<unsigned char>(prefix[2]) == 0xff;
    const bool webp = prefix.size() >= 12 && prefix.compare(0, 4, "RIFF") == 0 &&
                      prefix.compare(8, 4, "WEBP") == 0;
    if (video ? !mp4 : !png && !jpeg && !webp) {
        (void)MaiFileSystem::removeFile(MaiFilePath::fromUtf8(source));
        return fail(MaiErrorCode::Protocol, "protocol", "Provider output is not expected media");
    }
    const std::string extension = video ? ".mp4" : png ? ".png" : jpeg ? ".jpg" : ".webp";
    const std::string target = maiResolvePathWithinRoot(context.root, relative + extension);
    if (target.empty() || MaiFileSystem::exists(MaiFilePath::fromUtf8(target)) ||
        MaiFileSystem::publishNewFile(MaiFilePath::fromUtf8(source),
                                      MaiFilePath::fromUtf8(target))) {
        (void)MaiFileSystem::removeFile(MaiFilePath::fromUtf8(source));
        return fail(MaiErrorCode::Internal, "io_error", "Could not publish provider media");
    }
    return MaiToolResult::success(Json{{"path", target},
                                       {"mime_type", video  ? "video/mp4"
                                                     : png  ? "image/png"
                                                     : jpeg ? "image/jpeg"
                                                            : "image/webp"}}
                                      .dump());
}

enum class Provider { Kling, MiniMax };

class MaiCreativeMediaTool final : public MaiTool {
public:
    MaiCreativeMediaTool(Provider provider, bool video, MaiCreativeApiKeyProvider key,
                         std::string caBundle)
        : mProvider(provider),
          mVideo(video),
          mKey(std::move(key)),
          mCaBundle(std::move(caBundle)) {}

    std::string name() const override {
        return std::string(mProvider == Provider::Kling ? "kling_" : "minimax_") +
               (mVideo ? "video" : "image");
    }
    std::string description() const override {
        return std::string(mProvider == Provider::Kling ? "Kling" : "MiniMax") +
               (mVideo ? " paid video generation specialist. Supports text and reference image "
                         "generation. Existing-video editing is not wired."
                       : " paid image generation specialist. Supports text generation and "
                         "reference-image creation.") +
               " Use discover for capabilities, delegate after user confirmation, and continue "
               "for task status. The main Agent receives completed results automatically.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"action":{"type":"string","enum":["discover","delegate","continue"]},"message":{"type":"string"},"context":{"type":"string"},"image_path":{"type":"string"},"last_frame_path":{"type":"string"},"duration":{"type":"integer"},"resolution":{"type":"string"},"ratio":{"type":"string"},"conversation_id":{"type":"string"},"parent_task_id":{"type":"string"},"poll_once":{"type":"boolean"}},"required":["action"]})";
    }
    std::optional<MaiSpecialistInfo> specialistInfo() const override {
        const bool configured = mKey && !mKey().empty();
        const auto ready = configured ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                      : MaiSpecialistCapabilityStatus::NotConfigured;
        MaiSpecialistInfo info;
        info.toolName = name();
        info.modelId = mProvider == Provider::Kling ? (mVideo ? "kling-3.0-turbo" : "kling-v3-omni")
                                                    : (mVideo ? "MiniMax-Hailuo-2.3" : "image-01");
        info.configured = configured;
        info.capabilities.push_back({mVideo ? "text_to_video" : "text_to_image", true, true, ready,
                                     "Provider access and content policy require a live call"});
        info.capabilities.push_back(
            {mVideo                           ? "image_to_video"
             : mProvider == Provider::MiniMax ? "character_reference_image"
                                              : "image_to_image",
             true, true, ready,
             mProvider == Provider::MiniMax && !mVideo
                 ? "Reference images are limited to a character subject; PNG/JPEG, maximum 5 MB"
                 : "PNG or JPEG local reference, maximum 5 MB"});
        if (mVideo) {
            if (mProvider == Provider::Kling)
                info.capabilities.push_back(
                    {"first_last_frame_video", true, true, ready,
                     "Uses regular Kling 3.0; Kling 3.0 Turbo accepts only a first frame"});
            info.capabilities.push_back({"existing_video_edit", false, false,
                                         MaiSpecialistCapabilityStatus::NotImplemented,
                                         "This tool does not upload source videos for editing"});
        }
        return info;
    }
    bool requiresApproval(const std::string& argumentsJson) const override {
        return requiresPerCallApproval(argumentsJson);
    }
    bool requiresPerCallApproval(const std::string& argumentsJson) const override {
        const Json args = Json::parse(argumentsJson, nullptr, false);
        const std::string action = stringValue(args, "action");
        return action != "discover" && action != "continue";
    }
    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        const Json args = Json::parse(argumentsJson, nullptr, false);
        if (!args.is_object()) return invalid("arguments must be a JSON object");
        for (const char* field : {"action", "message", "context", "image_path", "last_frame_path",
                                  "resolution", "ratio", "conversation_id", "parent_task_id"}) {
            if (args.contains(field) && !args[field].is_string())
                return invalid(std::string(field) + " must be a string");
        }
        if (args.contains("duration") && !args["duration"].is_number_integer())
            return invalid("duration must be an integer");
        if (args.contains("poll_once") && !args["poll_once"].is_boolean())
            return invalid("poll_once must be boolean");
        const std::string action = stringValue(args, "action");
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
                                               {"reply", "Generation capabilities listed"}}
                                              .dump());
        }
        if (action != "delegate" && action != "continue")
            return invalid("action must be discover, delegate, or continue");
        if (args.contains("video_path") || args.contains("video_url"))
            return invalid("Existing-video editing is not wired in this tool");
        const std::string key = mKey ? mKey() : std::string{};
        if (key.empty())
            return fail(MaiErrorCode::NotConfigured, "not_configured",
                        "Provider API key is missing in the host configuration");
        return action == "delegate" ? delegate(args, key, context)
                                    : continueTask(args, key, context);
    }

private:
    MaiToolResult delegate(const Json& args, const std::string& key,
                           const MaiToolContext& context) const {
        const std::string message = stringValue(args, "message");
        const std::string extra = stringValue(args, "context");
        if (message.empty() || message.size() > 4000 || extra.size() > 4000)
            return invalid("message is required and message/context must be at most 4000 bytes");
        if (context.root.empty()) return invalid("Agent workspace is required");
        std::string prompt = message;
        if (!extra.empty()) prompt += "\nRelevant context: " + extra;
        const std::string imagePath = stringValue(args, "image_path");
        const std::string lastFramePath = stringValue(args, "last_frame_path");
        if (!lastFramePath.empty() &&
            (mProvider != Provider::Kling || !mVideo || imagePath.empty()))
            return invalid("last_frame_path requires a Kling first-frame image");
        Json body;
        std::string endpoint;
        if (mProvider == Provider::Kling) {
            if (mVideo) {
                if (!args.contains("duration") || !args.contains("resolution"))
                    return invalid("Confirm duration and resolution before paid video generation");
                const int duration = args["duration"].get<int>();
                const std::string resolution = stringValue(args, "resolution");
                if ((duration != 5 && duration != 10) ||
                    (resolution != "720p" && resolution != "1080p"))
                    return invalid("Kling duration must be 5 or 10 and resolution 720p or 1080p");
                Json settings = {{"duration", duration}, {"resolution", resolution}};
                if (!imagePath.empty()) {
                    std::string image;
                    if (auto error = readImage(imagePath, context, false, image)) return *error;
                    Json contents =
                        Json::array({Json{{"type", "prompt"}, {"text", prompt}},
                                     Json{{"type", "first_frame"}, {"url", std::move(image)}}});
                    if (!lastFramePath.empty()) {
                        std::string last;
                        if (auto error = readImage(lastFramePath, context, false, last))
                            return *error;
                        contents.push_back(Json{{"type", "last_frame"}, {"url", std::move(last)}});
                    }
                    body = {{"contents", std::move(contents)}, {"settings", std::move(settings)}};
                    endpoint = std::string(kKlingBase) + (lastFramePath.empty()
                                                              ? "/image-to-video/kling-3.0-turbo"
                                                              : "/image-to-video/kling-3.0");
                } else {
                    const std::string ratio = stringValue(args, "ratio");
                    if (ratio != "16:9" && ratio != "9:16" && ratio != "1:1")
                        return invalid("Confirm ratio as 16:9, 9:16, or 1:1");
                    settings["aspect_ratio"] = ratio;
                    body = {{"prompt", prompt}, {"settings", std::move(settings)}};
                    endpoint = std::string(kKlingBase) + "/text-to-video/kling-3.0-turbo";
                }
            } else {
                const std::string ratio = stringValue(args, "ratio");
                if (ratio != "16:9" && ratio != "9:16" && ratio != "1:1")
                    return invalid("Confirm ratio as 16:9, 9:16, or 1:1");
                body = {{"model_name", "kling-v3-omni"},
                        {"prompt", prompt},
                        {"resolution", "1k"},
                        {"n", 1},
                        {"aspect_ratio", ratio}};
                if (!imagePath.empty()) {
                    std::string image;
                    if (auto error = readImage(imagePath, context, false, image)) return *error;
                    body["image_list"] = Json::array({Json{{"image", std::move(image)}}});
                }
                endpoint = std::string(kKlingBase) + "/v1/images/omni-image";
            }
        } else if (mVideo) {
            if (!args.contains("duration") || !args.contains("resolution"))
                return invalid("Confirm duration and resolution before paid video generation");
            const int duration = args["duration"].get<int>();
            const std::string resolution = stringValue(args, "resolution");
            if ((duration != 6 && duration != 10) ||
                (resolution != "768P" && resolution != "1080P") ||
                (duration == 10 && resolution == "1080P"))
                return invalid("MiniMax Hailuo 2.3 supports 6s/10s at 768P or 6s at 1080P");
            body = {{"model", "MiniMax-Hailuo-2.3"},
                    {"prompt", prompt},
                    {"duration", duration},
                    {"resolution", resolution}};
            if (!imagePath.empty()) {
                std::string image;
                if (auto error = readImage(imagePath, context, true, image)) return *error;
                body["first_frame_image"] = std::move(image);
            }
            endpoint = std::string(kMiniMaxBase) + "/v1/video_generation";
        } else {
            const std::string ratio = stringValue(args, "ratio");
            if (ratio != "16:9" && ratio != "9:16" && ratio != "1:1")
                return invalid("Confirm ratio as 16:9, 9:16, or 1:1");
            body = {{"model", "image-01"},
                    {"prompt", prompt},
                    {"aspect_ratio", ratio},
                    {"response_format", "url"},
                    {"n", 1}};
            if (!imagePath.empty()) {
                std::string image;
                if (auto error = readImage(imagePath, context, true, image)) return *error;
                body["subject_reference"] =
                    Json::array({Json{{"type", "character"}, {"image_file", std::move(image)}}});
            }
            endpoint = std::string(kMiniMaxBase) + "/v1/image_generation";
        }
        const HttpResult submitted = requestJson(endpoint, key, mCaBundle, &body, context);
        if (submitted.error) return *submitted.error;
        if (mProvider == Provider::MiniMax && !mVideo) {
            const Json data = submitted.data.value("data", Json::object());
            if (!data.is_object() || !data.contains("image_urls") ||
                !data["image_urls"].is_array() || data["image_urls"].empty() ||
                !data["image_urls"][0].is_string())
                return fail(MaiErrorCode::Protocol, "protocol", "MiniMax returned no image URL");
            MaiToolResult media = downloadMedia(data["image_urls"][0].get<std::string>(), false,
                                                "minimax", context, mCaBundle);
            if (media.hasError()) return media;
            Json output = Json::parse(media.output());
            output["status"] = "succeeded";
            output["bound_model"] = "image-01";
            output["reply"] = "MiniMax image is ready for review at the returned path.";
            return MaiToolResult::success(output.dump());
        }
        const Json data = mProvider == Provider::Kling
                              ? submitted.data.value("data", Json::object())
                              : submitted.data;
        const std::string providerId = taskId(data);
        if (!validId(providerId))
            return fail(MaiErrorCode::Protocol, "protocol", "Provider returned no task ID");
        Json output = {
            {"task_id", providerId},
            {"conversation_id", providerId},
            {"status", "submitted"},
            {"bound_model", mProvider == Provider::Kling && mVideo && !lastFramePath.empty()
                                ? "kling-3.0"
                                : specialistInfo()->modelId},
            {"reply", "The generation task has started. The app will report its result."}};
        if (context.specialistTasks != nullptr && !context.sessionId.empty()) {
            MaiSpecialistTask task;
            task.id = MaiIdGenerator::generate("spt_");
            task.ownerSessionId = context.sessionId;
            task.specialistName = name();
            task.parentTaskId = stringValue(args, "parent_task_id");
            task.providerTaskId = providerId;
            task.intent = message;
            task.contextSummary = extra;
            task.inputReference = imagePath;
            task.created = MaiTime::getCurrentTime();
            const MaiError stored = context.specialistTasks->insertSpecialistTask(task);
            if (stored) {
                output["context_persistence_warning"] = stored.message();
                output["reply"] =
                    "The task was submitted but automatic notification could not be scheduled. "
                    "Continue using its provider task ID.";
            } else {
                output["conversation_id"] = task.id;
                output["specialist_task_id"] = task.id;
            }
        }
        return MaiToolResult::success(output.dump());
    }

    MaiToolResult continueTask(const Json& args, const std::string& key,
                               const MaiToolContext& context) const {
        if (mProvider == Provider::MiniMax && !mVideo)
            return invalid("MiniMax image generation completes during delegate");
        const std::string conversationId = stringValue(args, "conversation_id");
        if (!validId(conversationId)) return invalid("conversation_id must be a valid task ID");
        std::string providerId = conversationId;
        if (conversationId.compare(0, 4, "spt_") == 0) {
            MaiSpecialistTask previous;
            if (context.specialistTasks == nullptr ||
                !context.specialistTasks->getSpecialistTask(conversationId, context.sessionId,
                                                            previous) ||
                previous.specialistName != name())
                return fail(MaiErrorCode::NotFound, "not_found",
                            "Task was not found in this conversation");
            if (previous.status == MaiSpecialistTaskStatus::Succeeded &&
                !previous.outputPath.empty())
                return MaiToolResult::success(Json{
                    {"conversation_id", conversationId},
                    {"status", "succeeded"},
                    {"path", previous.outputPath},
                    {"reply", previous.finalText}}.dump());
            providerId = previous.providerTaskId;
        }
        if (!validId(providerId)) return invalid("Provider task ID is invalid");
        std::string url;
        if (mProvider == Provider::Kling) {
            url = mVideo ? std::string(kKlingBase) + "/tasks?task_ids=" + providerId
                         : std::string(kKlingBase) + "/v1/images/omni-image/" + providerId;
        } else {
            url = std::string(kMiniMaxBase) + "/v1/query/video_generation?task_id=" + providerId;
        }
        const HttpResult result = requestJson(url, key, mCaBundle, nullptr, context);
        if (result.error) return *result.error;
        if (mProvider == Provider::Kling)
            return resolveKling(result.data, conversationId, providerId, context);
        const std::string status = stringValue(result.data, "status");
        if (status == "Processing" || status == "Queueing" || status == "Preparing")
            return MaiToolResult::success(Json{{"conversation_id", conversationId},
                                               {"task_id", providerId},
                                               {"status", "running"},
                                               {"reply", "MiniMax video is still processing."}}
                                              .dump());
        if (status == "Fail")
            return fail(MaiErrorCode::Network, "task_failed", "MiniMax video generation failed");
        if (status != "Success")
            return fail(MaiErrorCode::Protocol, "protocol", "MiniMax returned unknown task status");
        const std::string fileId = taskId(Json{{"task_id", result.data.value("file_id", Json{})}});
        if (!validId(fileId))
            return fail(MaiErrorCode::Protocol, "protocol", "MiniMax returned no file ID");
        const HttpResult file =
            requestJson(std::string(kMiniMaxBase) + "/v1/files/retrieve?file_id=" + fileId, key,
                        mCaBundle, nullptr, context);
        if (file.error) return *file.error;
        const Json fileInfo = file.data.value("file", Json::object());
        return finish(stringValue(fileInfo, "download_url"), conversationId, providerId, context);
    }

    MaiToolResult resolveKling(const Json& result, const std::string& conversationId,
                               const std::string& providerId, const MaiToolContext& context) const {
        const Json payload = result.value("data", Json{});
        if (mVideo ? !payload.is_array() || payload.empty() : !payload.is_object())
            return fail(MaiErrorCode::Protocol, "protocol", "Kling returned no task data");
        const Json data = mVideo ? payload[0] : payload;
        if (!data.is_object())
            return fail(MaiErrorCode::Protocol, "protocol", "Kling returned invalid task data");
        const std::string status = stringValue(data, mVideo ? "status" : "task_status");
        if (status == "submitted" || status == "processing")
            return MaiToolResult::success(Json{{"conversation_id", conversationId},
                                               {"task_id", providerId},
                                               {"status", "running"},
                                               {"reply", "Kling task is still processing."}}
                                              .dump());
        if (status == "failed")
            return fail(MaiErrorCode::Network, "task_failed",
                        stringValue(data, mVideo ? "message" : "task_status_msg").empty()
                            ? "Kling generation failed"
                            : stringValue(data, mVideo ? "message" : "task_status_msg"));
        if (status != (mVideo ? "succeeded" : "succeed"))
            return fail(MaiErrorCode::Protocol, "protocol", "Kling returned unknown task status");
        const Json imageResult = data.value("task_result", Json::object());
        const Json taskResult = mVideo ? data.value("outputs", Json::array())
                                : imageResult.is_object()
                                    ? imageResult.value("images", Json::array())
                                    : Json::array();
        if (!taskResult.is_array() || taskResult.empty())
            return fail(MaiErrorCode::Protocol, "protocol", "Kling returned no media result");
        for (const Json& output : taskResult) {
            if (!mVideo || stringValue(output, "type") == "video")
                return finish(stringValue(output, "url"), conversationId, providerId, context);
        }
        return fail(MaiErrorCode::Protocol, "protocol", "Kling returned no video output");
    }

    MaiToolResult finish(const std::string& url, const std::string& conversationId,
                         const std::string& providerId, const MaiToolContext& context) const {
        MaiToolResult media = downloadMedia(
            url, mVideo, mProvider == Provider::Kling ? "kling" : "minimax", context, mCaBundle);
        if (media.hasError()) return media;
        Json output = Json::parse(media.output());
        output["conversation_id"] = conversationId;
        output["task_id"] = providerId;
        output["status"] = "succeeded";
        if (mProvider != Provider::Kling || !mVideo)
            output["bound_model"] = specialistInfo()->modelId;
        output["reply"] = "Generated media is ready for review at the returned path.";
        return MaiToolResult::success(output.dump());
    }

    Provider mProvider;
    bool mVideo;
    MaiCreativeApiKeyProvider mKey;
    std::string mCaBundle;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiKlingVideoTool(MaiCreativeApiKeyProvider apiKey,
                                               std::string caBundlePath) {
    return std::make_unique<MaiCreativeMediaTool>(Provider::Kling, true, std::move(apiKey),
                                                  std::move(caBundlePath));
}

std::unique_ptr<MaiTool> makeMaiKlingImageTool(MaiCreativeApiKeyProvider apiKey,
                                               std::string caBundlePath) {
    return std::make_unique<MaiCreativeMediaTool>(Provider::Kling, false, std::move(apiKey),
                                                  std::move(caBundlePath));
}

std::unique_ptr<MaiTool> makeMaiMiniMaxVideoTool(MaiCreativeApiKeyProvider apiKey,
                                                 std::string caBundlePath) {
    return std::make_unique<MaiCreativeMediaTool>(Provider::MiniMax, true, std::move(apiKey),
                                                  std::move(caBundlePath));
}

std::unique_ptr<MaiTool> makeMaiMiniMaxImageTool(MaiCreativeApiKeyProvider apiKey,
                                                 std::string caBundlePath) {
    return std::make_unique<MaiCreativeMediaTool>(Provider::MiniMax, false, std::move(apiKey),
                                                  std::move(caBundlePath));
}
