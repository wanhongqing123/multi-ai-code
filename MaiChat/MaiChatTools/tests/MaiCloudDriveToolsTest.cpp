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
    require(tools.specs().size() == 8, "cloud tool count changed");
    for (const auto& spec : tools.specs()) {
        require(nlohmann::json::parse(spec.parametersJson, nullptr, false).is_object(),
                "cloud tool schema is invalid JSON");
        MaiTool* tool = tools.findForModel(spec.name);
        require(tool != nullptr, "cloud tool is not model-visible");
        const bool write = spec.name == "maichat_cloud_drive_upload_media" ||
                           spec.name == "maichat_cloud_drive_resume_upload" ||
                           spec.name == "maichat_cloud_drive_abort_upload" ||
                           spec.name == "maichat_cloud_drive_delete_media";
        require(tool->requiresApproval("{}") == write, "cloud tool approval policy is wrong");
        require(tool->requiresPerCallApproval("{}") == write, "cloud write must be approved for every call");
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
            "abort should require approval on every call");
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
    MaiFileSystem::removeRecursively(directory);
    std::cout << "cloud drive Agent tool contract PASS\n";
}
