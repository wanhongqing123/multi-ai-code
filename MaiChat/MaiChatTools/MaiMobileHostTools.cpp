#include "MaiMobileHostTools.h"

#include <cstdlib>
#include <cstring>
#include <json.hpp>
#include <memory>
#include <mutex>
#include <string>

using Json = nlohmann::json;

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

namespace {

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

}  // namespace

std::shared_ptr<MaiMobileHostDispatcher> makeMaiMobileHostDispatcher() {
    return std::make_shared<MaiMobileHostDispatcher>();
}

bool setMaiMobileHostToolHandler(const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher,
                                 void* context, MaiMobileHostToolHandler handler,
                                 MaiMobileHostToolResponseFree responseFree,
                                 MaiMobileHostToolContextRelease contextRelease) {
    return dispatcher->set(context, handler, responseFree, contextRelease);
}

void clearMaiMobileHostToolHandler(const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher) {
    dispatcher->clear();
}

MaiToolResult callMaiMobileHostTool(const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher,
                                   const std::string& name, const std::string& argumentsJson) {
    return dispatcher->call(name, argumentsJson);
}

void addMaiMobileHostTool(MaiToolRegistry& tools, const char* name, const char* description,
                          const char* schema,
                          const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher,
                          bool approval) {
    tools.add(std::make_unique<MaiMobileHostTool>(name, description, schema, dispatcher, approval));
}

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
    add("maichat_send_media",
        "Send an accessible Agent workspace image or video as a persistent media bubble, "
        "or audio as a file card. The optional caption follows as a text message. "
        "Requires user approval.",
        R"({"type":"object","properties":{"peer_id":{"type":"string"},"file_path":{"type":"string"},"type":{"type":"string","enum":["image","video","audio"]},"caption":{"type":"string"}},"required":["peer_id","file_path","type"],"additionalProperties":false})",
        true);
    add("maichat_reply_message",
        "Send a quoted reply to a MaiChat message through the host. Requires user approval.",
        R"({"type":"object","properties":{"peer_id":{"type":"string"},"message_id":{"type":"string"},"text":{"type":"string"}},"required":["peer_id","message_id","text"]})",
        true);
    add("maichat_broadcast_text",
        "Send the same text to multiple MaiChat contacts through the host. The complete recipient list and text require user approval for every call.",
        R"({"type":"object","properties":{"peer_ids":{"type":"array","items":{"type":"string"},"minItems":1,"maxItems":200},"text":{"type":"string"}},"required":["peer_ids","text"]})",
        true);
    add("mobile_request_permission",
        "Ask the operating system for a MaiChat permission when a task needs it. "
        "Use photos before reading the system gallery. Ask only for the capability needed by "
        "the current task. Returns status, whether the system prompted, whether Settings is "
        "required, and the access level (including approximate or precise location). A limited "
        "photo grant can still expose selected items. Location is foreground-only.",
        R"({"type":"object","properties":{"permission":{"type":"string","enum":["photos","camera","microphone","location","contacts","calendar","notifications"]}},"required":["permission"],"additionalProperties":false})");
    add("mobile_get_location",
        "Read one current foreground location after location permission is granted. "
        "Returns latitude, longitude, horizontal accuracy, optional altitude, timestamp, "
        "and source. If permission is missing, call mobile_request_permission first.",
        R"({"type":"object","properties":{},"additionalProperties":false})");
    add("maichat_play_video",
        "Open a video from the Agent workspace in MaiChat's FFplay/Graphics popup. "
        "IM video messages use the same player.",
        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false})");
    add("maichat_video_command",
        "Control the currently open FFplay video: play, pause, step, relative, chapter, or percent seek, "
        "stream and filter cycling, mute, volume, fullscreen, or close.",
        R"({"type":"object","properties":{"action":{"type":"string","enum":["play","pause","toggle_pause","step","seek_forward","seek_backward","seek_minute_forward","seek_minute_backward","next_chapter","previous_chapter","seek_percent","next_audio","next_video","next_subtitle","next_program","next_filter","mute","volume_up","volume_down","fullscreen","close"]},"percent":{"type":"number","minimum":0,"maximum":100}},"required":["action"],"additionalProperties":false})");
}

void registerMobilePhotoTools(MaiToolRegistry& tools,
                              const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher) {
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_list_photos",
        "List photos, Live Photos, and videos visible in the system library after OS authorization. "
        "Each item includes mediaType; use offset and limit to page. A limited grant exposes "
        "only the user's selected media.",
        R"({"type":"object","properties":{"offset":{"type":"integer","minimum":0},"limit":{"type":"integer","minimum":1,"maximum":100},"album_id":{"type":"string"}}})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_list_albums",
        "List system photo albums available under the current OS photo-library permission.",
        R"({"type":"object","properties":{}})", dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_read_photo",
        "Copy one bounded JPEG preview by ID into the Agent working directory (up to 2048px). "
        "This loses original resolution and wide-gamut metadata; never use it as an editing "
        "source. Use mobile_export_photo_original before image processing. Use "
        "mobile_preview_image to show it to the user, or view_image with a vision-capable model. "
        "Only images authorized by the OS photo-library permission can be read.",
        R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_export_photo_original",
        "Copy the original photo or video bytes by ID from the authorized system library into "
        "the Agent working directory, preserving the source format and color metadata. Use this "
        "for any image edit or media conversion. The library item stays unchanged. For an iOS Live Photo, "
        "component=video exports its paired video; otherwise its still photo is exported.",
        R"({"type":"object","properties":{"id":{"type":"string"},"component":{"type":"string","enum":["photo","video"]}},"required":["id"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_export_media_original",
        "Export the original photo or video file from the authorized system library into the "
        "Agent workspace, preserving its media format. Use this for videos before ffprobe, "
        "ffmpeg, or playback. A Live Photo exports its still image unless component=video.",
        R"({"type":"object","properties":{"id":{"type":"string"},"component":{"type":"string","enum":["photo","video"]}},"required":["id"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_save_image",
        "Save an image from the Agent working directory as a NEW photo in the system library. "
        "The source file and all existing library photos remain unchanged. Pass a relative path "
        "inside the Agent working directory.",
        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_transform_image",
        "Create a NEW image in the Agent working directory using an on-device edit; the source "
        "is unchanged. Apply one operation per call: crop (top-left x,y,width,height), rotate "
        "(degrees 90/180/270 clockwise), resize (width,height), flip_horizontal, flip_vertical, "
        "grayscale, sharpen (amount 0..2), adjust (brightness -1..1, contrast 0..2, "
        "saturation 0..2), or beautify "
        "(strength 0..1, mild whole-image smoothing and brightening, not face-aware). Call "
        "mobile_preview_image on the returned path, then mobile_save_image to add the result "
        "to the system gallery if requested.",
        R"({"type":"object","properties":{"path":{"type":"string"},"operation":{"type":"string","enum":["crop","rotate","resize","flip_horizontal","flip_vertical","grayscale","sharpen","adjust","beautify"]},"x":{"type":"integer"},"y":{"type":"integer"},"width":{"type":"integer"},"height":{"type":"integer"},"degrees":{"type":"integer"},"brightness":{"type":"number"},"contrast":{"type":"number"},"saturation":{"type":"number"},"strength":{"type":"number"},"amount":{"type":"number"}},"required":["path","operation"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_beautify_image",
        "Make a NEW portrait-friendly image using mild whole-image smoothing, brightening, and "
        "slightly richer colors. This is not face-aware skin retouching. Strength is 0..1. "
        "The source remains unchanged; preview the returned path before saving it.",
        R"({"type":"object","properties":{"path":{"type":"string"},"strength":{"type":"number","minimum":0,"maximum":1}},"required":["path"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_image_info",
        "Read dimensions, format, and byte size of an image inside the Agent working directory. "
        "Use this before crop or resize when exact source dimensions are needed.",
        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_detect_faces",
        "Find faces and facial landmarks in an image from the Agent working directory using "
        "on-device vision. Return top-left pixel bounds and landmark positions for local image "
        "editing; this does not identify who a person is or modify the source image.",
        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_segment_person",
        "Create a grayscale person-versus-background mask from an image in the Agent working "
        "directory using on-device vision. White means person and black means background. "
        "Return a NEW mask PNG path and dimensions; do not modify the source image.",
        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_preview_image",
        "Show an image from the Agent working directory to the user in a full-screen mobile "
        "preview. This does not require a vision-capable model and does not save or modify it. "
        "Use after mobile_read_photo or mobile_transform_image so the user can inspect it.",
        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})",
        dispatcher, false));
    registerPlatformPhotoAlbumTools(tools, dispatcher);
}
