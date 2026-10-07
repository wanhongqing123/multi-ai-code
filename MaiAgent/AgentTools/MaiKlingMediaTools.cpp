#include "MaiKlingMediaTools.h"

#include <json.hpp>

#include <string>
#include <utility>

#include "MaiCreativeMediaSupport.h"
#include "MaiIdGenerator.h"

namespace {

using Json = nlohmann::json;
constexpr char kApiBase[] = "https://api-beijing.klingai.com";

std::string value(const Json& data, const char* field) {
    return data.is_object() && data.contains(field) && data[field].is_string()
               ? data[field].get<std::string>()
               : std::string{};
}

class MaiKlingMediaTool final : public MaiTool {
public:
    MaiKlingMediaTool(bool video, MaiKlingApiKeyProvider key, std::string caBundle)
        : mVideo(video), mKey(std::move(key)), mCaBundle(std::move(caBundle)) {}

    std::string name() const override {
        return mVideo ? "kling_video" : "kling_image";
    }
    std::string description() const override {
        return mVideo
                   ? "Kling paid video specialist supporting 3-15 second output. Text and "
                     "first-frame video use Kling 3.0 "
                     "Turbo; first-and-last-frame video uses Kling 3.0. Discover capabilities, "
                     "delegate after confirmation, or continue an async task. Results return "
                     "to the main Agent automatically."
                   : "Kling Image 3.0 Omni paid specialist for text-to-image and image-to-image. "
                     "Discover capabilities, delegate after confirmation, or continue an async "
                     "task. Results return to the main Agent automatically.";
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
        info.modelId = mVideo ? "kling-3.0-turbo" : "kling-v3-omni";
        info.configured = configured;
        info.capabilities.push_back(
            {mVideo ? "text_to_video" : "text_to_image", true, true, ready,
             mVideo
                 ? "3-15 second output; paid provider generation remains unverified on this account"
                 : "Paid provider generation remains unverified on this account"});
        info.capabilities.push_back({mVideo ? "image_to_video" : "image_to_image", true, true,
                                     ready, "PNG/JPEG workspace image, maximum 5 MB"});
        if (mVideo) {
            info.capabilities.push_back({"first_last_frame_video", true, true, ready,
                                         "Routes to Kling 3.0 instead of Kling 3.0 Turbo"});
            info.capabilities.push_back({"existing_video_edit", false, false,
                                         MaiSpecialistCapabilityStatus::NotImplemented,
                                         "This tool does not upload an existing video"});
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
        if (!args.is_object()) return maiCreativeInvalid("arguments must be a JSON object");
        for (const char* field : {"action", "message", "context", "image_path", "last_frame_path",
                                  "resolution", "ratio", "conversation_id", "parent_task_id"}) {
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
                                               {"capabilities", std::move(capabilities)},
                                               {"reply", "Kling capabilities listed"}}
                                              .dump());
        }
        if (action != "delegate" && action != "continue")
            return maiCreativeInvalid("action must be discover, delegate, or continue");
        if (args.contains("video_path") || args.contains("video_url"))
            return maiCreativeInvalid("Existing-video editing is not wired in this tool");
        const std::string key = mKey ? mKey() : std::string{};
        if (key.empty())
            return maiCreativeFailure(MaiErrorCode::NotConfigured, "not_configured",
                                      "Kling API key is missing in the host configuration");
        return action == "delegate" ? delegate(args, key, context)
                                    : continueTask(args, key, context);
    }

private:
    MaiToolResult delegate(const Json& args, const std::string& key,
                           const MaiToolContext& context) const {
        const std::string message = value(args, "message");
        const std::string extra = value(args, "context");
        if (message.empty() || message.size() > 4000 || extra.size() > 4000)
            return maiCreativeInvalid(
                "message is required and message/context must be at most 4000 bytes");
        if (context.root.empty()) return maiCreativeInvalid("Agent workspace is required");
        std::string prompt = message;
        if (!extra.empty()) prompt += "\nRelevant context: " + extra;
        const std::string imagePath = value(args, "image_path");
        const std::string lastFramePath = value(args, "last_frame_path");
        if (!lastFramePath.empty() && (!mVideo || imagePath.empty()))
            return maiCreativeInvalid("last_frame_path requires a video first-frame image");
        const std::string ratio = value(args, "ratio");
        Json body;
        std::string endpoint;
        std::string modelId;
        if (mVideo) {
            if (!args.contains("duration") || !args.contains("resolution"))
                return maiCreativeInvalid(
                    "Confirm duration and resolution before paid video generation");
            const int duration = args["duration"].get<int>();
            const std::string resolution = value(args, "resolution");
            if (duration < 3 || duration > 15 || (resolution != "720p" && resolution != "1080p"))
                return maiCreativeInvalid(
                    "Kling duration must be 3-15 seconds and resolution 720p or 1080p");
            Json settings = {{"duration", duration}, {"resolution", resolution}};
            if (!imagePath.empty()) {
                std::string first;
                if (auto error = maiCreativeReadImage(imagePath, context, false, first))
                    return *error;
                Json content =
                    Json::array({Json{{"type", "prompt"}, {"text", prompt}},
                                 Json{{"type", "first_frame"}, {"url", std::move(first)}}});
                if (!lastFramePath.empty()) {
                    std::string last;
                    if (auto error = maiCreativeReadImage(lastFramePath, context, false, last))
                        return *error;
                    content.push_back(Json{{"type", "last_frame"}, {"url", std::move(last)}});
                }
                body = {{"contents", std::move(content)}, {"settings", std::move(settings)}};
                modelId = lastFramePath.empty() ? "kling-3.0-turbo" : "kling-3.0";
                endpoint = std::string(kApiBase) + (lastFramePath.empty()
                                                        ? "/image-to-video/kling-3.0-turbo"
                                                        : "/image-to-video/kling-3.0");
            } else {
                if (ratio != "16:9" && ratio != "9:16" && ratio != "1:1")
                    return maiCreativeInvalid("Confirm ratio as 16:9, 9:16, or 1:1");
                settings["aspect_ratio"] = ratio;
                body = {{"prompt", prompt}, {"settings", std::move(settings)}};
                modelId = "kling-3.0-turbo";
                endpoint = std::string(kApiBase) + "/text-to-video/kling-3.0-turbo";
            }
        } else {
            if (ratio != "16:9" && ratio != "9:16" && ratio != "1:1")
                return maiCreativeInvalid("Confirm ratio as 16:9, 9:16, or 1:1");
            body = {{"model_name", "kling-v3-omni"},
                    {"prompt", prompt},
                    {"resolution", "1k"},
                    {"n", 1},
                    {"aspect_ratio", ratio}};
            if (!imagePath.empty()) {
                std::string image;
                if (auto error = maiCreativeReadImage(imagePath, context, false, image))
                    return *error;
                body["image_list"] = Json::array({Json{{"image", std::move(image)}}});
            }
            modelId = "kling-v3-omni";
            endpoint = std::string(kApiBase) + "/v1/images/omni-image";
        }
        const std::string payload = body.dump();
        const MaiCreativeHttpResult submitted =
            maiCreativeRequestJson(endpoint, key, mCaBundle, &payload, context);
        if (submitted.error) return *submitted.error;
        const Json response = Json::parse(submitted.body, nullptr, false);
        const Json data = response.value("data", Json::object());
        const std::string providerId = value(data, mVideo ? "id" : "task_id");
        if (!maiCreativeValidId(providerId))
            return maiCreativeFailure(MaiErrorCode::Protocol, "protocol",
                                      "Kling returned no task ID");
        Json output = {{"task_id", providerId},
                       {"conversation_id", providerId},
                       {"status", "submitted"},
                       {"bound_model", modelId},
                       {"reply", "The Kling task has started. The app will report its result."}};
        if (context.specialistTasks != nullptr && !context.sessionId.empty()) {
            MaiSpecialistTask task;
            task.id = MaiIdGenerator::generate("spt_");
            task.ownerSessionId = context.sessionId;
            task.specialistName = name();
            task.parentTaskId = value(args, "parent_task_id");
            task.providerTaskId = providerId;
            task.intent = message;
            task.contextSummary = extra;
            task.inputReference = imagePath;
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
        if (!maiCreativeValidId(conversationId))
            return maiCreativeInvalid("conversation_id must be a valid task ID");
        std::string providerId = conversationId;
        if (conversationId.compare(0, 4, "spt_") == 0) {
            MaiSpecialistTask previous;
            if (context.specialistTasks == nullptr ||
                !context.specialistTasks->getSpecialistTask(conversationId, context.sessionId,
                                                            previous) ||
                previous.specialistName != name())
                return maiCreativeFailure(MaiErrorCode::NotFound, "not_found",
                                          "Kling task was not found in this conversation");
            if (previous.status == MaiSpecialistTaskStatus::Succeeded &&
                !previous.outputPath.empty())
                return MaiToolResult::success(Json{
                    {"conversation_id", conversationId},
                    {"status", "succeeded"},
                    {"path", previous.outputPath},
                    {"reply", previous.finalText}}.dump());
            providerId = previous.providerTaskId;
        }
        if (!maiCreativeValidId(providerId))
            return maiCreativeInvalid("Kling provider task ID is invalid");
        const std::string url = mVideo
                                    ? std::string(kApiBase) + "/tasks?task_ids=" + providerId
                                    : std::string(kApiBase) + "/v1/images/omni-image/" + providerId;
        const MaiCreativeHttpResult result =
            maiCreativeRequestJson(url, key, mCaBundle, nullptr, context);
        if (result.error) return *result.error;
        const Json response = Json::parse(result.body, nullptr, false);
        const Json payload = response.value("data", Json{});
        if (mVideo ? !payload.is_array() || payload.empty() : !payload.is_object())
            return maiCreativeFailure(MaiErrorCode::Protocol, "protocol",
                                      "Kling returned no task data");
        const Json task = mVideo ? payload[0] : payload;
        if (!task.is_object())
            return maiCreativeFailure(MaiErrorCode::Protocol, "protocol",
                                      "Kling returned invalid task data");
        const std::string status = value(task, mVideo ? "status" : "task_status");
        if (status == "submitted" || status == "processing")
            return MaiToolResult::success(Json{{"conversation_id", conversationId},
                                               {"task_id", providerId},
                                               {"status", "running"},
                                               {"reply", "Kling task is still processing."}}
                                              .dump());
        if (status == "failed")
            return maiCreativeFailure(MaiErrorCode::Network, "task_failed",
                                      value(task, mVideo ? "message" : "task_status_msg").empty()
                                          ? "Kling generation failed"
                                          : value(task, mVideo ? "message" : "task_status_msg"));
        if (status != (mVideo ? "succeeded" : "succeed"))
            return maiCreativeFailure(MaiErrorCode::Protocol, "protocol",
                                      "Kling returned unknown task status");
        const Json imageResult = task.value("task_result", Json::object());
        const Json outputs = mVideo                    ? task.value("outputs", Json::array())
                             : imageResult.is_object() ? imageResult.value("images", Json::array())
                                                       : Json::array();
        if (!outputs.is_array() || outputs.empty())
            return maiCreativeFailure(MaiErrorCode::Protocol, "protocol",
                                      "Kling returned no media result");
        std::string mediaUrl;
        for (const Json& output : outputs) {
            if (!mVideo || value(output, "type") == "video") {
                mediaUrl = value(output, "url");
                break;
            }
        }
        if (mediaUrl.empty())
            return maiCreativeFailure(MaiErrorCode::Protocol, "protocol",
                                      "Kling returned no expected media output");
        MaiToolResult media =
            maiCreativeDownloadMedia(mediaUrl, mVideo, "kling", context, mCaBundle);
        if (media.hasError()) return media;
        Json output = Json::parse(media.output());
        output["conversation_id"] = conversationId;
        output["task_id"] = providerId;
        output["status"] = "succeeded";
        if (!mVideo) output["bound_model"] = "kling-v3-omni";
        output["reply"] = "Kling media is ready for review at the returned path.";
        return MaiToolResult::success(output.dump());
    }

    bool mVideo;
    MaiKlingApiKeyProvider mKey;
    std::string mCaBundle;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiKlingVideoTool(MaiKlingApiKeyProvider apiKey,
                                               std::string caBundlePath) {
    return std::make_unique<MaiKlingMediaTool>(true, std::move(apiKey), std::move(caBundlePath));
}

std::unique_ptr<MaiTool> makeMaiKlingImageTool(MaiKlingApiKeyProvider apiKey,
                                               std::string caBundlePath) {
    return std::make_unique<MaiKlingMediaTool>(false, std::move(apiKey), std::move(caBundlePath));
}
