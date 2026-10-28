#include "MaiCloudDriveTools.h"

#include <cstdlib>
#include <iostream>
#include <string>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    MaiToolRegistry tools;
    int calls = 0;
    std::string lastName;
    std::string lastArguments;
    registerMaiCloudDriveTools(tools,
                               [&](const std::string& name, const std::string& arguments, const MaiToolContext&) {
                                   ++calls;
                                   lastName = name;
                                   lastArguments = arguments;
                                   return MaiToolResult::success("{\"ok\":true}");
                               });
    require(tools.specs().size() == 15, "cloud tool count changed");
    for (const auto& spec : tools.specs()) {
        require(nlohmann::json::parse(spec.parametersJson, nullptr, false).is_object(),
                "cloud tool schema is invalid JSON");
        MaiTool* tool = tools.findForModel(spec.name);
        require(tool != nullptr, "cloud tool is not model-visible");
        const bool write = spec.name == "maichat_cloud_drive_upload_media" ||
                           spec.name == "maichat_cloud_drive_resume_upload" ||
                           spec.name == "maichat_cloud_drive_abort_upload" ||
                           spec.name == "maichat_cloud_drive_delete_media" ||
                           spec.name == "maichat_cloud_drive_create_folder" ||
                           spec.name == "maichat_cloud_drive_update_folder" ||
                           spec.name == "maichat_cloud_drive_delete_folder" ||
                           spec.name == "maichat_cloud_drive_move_media" ||
                           spec.name == "maichat_cloud_drive_rename_media";
        require(tool->requiresApproval("{}") == write, "cloud tool approval policy is wrong");
        require(tool->requiresPerCallApproval("{}") == write,
                "cloud write must ask in prompting modes");
    }

    const MaiFilePath directory =
        MaiFileSystem::temporaryDirectory().append(MaiFilePath::fromUtf8(MaiIdGenerator::generate("cloud_tools_")));
    require(!MaiFileSystem::createDirectories(directory).hasError(), "cannot create temporary workspace");
    const MaiFilePath video = directory.append(MaiFilePath::fromUtf8("sample.MP4"));
    require(!MaiFileSystem::writeFile(video, "video").hasError(), "cannot create temporary video");
    const MaiFilePath image = directory.append(MaiFilePath::fromUtf8("sample.HEIC"));
    require(!MaiFileSystem::writeFile(image, "image").hasError(), "cannot create temporary image");
    MaiToolContext context;
    context.root = directory.toUtf8();

    MaiTool* upload = tools.findForModel("maichat_cloud_drive_upload_media");
    require(upload->execute(R"({"file_path":"sample.MP4"})", context).hasError() == false, "valid upload was rejected");
    require(lastName == upload->name() &&
                nlohmann::json::parse(lastArguments)["file_path"] == MaiFileSystem::resolve(video).toUtf8(),
            "upload did not pass a resolved local path");
    require(!upload->execute(R"({"file_path":"sample.HEIC"})", context).hasError(),
            "valid iPhone image upload was rejected");
    require(nlohmann::json::parse(lastArguments)["file_path"] == MaiFileSystem::resolve(image).toUtf8(),
            "image upload did not pass a resolved local path");
    const int beforeInvalid = calls;
    require(upload->execute(R"({"file_path":"../outside.mp4"})", context).hasError(), "upload escaped the workspace");
    require(upload->execute(R"({"file_path":"sample.MP4","secret":true})", context).hasError(),
            "unknown upload argument was accepted");
    require(calls == beforeInvalid, "invalid upload reached the host");

    MaiTool* list = tools.findForModel("maichat_cloud_drive_list_media");
    require(!list->execute("{}", context).hasError(), "read-only list failed");
    require(list->execute(R"({"before":""})", context).hasError(), "empty list cursor was accepted");
    require(!list->execute(R"({"folder_id":""})", context).hasError(),
            "root folder filter was rejected");
    require(nlohmann::json::parse(lastArguments)["folder_id"] == "",
            "root filter was lost");
    require(list->execute(R"({"folder_id":null})", context).hasError(),
            "null folder filter was accepted");
    MaiTool* createFolder = tools.findForModel("maichat_cloud_drive_create_folder");
    MaiTool* updateFolder = tools.findForModel("maichat_cloud_drive_update_folder");
    MaiTool* deleteFolder = tools.findForModel("maichat_cloud_drive_delete_folder");
    MaiTool* moveMedia = tools.findForModel("maichat_cloud_drive_move_media");
    require(!tools.findForModel("maichat_cloud_drive_list_folders")->execute("{}", context).hasError(),
            "folder tree was not exposed to the model");
    require(!createFolder->execute(R"({"name":"Travel","parent_id":"123E4567-E89B-42D3-A456-426614174000"})",
                                   context).hasError(), "nested folder create failed");
    require(nlohmann::json::parse(lastArguments)["parent_id"] == "123e4567-e89b-42d3-a456-426614174000",
            "folder parent ID was not normalized");
    require(!updateFolder->execute(R"({"folder_id":"123E4567-E89B-42D3-A456-426614174000","name":"Trips","parent_id":""})",
                                   context).hasError(), "folder rename and reparent failed");
    require(!moveMedia->execute(R"({"media_id":"123E4567-E89B-42D3-A456-426614174001","folder_id":""})",
                                context).hasError(), "move to root failed");
    const int beforeInvalidFolder = calls;
    for (const char* name : {"", "..", "a/b", "a\\b", " space", "line\n"}) {
        const auto arguments = nlohmann::json{{"name", name}}.dump();
        require(createFolder->execute(arguments, context).hasError(), "unsafe folder name reached host");
    }
    require(createFolder->execute(nlohmann::json{{"name", std::string("a\0b", 3)}}.dump(), context).hasError(),
            "embedded NUL folder name reached host");
    require(updateFolder->execute(R"({"folder_id":"123e4567-e89b-42d3-a456-426614174000"})",
                                   context).hasError(), "empty folder update reached host");
    require(deleteFolder->execute(R"({"folder_id":"bad"})", context).hasError(),
            "invalid folder deletion reached host");
    require(moveMedia->execute(R"({"media_id":"123e4567-e89b-42d3-a456-426614174000","folder_id":"bad"})",
                                context).hasError(), "invalid move target reached host");
    require(moveMedia->execute(R"({"media_id":"123e4567-e89b-42d3-a456-426614174000","folder_id":"","recursive":true})",
                                context).hasError(), "unknown move argument reached host");
    require(calls == beforeInvalidFolder, "invalid folder arguments reached host");
    MaiTool* rename = tools.findForModel("maichat_cloud_drive_rename_media");
    require(!rename->execute(R"({"media_id":"123E4567-E89B-42D3-A456-426614174000","name":"Travel.MOV"})",
                             context).hasError(), "valid file rename was rejected");
    require(lastName == rename->name() && nlohmann::json::parse(lastArguments)["media_id"] ==
                "123e4567-e89b-42d3-a456-426614174000", "rename ID was not normalized");
    const int beforeInvalidRename = calls;
    for (const char* name : {"", "../x.mp4", "a/b.mp4", "name", "x.pdf", "CON.mp4", "a\n.mp4"}) {
        const auto arguments = nlohmann::json{{"media_id", "123e4567-e89b-42d3-a456-426614174000"},
                                             {"name", name}}.dump();
        require(rename->execute(arguments, context).hasError(), "unsafe rename reached host");
    }
    require(rename->execute(R"({"media_id":"123e4567-e89b-42d3-a456-426614174000","name":"Travel.MOV","folder_id":""})",
                            context).hasError(), "extra rename argument reached host");
    require(calls == beforeInvalidRename, "invalid rename arguments reached host");
    require(tools.findForModel("maichat_cloud_drive_open_media")
                ->execute(R"({"media_id":"not-a-media-id"})", context)
                .hasError(),
            "invalid media ID reached host");
    require(tools.findForModel("maichat_cloud_drive_resume_upload")
                ->execute(R"({"task_id":"not-a-task"})", context)
                .hasError(),
            "invalid task ID reached host");
    MaiTool* pause = tools.findForModel("maichat_cloud_drive_pause_upload");
    MaiTool* abort = tools.findForModel("maichat_cloud_drive_abort_upload");
    require(pause && abort, "pause or abort tool is not model-visible");
    require(!pause->requiresApproval("{}") && !pause->requiresPerCallApproval("{}"),
            "pause should not require approval");
    require(abort->requiresApproval("{}") && abort->requiresPerCallApproval("{}"),
            "abort should ask in prompting modes");
    const int beforeInvalidTask = calls;
    require(pause->execute(R"({"task_id":"bad"})", context).hasError(), "invalid pause ID reached host");
    require(abort->execute(R"({"task_id":"bad"})", context).hasError(), "invalid abort ID reached host");
    require(pause->execute(R"({"task_id":"123e4567-e89b-42d3-a456-426614174000","x":1})", context)
                .hasError(), "extra pause argument reached host");
    require(calls == beforeInvalidTask, "invalid task arguments reached host");
    require(!pause->execute(R"({"task_id":"123E4567-E89B-42D3-A456-426614174000"})", context)
                 .hasError(), "valid pause ID was rejected");
    require(lastName == pause->name() && nlohmann::json::parse(lastArguments)["task_id"] ==
                "123e4567-e89b-42d3-a456-426614174000", "pause ID was not normalized");
    require(!abort->execute(R"({"task_id":"123e4567-e89b-42d3-a456-426614174000"})", context)
                 .hasError(), "valid abort ID was rejected");
    MaiTool* attachment = tools.findForModel("maichat_session_attachment_open");
    require(attachment != nullptr && !attachment->requiresApproval("{}"),
            "Session attachment open tool is unavailable");
    require(attachment->execute(R"({"attachment_id":"123e4567-e89b-42d3-a456-426614174000"})",
                                context).hasError(),
            "Session attachment tool accepted a missing current Session");
    context.sessionId = "ses_local";
    require(!attachment->execute(R"({"attachment_id":"123E4567-E89B-42D3-A456-426614174000"})",
                                 context).hasError(),
            "valid Session attachment was rejected");
    require(lastName == attachment->name() &&
            nlohmann::json::parse(lastArguments)["attachment_id"] ==
                "123e4567-e89b-42d3-a456-426614174000" &&
            nlohmann::json::parse(lastArguments)["session_id"] == "ses_local",
            "Session attachment tool did not bind the current Session");
    MaiFileSystem::removeRecursively(directory);
    std::cout << "cloud drive Agent tool contract PASS\n";
}
