#include "MaiModelStudioTools.h"

#include <json.hpp>

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <string>
#include <thread>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiMemoryStore.h"

namespace {

int failures = 0;

#define CHECK(condition)                                                                 \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #condition); \
            ++failures;                                                                  \
        }                                                                                \
    } while (false)

void testRevisionIsSessionBound() {
    auto store = makeMaiMemoryStore();
    MaiSession owner;
    owner.id = "owner";
    store->putSession(owner);
    MaiSpecialistTask previous;
    previous.id = MaiIdGenerator::generate("spt_");
    previous.ownerSessionId = "owner";
    previous.specialistName = "wan_video";
    previous.providerTaskId = "valid-task-id";
    CHECK(!store->insertSpecialistTask(previous));
    MaiToolContext context;
    context.sessionId = "other";
    context.specialistTasks = store.get();
    auto tool = makeMaiWanVideoTool([] { return MaiWanCredentials{"test-key", "ws-test123"}; });
    const auto result = tool->execute(nlohmann::json{{"action", "revise"},
                                                     {"message", "Change only the collar"},
                                                     {"conversation_id", previous.id}}
                                          .dump(),
                                      context);
    CHECK(result.hasError());
    CHECK(result.error().message().find("not_found") != std::string::npos);
}

void testOtherSpecialistBoundaries() {
    MaiWanCredentials credentials;
    auto video = makeMaiWanVideoTool([&] { return credentials; });
    auto image = makeMaiWanImageTool([&] { return credentials; });
    CHECK(video->requiresPerCallApproval(R"({"action":"delegate"})"));
    CHECK(image->requiresPerCallApproval(R"({"action":"delegate"})"));
    CHECK(!video->requiresPerCallApproval(R"({"action":"continue"})"));
    CHECK(video
              ->execute(R"({"action":"continue","message":"","conversation_id":"../other"})",
                        MaiToolContext{})
              .hasError());
    MaiToolContext context;
    context.root = MaiFileSystem::temporaryDirectory().toUtf8();
    const auto videoInfo = video->execute(R"({"action":"discover"})", context);
    const auto imageInfo = image->execute(R"({"action":"discover"})", context);
    CHECK(!videoInfo.hasError());
    CHECK(!imageInfo.hasError());
    CHECK(nlohmann::json::parse(videoInfo.output()).at("bound_model") == "wan3.0-video");
    CHECK(nlohmann::json::parse(imageInfo.output()).at("bound_model") == "wan2.7-image");
    CHECK(nlohmann::json::parse(videoInfo.output()).at("missing_configuration").size() == 2);
    CHECK(nlohmann::json::parse(imageInfo.output()).at("missing_configuration").size() == 2);
    credentials = {"test-key", "ws-test123"};
    CHECK(video->specialistInfo()->configured);
    CHECK(image->specialistInfo()->configured);
    const nlohmann::json wanSchema = nlohmann::json::parse(video->parametersSchema());
    CHECK(wanSchema["properties"].contains("file_path"));
    CHECK(wanSchema["properties"].contains("file_url"));
    CHECK(wanSchema["properties"].contains("link_url"));
    CHECK(wanSchema["properties"]["duration"]["description"].get<std::string>().find("30") !=
          std::string::npos);
    CHECK(wanSchema["properties"]["local_image_paths"]["description"].get<std::string>().find(
              "ten") != std::string::npos);
    const auto wanCapabilities =
        nlohmann::json::parse(video->execute(R"({"action":"discover"})", context).output());
    CHECK(wanCapabilities.dump().find("reference_audio") != std::string::npos);
    CHECK(wanCapabilities.at("capabilities")[0].contains("model_support"));
    CHECK(wanCapabilities.at("capabilities")[0].contains("api_support"));
    for (const auto& capability : wanCapabilities.at("capabilities"))
        CHECK(!capability.at("limitation").get<std::string>().empty());
    CHECK(nlohmann::json::parse(video->execute(R"({"action":"discover"})", context).output())
              .dump()
              .find("reference_file") != std::string::npos);
    const nlohmann::json filePlan = {{"action", "delegate"},
                                     {"mode", "reference"},
                                     {"message", "Make a product video from the proposal"},
                                     {"file_url", "https://example.com/proposal.pptx"},
                                     {"duration", 10},
                                     {"resolution", "480P"},
                                     {"ratio", "adaptive"},
                                     {"prompt_extend", false}};
    const MaiToolResult invalidFileExtend = video->execute(filePlan.dump(), context);
    CHECK(invalidFileExtend.hasError());
    CHECK(invalidFileExtend.error().message().find("prompt_extend must be true") !=
          std::string::npos);
    nlohmann::json fileWithLink = filePlan;
    fileWithLink["link_url"] = "https://example.com/article";
    CHECK(video->execute(fileWithLink.dump(), context).hasError());
    nlohmann::json fileWithFirstFrame = filePlan;
    fileWithFirstFrame["first_frame_path"] = "first.png";
    CHECK(video->execute(fileWithFirstFrame.dump(), context).hasError());
    nlohmann::json missingLocalFile = filePlan;
    missingLocalFile.erase("file_url");
    missingLocalFile["file_path"] = "missing.pptx";
    missingLocalFile["prompt_extend"] = true;
    CHECK(video->execute(missingLocalFile.dump(), context).hasError());
    const auto missingVideoPlan =
        video->execute(R"({"action":"delegate","message":"Create","mode":"create"})", context);
    CHECK(missingVideoPlan.hasError());
    CHECK(missingVideoPlan.error().message().find("confirm duration") != std::string::npos);
    CHECK(video->execute(R"({"action":"delegate","message":"Edit","mode":"edit"})", context)
              .hasError());
    CHECK(
        video
            ->execute(
                R"({"action":"delegate","message":"Create","mode":"create","video_path":"x.mp4"})",
                context)
            .hasError());
    CHECK(video
              ->execute(R"({"action":"delegate","message":"Create","last_frame_path":"x.png"})",
                        context)
              .hasError());
    CHECK(video->execute(R"({"action":"delegate","message":"Create","duration":31})", context)
              .hasError());
    CHECK(
        image
            ->execute(
                R"({"action":"delegate","message":"Draw","image_paths":["a.png","b.png","c.png","d.png"]})",
                context)
            .hasError());
    CHECK(image
              ->execute(R"({"action":"delegate","message":"Draw","output_path":"../out.png"})",
                        context)
              .hasError());
    CHECK(image->execute(R"({"action":"delegate","message":"Draw","size":"999999999x1"})", context)
              .hasError());
}

void testLiveWan3WhenExplicitlyConfigured() {
    const char* key = std::getenv("MAI_WAN3_LIVE_KEY");
    const char* workspaceId = std::getenv("MAI_WAN3_LIVE_WORKSPACE_ID");
    const char* videoPath = std::getenv("MAI_WAN3_LIVE_VIDEO");
    if (key == nullptr || workspaceId == nullptr || videoPath == nullptr) return;
    MaiToolContext context;
    context.root = MaiFilePath::fromUtf8(videoPath).dirName().toUtf8();
    auto video = makeMaiWanVideoTool([=] { return MaiWanCredentials{key, workspaceId}; });
    const auto submitted = video->execute(
        nlohmann::json{{"action", "delegate"},
                       {"message",
                        "Edit video 1: replace only the black T-shirt with a dark blue denim "
                        "jacket. Preserve the face, phone, park and camera motion."},
                       {"mode", "edit"},
                       {"video_path", MaiFilePath::fromUtf8(videoPath).baseName().toUtf8()},
                       {"resolution", "480P"},
                       {"duration", 5},
                       {"audio", false},
                       {"watermark", true}}
            .dump(),
        context);
    if (submitted.hasError()) {
        std::fprintf(stderr, "LIVE Wan3 submit failed: %s\n", submitted.error().message().c_str());
        ++failures;
        return;
    }
    const std::string taskId = nlohmann::json::parse(submitted.output()).at("task_id");
    std::printf("LIVE Wan3 task ID: %s\n", taskId.c_str());
    std::fflush(stdout);
    const std::string check = nlohmann::json{
        {"action", "continue"},
        {"message", "Check edit status"},
        {"conversation_id",
         taskId}}.dump();
    for (int attempt = 0; attempt < 48; ++attempt) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        const auto progress = video->execute(check, context);
        if (progress.hasError()) {
            std::fprintf(stderr, "LIVE Wan3 task failed: %s\n", progress.error().message().c_str());
            ++failures;
            return;
        }
        const auto output = nlohmann::json::parse(progress.output());
        if (output.value("status", std::string{}) == "SUCCEEDED") {
            std::printf("LIVE Wan3 output: %s\n", output.at("path").get<std::string>().c_str());
            CHECK(
                MaiFileSystem::exists(MaiFilePath::fromUtf8(output.at("path").get<std::string>())));
            return;
        }
    }
    std::fprintf(stderr, "LIVE Wan3 task still running: %s\n", taskId.c_str());
    ++failures;
}

void testLiveDiagnosticsWhenExplicitlyConfigured() {
    const char* key = std::getenv("MAI_MODEL_STUDIO_DIAG_KEY");
    const char* workspaceId = std::getenv("MAI_MODEL_STUDIO_DIAG_WORKSPACE_ID");
    if (key == nullptr || workspaceId == nullptr) return;
    MaiToolContext context;
    const auto credentials = [=] { return MaiWanCredentials{key, workspaceId}; };
    auto video = makeMaiWanVideoTool(credentials);
    auto image = makeMaiWanImageTool(credentials);
    auto klingImage = makeMaiBailianKlingImageTool(credentials);
    for (MaiTool* tool : {video.get(), image.get(), klingImage.get()}) {
        const MaiToolResult result = tool->execute(R"({"action":"diagnose"})", context);
        if (result.hasError()) {
            std::fprintf(stderr, "LIVE %s diagnose failed: %s\n", tool->name().c_str(),
                         result.error().message().c_str());
            ++failures;
            continue;
        }
        const auto output = nlohmann::json::parse(result.output());
        std::printf("LIVE %s model_visible=%s\n", tool->name().c_str(),
                    output.at("model_visible").get<bool>() ? "true" : "false");
        CHECK(output.at("model_visible") == true);
    }
}

void testExistingPaidTasksCanBeRecoveredWithoutResubmission() {
    const char* key = std::getenv("MAI_MODEL_STUDIO_RECOVERY_KEY");
    const char* workspace = std::getenv("MAI_MODEL_STUDIO_RECOVERY_WORKSPACE");
    const char* wanTask = std::getenv("MAI_MODEL_STUDIO_RECOVERY_WAN_TASK");
    const char* klingTask = std::getenv("MAI_MODEL_STUDIO_RECOVERY_KLING_TASK");
    const char* certificate = std::getenv("MAI_MODEL_STUDIO_RECOVERY_CA");
    if (key == nullptr || workspace == nullptr || wanTask == nullptr || klingTask == nullptr ||
        certificate == nullptr)
        return;
    const MaiFilePath directory = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_paid_recovery_")));
    CHECK(!MaiFileSystem::createDirectories(directory));
    MaiToolContext context;
    context.root = directory.toUtf8();
    const auto credentials = [key, workspace] { return MaiWanCredentials{key, workspace}; };
    auto wan = makeMaiWanVideoTool(credentials, certificate);
    auto kling = makeMaiBailianKlingVideoTool(credentials, certificate);
    for (const auto& entry :
         {std::pair<MaiTool*, const char*>{wan.get(), wanTask}, {kling.get(), klingTask}}) {
        const nlohmann::json args = {
            {"action", "continue"}, {"conversation_id", entry.second}, {"poll_once", true}};
        const MaiToolResult result = entry.first->execute(args.dump(), context);
        if (result.hasError()) {
            std::fprintf(stderr, "Recovery %s failed: %s\n", entry.first->name().c_str(),
                         result.error().message().c_str());
            ++failures;
            continue;
        }
        const nlohmann::json output = nlohmann::json::parse(result.output());
        CHECK(output.value("status", "") == "SUCCEEDED");
        CHECK(MaiFileSystem::exists(MaiFilePath::fromUtf8(output.value("path", ""))));
    }
    MaiFileSystem::removeRecursively(directory);
}

void testLocalMediaStopsBeforeWanSubmissionWhenOssFails() {
    const MaiFilePath source = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("wan_oss_input_") + ".mp4"));
    CHECK(!MaiFileSystem::writeFile(source, "synthetic video fixture"));
    MaiToolContext context;
    context.root = source.dirName().toUtf8();
    int uploads = 0;
    const MaiCreativeMediaUploadProvider rejected =
        [&uploads](const std::string&, const MaiToolContext&) -> MaiResult<std::string> {
        ++uploads;
        return {MaiErrorCode::Network, "Synthetic OSS failure"};
    };
    auto tool = makeMaiWanVideoTool([] { return MaiWanCredentials{"test-key", "ws-test123"}; }, {},
                                    rejected);
    const nlohmann::json request = {{"action", "delegate"},
                                    {"message", "Adjust the scene"},
                                    {"video_path", source.baseName().toUtf8()},
                                    {"mode", "reference"},
                                    {"duration", 5},
                                    {"ratio", "9:16"},
                                    {"resolution", "720P"}};
    const MaiToolResult result = tool->execute(request.dump(), context);
    CHECK(result.hasError());
    CHECK(result.error().message().find("upload_failed") != std::string::npos);
    CHECK(uploads == 1);
    (void)MaiFileSystem::removeFile(source);
}

void testBailianKlingUsesWanCredentialsAndRequiresOssForMedia() {
    MaiWanCredentials credentials;
    auto tool = makeMaiBailianKlingVideoTool([&] { return credentials; });
    MaiToolContext context;
    context.root = MaiFileSystem::temporaryDirectory().toUtf8();
    CHECK(tool->name() == "kling_bailian_video");
    CHECK(nlohmann::json::parse(tool->parametersSchema())["properties"].contains("poll_once"));
    const auto klingSchema = nlohmann::json::parse(tool->parametersSchema());
    CHECK(klingSchema["properties"]["local_image_paths"]["description"].get<std::string>().find(
              "four with") != std::string::npos);
    CHECK(klingSchema["properties"]["duration"]["description"].get<std::string>().find("3-10") !=
          std::string::npos);
    CHECK(!tool->requiresPerCallApproval(R"({"action":"discover"})"));
    CHECK(!tool->requiresPerCallApproval(R"({"action":"diagnose"})"));
    CHECK(tool->requiresPerCallApproval(R"({"action":"delegate"})"));
    auto discovered =
        nlohmann::json::parse(tool->execute(R"({"action":"discover"})", context).output());
    CHECK(discovered.at("configured") == false);
    credentials = {"test-key", "ws-test123"};
    discovered = nlohmann::json::parse(tool->execute(R"({"action":"discover"})", context).output());
    CHECK(discovered.at("configured") == true);
    CHECK(discovered.dump().find("advanced_omni_controls") != std::string::npos);
    CHECK(discovered.at("capabilities")[0].contains("model_support"));
    for (const auto& capability : discovered.at("capabilities"))
        CHECK(!capability.at("limitation").get<std::string>().empty());
    CHECK(discovered.at("capabilities")[1].at("tool_status") == "upload_not_configured");
    const MaiFilePath source = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("kling_oss_input_") + ".jpg"));
    CHECK(!MaiFileSystem::writeFile(source, "synthetic image fixture"));
    const nlohmann::json request = {{"action", "delegate"},
                                    {"message", "Animate this frame"},
                                    {"image_path", source.baseName().toUtf8()},
                                    {"duration", 5},
                                    {"resolution", "std"}};
    const MaiToolResult missingUpload = tool->execute(request.dump(), context);
    CHECK(missingUpload.hasError());
    CHECK(missingUpload.error().message().find("upload_not_configured") != std::string::npos);
    int uploads = 0;
    const MaiCreativeMediaUploadProvider rejected =
        [&uploads](const std::string&, const MaiToolContext&) -> MaiResult<std::string> {
        ++uploads;
        return {MaiErrorCode::Network, "Synthetic OSS failure"};
    };
    auto withUpload = makeMaiBailianKlingVideoTool([&] { return credentials; }, {}, rejected);
    const MaiToolResult failedUpload = withUpload->execute(request.dump(), context);
    CHECK(failedUpload.hasError());
    CHECK(failedUpload.error().message().find("upload_failed") != std::string::npos);
    CHECK(uploads == 1);
    (void)MaiFileSystem::removeFile(source);
}

void testWanAndKlingImageToolsValidateBeforePaidSubmission() {
    const auto credentials = [] { return MaiWanCredentials{"test-key", "ws-test123"}; };
    auto wan = makeMaiWanImageTool(credentials);
    auto kling = makeMaiBailianKlingImageTool(credentials);
    MaiToolContext context;
    context.root = MaiFileSystem::temporaryDirectory().toUtf8();
    CHECK(wan->name() == "wan_image");
    CHECK(kling->name() == "kling_bailian_image");
    CHECK(nlohmann::json::parse(wan->parametersSchema())["properties"]["image_paths"].at(
              "maxItems") == 9);
    CHECK(nlohmann::json::parse(kling->parametersSchema())["properties"].contains("poll_once"));
    CHECK(!kling->requiresPerCallApproval(R"({"action":"discover"})"));
    CHECK(kling->requiresPerCallApproval(R"({"action":"delegate"})"));
    CHECK(wan->execute(R"({"action":"delegate","message":"Draw a cat","negative_prompt":"dog"})",
                       context)
              .hasError());
    const MaiToolResult missingReference = wan->execute(
        R"({"action":"delegate","message":"Edit image","image_path":"missing.jpg","size":"2K"})",
        context);
    CHECK(missingReference.hasError());
    CHECK(missingReference.error().message().find("Media file") != std::string::npos);
    const MaiToolResult unsupportedMulti = kling->execute(
        R"({"action":"delegate","message":"Blend two pictures","model":"kling/kling-v3-image-generation","image_paths":["a.jpg","b.jpg"],"ratio":"1:1","resolution":"1k"})",
        context);
    CHECK(unsupportedMulti.hasError());
    CHECK(unsupportedMulti.error().message().find("require Kling Omni") != std::string::npos);
    const MaiToolResult unsupported4k = kling->execute(
        R"({"action":"delegate","message":"Draw a cat","ratio":"1:1","resolution":"4k"})", context);
    CHECK(unsupported4k.hasError());
    CHECK(unsupported4k.error().message().find("resolution") != std::string::npos);
}

void testVideoTaskManagementNeverDiscardsActiveOrForeignTasks() {
    auto store = makeMaiMemoryStore();
    MaiSession owner;
    owner.id = "video-owner";
    store->putSession(owner);
    MaiSpecialistTask task;
    task.id = MaiIdGenerator::generate("spt_");
    task.ownerSessionId = owner.id;
    task.specialistName = "wan_video";
    task.providerTaskId = "provider-task-1";
    task.intent = "Generate a test video";
    task.created = MaiTime::getCurrentTime();
    CHECK(!store->insertSpecialistTask(task));
    const auto credentials = [] { return MaiWanCredentials{"test-key", "ws-test123"}; };
    auto wan = makeMaiWanVideoTool(credentials);
    auto kling = makeMaiBailianKlingVideoTool(credentials);
    for (MaiTool* tool : {wan.get(), kling.get()}) {
        const auto schema = nlohmann::json::parse(tool->parametersSchema());
        CHECK(schema["properties"]["action"]["enum"].dump().find("cancel") != std::string::npos);
        CHECK(schema["properties"]["action"]["enum"].dump().find("delete") != std::string::npos);
        CHECK(tool->requiresPerCallApproval(R"({"action":"cancel"})"));
        CHECK(tool->requiresPerCallApproval(R"({"action":"delete"})"));
    }
    MaiToolContext context;
    context.specialistTasks = store.get();
    context.sessionId = "foreign-owner";
    const nlohmann::json remove = {{"action", "delete"}, {"conversation_id", task.id}};
    CHECK(wan->execute(remove.dump(), context).hasError());
    context.sessionId = owner.id;
    CHECK(wan->execute(remove.dump(), context).hasError());
    CHECK(!store->finishSpecialistTask(task.id, owner.id, MaiSpecialistTaskStatus::Failed,
                                       "The test task failed", {}, MaiTime::getCurrentTime()));
    CHECK(wan->execute(remove.dump(), context).hasError());
    const auto reservation = store->reserveSpecialistNotification(
        task.id, owner.id, "notification-1", MaiTime::getCurrentTime());
    CHECK(reservation);
    CHECK(!store->markSpecialistNotified(task.id, owner.id, reservation.value(),
                                         MaiTime::getCurrentTime()));
    const MaiToolResult removed = wan->execute(remove.dump(), context);
    CHECK(!removed.hasError());
    CHECK(nlohmann::json::parse(removed.output()).at("provider_deleted") == false);
    CHECK(!store->getSpecialistTask(task.id, owner.id, task));
}

}  // namespace

int main() {
    testOtherSpecialistBoundaries();
    testRevisionIsSessionBound();
    testLiveWan3WhenExplicitlyConfigured();
    testLiveDiagnosticsWhenExplicitlyConfigured();
    testExistingPaidTasksCanBeRecoveredWithoutResubmission();
    testLocalMediaStopsBeforeWanSubmissionWhenOssFails();
    testBailianKlingUsesWanCredentialsAndRequiresOssForMedia();
    testWanAndKlingImageToolsValidateBeforePaidSubmission();
    testVideoTaskManagementNeverDiscardsActiveOrForeignTasks();
    return failures == 0 ? 0 : 1;
}
