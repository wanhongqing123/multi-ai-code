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

void testDiscoveryAndBoundaries() {
    MaiWanCredentials credentials;
    auto tool = makeMaiWanVideoEditTool([&] { return credentials; });
    CHECK(tool->name() == "wan_video_edit");
    CHECK(!tool->requiresPerCallApproval(R"({"action":"discover"})"));
    CHECK(!tool->requiresPerCallApproval(R"({"action":"diagnose"})"));
    CHECK(!tool->requiresPerCallApproval(R"({"action":"continue"})"));
    CHECK(tool->requiresPerCallApproval(R"({"action":"delegate"})"));
    CHECK(tool->requiresPerCallApproval(R"({"action":"revise"})"));
    MaiToolContext context;
    context.root = MaiFileSystem::temporaryDirectory().toUtf8();
    const auto discover = tool->execute(R"({"action":"discover"})", context);
    CHECK(!discover.hasError());
    const auto info = nlohmann::json::parse(discover.output());
    CHECK(info.at("bound_model") == "wan2.7-videoedit");
    CHECK(info.at("configured") == false);
    CHECK(info.at("missing_configuration").size() == 2);
    CHECK(info.at("capabilities").size() == 3);
    CHECK(
        tool->execute(R"({"action":"delegate","message":"Change the shirt","video_path":"x.mp4"})",
                      context)
            .hasError());

    credentials = {"test-key", "ws-test123"};
    CHECK(tool->specialistInfo()->configured);
    const auto configured = tool->execute(R"({"action":"discover","message":"Status"})", context);
    CHECK(!configured.hasError());
    CHECK(nlohmann::json::parse(configured.output()).at("missing_configuration").empty());
    const auto missingResolution = tool->execute(
        R"({"action":"delegate","message":"Change the shirt","video_path":"x.mp4"})", context);
    CHECK(missingResolution.hasError());
    CHECK(missingResolution.error().message().find("confirm 720P") != std::string::npos);
    CHECK(
        tool->execute(R"({"action":"delegate","message":"Change the shirt"})", context).hasError());
    CHECK(
        tool->execute(
                R"({"action":"delegate","message":"Change the shirt","video_path":"../secret.mp4"})",
                context)
            .hasError());
    CHECK(
        tool->execute(
                R"({"action":"delegate","message":"Change the shirt","video_path":"x.mp4","reference_image_paths":[1]})",
                context)
            .hasError());
    CHECK(
        tool->execute(
                R"({"action":"delegate","message":"Change the shirt","video_path":"x.mp4","resolution":"480P"})",
                context)
            .hasError());
    CHECK(tool->execute(R"({"action":"continue","message":"Status","conversation_id":"../other"})",
                        context)
              .hasError());
}

void testRevisionIsSessionBound() {
    auto store = makeMaiMemoryStore();
    MaiSession owner;
    owner.id = "owner";
    store->putSession(owner);
    MaiSpecialistTask previous;
    previous.id = MaiIdGenerator::generate("spt_");
    previous.ownerSessionId = "owner";
    previous.specialistName = "wan_video_edit";
    previous.providerTaskId = "valid-task-id";
    CHECK(!store->insertSpecialistTask(previous));
    MaiToolContext context;
    context.sessionId = "other";
    context.specialistTasks = store.get();
    auto tool = makeMaiWanVideoEditTool([] { return MaiWanCredentials{"test-key", "ws-test123"}; });
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
    auto image = makeMaiQwenImageTool([&] { return credentials; });
    CHECK(video->requiresPerCallApproval(R"({"action":"delegate"})"));
    CHECK(image->requiresPerCallApproval(R"({"action":"delegate"})"));
    CHECK(!video->requiresPerCallApproval(R"({"action":"continue"})"));
    MaiToolContext context;
    context.root = MaiFileSystem::temporaryDirectory().toUtf8();
    const auto videoInfo = video->execute(R"({"action":"discover"})", context);
    const auto imageInfo = image->execute(R"({"action":"discover"})", context);
    CHECK(!videoInfo.hasError());
    CHECK(!imageInfo.hasError());
    CHECK(nlohmann::json::parse(videoInfo.output()).at("bound_model") == "wan3.0-video");
    CHECK(nlohmann::json::parse(imageInfo.output()).at("bound_model") == "qwen-image-3.0-pro");
    CHECK(nlohmann::json::parse(videoInfo.output()).at("missing_configuration").size() == 2);
    CHECK(nlohmann::json::parse(imageInfo.output()).at("missing_configuration").size() == 2);
    credentials = {"test-key", "ws-test123"};
    CHECK(video->specialistInfo()->configured);
    CHECK(image->specialistInfo()->configured);
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

void testLiveWan2WhenExplicitlyConfigured() {
    const char* key = std::getenv("MAI_WAN_LIVE_KEY");
    const char* workspaceId = std::getenv("MAI_WAN_LIVE_WORKSPACE_ID");
    const char* videoPath = std::getenv("MAI_WAN_LIVE_VIDEO");
    if (key == nullptr || workspaceId == nullptr || videoPath == nullptr) return;
    MaiToolContext context;
    context.root = MaiFilePath::fromUtf8(videoPath).dirName().toUtf8();
    auto tool = makeMaiWanVideoEditTool([=] { return MaiWanCredentials{key, workspaceId}; });
    const auto submitted = tool->execute(
        nlohmann::json{{"action", "delegate"},
                       {"message",
                        "Replace only the man's black T-shirt with a dark blue denim jacket. "
                        "Preserve his face, pose, phone, background and camera motion."},
                       {"video_path", MaiFilePath::fromUtf8(videoPath).baseName().toUtf8()},
                       {"resolution", "720P"},
                       {"audio_setting", "origin"},
                       {"watermark", true}}
            .dump(),
        context);
    if (submitted.hasError()) {
        std::fprintf(stderr, "LIVE Wan submit failed: %s\n", submitted.error().message().c_str());
        ++failures;
        return;
    }
    const auto task = nlohmann::json::parse(submitted.output());
    const std::string taskId = task.at("task_id").get<std::string>();
    std::printf("LIVE Wan task ID: %s\n", taskId.c_str());
    const std::string check = nlohmann::json{
        {"action", "continue"},
        {"message", "Check edit status"},
        {"conversation_id",
         taskId}}.dump();
    for (int attempt = 0; attempt < 48; ++attempt) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        const auto progress = tool->execute(check, context);
        if (progress.hasError()) {
            std::fprintf(stderr, "LIVE Wan task failed: %s\n", progress.error().message().c_str());
            ++failures;
            return;
        }
        const auto output = nlohmann::json::parse(progress.output());
        if (output.value("status", std::string{}) == "SUCCEEDED") {
            std::printf("LIVE Wan output: %s\n", output.at("path").get<std::string>().c_str());
            CHECK(
                MaiFileSystem::exists(MaiFilePath::fromUtf8(output.at("path").get<std::string>())));
            return;
        }
    }
    std::fprintf(stderr, "LIVE Wan task still running: %s\n", taskId.c_str());
    ++failures;
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

void testLiveQwenWhenExplicitlyConfigured() {
    const char* key = std::getenv("MAI_QWEN_LIVE_KEY");
    const char* workspaceId = std::getenv("MAI_QWEN_LIVE_WORKSPACE_ID");
    const char* imagePath = std::getenv("MAI_QWEN_LIVE_IMAGE");
    if (key == nullptr || workspaceId == nullptr || imagePath == nullptr) return;
    MaiToolContext context;
    context.root = MaiFilePath::fromUtf8(imagePath).dirName().toUtf8();
    auto image = makeMaiQwenImageTool([=] { return MaiWanCredentials{key, workspaceId}; });
    const auto result = image->execute(
        nlohmann::json{
            {"action", "delegate"},
            {"message",
             "Edit the input picture into a clean graphic illustration of a yellow paper airplane "
             "on a pale blue background. Keep one centered subject and no text."},
            {"image_path", MaiFilePath::fromUtf8(imagePath).baseName().toUtf8()},
            {"size", "1024x1024"},
            {"prompt_extend", false}}
            .dump(),
        context);
    if (result.hasError()) {
        std::fprintf(stderr, "LIVE Qwen failed: %s\n", result.error().message().c_str());
        ++failures;
        return;
    }
    const auto output = nlohmann::json::parse(result.output());
    std::printf("LIVE Qwen output: %s\n", output.at("path").get<std::string>().c_str());
    CHECK(MaiFileSystem::exists(MaiFilePath::fromUtf8(output.at("path").get<std::string>())));
}

void testLiveQwenTextWhenExplicitlyConfigured() {
    const char* key = std::getenv("MAI_QWEN_TEXT_LIVE_KEY");
    const char* workspaceId = std::getenv("MAI_QWEN_TEXT_LIVE_WORKSPACE_ID");
    const char* outputDirectory = std::getenv("MAI_QWEN_TEXT_LIVE_OUTPUT_DIR");
    if (key == nullptr || workspaceId == nullptr || outputDirectory == nullptr) return;
    MaiToolContext context;
    context.root = outputDirectory;
    auto image = makeMaiQwenImageTool([=] { return MaiWanCredentials{key, workspaceId}; });
    const auto result =
        image->execute(nlohmann::json{{"action", "delegate"},
                                      {"message",
                                       "A single green origami boat on a clean cream background, "
                                       "minimal flat illustration, no text."},
                                      {"size", "1024x1024"},
                                      {"prompt_extend", false}}
                           .dump(),
                       context);
    if (result.hasError()) {
        std::fprintf(stderr, "LIVE Qwen text-to-image failed: %s\n",
                     result.error().message().c_str());
        ++failures;
        return;
    }
    const auto output = nlohmann::json::parse(result.output());
    std::printf("LIVE Qwen text output: %s\n", output.at("path").get<std::string>().c_str());
    CHECK(MaiFileSystem::exists(MaiFilePath::fromUtf8(output.at("path").get<std::string>())));
}

void testLiveDiagnosticsWhenExplicitlyConfigured() {
    const char* key = std::getenv("MAI_MODEL_STUDIO_DIAG_KEY");
    const char* workspaceId = std::getenv("MAI_MODEL_STUDIO_DIAG_WORKSPACE_ID");
    if (key == nullptr || workspaceId == nullptr) return;
    MaiToolContext context;
    const auto credentials = [=] { return MaiWanCredentials{key, workspaceId}; };
    auto edit = makeMaiWanVideoEditTool(credentials);
    auto video = makeMaiWanVideoTool(credentials);
    auto image = makeMaiQwenImageTool(credentials);
    for (MaiTool* tool : {edit.get(), video.get(), image.get()}) {
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

}  // namespace

int main() {
    testDiscoveryAndBoundaries();
    testOtherSpecialistBoundaries();
    testRevisionIsSessionBound();
    testLiveWan2WhenExplicitlyConfigured();
    testLiveWan3WhenExplicitlyConfigured();
    testLiveQwenWhenExplicitlyConfigured();
    testLiveQwenTextWhenExplicitlyConfigured();
    testLiveDiagnosticsWhenExplicitlyConfigured();
    return failures == 0 ? 0 : 1;
}
