#include "MaiMiniMaxMediaTools.h"

#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "MaiCreativeMediaSupport.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiTime.h"

namespace {

using Json = nlohmann::json;
constexpr char kApiBase[] = "https://api.minimax.cn";

std::string value(const Json& data, const char* field) {
    return data.is_object() && data.contains(field) && data[field].is_string()
               ? data[field].get<std::string>()
               : std::string{};
}

std::string idValue(const Json& data, const char* field) {
    if (!data.is_object() || !data.contains(field)) return {};
    const Json& id = data[field];
    if (id.is_string()) return id.get<std::string>();
    return id.is_number_integer() ? std::to_string(id.get<std::int64_t>()) : std::string{};
}

enum class MediaKind { Image, Video, Audio };

struct Attachment {
    MediaKind kind;
    std::string role;
    std::string path;
};

struct UploadedReference {
    std::string reference;
    std::optional<MaiToolResult> error;
};

struct LocalReference {
    std::string path;
    std::string extension;
    std::uint64_t size = 0;
};

std::optional<MaiToolResult> inspectReference(const Attachment& attachment,
                                              const MaiToolContext& context,
                                              LocalReference& local) {
    local.path = context.resolvePath(attachment.path);
    if (local.path.empty()) return maiCreativeInvalid("Reference file is not accessible");
    const std::size_t dot = local.path.find_last_of('.');
    if (dot == std::string::npos)
        return maiCreativeInvalid("Reference file needs a supported extension");
    local.extension = local.path.substr(dot);
    std::transform(local.extension.begin(), local.extension.end(), local.extension.begin(),
                   [](unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
    const bool supported = attachment.kind == MediaKind::Image
                               ? local.extension == ".jpg" || local.extension == ".jpeg" ||
                                     local.extension == ".png" || local.extension == ".webp" ||
                                     local.extension == ".heic" || local.extension == ".heif"
                           : attachment.kind == MediaKind::Video
                               ? local.extension == ".mp4" || local.extension == ".mov"
                               : local.extension == ".wav" || local.extension == ".mp3";
    if (!supported) return maiCreativeInvalid("Reference file format is unsupported");
    const std::uint64_t maximum = attachment.kind == MediaKind::Image   ? 30'000'000
                                  : attachment.kind == MediaKind::Video ? 50'000'000
                                                                        : 15'000'000;
    if (!MaiFileSystem::fileSize(MaiFilePath::fromUtf8(local.path), local.size) ||
        local.size == 0 || local.size > maximum)
        return maiCreativeInvalid("Reference file exceeds its provider size limit");
    return std::nullopt;
}

UploadedReference uploadReference(const Attachment& attachment, const std::string& key,
                                  const std::string& caBundle, const MaiToolContext& context) {
    LocalReference local;
    if (auto error = inspectReference(attachment, context, local)) return {{}, *error};
    std::string bytes;
    bool truncated = false;
    if (MaiFileSystem::readFile(MaiFilePath::fromUtf8(local.path), bytes, local.size + 1,
                                &truncated) ||
        truncated)
        return {{}, maiCreativeInvalid("Reference file could not be read")};
    const MaiCreativeHttpResult uploaded =
        maiCreativeUploadFile(std::string(kApiBase) + "/v1/files/upload", key, caBundle,
                              "asset" + local.extension, bytes, "video_generation_input", context);
    if (uploaded.error) return {{}, *uploaded.error};
    const Json data = Json::parse(uploaded.body, nullptr, false);
    const std::string id = idValue(data.value("file", Json::object()), "file_id");
    if (!maiCreativeValidId(id))
        return {{},
                maiCreativeFailure(MaiErrorCode::Protocol, "protocol",
                                   "MiniMax upload returned no file ID")};
    return {"mm_file://" + id, std::nullopt};
}

std::optional<MaiToolResult> appendAttachment(std::vector<Attachment>& attachments, MediaKind kind,
                                              const std::string& role, const std::string& path) {
    if (path.empty()) return maiCreativeInvalid("Reference path is required");
    const bool validRole = kind == MediaKind::Image
                               ? role.empty() || role == "reference" || role == "first_frame" ||
                                     role == "last_frame" || role == "reference_image"
                           : kind == MediaKind::Video
                               ? role.empty() || role == "reference" || role == "reference_video"
                               : role.empty() || role == "reference" || role == "reference_audio";
    if (!validRole) return maiCreativeInvalid("Reference role does not match its media type");
    attachments.push_back({kind, role, path});
    return std::nullopt;
}

const char* referenceRole(MediaKind kind) {
    return kind == MediaKind::Image   ? "reference_image"
           : kind == MediaKind::Video ? "reference_video"
                                      : "reference_audio";
}

std::optional<MaiToolResult> validateH3OutputSpec(const Json& args, const std::string& model) {
    if (model != "MiniMax-H3" && model != "MiniMax-H3-Max")
        return maiCreativeInvalid("Select MiniMax-H3 or MiniMax-H3-Max");
    if (!args.contains("duration"))
        return maiCreativeInvalid("Missing top-level duration for paid video generation");
    if (!args["duration"].is_number_integer())
        return maiCreativeInvalid("duration must be an integer");
    if (!args.contains("resolution"))
        return maiCreativeInvalid("Missing top-level resolution for paid video generation");
    if (!args["resolution"].is_string()) return maiCreativeInvalid("resolution must be a string");
    const int duration = args["duration"].get<int>();
    const std::string resolution = value(args, "resolution");
    if (model == "MiniMax-H3" &&
        (duration < 4 || duration > 15 || (resolution != "768P" && resolution != "2K")))
        return maiCreativeInvalid(
            "MiniMax-H3 supports 4-15 seconds at 768P or 2K; "
            "native 1080P and 720P are unavailable");
    if (model == "MiniMax-H3-Max" &&
        (duration < 5 || duration > 15 || (resolution != "480P" && resolution != "768P")))
        return maiCreativeInvalid("MiniMax-H3-Max supports 5-15 seconds at 480P or 768P");
    return std::nullopt;
}

std::optional<MaiToolResult> prepareH3Request(const Json& args, const std::string& model,
                                              std::string prompt, const std::string& key,
                                              const std::string& caBundle,
                                              const MaiToolContext& context, bool localOnly,
                                              Json& body, std::string& inputReference) {
    std::vector<Attachment> attachments;
    const std::string first =
        value(args, "first_frame").empty() ? value(args, "image_path") : value(args, "first_frame");
    const std::string last = value(args, "last_frame").empty() ? value(args, "last_frame_path")
                                                               : value(args, "last_frame");
    if (!first.empty()) attachments.push_back({MediaKind::Image, "first_frame", first});
    if (!last.empty()) attachments.push_back({MediaKind::Image, "last_frame", last});
    const std::string referenceVideo =
        !value(args, "reference_video").empty()        ? value(args, "reference_video")
        : !value(args, "reference_video_path").empty() ? value(args, "reference_video_path")
                                                       : value(args, "video_path");
    if (!referenceVideo.empty())
        attachments.push_back({MediaKind::Video, "reference_video", referenceVideo});
    const std::string referenceAudio = !value(args, "reference_audio").empty()
                                           ? value(args, "reference_audio")
                                           : value(args, "reference_audio_path");
    if (!referenceAudio.empty())
        attachments.push_back({MediaKind::Audio, "reference_audio", referenceAudio});
    if (args.contains("reference_image_paths")) {
        if (!args["reference_image_paths"].is_array())
            return maiCreativeInvalid("reference_image_paths must be an array");
        for (const Json& path : args["reference_image_paths"]) {
            if (!path.is_string())
                return maiCreativeInvalid("reference_image_paths entries must be strings");
            attachments.push_back({MediaKind::Image, "reference_image", path.get<std::string>()});
        }
    }
    if (args.contains("content")) {
        if (!args["content"].is_array()) return maiCreativeInvalid("content must be an array");
        for (const Json& item : args["content"]) {
            if (!item.is_object()) return maiCreativeInvalid("content entries must be objects");
            if (item.contains("role") && !item["role"].is_string())
                return maiCreativeInvalid("content role must be a string");
            const std::string type = value(item, "type");
            if (type == "text") {
                const std::string text = value(item, "text");
                if (text.empty()) return maiCreativeInvalid("text content cannot be empty");
                if (!prompt.empty()) prompt += "\n";
                prompt += text;
                continue;
            }
            const MediaKind kind = type == "image_url"   ? MediaKind::Image
                                   : type == "video_url" ? MediaKind::Video
                                                         : MediaKind::Audio;
            if (type != "image_url" && type != "video_url" && type != "audio_url")
                return maiCreativeInvalid(
                    "content type must be text, image_url, video_url, or audio_url");
            if (auto error =
                    appendAttachment(attachments, kind, value(item, "role"), value(item, "path")))
                return error;
        }
    }
    const std::size_t imageCount = static_cast<std::size_t>(
        std::count_if(attachments.begin(), attachments.end(),
                      [](const Attachment& item) { return item.kind == MediaKind::Image; }));
    const bool referenceMode =
        imageCount > 1 ||
        std::any_of(attachments.begin(), attachments.end(), [](const Attachment& item) {
            return item.kind != MediaKind::Image || item.role == "reference" ||
                   item.role == "reference_image";
        });
    for (Attachment& item : attachments) {
        if (item.role == "reference") item.role = referenceRole(item.kind);
        if (item.role.empty())
            item.role = item.kind == MediaKind::Image && !referenceMode ? "first_frame"
                                                                        : referenceRole(item.kind);
    }
    if (auto error = validateH3OutputSpec(args, model)) return error;
    const int duration = args["duration"].get<int>();
    const std::string resolution = value(args, "resolution");
    if (prompt.empty() || prompt.size() > 8000)
        return maiCreativeInvalid("A text instruction is required and must be at most 8000 bytes");
    std::size_t firstCount = 0;
    std::size_t lastCount = 0;
    std::size_t referenceImages = 0;
    std::size_t referenceVideos = 0;
    std::size_t referenceAudios = 0;
    for (const Attachment& item : attachments) {
        if (item.path.empty()) return maiCreativeInvalid("Reference path is empty");
        if (item.role == "first_frame") ++firstCount;
        if (item.role == "last_frame") ++lastCount;
        if (item.role == "reference_image") ++referenceImages;
        if (item.role == "reference_video") ++referenceVideos;
        if (item.role == "reference_audio") ++referenceAudios;
    }
    if (firstCount > 1 || lastCount > 1 || referenceImages > 9 || referenceVideos > 3 ||
        referenceAudios > 3)
        return maiCreativeInvalid("Reference count exceeds the H3 input limit");
    const bool hasFrames = firstCount != 0 || lastCount != 0;
    const bool hasReferences = referenceImages != 0 || referenceVideos != 0 || referenceAudios != 0;
    if (hasFrames && hasReferences)
        return maiCreativeInvalid("Frame control and multimodal references cannot be combined");
    const std::string ratio = value(args, "ratio");
    const bool validRatio = ratio == "adaptive" || ratio == "21:9" || ratio == "16:9" ||
                            ratio == "4:3" || ratio == "1:1" || ratio == "3:4" || ratio == "9:16";
    if ((!ratio.empty() && !validRatio) ||
        (!hasFrames && !hasReferences && (ratio.empty() || ratio == "adaptive")))
        return maiCreativeInvalid("Confirm a concrete ratio for text-to-video");
    const std::string expansion = value(args, "prompt_expansion_mode");
    if (!expansion.empty() &&
        (model != "MiniMax-H3-Max" ||
         (expansion != "disabled" && expansion != "balanced" && expansion != "quality")))
        return maiCreativeInvalid("prompt_expansion_mode requires MiniMax-H3-Max");
    Json content = Json::array({Json{{"type", "text"}, {"text", prompt}}});
    for (const Attachment& item : attachments) {
        if (inputReference.empty()) inputReference = item.path;
        const char* type = item.kind == MediaKind::Image   ? "image_url"
                           : item.kind == MediaKind::Video ? "video_url"
                                                           : "audio_url";
        if (localOnly) {
            LocalReference local;
            if (auto error = inspectReference(item, context, local)) return error;
            content.push_back(Json{{"type", type},
                                   {"role", item.role},
                                   {"path", item.path},
                                   {"file_size_bytes", local.size}});
            continue;
        }
        const UploadedReference uploaded = uploadReference(item, key, caBundle, context);
        if (uploaded.error) return uploaded.error;
        content.push_back(
            Json{{"type", type}, {type, Json{{"url", uploaded.reference}}}, {"role", item.role}});
    }
    body = {{"model", model},
            {"content", std::move(content)},
            {"duration", duration},
            {"resolution", resolution},
            {"ratio", hasFrames       ? "adaptive"
                      : ratio.empty() ? "adaptive"
                                      : ratio}};
    if (!expansion.empty()) {
        body["extra"] = Json{{"prompt_expansion_mode", expansion}};
    }
    return std::nullopt;
}

std::string providerFailure(const MaiToolResult& result) {
    if (!result.hasError()) return {};
    const Json error = Json::parse(result.error().message(), nullptr, false);
    if (!error.is_object() || value(error, "code") != "provider_error") return {};
    const Json code = error.value("provider_code", Json{});
    const int number = code.is_number_integer() ? code.get<int>() : 0;
    const int status = error.value("http_status", Json{}).is_number_integer()
                           ? error["http_status"].get<int>()
                           : 0;
    const std::string message = value(error, "message");
    const bool sensitive =
        message.find("(1026)") != std::string::npos || message.find("(1027)") != std::string::npos;
    const bool terminal = number == 1004 || number == 1026 || number == 1027 || number == 2013 ||
                          sensitive || status == 401 || status == 403 || status == 422;
    return terminal ? (message.empty() ? "MiniMax provider rejected task" : message)
                    : std::string{};
}

class MaiMiniMaxMediaTool final : public MaiTool {
public:
    MaiMiniMaxMediaTool(bool video, MaiMiniMaxApiKeyProvider key, std::string caBundle)
        : mVideo(video), mKey(std::move(key)), mCaBundle(std::move(caBundle)) {}

    std::string name() const override {
        return mVideo ? "minimax_video" : "minimax_image";
    }
    std::string description() const override {
        return mVideo ? "MiniMax H3 paid video specialist. Delegate text, first/last frame images, "
                        "or multimodal reference images/videos/audio using workspace paths. "
                        "For content[] multi-image input, use role=reference_image; role=reference "
                        "or omitted roles are normalized. Roles on text items are ignored. "
                        "duration and resolution are top-level fields. H3 supports 768P or 2K "
                        "only; H3 Max supports 480P or 768P, neither supports native 1080P. "
                        "For an exact 1080P delivery, disclose a 2K generation plus local "
                        "downscale before paid approval. Model MiniMax-H3-Max is "
                        "selectable. Use validate to inspect local inputs without upload or "
                        "charge, then delegate after confirmation or continue a task. "
                        "The main Agent receives terminal results automatically."
                      : "MiniMax image-01 paid image specialist. Text-to-image and character "
                        "reference image generation are wired. Use discover or delegate after "
                        "confirmation; image generation completes in the delegate call.";
    }
    std::string parametersSchema() const override {
        return mVideo
                   ? R"({"type":"object","properties":{"action":{"type":"string","enum":["discover","validate","delegate","continue"]},"model":{"type":"string","enum":["MiniMax-H3","MiniMax-H3-Max"]},"message":{"type":"string"},"context":{"type":"string"},"image_path":{"type":"string"},"first_frame":{"type":"string"},"last_frame_path":{"type":"string"},"last_frame":{"type":"string"},"reference_image_paths":{"type":"array","items":{"type":"string"}},"reference_video_path":{"type":"string"},"reference_video":{"type":"string"},"video_path":{"type":"string"},"reference_audio_path":{"type":"string"},"reference_audio":{"type":"string"},"content":{"type":"array","items":{"type":"object","properties":{"type":{"type":"string","enum":["text","image_url","video_url","audio_url"]},"text":{"type":"string"},"path":{"type":"string"},"role":{"type":"string"}},"required":["type"]}},"duration":{"type":"integer"},"resolution":{"type":"string"},"ratio":{"type":"string"},"prompt_expansion_mode":{"type":"string"},"conversation_id":{"type":"string"},"parent_task_id":{"type":"string"},"poll_once":{"type":"boolean"}},"required":["action"]})"
                   : R"({"type":"object","properties":{"action":{"type":"string","enum":["discover","delegate"]},"message":{"type":"string"},"context":{"type":"string"},"image_path":{"type":"string"},"ratio":{"type":"string"}},"required":["action"]})";
    }
    std::optional<MaiSpecialistInfo> specialistInfo() const override {
        const bool configured = mKey && !mKey().empty();
        const auto ready = configured ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                      : MaiSpecialistCapabilityStatus::NotConfigured;
        MaiSpecialistInfo info;
        info.toolName = name();
        info.modelId = mVideo ? "MiniMax-H3" : "image-01";
        info.configured = configured;
        if (mVideo) {
            info.capabilities.push_back(
                {"text_to_video", true, true, ready,
                 "H3 V2 text-to-video completed a live account smoke test"});
            info.capabilities.push_back(
                {"image_to_video", true, true, ready,
                 "First frame uses source aspect ratio; PNG/JPEG/WEBP/HEIC/HEIF"});
            info.capabilities.push_back(
                {"first_last_frame_video", true, true, ready,
                 "One first and one last frame; source aspect ratio applies"});
            info.capabilities.push_back(
                {"multi_reference_video", true, true, ready,
                 "Up to 9 images, 3 videos, and 3 audio clips via file upload; "
                 "content image role reference_image or reference, or omit roles for multiple "
                 "images. Three-image H3 generation passed on the Mac shared core; iOS host "
                 "delivery remains unverified"});
            info.capabilities.push_back(
                {"native_audio_output", true, true, ready,
                 "H3 smoke test produced AAC; the V2 API has no separate audio-output switch"});
            info.capabilities.push_back(
                {"two_k_video", true, true, ready,
                 "MiniMax-H3 supports 768P or 2K, never native 1080P; H3 Max supports "
                 "480P or 768P"});
        } else {
            info.capabilities.push_back(
                {"text_to_image", true, true, ready,
                 "image-01 text generation completed a live account smoke test"});
            info.capabilities.push_back(
                {"character_reference_image", true, true, ready,
                 "Reference image must contain a character; PNG/JPEG under 5 MB"});
        }
        return info;
    }
    bool requiresApproval(const std::string& argumentsJson) const override {
        return requiresPerCallApproval(argumentsJson);
    }
    bool requiresPerCallApproval(const std::string& argumentsJson) const override {
        const Json args = Json::parse(argumentsJson, nullptr, false);
        const std::string action = value(args, "action");
        if (mVideo && action == "delegate" && args.is_object()) {
            const std::string model =
                value(args, "model").empty() ? "MiniMax-H3" : value(args, "model");
            if (validateH3OutputSpec(args, model)) return false;
        }
        return action != "discover" && action != "validate" && action != "continue";
    }
    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        const Json args = Json::parse(argumentsJson, nullptr, false);
        if (!args.is_object()) return maiCreativeInvalid("arguments must be a JSON object");
        for (const char* field :
             {"action", "model", "message", "context", "image_path", "first_frame",
              "last_frame_path", "last_frame", "reference_video_path", "reference_video",
              "video_path", "reference_audio_path", "reference_audio", "resolution", "ratio",
              "prompt_expansion_mode", "conversation_id", "parent_task_id"}) {
            if (args.contains(field) && !args[field].is_string())
                return maiCreativeInvalid(std::string(field) + " must be a string");
        }
        if (args.contains("duration") && !args["duration"].is_number_integer())
            return maiCreativeInvalid("duration must be an integer");
        if (args.contains("poll_once") && !args["poll_once"].is_boolean())
            return maiCreativeInvalid("poll_once must be boolean");
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
                                               {"local_validation", mVideo},
                                               {"capabilities", std::move(capabilities)},
                                               {"reply", "MiniMax capabilities listed"}}
                                              .dump());
        }
        if (action == "validate")
            return mVideo ? validate(args, context)
                          : maiCreativeInvalid("Local validation is only available for H3 video");
        if (action != "delegate" && action != "continue")
            return maiCreativeInvalid("action must be discover, validate, delegate, or continue");
        if (!mVideo && action == "continue")
            return maiCreativeInvalid("MiniMax image generation completes during delegate");
        if (args.contains("video_url"))
            return maiCreativeInvalid("Use workspace reference_video_path instead of video_url");
        const std::string key = mKey ? mKey() : std::string{};
        if (key.empty())
            return maiCreativeFailure(MaiErrorCode::NotConfigured, "not_configured",
                                      "MiniMax API key is missing in the host configuration");
        return action == "delegate" ? delegate(args, key, context)
                                    : continueTask(args, key, context);
    }

private:
    MaiToolResult validate(const Json& args, const MaiToolContext& context) const {
        if (context.root.empty()) return maiCreativeInvalid("Agent workspace is required");
        std::string model = value(args, "model");
        if (model.empty()) model = "MiniMax-H3";
        if (model != "MiniMax-H3" && model != "MiniMax-H3-Max")
            return maiCreativeInvalid("Select MiniMax-H3 or MiniMax-H3-Max");
        std::string prompt = value(args, "message");
        const std::string extra = value(args, "context");
        if (prompt.size() > 4000 || extra.size() > 4000)
            return maiCreativeInvalid("message/context must be at most 4000 bytes");
        if (!extra.empty()) prompt += "\nRelevant context: " + extra;
        Json body;
        std::string inputReference;
        if (auto error = prepareH3Request(args, model, prompt, {}, mCaBundle, context, true, body,
                                          inputReference))
            return *error;
        return MaiToolResult::success(Json{
            {"status", "validated"},
            {"submitted", false},
            {"uploaded", false},
            {"charged", false},
            {"request_preview",
             std::move(body)}}.dump());
    }

    MaiToolResult delegate(const Json& args, const std::string& key,
                           const MaiToolContext& context) const {
        const std::string message = value(args, "message");
        const std::string extra = value(args, "context");
        if (message.size() > 4000 || extra.size() > 4000)
            return maiCreativeInvalid("message/context must be at most 4000 bytes");
        if (context.root.empty()) return maiCreativeInvalid("Agent workspace is required");
        std::string prompt = message;
        if (!extra.empty()) prompt += "\nRelevant context: " + extra;
        if (!mVideo) return delegateImage(args, key, prompt, context);
        std::string model = value(args, "model");
        if (model.empty()) model = "MiniMax-H3";
        if (model != "MiniMax-H3" && model != "MiniMax-H3-Max")
            return maiCreativeInvalid("Select MiniMax-H3 or MiniMax-H3-Max");
        Json body;
        std::string inputReference;
        if (auto error = prepareH3Request(args, model, prompt, key, mCaBundle, context, false, body,
                                          inputReference))
            return *error;
        prompt = value(body["content"][0], "text");
        const std::string endpoint = std::string(kApiBase) + "/v2/video_generation";
        const std::string payload = body.dump();
        const MaiCreativeHttpResult submitted =
            maiCreativeRequestJson(endpoint, key, mCaBundle, &payload, context);
        if (submitted.error) return *submitted.error;
        const Json response = Json::parse(submitted.body, nullptr, false);
        const std::string providerId = idValue(response, "task_id");
        if (!maiCreativeValidId(providerId))
            return maiCreativeFailure(MaiErrorCode::Protocol, "protocol",
                                      "MiniMax returned no task ID");
        const std::string lookupId =
            model == "MiniMax-H3" ? "h3_" + providerId : "h3max_" + providerId;
        const MaiToolResult initial = continueTask(
            Json{{"conversation_id", lookupId}, {"input_reference", inputReference}}, key, context);
        if (!initial.hasError()) {
            const Json progress = Json::parse(initial.output(), nullptr, false);
            if (value(progress, "status") == "failed")
                return maiCreativeFailure(MaiErrorCode::Network, "task_failed",
                                          value(progress, "reply"));
            if (value(progress, "status") == "succeeded") return initial;
        }
        Json output = {{"task_id", providerId},
                       {"conversation_id", lookupId},
                       {"status", "submitted"},
                       {"bound_model", model},
                       {"reply", "The MiniMax task has started. The app will report its result."}};
        if (initial.hasError())
            output["initial_check_warning"] =
                "The first status check failed; background status checks will retry.";
        if (!value(args, "image_path").empty() || !value(args, "first_frame").empty())
            output["ratio_policy"] = "The first frame controls the output aspect ratio.";
        if (context.specialistTasks != nullptr && !context.sessionId.empty()) {
            MaiSpecialistTask task;
            task.id = MaiIdGenerator::generate("spt_");
            task.ownerSessionId = context.sessionId;
            task.specialistName = name();
            task.parentTaskId = value(args, "parent_task_id");
            task.providerTaskId = lookupId;
            task.intent = message.empty() ? prompt : message;
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

    MaiToolResult delegateImage(const Json& args, const std::string& key, const std::string& prompt,
                                const MaiToolContext& context) const {
        if (prompt.empty() || prompt.size() > 4000)
            return maiCreativeInvalid("message is required for image generation");
        const std::string ratio = value(args, "ratio");
        if (ratio != "16:9" && ratio != "9:16" && ratio != "1:1")
            return maiCreativeInvalid("Confirm ratio as 16:9, 9:16, or 1:1");
        Json body = {{"model", "image-01"},
                     {"prompt", prompt},
                     {"aspect_ratio", ratio},
                     {"response_format", "url"},
                     {"n", 1}};
        const std::string imagePath = value(args, "image_path");
        if (!imagePath.empty()) {
            std::string image;
            if (auto error = maiCreativeReadImage(imagePath, context, true, image)) return *error;
            body["subject_reference"] =
                Json::array({Json{{"type", "character"}, {"image_file", std::move(image)}}});
        }
        const std::string payload = body.dump();
        const MaiCreativeHttpResult result = maiCreativeRequestJson(
            std::string(kApiBase) + "/v1/image_generation", key, mCaBundle, &payload, context);
        if (result.error) return *result.error;
        const Json response = Json::parse(result.body, nullptr, false);
        const Json data = response.value("data", Json::object());
        if (!data.is_object() || !data.contains("image_urls") || !data["image_urls"].is_array() ||
            data["image_urls"].empty() || !data["image_urls"][0].is_string())
            return maiCreativeFailure(MaiErrorCode::Protocol, "protocol",
                                      "MiniMax returned no image URL");
        MaiToolResult media = maiCreativeDownloadMedia(data["image_urls"][0].get<std::string>(),
                                                       false, "minimax", context, mCaBundle);
        if (media.hasError()) return media;
        Json output = Json::parse(media.output());
        output["status"] = "succeeded";
        output["bound_model"] = "image-01";
        output["reply"] = "MiniMax image is ready for review at the returned path.";
        return MaiToolResult::success(output.dump());
    }

    MaiToolResult continueTask(const Json& args, const std::string& key,
                               const MaiToolContext& context) const {
        const std::string conversationId = value(args, "conversation_id");
        if (!maiCreativeValidId(conversationId))
            return maiCreativeInvalid("conversation_id must be a valid task ID");
        std::string lookupId = conversationId;
        std::string inputReference = value(args, "input_reference");
        if (conversationId.compare(0, 4, "spt_") == 0) {
            MaiSpecialistTask previous;
            if (context.specialistTasks == nullptr ||
                !context.specialistTasks->getSpecialistTask(conversationId, context.sessionId,
                                                            previous) ||
                previous.specialistName != name())
                return maiCreativeFailure(MaiErrorCode::NotFound, "not_found",
                                          "MiniMax task was not found in this conversation");
            if (previous.status == MaiSpecialistTaskStatus::Failed &&
                previous.errorText.rfind("Status check failed: ", 0) != 0)
                return failed(conversationId, previous.providerTaskId, previous.errorText,
                              !previous.inputReference.empty());
            if (previous.status == MaiSpecialistTaskStatus::Succeeded &&
                !previous.outputPath.empty())
                return MaiToolResult::success(Json{
                    {"conversation_id", conversationId},
                    {"status", "succeeded"},
                    {"path", previous.outputPath},
                    {"reply", previous.finalText}}.dump());
            lookupId = previous.providerTaskId;
            inputReference = previous.inputReference;
        }
        const bool h3Max = lookupId.compare(0, 6, "h3max_") == 0;
        const bool h3 = h3Max || lookupId.compare(0, 3, "h3_") == 0;
        if (!h3)
            return terminalFailure(conversationId, lookupId,
                                   "Legacy Hailuo 2.3 task is no longer supported", context,
                                   inputReference);
        const std::string providerId = lookupId.substr(h3Max ? 6 : 3);
        if (!maiCreativeValidId(providerId))
            return maiCreativeInvalid("MiniMax provider task ID is invalid");
        const std::string url = std::string(kApiBase) + "/v2/query/video_generation/" + providerId;
        const MaiCreativeHttpResult result =
            maiCreativeRequestJson(url, key, mCaBundle, nullptr, context);
        if (result.error) {
            const std::string reason = providerFailure(*result.error);
            if (!reason.empty())
                return terminalFailure(conversationId, providerId, reason, context, inputReference);
            return *result.error;
        }
        const Json response = Json::parse(result.body, nullptr, false);
        const Json task = response.value("task", Json::object());
        if (!task.is_object())
            return maiCreativeFailure(MaiErrorCode::Protocol, "protocol",
                                      "MiniMax H3 returned no task");
        const std::string status = value(task, "status");
        if (status == "queued" || status == "running") return running(conversationId, providerId);
        if (status == "failed" || status == "cancelled") {
            const Json error = task.value("error", Json::object());
            std::string reason = value(error, "message");
            if (reason.empty()) reason = "MiniMax H3 generation failed";
            const std::string code = value(error, "code");
            if (!code.empty()) reason += " (" + code + ")";
            return terminalFailure(conversationId, providerId, reason, context, inputReference);
        }
        if (status != "succeeded")
            return maiCreativeFailure(MaiErrorCode::Protocol, "protocol",
                                      "MiniMax H3 returned unknown task status");
        const Json content = task.value("content", Json::object());
        const std::string mediaUrl = value(content, "url");
        return finish(mediaUrl, conversationId, providerId, h3Max ? "MiniMax-H3-Max" : "MiniMax-H3",
                      task, context);
    }

    MaiToolResult running(const std::string& conversationId, const std::string& providerId) const {
        return MaiToolResult::success(Json{{"conversation_id", conversationId},
                                           {"task_id", providerId},
                                           {"status", "running"},
                                           {"reply", "MiniMax video is still processing."}}
                                          .dump());
    }

    MaiToolResult failed(const std::string& conversationId, const std::string& providerId,
                         const std::string& reason, bool hasReference) const {
        const bool imageExplicit = reason.find("image sensitive") != std::string::npos ||
                                   reason.find("image safety") != std::string::npos;
        const bool textLabeled = reason.find("text sensitive") != std::string::npos;
        const std::string attribution = imageExplicit ? "provider_identified_image"
                                        : textLabeled ? "provider_labeled_text"
                                                      : "provider_unspecified";
        return MaiToolResult::success(Json{
            {"conversation_id", conversationId},
            {"task_id", providerId},
            {"status", "failed"},
            {"input_reference_present", hasReference},
            {"cause_attribution", attribution},
            {"reply", reason.empty()
                          ? "MiniMax generation failed"
                          : reason}}.dump());
    }

    MaiToolResult terminalFailure(const std::string& conversationId, const std::string& providerId,
                                  const std::string& reason, const MaiToolContext& context,
                                  const std::string& inputReference) const {
        std::string explanation = reason.empty() ? "MiniMax generation failed" : reason;
        const bool moderation = explanation.find("1026") != std::string::npos ||
                                explanation.find("sensitive") != std::string::npos;
        if (moderation && !inputReference.empty() &&
            explanation.find("Reference media was included") == std::string::npos) {
            explanation +=
                " Reference media was included. The provider did not establish "
                "whether the media or text caused this rejection.";
        }
        if (conversationId.compare(0, 4, "spt_") == 0 && context.specialistTasks != nullptr) {
            (void)context.specialistTasks->finishSpecialistTask(
                conversationId, context.sessionId, MaiSpecialistTaskStatus::Failed, explanation, {},
                MaiTime::getCurrentTime());
        }
        return failed(conversationId, providerId, explanation, !inputReference.empty());
    }

    MaiToolResult finish(const std::string& url, const std::string& conversationId,
                         const std::string& providerId, const std::string& model, const Json& task,
                         const MaiToolContext& context) const {
        MaiToolResult media = maiCreativeDownloadMedia(url, true, "minimax", context, mCaBundle);
        if (media.hasError()) return media;
        Json output = Json::parse(media.output());
        output["conversation_id"] = conversationId;
        output["task_id"] = providerId;
        output["status"] = "succeeded";
        output["bound_model"] = model;
        if (!value(task, "ratio").empty()) output["ratio"] = value(task, "ratio");
        if (!value(task, "resolution").empty()) output["resolution"] = value(task, "resolution");
        output["reply"] = "MiniMax video is ready for review at the returned path.";
        return MaiToolResult::success(output.dump());
    }

    bool mVideo;
    MaiMiniMaxApiKeyProvider mKey;
    std::string mCaBundle;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiMiniMaxVideoTool(MaiMiniMaxApiKeyProvider apiKey,
                                                 std::string caBundlePath) {
    return std::make_unique<MaiMiniMaxMediaTool>(true, std::move(apiKey), std::move(caBundlePath));
}

std::unique_ptr<MaiTool> makeMaiMiniMaxImageTool(MaiMiniMaxApiKeyProvider apiKey,
                                                 std::string caBundlePath) {
    return std::make_unique<MaiMiniMaxMediaTool>(false, std::move(apiKey), std::move(caBundlePath));
}
