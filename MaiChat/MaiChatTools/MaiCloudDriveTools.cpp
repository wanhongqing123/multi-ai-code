#include "MaiCloudDriveTools.h"
#include "MaiCloudDriveClient.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"

namespace {

using Json = nlohmann::json;

enum class Action { List, Pending, Upload, Pause, Resume, Abort, Play, Delete,
                    SessionOpen, ListFolders, CreateFolder, UpdateFolder,
                    DeleteFolder, MoveMedia };

const char* toolName(Action action) {
    switch (action) {
        case Action::List: return "maichat_cloud_drive_list_media";
        case Action::Pending: return "maichat_cloud_drive_list_pending_uploads";
        case Action::Upload: return "maichat_cloud_drive_upload_media";
        case Action::Pause: return "maichat_cloud_drive_pause_upload";
        case Action::Resume: return "maichat_cloud_drive_resume_upload";
        case Action::Abort: return "maichat_cloud_drive_abort_upload";
        case Action::Play: return "maichat_cloud_drive_open_media";
        case Action::Delete: return "maichat_cloud_drive_delete_media";
        case Action::SessionOpen: return "maichat_session_attachment_open";
        case Action::ListFolders: return "maichat_cloud_drive_list_folders";
        case Action::CreateFolder: return "maichat_cloud_drive_create_folder";
        case Action::UpdateFolder: return "maichat_cloud_drive_update_folder";
        case Action::DeleteFolder: return "maichat_cloud_drive_delete_folder";
        case Action::MoveMedia: return "maichat_cloud_drive_move_media";
    }
    return "";
}

const char* toolDescription(Action action) {
    switch (action) {
        case Action::List:
            return "Check whether this MaiChat account's private cloud drive is enabled, "
                   "then list its images, videos and quota. A not_configured result means the "
                   "feature is disabled; do not attempt other cloud drive actions. "
                   "Omit folder_id for all files, use an empty string for root, or a folder UUID "
                   "from maichat_cloud_drive_list_folders for that folder's direct files.";
        case Action::Pending:
            return "List this device's paused or active private cloud uploads. "
                   "The list never exposes local paths to the model. Remote resume still "
                   "requires an enabled account cloud drive.";
        case Action::Upload:
            return "Only after maichat_cloud_drive_list_media succeeds, upload an existing "
                   "local MP4 or MOV video (maximum 2 GB) or JPEG, PNG, WebP or HEIC image "
                   "(maximum 20 MB) to this account's private drive. Account quota is 10 GB. "
                   "Approval follows the Agent policy. A queued result is not completion.";
        case Action::Pause:
            return "Pause an active upload by task ID from maichat_cloud_drive_list_pending_uploads. "
                   "Confirmed parts remain available for later resume. A pausing result is a request, "
                   "not confirmation that the transfer has stopped.";
        case Action::Resume:
            return "Only while this account's private cloud drive is enabled, resume a "
                   "paused upload by task ID from maichat_cloud_drive_list_pending_uploads. "
                   "Approval follows the Agent policy.";
        case Action::Abort:
            return "Only while this account's private cloud drive is enabled, permanently discard "
                   "a paused upload by task ID from maichat_cloud_drive_list_pending_uploads. "
                   "This starts cleanup; reserved cloud space is released after cleanup completes. "
                   "Pause an active upload first. "
                   "Approval follows the Agent policy.";
        case Action::Play:
            return "Only while this account's private cloud drive is enabled, open one "
                   "of its images or videos in the device viewer. Use a media_id from "
                   "maichat_cloud_drive_list_media.";
        case Action::Delete:
            return "Only while this account's private cloud drive is enabled, permanently "
                   "delete one of its images or videos. Approval follows the Agent policy.";
        case Action::SessionOpen:
            return "Open an attachment ID already shared in this MaiChat Agent Session. "
                   "Download it from the account's separate private Session space into this "
                   "device so local file tools can inspect it. The ID must belong to this Session.";
        case Action::ListFolders:
            return "List this account's private cloud folder tree. Each folder has id, name and "
                   "parent_id (null at root). Use these IDs to browse or organize cloud media. "
                   "Folder membership is account-synced across devices.";
        case Action::CreateFolder:
            return "Create a named folder in this account's private cloud drive. Omit parent_id "
                   "or use an empty string for root, or supply a folder UUID for a nested folder. "
                   "Sibling folder names must be unique.";
        case Action::UpdateFolder:
            return "Rename or move a private cloud folder. Supply name, parent_id, or both; "
                   "empty parent_id moves it to root. Moving into itself or a descendant is rejected. "
                   "The folder's files and descendants remain together.";
        case Action::DeleteFolder:
            return "Delete an empty private cloud folder by ID. Nonempty folders are rejected. "
                   "Move files and child folders out first; this tool never recursively deletes files.";
        case Action::MoveMedia:
            return "Move one existing private cloud image or video to a folder by media_id. "
                   "Use an empty folder_id to move it to root. The file ID, content and cloud quota "
                   "stay unchanged; no file bytes are uploaded again.";
    }
    return "";
}

const char* toolSchema(Action action) {
    switch (action) {
        case Action::List:
            return R"({"type":"object","properties":{"before":{"type":"string","minLength":1,"maxLength":256},"folder_id":{"type":"string","maxLength":36}},"additionalProperties":false})";
        case Action::ListFolders:
        case Action::Pending: return R"({"type":"object","properties":{},"additionalProperties":false})";
        case Action::CreateFolder:
            return R"({"type":"object","properties":{"name":{"type":"string","minLength":1,"maxLength":240},"parent_id":{"type":"string","maxLength":36}},"required":["name"],"additionalProperties":false})";
        case Action::UpdateFolder:
            return R"({"type":"object","properties":{"folder_id":{"type":"string","minLength":36,"maxLength":36},"name":{"type":"string","minLength":1,"maxLength":240},"parent_id":{"type":"string","maxLength":36}},"required":["folder_id"],"additionalProperties":false})";
        case Action::DeleteFolder:
            return R"({"type":"object","properties":{"folder_id":{"type":"string","minLength":36,"maxLength":36}},"required":["folder_id"],"additionalProperties":false})";
        case Action::MoveMedia:
            return R"({"type":"object","properties":{"media_id":{"type":"string","minLength":36,"maxLength":36},"folder_id":{"type":"string","maxLength":36}},"required":["media_id","folder_id"],"additionalProperties":false})";
        case Action::SessionOpen:
            return R"({"type":"object","properties":{"attachment_id":{"type":"string","minLength":36,"maxLength":36}},"required":["attachment_id"],"additionalProperties":false})";
        case Action::Upload:
            return R"({"type":"object","properties":{"file_path":{"type":"string","minLength":1,"maxLength":4096}},"required":["file_path"],"additionalProperties":false})";
        case Action::Pause:
        case Action::Resume:
        case Action::Abort:
            return R"({"type":"object","properties":{"task_id":{"type":"string"}},"required":["task_id"],"additionalProperties":false})";
        case Action::Play:
        case Action::Delete:
            return R"({"type":"object","properties":{"media_id":{"type":"string"}},"required":["media_id"],"additionalProperties":false})";
    }
    return "{}";
}

bool validUuid(const std::string& value) {
    if (value.size() != 36) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!std::isxdigit(c)) {
            return false;
        }
    }
    return true;
}

bool onlyKeys(const Json& value, const char* allowed) {
    if (!value.is_object()) return false;
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.key() != allowed) return false;
    }
    return true;
}

bool onlyKeys(const Json& value, std::initializer_list<const char*> allowed) {
    if (!value.is_object()) return false;
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (std::none_of(allowed.begin(), allowed.end(),
                         [&](const char* key) { return it.key() == key; })) return false;
    }
    return true;
}

bool normalizeId(Json& arguments, const char* key, bool allowRoot = false) {
    if (!arguments.contains(key) || !arguments[key].is_string()) return false;
    std::string id = arguments[key].get<std::string>();
    if (allowRoot && id.empty()) return true;
    if (!validUuid(id)) return false;
    std::transform(id.begin(), id.end(), id.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    arguments[key] = id;
    return true;
}

bool validName(const Json& arguments) {
    if (!arguments.contains("name") || !arguments["name"].is_string()) return false;
    const std::string name = arguments["name"].get<std::string>();
    return name.find('\0') == std::string::npos &&
        mai_cloud_drive_folder_name_valid(name.c_str()) != 0;
}

MaiToolResult invalid(const std::string& message) {
    return MaiToolResult::failure(MaiErrorCode::InvalidInput, message);
}

class MaiCloudDriveTool final : public MaiTool {
public:
    MaiCloudDriveTool(Action action, MaiCloudDriveToolHandler handler)
        : mAction(action), mHandler(std::move(handler)) {}

    std::string name() const override {
        return toolName(mAction);
    }
    std::string description() const override {
        return toolDescription(mAction);
    }
    std::string parametersSchema() const override {
        return toolSchema(mAction);
    }
    bool requiresApproval(const std::string&) const override {
        return mAction == Action::Upload || mAction == Action::Resume ||
               mAction == Action::Abort || mAction == Action::Delete ||
               mAction == Action::CreateFolder || mAction == Action::UpdateFolder ||
               mAction == Action::DeleteFolder || mAction == Action::MoveMedia;
    }
    bool requiresPerCallApproval(const std::string& arguments) const override {
        return requiresApproval(arguments);
    }

    MaiToolResult execute(const std::string& argumentsJson, const MaiToolContext& context) override {
        if (!mHandler)
            return MaiToolResult::failure(MaiErrorCode::NotConfigured, "the private cloud drive is unavailable");
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "the cloud drive operation was canceled");
        Json arguments = Json::parse(argumentsJson, nullptr, false);
        if (!arguments.is_object()) return invalid("arguments must be an object");
        switch (mAction) {
            case Action::List:
                if (!onlyKeys(arguments, {"before", "folder_id"}) ||
                    (arguments.contains("before") &&
                     (!arguments["before"].is_string() || arguments["before"].get<std::string>().empty() ||
                      arguments["before"].get<std::string>().size() > 256)))
                    return invalid("before must be a nonempty cursor when supplied");
                if (arguments.contains("folder_id") && !normalizeId(arguments, "folder_id", true))
                    return invalid("folder_id must be a folder UUID or an empty string for root");
                break;
            case Action::ListFolders:
            case Action::Pending:
                if (!arguments.empty()) return invalid("this tool accepts no arguments");
                break;
            case Action::CreateFolder:
                if (!onlyKeys(arguments, {"name", "parent_id"}) || !validName(arguments) ||
                    (arguments.contains("parent_id") && !normalizeId(arguments, "parent_id", true)))
                    return invalid("name must be a valid folder name and parent_id a UUID or root");
                break;
            case Action::UpdateFolder:
                if (!onlyKeys(arguments, {"folder_id", "name", "parent_id"}) ||
                    !normalizeId(arguments, "folder_id") ||
                    (!arguments.contains("name") && !arguments.contains("parent_id")) ||
                    (arguments.contains("name") && !validName(arguments)) ||
                    (arguments.contains("parent_id") && !normalizeId(arguments, "parent_id", true)))
                    return invalid("supply folder_id and a valid name and/or parent_id");
                break;
            case Action::DeleteFolder:
                if (!onlyKeys(arguments, "folder_id") || !normalizeId(arguments, "folder_id"))
                    return invalid("folder_id must be a valid UUID");
                break;
            case Action::MoveMedia:
                if (!onlyKeys(arguments, {"media_id", "folder_id"}) ||
                    !normalizeId(arguments, "media_id") || !normalizeId(arguments, "folder_id", true))
                    return invalid("supply media_id and folder_id (an empty string for root)");
                break;
            case Action::SessionOpen: {
                if (!onlyKeys(arguments, "attachment_id") ||
                    !arguments.contains("attachment_id") ||
                    !arguments["attachment_id"].is_string() ||
                    !validUuid(arguments["attachment_id"].get<std::string>()) ||
                    context.sessionId.empty())
                    return invalid("attachment_id must belong to the current Session");
                std::string id = arguments["attachment_id"].get<std::string>();
                std::transform(id.begin(), id.end(), id.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                arguments["attachment_id"] = id;
                arguments["session_id"] = context.sessionId;
                break;
            }
            case Action::Upload: {
                if (!onlyKeys(arguments, "file_path") || !arguments.contains("file_path") ||
                    !arguments["file_path"].is_string())
                    return invalid("file_path is required");
                const std::string candidate = arguments["file_path"].get<std::string>();
                if (candidate.empty() || candidate.size() > 4096)
                    return invalid("file_path must be a local image or video path");
                const std::string resolved = context.resolvePath(candidate);
                if (resolved.empty()) return invalid("file_path is outside the authorized workspace");
                const MaiFilePath path = MaiFilePath::fromUtf8(resolved);
                if (!MaiFileSystem::exists(path) || MaiFileSystem::isDirectory(path))
                    return MaiToolResult::failure(MaiErrorCode::NotFound, "the local media file was not found");
                const std::size_t separator = resolved.find_last_of("/\\");
                const std::string name = resolved.substr(separator == std::string::npos ? 0 : separator + 1);
                MaiCloudDriveMediaInfo media{};
                if (!mai_cloud_drive_media_info(name.c_str(), &media))
                    return invalid("only MP4, MOV, JPEG, PNG, WebP and HEIC files can be uploaded");
                std::uint64_t bytes = 0;
                if (!MaiFileSystem::fileSize(path, bytes) || bytes == 0 || bytes > media.max_size_bytes)
                    return invalid(std::string("the ") + media.media_type_utf8 +
                        " exceeds its per-file size limit or is empty");
                arguments["file_path"] = resolved;
                break;
            }
            case Action::Pause:
            case Action::Resume:
            case Action::Abort:
            case Action::Play:
            case Action::Delete: {
                const char* key = mAction == Action::Pause || mAction == Action::Resume ||
                                          mAction == Action::Abort ? "task_id" : "media_id";
                if (!onlyKeys(arguments, key) || !arguments.contains(key) || !arguments[key].is_string() ||
                    !validUuid(arguments[key].get<std::string>()))
                    return invalid(std::string(key) + " must be a valid UUID");
                std::string id = arguments[key].get<std::string>();
                std::transform(id.begin(), id.end(), id.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                arguments[key] = id;
                break;
            }
        }
        return mHandler(name(), arguments.dump(), context);
    }

private:
    Action mAction;
    MaiCloudDriveToolHandler mHandler;
};

}  // namespace

void registerMaiCloudDriveTools(MaiToolRegistry& tools, MaiCloudDriveToolHandler handler) {
    for (Action action :
         {Action::List, Action::Pending, Action::Upload, Action::Pause,
          Action::Resume, Action::Abort, Action::Play, Action::Delete,
          Action::SessionOpen, Action::ListFolders, Action::CreateFolder,
          Action::UpdateFolder, Action::DeleteFolder, Action::MoveMedia}) {
        tools.add(std::make_unique<MaiCloudDriveTool>(action, handler));
    }
}
