#include "MaiArkMediaTools.h"

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
    const Json args = Json::parse(raw, nullptr, false);
    if (!args.is_object() || !args.value("action", Json{}).is_string()) return true;
    const std::string action = args["action"].get<std::string>();
    return action != "discover" && action != "continue";
}

constexpr char kVideoModel[] = "doubao-seedance-2-0-260128";
constexpr char kImageModel[] = "doubao-seedream-5-0-flash-260915";
constexpr char kVideoTasksUrl[] =
    "https://ark.cn-beijing.volces.com/api/v3/contents/generations/tasks";
constexpr char kImagesUrl[] = "https://ark.cn-beijing.volces.com/api/v3/images/generations";
constexpr std::size_t kMaxResponseBytes = 2 * 1024 * 1024;

std::string stringValue(const Json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) || !object[key].is_string()) return {};
    return object[key].get<std::string>();
}

MaiToolResult discoverSpecialist(const MaiSpecialistInfo& info, const std::string& reply) {
    Json capabilities = Json::array();
    for (const MaiSpecialistCapability& capability : info.capabilities) {
        capabilities.push_back(
            Json{{"id", capability.id},
                 {"model_support", capability.modelSupported},
                 {"api_support", capability.apiSupported},
                 {"tool_status", maiSpecialistCapabilityStatusToString(capability.status)},
                 {"limitation", capability.limitation}});
    }
    return MaiToolResult::success(Json{
        {"tool_kind", "model_backed"},
        {"bound_model", info.modelId},
        {"configured", info.configured},
        {"reply", reply},
        {"capabilities",
         std::move(capabilities)}}.dump());
}

MaiToolResult fail(MaiErrorCode error, const char* code, const std::string& message) {
    return MaiToolResult::failure(error, Json{{"code", code}, {"message", message}}.dump());
}

MaiToolResult invalid(const std::string& message) {
    return fail(MaiErrorCode::InvalidInput, "invalid_input", message);
}

bool isHttpsUrl(const std::string& url) {
    if (url.compare(0, 8, "https://") != 0) return false;
    CURLU* parsed = curl_url();
    if (parsed == nullptr) return false;
    const bool valid = curl_url_set(parsed, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK;
    char* host = nullptr;
    const bool hasHost = valid && curl_url_get(parsed, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
                         host != nullptr && *host != '\0';
    if (host != nullptr) curl_free(host);
    curl_url_cleanup(parsed);
    return hasHost;
}

bool validTaskId(const std::string& id) {
    return !id.empty() && id.size() <= 128 &&
           std::all_of(id.begin(), id.end(), [](unsigned char value) {
               return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
                      (value >= 'a' && value <= 'z') || value == '-' || value == '_';
           });
}

std::string lowerExtension(const std::string& path) {
    const std::string base = MaiFilePath::fromUtf8(path).baseName().toUtf8();
    const std::size_t dot = base.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string extension = base.substr(dot + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension;
}

std::string imageMime(const std::string& path) {
    const std::string extension = lowerExtension(path);
    if (extension == "jpg" || extension == "jpeg") return "image/jpeg";
    if (extension == "png") return "image/png";
    if (extension == "webp") return "image/webp";
    if (extension == "heic") return "image/heic";
    if (extension == "heif") return "image/heif";
    if (extension == "bmp") return "image/bmp";
    if (extension == "tif" || extension == "tiff") return "image/tiff";
    if (extension == "gif") return "image/gif";
    return {};
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

struct ArkResponse {
    Json data;
    std::optional<MaiToolResult> error;
};

struct ResponseBuffer {
    std::string bytes;
    bool exceeded = false;
};

std::size_t receiveResponse(char* data, std::size_t size, std::size_t count, void* user) {
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

ArkResponse requestJson(const std::string& url, const std::string& key, const std::string& caBundle,
                        const Json* body, const MaiToolContext& context) {
    maiAssertBlockingAllowed("ark_media_tool");
    CURL* curl = curl_easy_init();
    if (curl == nullptr)
        return {{}, fail(MaiErrorCode::Internal, "internal", "Could not initialize HTTP")};
    ResponseBuffer buffer;
    const std::string payload = body != nullptr ? body->dump() : std::string{};
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: Bearer " + key).c_str());
    headers = curl_slist_append(headers, "Accept: application/json");
    if (body != nullptr) headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receiveResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, body != nullptr ? 180L : 60L);
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
    if (buffer.exceeded)
        return {{}, fail(MaiErrorCode::Protocol, "protocol", "Ark response was too large")};
    if (result != CURLE_OK)
        return {{}, fail(MaiErrorCode::Network, "network", curl_easy_strerror(result))};
    Json object = Json::parse(buffer.bytes, nullptr, false);
    if (!object.is_object())
        return {{}, fail(MaiErrorCode::Protocol, "protocol", "Ark returned invalid JSON")};
    if (status < 200 || status >= 300) {
        const Json detail = object.value("error", Json::object());
        const std::string message = stringValue(detail, "message");
        return {{},
                fail(MaiErrorCode::Network, "ark_error",
                     message.empty() ? "Ark request failed (HTTP " + std::to_string(status) + ")"
                                     : message)};
    }
    return {std::move(object), std::nullopt};
}

std::optional<MaiToolResult> readImage(const std::string& candidate, const MaiToolContext& context,
                                       Json& image) {
    const std::string path = context.resolvePath(candidate);
    const std::string mime = imageMime(path);
    if (path.empty() || mime.empty())
        return invalid("image_path must be an accessible supported image");
    std::uint64_t size = 0;
    if (!MaiFileSystem::fileSize(MaiFilePath::fromUtf8(path), size) || size == 0 ||
        size > 20'000'000)
        return invalid("image_path must contain 1 to 20000000 bytes");
    std::string bytes;
    bool truncated = false;
    const MaiError error =
        MaiFileSystem::readFile(MaiFilePath::fromUtf8(path), bytes, 20'000'001, &truncated);
    if (error) return fail(MaiErrorCode::Internal, "io_error", error.message());
    if (truncated || bytes.size() > 20'000'000) return invalid("image_path exceeds 20000000 bytes");
    image = "data:" + mime + ";base64," + encodeBase64(bytes);
    return std::nullopt;
}

std::optional<MaiToolResult> outputName(const std::string& requested, const std::string& fallback,
                                        const char* extension, const MaiToolContext& context,
                                        std::string& relative) {
    relative = requested.empty() ? fallback : requested;
    if (context.root.empty() || MaiFilePath::fromUtf8(relative).isAbsolute() ||
        lowerExtension(relative) != extension ||
        maiResolvePathWithinRoot(context.root, relative).empty())
        return invalid(std::string("output_path must be a relative .") + extension +
                       " path inside the Agent workspace");
    return std::nullopt;
}

MaiToolResult downloadResult(const std::string& url, const std::string& relative, int maxSizeMb,
                             const MaiToolContext& context, bool png) {
    if (!isHttpsUrl(url))
        return fail(MaiErrorCode::Protocol, "protocol", "Ark returned no HTTPS media URL");
    auto downloader = makeMaiDownloadFileTool();
    const Json arguments = {
        {"url", url}, {"output_path", relative}, {"max_size_mb", maxSizeMb}, {"timeout_s", 180}};
    MaiToolResult result = downloader->execute(arguments.dump(), context);
    if (result.hasError()) return result;
    const Json downloaded = Json::parse(result.output(), nullptr, false);
    const std::string path = stringValue(downloaded, "path");
    std::string prefix;
    const MaiError readError = MaiFileSystem::readFile(MaiFilePath::fromUtf8(path), prefix, 12);
    const bool valid =
        !readError && (png ? prefix.size() >= 8 && prefix.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0
                           : prefix.size() >= 8 && prefix.compare(4, 4, "ftyp") == 0);
    if (!valid) {
        if (!path.empty()) (void)MaiFileSystem::removeFile(MaiFilePath::fromUtf8(path));
        return fail(MaiErrorCode::Protocol, "protocol", "Ark returned invalid media bytes");
    }
    Json output = downloaded;
    output["mime_type"] = png ? "image/png" : "video/mp4";
    return MaiToolResult::success(output.dump());
}

class MaiSeedanceVideoTool final : public MaiTool {
public:
    MaiSeedanceVideoTool(MaiArkApiKeyProvider provider, std::string caBundle)
        : mKey(std::move(provider)), mCaBundle(std::move(caBundle)) {}

    std::string name() const override {
        return "seedance_video";
    }
    bool requiresApproval(const std::string& raw) const override {
        return isPaidGenerationAction(raw);
    }
    bool requiresPerCallApproval(const std::string& raw) const override {
        return isPaidGenerationAction(raw);
    }
    std::optional<MaiSpecialistInfo> specialistInfo() const override {
        const bool configured = mKey && !mKey().empty();
        const auto unverified = configured ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                           : MaiSpecialistCapabilityStatus::NotConfigured;
        return MaiSpecialistInfo{
            name(),
            kVideoModel,
            configured,
            {{"text_to_video", true, true, unverified,
              "The shared C++ request path still needs a live Ark validation"},
             {"first_frame_to_video", true, true, unverified, {}},
             {"video_edit_from_url", true, true, unverified, {}},
             {"video_edit_from_seedance_task", true, true, unverified, {}},
             {"video_extend_from_url", true, true, unverified, {}},
             {"video_reference_from_url", true, true, unverified, {}},
             {"first_and_last_frame_to_video", true, true, unverified,
              "Requires image_path and last_frame_path"},
             {"multi_reference_video", true, true, MaiSpecialistCapabilityStatus::NotImplemented,
              "Multiple references are not wired"},
             {"video_edit_from_local_file", true, true,
              MaiSpecialistCapabilityStatus::UploadNotConfigured,
              "Local video upload is not configured"}}};
    }
    std::string description() const override {
        if (!specialistInfo()->configured)
            return "Seedance video specialist bound to doubao-seedance-2-0-260128. Ark API Key "
                   "is not configured on this device. Use discover for capabilities, or ask the "
                   "user to configure the key before delegating.";
        return "A model-backed Seedance video tool. Use discover for capabilities, delegate "
               "to start a task, or revise with a previous "
               "conversation_id and new feedback. Revision keeps the previous goal and source "
               "video in the same AI session. Text, first-frame, "
               "first-and-last-frame, and reference-video paths are implemented; cloud validation "
               "is still needed for the shared C++ path. Confirm duration, ratio and resolution "
               "with the user before paid create or reference calls. Local video "
               "upload is unavailable. Direct references containing real human faces are not "
               "supported. The app checks tasks and hands completed results back automatically; "
               "continue is for manual diagnostics only. Use agent_send_media to deliver a "
               "completed video.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("action":{"type":"string","enum":["discover","delegate","continue","revise"]},)"
               R"("message":{"type":"string"},"context":{"type":"string"},)"
               R"("conversation_id":{"type":"string"},"mode":{"type":"string",)"
               R"("enum":["create","edit","extend","reference"]},)"
               R"("image_path":{"type":"string"},"last_frame_path":{"type":"string"},)"
               R"("video_path":{"type":"string"},"video_url":{"type":"string"},)"
               R"("video_task_id":{"type":"string"},"poll_once":{"type":"boolean"},)"
               R"("production":{"type":"object",)"
               R"("properties":{"duration":{"type":"integer"},"ratio":{"type":"string"},)"
               R"("resolution":{"type":"string"},"generate_audio":{"type":"boolean"}},)"
               R"("additionalProperties":false}},"required":["action","message"],)"
               R"("additionalProperties":false})";
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object()) return invalid("arguments must be an object");
        for (const char* field :
             {"action", "message", "context", "conversation_id", "mode", "image_path",
              "last_frame_path", "video_path", "video_url", "video_task_id"}) {
            if (args.contains(field) && !args[field].is_string())
                return invalid(std::string(field) + " must be a string");
        }
        const std::string action = stringValue(args, "action");
        const std::string message = stringValue(args, "message");
        if (message.empty()) return invalid("message is required");
        if (action == "discover") {
            return discoverSpecialist(*specialistInfo(),
                                      "I can create a video from text and return a local MP4. "
                                      "Ask for the capability list before delegating reference "
                                      "media; local video upload is not configured.");
        }
        const std::string key = mKey ? mKey() : std::string{};
        if (key.empty())
            return fail(MaiErrorCode::NotConfigured, "not_configured",
                        "Configure an Ark API key in this platform's Agent settings");
        if (action == "delegate") return delegate(args, key, context);
        if (action == "continue") return continueTask(args, key, context);
        if (action == "revise") {
            if (context.specialistTasks == nullptr)
                return fail(MaiErrorCode::NotConfigured, "not_configured",
                            "Specialist task history is unavailable");
            const std::string previousId = stringValue(args, "conversation_id");
            MaiSpecialistTask previous;
            if (!context.specialistTasks->getSpecialistTask(previousId, context.sessionId,
                                                            previous) ||
                previous.specialistName != name() || previous.providerTaskId.empty())
                return fail(MaiErrorCode::NotFound, "not_found",
                            "Previous Seedance task was not found in this AI conversation");
            if (!stringValue(args, "video_url").empty() ||
                !stringValue(args, "video_path").empty() ||
                !stringValue(args, "video_task_id").empty())
                return invalid("revise uses the previous task video; do not override its source");
            Json revision = args;
            revision["mode"] = "edit";
            revision["video_task_id"] = previous.providerTaskId;
            revision["parent_task_id"] = previous.id;
            std::string summary = previous.contextSummary;
            if (!summary.empty()) summary += "\n";
            summary += "Previous goal: " + previous.intent;
            const std::string current = stringValue(args, "context");
            if (!current.empty()) summary += "\nCurrent constraints: " + current;
            if (summary.size() > 4000)
                return invalid("Revision context is too long; summarize the prior goal");
            revision["context"] = std::move(summary);
            return delegate(revision, key, context);
        }
        return invalid("action must be discover, delegate, continue, or revise");
    }

private:
    MaiToolResult delegate(const Json& args, const std::string& key,
                           const MaiToolContext& context) const {
        const std::string message = stringValue(args, "message");
        if (message.size() > 4000) return invalid("message must be at most 4000 characters");
        const std::string extra = stringValue(args, "context");
        if (extra.size() > 4000) return invalid("context must be at most 4000 characters");
        const std::string mode =
            stringValue(args, "mode").empty() ? "create" : stringValue(args, "mode");
        if (mode != "create" && mode != "edit" && mode != "extend" && mode != "reference")
            return invalid("unsupported video mode");
        const std::string imagePath = stringValue(args, "image_path");
        const std::string lastFramePath = stringValue(args, "last_frame_path");
        const std::string videoPath = stringValue(args, "video_path");
        const std::string videoUrl = stringValue(args, "video_url");
        const std::string videoTaskId = stringValue(args, "video_task_id");
        const int sourceCount = static_cast<int>(!videoPath.empty()) +
                                static_cast<int>(!videoUrl.empty()) +
                                static_cast<int>(!videoTaskId.empty());
        if ((mode == "create" && sourceCount != 0) || (mode != "create" && sourceCount != 1))
            return invalid("provide one video source for edit, extend, or reference only");
        if (!lastFramePath.empty() && (mode != "create" || imagePath.empty()))
            return invalid("last_frame_path requires create mode and image_path as first frame");
        if (!videoPath.empty())
            return fail(MaiErrorCode::NotConfigured, "upload_not_configured",
                        "Local video upload is unavailable; use video_url or video_task_id");
        const Json production = args.value("production", Json::object());
        if (!production.is_object()) return invalid("production must be an object");
        if ((mode == "create" || mode == "reference") &&
            (!production.contains("duration") || !production.contains("ratio") ||
             !production.contains("resolution")))
            return invalid("confirm duration, ratio, and resolution before video generation");
        if ((production.contains("duration") && !production["duration"].is_number_integer()) ||
            (production.contains("ratio") && !production["ratio"].is_string()) ||
            (production.contains("resolution") && !production["resolution"].is_string()) ||
            (production.contains("generate_audio") && !production["generate_audio"].is_boolean()))
            return invalid("production contains an invalid value");
        const int duration =
            production.value("duration", mode == "edit" || mode == "extend" ? -1 : 5);
        const std::string ratio = production.value("ratio", std::string("adaptive"));
        const std::string resolution = production.value("resolution", std::string("720p"));
        if (duration != -1 && (duration < 4 || duration > 15))
            return invalid("duration must be -1 or 4 to 15 seconds");
        if (ratio != "adaptive" && ratio != "16:9" && ratio != "9:16" && ratio != "1:1" &&
            ratio != "4:3" && ratio != "3:4" && ratio != "21:9")
            return invalid("unsupported video ratio");
        if (resolution != "480p" && resolution != "720p" && resolution != "1080p" &&
            resolution != "4k")
            return invalid("unsupported video resolution");
        std::string instruction = message;
        if (!extra.empty()) instruction += "\nRelevant context: " + extra;
        if (mode == "edit")
            instruction =
                "Edit reference video 1; keep unmentioned content unchanged. " + instruction;
        if (mode == "extend")
            instruction =
                "Extend reference video 1; preserve its subject and style. " + instruction;
        if (mode == "reference")
            instruction = "Create a video inspired by reference video 1. " + instruction;
        if (instruction.size() > 8000) return invalid("video instruction is too long");
        Json content = Json::array({Json{{"type", "text"}, {"text", instruction}}});
        if (!imagePath.empty()) {
            Json image;
            if (auto error = readImage(imagePath, context, image)) return *error;
            content.push_back(Json{{"type", "image_url"},
                                   {"image_url", {{"url", image}}},
                                   {"role", mode == "create" ? "first_frame" : "reference_image"}});
        }
        if (!lastFramePath.empty()) {
            Json lastFrame;
            if (auto error = readImage(lastFramePath, context, lastFrame)) return *error;
            content.push_back(Json{{"type", "image_url"},
                                   {"image_url", {{"url", lastFrame}}},
                                   {"role", "last_frame"}});
        }
        std::string reference = videoUrl;
        if (!videoTaskId.empty()) {
            if (!validTaskId(videoTaskId)) return invalid("invalid video_task_id");
            ArkResponse task = requestJson(std::string(kVideoTasksUrl) + "/" + videoTaskId, key,
                                           mCaBundle, nullptr, context);
            if (task.error) return *task.error;
            if (stringValue(task.data, "status") != "succeeded")
                return invalid("video_task_id is not a completed task");
            reference = stringValue(task.data.value("content", Json::object()), "video_url");
        }
        if (!reference.empty()) {
            if (!isHttpsUrl(reference)) return invalid("video_url must be a public HTTPS URL");
            content.push_back(Json{{"type", "video_url"},
                                   {"video_url", {{"url", reference}}},
                                   {"role", "reference_video"}});
        }
        const Json body = {{"model", kVideoModel},
                           {"content", content},
                           {"duration", duration},
                           {"ratio", ratio},
                           {"resolution", resolution},
                           {"generate_audio", production.value("generate_audio", true)}};
        ArkResponse result = requestJson(kVideoTasksUrl, key, mCaBundle, &body, context);
        if (result.error) return *result.error;
        const std::string taskId = stringValue(result.data, "id");
        if (!validTaskId(taskId))
            return fail(MaiErrorCode::Protocol, "protocol", "Ark returned no valid task ID");
        Json output = {{"task_id", taskId},
                       {"conversation_id", taskId},
                       {"status", stringValue(result.data, "status")},
                       {"bound_model", kVideoModel},
                       {"reply", "The video task has started. The app will report its result."}};
        if (context.specialistTasks != nullptr && !context.sessionId.empty()) {
            MaiSpecialistTask task;
            task.id = MaiIdGenerator::generate("spt_");
            task.ownerSessionId = context.sessionId;
            task.specialistName = name();
            task.parentTaskId = stringValue(args, "parent_task_id");
            task.providerTaskId = taskId;
            task.intent = message;
            task.contextSummary = extra;
            task.inputReference = !videoTaskId.empty() ? videoTaskId
                                  : !videoUrl.empty()  ? videoUrl
                                                       : imagePath;
            task.created = MaiTime::getCurrentTime();
            const MaiError stored = context.specialistTasks->insertSpecialistTask(task);
            if (stored) {
                output["context_persistence_warning"] = stored.message();
                output["reply"] =
                    "The paid video task was submitted, but revision context "
                    "could not be saved. Do not submit it again; continue with "
                    "the returned provider task ID.";
            } else {
                output["conversation_id"] = task.id;
                output["specialist_task_id"] = task.id;
                if (!task.parentTaskId.empty()) output["revision_of"] = task.parentTaskId;
            }
        }
        return MaiToolResult::success(output.dump());
    }

    MaiToolResult continueTask(const Json& args, const std::string& key,
                               const MaiToolContext& context) const {
        const std::string conversationId = stringValue(args, "conversation_id");
        if (!validTaskId(conversationId))
            return invalid("conversation_id is required and must be valid");
        std::string taskId = conversationId;
        if (conversationId.compare(0, 4, "spt_") == 0) {
            MaiSpecialistTask previous;
            if (context.specialistTasks == nullptr ||
                !context.specialistTasks->getSpecialistTask(conversationId, context.sessionId,
                                                            previous) ||
                previous.specialistName != name())
                return fail(MaiErrorCode::NotFound, "not_found",
                            "Seedance task was not found in this AI conversation");
            taskId = previous.providerTaskId;
        }
        ArkResponse task;
        std::string status;
        const int attempts = args.value("poll_once", false) ? 1 : 10;
        for (int attempt = 0; attempt < attempts; ++attempt) {
            task = requestJson(std::string(kVideoTasksUrl) + "/" + taskId, key, mCaBundle, nullptr,
                               context);
            if (task.error) return *task.error;
            status = stringValue(task.data, "status");
            if (status.empty())
                return fail(MaiErrorCode::Protocol, "protocol", "Ark returned no task status");
            if (status != "queued" && status != "running") break;
            if (attempt + 1 < attempts) {
                for (int tick = 0; tick < 50; ++tick) {
                    if (context.isCanceled())
                        return fail(MaiErrorCode::Canceled, "canceled", "Task was canceled");
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        }
        if (status == "failed" || status == "cancelled" || status == "expired") {
            const Json detail = task.data.value("error", Json::object());
            const std::string message = stringValue(detail, "message");
            return fail(MaiErrorCode::Network, "task_failed",
                        message.empty() ? "Seedance task " + status : message);
        }
        if (status != "succeeded")
            return MaiToolResult::success(
                Json{{"conversation_id", conversationId},
                     {"task_id", taskId},
                     {"status", status},
                     {"reply", "The video task is still " + status + "."},
                     {"notice", "continue checks status only; use revise for new feedback"}}
                    .dump());
        const std::string url =
            stringValue(task.data.value("content", Json::object()), "video_url");
        std::string relative;
        if (auto error = outputName({}, "seedance-" + taskId + ".mp4", "mp4", context, relative))
            return *error;
        const std::string path = maiResolvePathWithinRoot(context.root, relative);
        if (MaiFileSystem::exists(MaiFilePath::fromUtf8(path))) {
            std::uint64_t size = 0;
            if (!MaiFileSystem::fileSize(MaiFilePath::fromUtf8(path), size) || size == 0)
                return fail(MaiErrorCode::Internal, "io_error", "Existing video is empty");
            return MaiToolResult::success(
                Json{{"task_id", taskId},
                     {"conversation_id", conversationId},
                     {"status", status},
                     {"path", path},
                     {"bytes", size},
                     {"mime_type", "video/mp4"},
                     {"reply", "The video is complete and saved locally."}}
                    .dump());
        }
        MaiToolResult result = downloadResult(url, relative, 500, context, false);
        if (result.hasError()) return result;
        Json output = Json::parse(result.output());
        output.update(Json{{"task_id", taskId},
                           {"conversation_id", conversationId},
                           {"status", status},
                           {"reply", "The video is complete and saved locally."}});
        return MaiToolResult::success(output.dump());
    }

    MaiArkApiKeyProvider mKey;
    std::string mCaBundle;
};

class MaiSeedreamImageTool final : public MaiTool {
public:
    MaiSeedreamImageTool(MaiArkApiKeyProvider provider, std::string caBundle)
        : mKey(std::move(provider)), mCaBundle(std::move(caBundle)) {}

    std::string name() const override {
        return "seedream_image";
    }
    bool requiresApproval(const std::string& raw) const override {
        return isPaidGenerationAction(raw);
    }
    bool requiresPerCallApproval(const std::string& raw) const override {
        return isPaidGenerationAction(raw);
    }
    std::optional<MaiSpecialistInfo> specialistInfo() const override {
        const bool configured = mKey && !mKey().empty();
        const auto unverified = configured ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                           : MaiSpecialistCapabilityStatus::NotConfigured;
        return MaiSpecialistInfo{
            name(),
            kImageModel,
            configured,
            {{"text_to_image", true, true, unverified, {}},
             {"single_image_edit", true, true, unverified, {}},
             {"multi_image_edit", true, true, unverified,
              "Accepts 2 to 10 accessible images and produces one image"},
             {"interactive_region_edit", true, true, unverified,
              "Region coordinates must be supplied in the instruction"},
             {"layer_split", true, true, MaiSpecialistCapabilityStatus::NotImplemented,
              "Layer output and metadata are not wired"},
             {"image_series_output", false, false, MaiSpecialistCapabilityStatus::NotImplemented,
              "This model does not provide sequential image series output"}}};
    }
    std::string description() const override {
        if (!specialistInfo()->configured)
            return "Seedream image specialist bound to doubao-seedream-5-0-flash-260915. Ark API "
                   "Key is not configured on this device. Use discover for capabilities, or ask "
                   "the user to configure the key before delegating.";
        return "A model-backed Seedream image specialist. Use discover for capabilities, delegate "
               "a new goal, or revise with parent_task_id set to a previous specialist_task_id "
               "and new feedback. It can create from text, edit one image_path, or combine 2 to "
               "10 image_paths into one new PNG. "
               "Describe edits and desired output in the message, leaving creative details to "
               "Seedream; this specialist handles model "
               "request details. Use agent_send_media to deliver the result.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("action":{"type":"string","enum":["discover","delegate","revise"]},)"
               R"("message":{"type":"string"},"context":{"type":"string"},)"
               R"("image_path":{"type":"string"},"image_paths":{"type":"array",)"
               R"("items":{"type":"string"},"minItems":2,"maxItems":10},)"
               R"("parent_task_id":{"type":"string"},"output_path":{"type":"string"},)"
               R"("size":{"type":"string",)"
               R"("enum":["1K","1.5K","2K"]}},"required":["action","message"],)"
               R"("additionalProperties":false})";
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object()) return invalid("arguments must be an object");
        for (const char* field : {"action", "message", "context", "image_path", "parent_task_id",
                                  "output_path", "size"}) {
            if (args.contains(field) && !args[field].is_string())
                return invalid(std::string(field) + " must be a string");
        }
        if (args.contains("image_paths") && !args["image_paths"].is_array())
            return invalid("image_paths must be an array of 2 to 10 paths");
        const std::string action = stringValue(args, "action");
        const std::string message = stringValue(args, "message");
        if (message.empty()) return invalid("message is required");
        if (action == "discover") {
            return discoverSpecialist(*specialistInfo(),
                                      "I can create a PNG from text, edit one accessible image, "
                                      "or combine multiple accessible reference images into one. "
                                      "Layer splitting is not wired yet.");
        }
        const std::string key = mKey ? mKey() : std::string{};
        if (action != "delegate" && action != "revise")
            return invalid("action must be discover, delegate, or revise");
        if (key.empty())
            return fail(MaiErrorCode::NotConfigured, "not_configured",
                        "Configure an Ark API key in this platform's Agent settings");
        if (action == "revise") {
            if (context.specialistTasks == nullptr)
                return fail(MaiErrorCode::NotConfigured, "not_configured",
                            "Specialist task history is unavailable");
            MaiSpecialistTask previous;
            const std::string previousId = stringValue(args, "parent_task_id");
            if (!context.specialistTasks->getSpecialistTask(previousId, context.sessionId,
                                                            previous) ||
                previous.specialistName != name() || previous.outputPath.empty())
                return fail(MaiErrorCode::NotFound, "not_found",
                            "Previous Seedream image was not found in this AI conversation");
            if (!stringValue(args, "image_path").empty() || args.contains("image_paths"))
                return invalid("revise uses the previous output; do not override its source");
            args["image_path"] = previous.outputPath;
            std::string summary = previous.contextSummary;
            if (!summary.empty()) summary += "\n";
            summary += "Previous goal: " + previous.intent;
            const std::string current = stringValue(args, "context");
            if (!current.empty()) summary += "\nCurrent constraints: " + current;
            if (summary.size() > 3000)
                return invalid("Revision context is too long; summarize the prior goal");
            args["context"] = std::move(summary);
        }
        std::string prompt = message;
        const std::string extra = stringValue(args, "context");
        if (!extra.empty()) prompt += "\nRelevant context: " + extra;
        if (prompt.size() > 4000) return invalid("image task text is too long");
        const std::string size =
            stringValue(args, "size").empty() ? "2K" : stringValue(args, "size");
        if (size != "1K" && size != "1.5K" && size != "2K")
            return invalid("size must be 1K, 1.5K, or 2K");
        Json body = {{"model", kImageModel},   {"prompt", prompt},         {"size", size},
                     {"output_format", "png"}, {"response_format", "url"}, {"watermark", true}};
        const std::string imagePath = stringValue(args, "image_path");
        const Json imagePaths = args.value("image_paths", Json{});
        if (!imagePath.empty() && !imagePaths.is_null())
            return invalid("provide image_path or image_paths, not both");
        if (!imagePaths.is_null()) {
            if (!imagePaths.is_array() || imagePaths.size() < 2 || imagePaths.size() > 10)
                return invalid("image_paths must contain 2 to 10 paths");
            Json images = Json::array();
            std::size_t totalEncodedBytes = 0;
            for (const Json& candidate : imagePaths) {
                if (!candidate.is_string() || candidate.get<std::string>().empty())
                    return invalid("each image_paths entry must be a nonempty path");
                Json image;
                if (auto error = readImage(candidate.get<std::string>(), context, image))
                    return *error;
                const std::size_t encodedBytes = image.get_ref<const std::string&>().size();
                if (encodedBytes > 60'000'000 - totalEncodedBytes)
                    return invalid("combined image inputs exceed 60 MB after encoding");
                totalEncodedBytes += encodedBytes;
                images.push_back(std::move(image));
            }
            body["image"] = std::move(images);
        }
        if (!imagePath.empty()) {
            Json image;
            if (auto error = readImage(imagePath, context, image)) return *error;
            body["image"] = std::move(image);
        }
        std::string relative;
        if (auto error = outputName(stringValue(args, "output_path"),
                                    "seedream-" + MaiIdGenerator::generate("file_") + ".png", "png",
                                    context, relative))
            return *error;
        if (MaiFileSystem::exists(
                MaiFilePath::fromUtf8(maiResolvePathWithinRoot(context.root, relative))))
            return invalid("output_path already exists");
        ArkResponse response = requestJson(kImagesUrl, key, mCaBundle, &body, context);
        if (response.error) return *response.error;
        if (!response.data.contains("data") || !response.data["data"].is_array() ||
            response.data["data"].empty())
            return fail(MaiErrorCode::Protocol, "protocol", "Ark returned no image");
        const std::string url = stringValue(response.data["data"][0], "url");
        MaiToolResult result = downloadResult(url, relative, 50, context, true);
        if (result.hasError()) return result;
        Json output = Json::parse(result.output());
        output["model"] = kImageModel;
        output["size"] = size;
        if (context.specialistTasks != nullptr && !context.sessionId.empty()) {
            MaiSpecialistTask task;
            task.id = MaiIdGenerator::generate("spt_");
            task.ownerSessionId = context.sessionId;
            task.specialistName = name();
            task.parentTaskId = stringValue(args, "parent_task_id");
            task.intent = message;
            task.contextSummary = extra;
            task.inputReference = imagePath.empty() ? "multiple_images" : imagePath;
            task.outputPath = stringValue(output, "path");
            task.created = MaiTime::getCurrentTime();
            const MaiError stored = context.specialistTasks->insertSpecialistTask(task);
            if (stored) {
                output["context_persistence_warning"] = stored.message();
                output["revision_hint"] =
                    "The image was generated; do not repeat this paid "
                    "request. Use the returned path for any further edit.";
            } else {
                output["specialist_task_id"] = task.id;
                if (!task.parentTaskId.empty()) output["revision_of"] = task.parentTaskId;
            }
        }
        return MaiToolResult::success(output.dump());
    }

private:
    MaiArkApiKeyProvider mKey;
    std::string mCaBundle;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiSeedanceVideoTool(MaiArkApiKeyProvider apiKey,
                                                  std::string caBundlePath) {
    return std::make_unique<MaiSeedanceVideoTool>(std::move(apiKey), std::move(caBundlePath));
}

std::unique_ptr<MaiTool> makeMaiSeedreamImageTool(MaiArkApiKeyProvider apiKey,
                                                  std::string caBundlePath) {
    return std::make_unique<MaiSeedreamImageTool>(std::move(apiKey), std::move(caBundlePath));
}
