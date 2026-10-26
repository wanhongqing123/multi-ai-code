// 文件作用：把本地会话列表、新建、删除和工作目录切换暴露给模型，复用 MaiAgent 的普通操作入口。
#include "MaiSessionTools.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include <json.hpp>

#include "MaiAgent.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"

namespace {

using Json = nlohmann::json;

MaiResult<Json> parseArguments(const std::string& raw) {
    const Json parsed = Json::parse(raw, nullptr, false);
    if (!parsed.is_object())
        return {MaiErrorCode::InvalidInput, "tool arguments must be a JSON object"};
    return parsed;
}

MaiToolResult unavailable() {
    return MaiToolResult::failure(MaiErrorCode::NotSupported,
                                  "session management is unavailable in this host");
}

MaiToolResult failed(const MaiError& error) {
    return MaiToolResult::failure(error.code(), error.message());
}

class ListSessionsTool final : public MaiTool {
public:
    std::string name() const override {
        return "session_list";
    }
    std::string description() const override {
        return "List this device's top-level conversations with their IDs and titles. "
               "Use an ID from this list before asking to delete a conversation. "
               "Use offset to continue when results are truncated.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"offset":{"type":"integer",)"
               R"("minimum":0}},"required":[],"additionalProperties":false})";
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        if (context.sessionAgent == nullptr) return unavailable();
        const auto parsed = parseArguments(raw);
        if (!parsed) return failed(parsed.error());
        const Json& args = parsed.value();
        if (args.size() > 1 || (args.size() == 1 && !args.contains("offset")) ||
            (args.contains("offset") && !args["offset"].is_number_integer()))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "offset must be an integer from 0 to 10000");
        const long long requestedOffset =
            args.contains("offset") ? args["offset"].get<long long>() : 0;
        if (requestedOffset < 0 || requestedOffset > 10000)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "offset must be an integer from 0 to 10000");
        const int offset = static_cast<int>(requestedOffset);

        Json sessions = Json::array();
        bool truncated = false;
        int skipped = 0;
        for (const MaiSession& session : context.sessionAgent->listSessions()) {
            if (!session.isRoot()) continue;
            if (skipped < offset) {
                ++skipped;
                continue;
            }
            if (sessions.size() == 50) {
                truncated = true;
                break;
            }
            sessions.push_back({{"id", session.id},
                                {"title", session.title},
                                {"busy", context.sessionAgent->isBusy(session.id)}});
        }
        return MaiToolResult::success(Json{{"sessions", sessions},
                                           {"truncated", truncated},
                                           {"next_offset", truncated ? offset + 50 : 0}}
                                          .dump(),
                                      truncated);
    }
};

class CreateSessionTool final : public MaiTool {
public:
    CreateSessionTool(MaiSessionCreateMode mode, std::string defaultDirectory)
        : mMode(mode), mDefaultDirectory(std::move(defaultDirectory)) {}

    std::string name() const override {
        return "session_create";
    }
    std::string description() const override {
        if (mMode == MaiSessionCreateMode::DefaultOrExplicitDirectory) {
            return "Create a separate conversation on this computer. If the user names a "
                   "workspace, pass that absolute existing directory; otherwise use MaiChat's "
                   "default workspace. Do not guess a path or silently reuse the current "
                   "conversation's custom workspace. This does not switch the current view.";
        }
        return "Create a separate conversation on this device when the user asks for one. "
               "It inherits the current conversation's workspace and model; it does not switch "
               "the user's view. Do not use it merely to change the current workspace.";
    }
    std::string parametersSchema() const override {
        if (mMode == MaiSessionCreateMode::DefaultOrExplicitDirectory) {
            return R"({"type":"object","properties":{"title":{"type":"string",)"
                   R"("description":"Optional title for the new conversation"},)"
                   R"("directory":{"type":"string","description":"Absolute existing directory )"
                   R"(explicitly chosen by the user; omit for the default Workspace"}},"required":[],)"
                   R"("additionalProperties":false})";
        }
        return R"({"type":"object","properties":{"title":{"type":"string",)"
               R"("description":"Optional title for the new conversation"}},)"
               R"("required":[],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string& raw) const override {
        if (mMode != MaiSessionCreateMode::DefaultOrExplicitDirectory) return false;
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object()) return true;
        if (!args.contains("directory") || !args["directory"].is_string()) return false;
        const MaiFilePath path = MaiFilePath::fromUtf8(args["directory"].get<std::string>());
        return path.isAbsolute() && MaiFileSystem::isDirectory(path);
    }
    bool requiresPerCallApproval(const std::string& raw) const override {
        return requiresApproval(raw);
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        if (context.sessionAgent == nullptr) return unavailable();
        const auto parsed = parseArguments(raw);
        if (!parsed) return failed(parsed.error());
        const Json& args = parsed.value();
        const bool desktopDirectory = mMode == MaiSessionCreateMode::DefaultOrExplicitDirectory;
        if (args.size() > (desktopDirectory ? 2u : 1u) ||
            (args.contains("title") && !args["title"].is_string()) ||
            (args.contains("directory") && (!desktopDirectory || !args["directory"].is_string())))
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                desktopDirectory ? "session_create accepts an optional absolute directory and title"
                                 : "session_create accepts an optional string title");
        for (auto it = args.begin(); it != args.end(); ++it)
            if (it.key() != "title" && (it.key() != "directory" || !desktopDirectory))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "session_create has an unknown argument");
        const std::string title = args.value("title", std::string{});
        if (title.size() > 256 || std::any_of(title.begin(), title.end(), [](unsigned char byte) {
                return byte < 0x20 || byte == 0x7f;
            }))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "session title must be plain text of at most 256 bytes");

        MaiSession current;
        if (!context.sessionAgent->getSession(context.sessionId, current))
            return MaiToolResult::failure(MaiErrorCode::NotFound, "current session not found");
        if (!current.isRoot())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "sub-agents cannot create user conversations");
        if (!desktopDirectory && current.directory.empty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "current session has no workspace to inherit");

        std::string directory =
            desktopDirectory && !mDefaultDirectory.empty() ? mDefaultDirectory : current.directory;
        if (desktopDirectory && args.contains("directory")) {
            const std::string requested = args["directory"].get<std::string>();
            const MaiFilePath requestedPath = MaiFilePath::fromUtf8(requested);
            if (!requestedPath.isAbsolute() || !MaiFileSystem::isDirectory(requestedPath))
                return MaiToolResult::failure(
                    MaiErrorCode::InvalidInput,
                    "directory must be an absolute path to an existing folder chosen by the user");
            const MaiFilePath resolved = MaiFileSystem::resolve(requestedPath);
            if (resolved.isEmpty() || !MaiFileSystem::isDirectory(resolved))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "chosen directory is not accessible");
            directory = resolved.toUtf8();
        } else if (desktopDirectory) {
            const MaiFilePath defaultPath = MaiFilePath::fromUtf8(directory);
            if (!defaultPath.isAbsolute())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "default workspace is unavailable");
            const MaiError created = MaiFileSystem::createDirectories(defaultPath);
            if (created) return failed(created);
            const MaiFilePath resolved = MaiFileSystem::resolve(defaultPath);
            if (resolved.isEmpty() || !MaiFileSystem::isDirectory(resolved))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "default workspace is unavailable");
            directory = resolved.toUtf8();
        }

        const auto created =
            context.sessionAgent->submit(MaiCreateSession{directory, title, current.model});
        if (!created) return failed(created.error());
        return MaiToolResult::success(Json{
            {"session_id", created.value()},
            {"directory", directory},
            {"title", title.empty()
                          ? kMaiDefaultSessionTitle
                          : title}}.dump());
    }

private:
    MaiSessionCreateMode mMode;
    std::string mDefaultDirectory;
};

class DeleteSessionTool final : public MaiTool {
public:
    std::string name() const override {
        return "session_delete";
    }
    std::string description() const override {
        return "Permanently delete an idle conversation and its message history on this device "
               "after the user's explicit approval. Workspace files are not deleted. "
               "The conversation running this tool cannot delete itself.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"session_id":{"type":"string",)"
               R"("description":"ID from session_list"}},"required":["session_id"],)"
               R"("additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override {
        return true;
    }
    bool requiresPerCallApproval(const std::string&) const override {
        return true;
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        if (context.sessionAgent == nullptr) return unavailable();
        const auto parsed = parseArguments(raw);
        if (!parsed) return failed(parsed.error());
        const Json& args = parsed.value();
        if (args.size() != 1 || !args.contains("session_id") || !args["session_id"].is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "session_delete requires a session_id string");
        const std::string id = args["session_id"].get<std::string>();
        if (id.size() > 128 || id.rfind("ses_", 0) != 0)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "session_id must be a valid conversation ID");
        if (id == context.sessionId)
            return MaiToolResult::failure(MaiErrorCode::Busy,
                                          "cannot delete the conversation running this tool");

        MaiSession target;
        if (!context.sessionAgent->getSession(id, target))
            return MaiToolResult::failure(MaiErrorCode::NotFound, "session not found");
        if (!target.isRoot())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "only visible root conversations can be deleted");
        if (context.sessionAgent->isBusy(id))
            return MaiToolResult::failure(MaiErrorCode::Busy,
                                          "target conversation is still running");

        const auto deleted = context.sessionAgent->submit(MaiDeleteSession{id});
        if (!deleted) return failed(deleted.error());
        return MaiToolResult::success(Json{
            {"deleted_session_id", id}, {"title", target.title}, {"workspace_files_deleted", false}}
                                          .dump());
    }
};

class SetWorkspaceTool final : public MaiTool {
public:
    std::string name() const override {
        return "session_set_workspace";
    }
    std::string description() const override {
        return "Change an existing conversation's Workspace without creating a new Session. "
               "The path must be an absolute existing directory on this device. "
               "Changing the conversation running this tool takes effect on its next turn. "
               "Requires approval for each call.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"session_id":{"type":"string"},)"
               R"("workspace_path":{"type":"string"}},"required":["session_id",)"
               R"("workspace_path"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override {
        return true;
    }
    bool requiresPerCallApproval(const std::string&) const override {
        return true;
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        if (context.sessionAgent == nullptr) return unavailable();
        const auto parsed = parseArguments(raw);
        if (!parsed) return failed(parsed.error());
        const Json& args = parsed.value();
        if (args.size() != 2 || !args.contains("session_id") || !args["session_id"].is_string() ||
            !args.contains("workspace_path") || !args["workspace_path"].is_string())
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "session_set_workspace requires session_id and workspace_path strings");
        const std::string id = args["session_id"].get<std::string>();
        const std::string requested = args["workspace_path"].get<std::string>();
        if (id.size() > 128 || id.rfind("ses_", 0) != 0)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "session_id must be a valid conversation ID");
        if (requested.empty() || requested.size() > 4096 ||
            std::any_of(requested.begin(), requested.end(),
                        [](unsigned char byte) { return byte < 0x20 || byte == 0x7f; }))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "workspace_path must be a plain absolute path");
        const MaiFilePath path = MaiFilePath::fromUtf8(requested);
        if (!path.isAbsolute() || !MaiFileSystem::isDirectory(path))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "workspace_path must be an existing absolute directory");
        const MaiFilePath resolved = MaiFileSystem::resolve(path);
        if (resolved.isEmpty() || !MaiFileSystem::isDirectory(resolved))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "workspace_path is not accessible");
        MaiSession target;
        if (!context.sessionAgent->getSession(id, target))
            return MaiToolResult::failure(MaiErrorCode::NotFound, "session not found");
        if (!target.isRoot())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "only root conversations can change Workspace");
        const bool busy = context.sessionAgent->isBusy(id);
        if (busy && id != context.sessionId)
            return MaiToolResult::failure(MaiErrorCode::Busy,
                                          "target conversation is still running");
        const std::string directory = resolved.toUtf8();
        if (target.directory == directory)
            return MaiToolResult::success(Json{
                {"session_id", id},
                {"workspace_path", directory},
                {"changed", false},
                {"effective_next_turn",
                 false}}.dump());
        const auto changed =
            context.sessionAgent->submit(MaiUpdateSession{id, "", "", "", directory});
        if (!changed) return failed(changed.error());
        return MaiToolResult::success(Json{{"session_id", id},
                                           {"workspace_path", directory},
                                           {"changed", true},
                                           {"effective_next_turn", busy && id == context.sessionId}}
                                          .dump());
    }
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiCreateSessionTool(MaiSessionCreateMode mode,
                                                  std::string defaultDirectory) {
    return std::make_unique<CreateSessionTool>(mode, std::move(defaultDirectory));
}

void registerMaiSessionTools(MaiToolRegistry& registry, bool includeWorkspaceSwitch) {
    registry.add(std::make_unique<ListSessionsTool>());
    registry.add(makeMaiCreateSessionTool(MaiSessionCreateMode::InheritWorkspace));
    registry.add(std::make_unique<DeleteSessionTool>());
    if (includeWorkspaceSwitch) registry.add(std::make_unique<SetWorkspaceTool>());
}
