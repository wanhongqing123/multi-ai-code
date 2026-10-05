#include <cstdlib>
#include <cstdio>
#include <string>

#include <json.hpp>

#include "MaiArkMediaTools.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiMemoryStore.h"

namespace {

int failures = 0;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

void testDynamicCredentialAndToolIdentity() {
    std::string key;
    auto video = makeMaiSeedanceVideoTool([&] { return key; });
    auto image = makeMaiSeedreamImageTool([&] { return key; });
    MaiToolContext context;
    CHECK(video->name() == "seedance_video");
    CHECK(image->name() == "seedream_image");
    CHECK(nlohmann::json::parse(video->parametersSchema(), nullptr, false).is_object());
    CHECK(nlohmann::json::parse(image->parametersSchema(), nullptr, false).is_object());
    CHECK(video->description().find("not configured") != std::string::npos);
    CHECK(image->description().find("not configured") != std::string::npos);
    const std::string discover = R"({"action":"discover","message":"capabilities"})";
    CHECK(!video->requiresPerCallApproval(discover));
    CHECK(!image->requiresPerCallApproval(discover));
    CHECK(video->requiresPerCallApproval(R"({"action":"delegate","message":"create"})"));
    CHECK(image->requiresPerCallApproval(R"({"action":"revise","message":"adjust"})"));
    CHECK(!video->requiresPerCallApproval(R"({"action":"continue"})"));
    auto videoInfo = nlohmann::json::parse(video->execute(discover, context).output());
    auto imageInfo = nlohmann::json::parse(image->execute(discover, context).output());
    CHECK(!videoInfo.at("configured").get<bool>());
    CHECK(!imageInfo.at("configured").get<bool>());
    CHECK(videoInfo.at("capabilities").is_array());
    CHECK(videoInfo.dump().find("platform_virtual_avatar") != std::string::npos);
    CHECK(videoInfo.dump().find("authorized_real_portrait") != std::string::npos);
    CHECK(imageInfo.at("capabilities").is_array());
    CHECK(videoInfo.at("capabilities")[0].at("tool_status") == "not_configured");
    CHECK(imageInfo.at("capabilities")[0].at("tool_status") == "not_configured");
    key = "test-key";
    CHECK(video->description().find("not configured") == std::string::npos);
    CHECK(image->description().find("not configured") == std::string::npos);
    videoInfo = nlohmann::json::parse(video->execute(discover, context).output());
    imageInfo = nlohmann::json::parse(image->execute(discover, context).output());
    CHECK(videoInfo.at("configured").get<bool>());
    CHECK(imageInfo.at("configured").get<bool>());
    CHECK(videoInfo.at("capabilities")[0].at("tool_status") == "implemented_unverified");
    CHECK(videoInfo.dump().find(key) == std::string::npos);
    CHECK(imageInfo.dump().find(key) == std::string::npos);
    MaiToolRegistry registry;
    registry.add(std::move(video));
    registry.add(std::move(image));
    auto specialists = registry.specialists();
    CHECK(specialists.size() == 2);
    CHECK(specialists[0].toolName == "seedance_video");
    CHECK(specialists[1].toolName == "seedream_image");
    CHECK(specialists[1].capabilities[2].id == "multi_image_edit");
    CHECK(specialists[1].capabilities[2].status ==
          MaiSpecialistCapabilityStatus::ImplementedUnverified);
    key.clear();
    specialists = registry.specialists();
    CHECK(!specialists[0].configured);
    CHECK(specialists[0].capabilities[0].status == MaiSpecialistCapabilityStatus::NotConfigured);
}

void testInvalidInputsDoNotReachNetwork() {
    auto video = makeMaiSeedanceVideoTool([] { return std::string("test-key"); });
    auto image = makeMaiSeedreamImageTool([] { return std::string("test-key"); });
    MaiToolContext context;
    context.root = MaiFileSystem::temporaryDirectory().toUtf8();
    const auto missingPlan =
        video->execute(R"({"action":"delegate","message":"Create a video"})", context);
    CHECK(missingPlan.hasError());
    CHECK(missingPlan.error().message().find("confirm duration") != std::string::npos);
    CHECK(video
              ->execute(
                  R"({"action":"delegate","message":"test","mode":"edit","video_path":"clip.mp4"})",
                  context)
              .hasError());
    CHECK(video
              ->execute(R"({"action":"delegate","message":"test","production":{"duration":"bad"}})",
                        context)
              .hasError());
    CHECK(video
              ->execute(R"({"action":"delegate","message":"test","last_frame_path":"last.png"})",
                        context)
              .hasError());
    const auto invalidAvatar = video->execute(
        R"({"action":"delegate","message":"test","virtual_avatar_asset_id":"https://example.com/face.jpg"})",
        context);
    CHECK(invalidAvatar.hasError());
    CHECK(invalidAvatar.error().message().find("asset ID") != std::string::npos);
    const auto authorized = video->execute(
        R"({"action":"delegate","message":"Keep the authorized person's face","authorized_portrait_asset_id":"asset://asset-20261005230545-tb2zr"})",
        context);
    CHECK(authorized.hasError());
    CHECK(authorized.error().message().find("confirm duration") != std::string::npos);
    CHECK(video->execute(
                   R"({"action":"delegate","message":"test","virtual_avatar_asset_id":"asset-20260401123823-6d4x2","authorized_portrait_asset_id":"asset-20261005230545-tb2zr"})",
                   context)
              .hasError());
    CHECK(
        video
            ->execute(
                R"({"action":"delegate","message":"test","mode":"edit","video_url":"https://example.com/clip.mp4","virtual_avatar_asset_id":"asset-20260401123823-6d4x2"})",
                context)
            .hasError());
    CHECK(image
              ->execute(R"({"action":"delegate","message":"test","output_path":"../outside.png"})",
                        context)
              .hasError());
    CHECK(image->execute("{", context).hasError());
    CHECK(
        image->execute(R"({"action":"delegate","message":"test","image_paths":["a.png"]})", context)
            .hasError());
    CHECK(
        image
            ->execute(
                R"({"action":"delegate","message":"test","image_paths":["a.png","b.png"],"image_path":"c.png"})",
                context)
            .hasError());
}

void testRevisionRejectsAnotherConversation() {
    auto store = makeMaiMemoryStore();
    MaiSession owner;
    owner.id = MaiIdGenerator::newSessionId();
    store->putSession(owner);
    MaiSession other;
    other.id = MaiIdGenerator::newSessionId();
    store->putSession(other);
    MaiSpecialistTask prior;
    prior.id = MaiIdGenerator::generate("spt_");
    prior.ownerSessionId = owner.id;
    prior.specialistName = "seedance_video";
    prior.providerTaskId = "provider-task";
    prior.intent = "Original edit";
    CHECK(!store->insertSpecialistTask(prior));
    MaiToolContext context;
    context.sessionId = other.id;
    context.root = MaiFileSystem::temporaryDirectory().toUtf8();
    context.specialistTasks = store.get();
    auto video = makeMaiSeedanceVideoTool([] { return std::string("test-key"); });
    const nlohmann::json revision = {
        {"action", "revise"}, {"message", "Fix the edge"}, {"conversation_id", prior.id}};
    const MaiToolResult result = video->execute(revision.dump(), context);
    CHECK(result.hasError());
    CHECK(result.error().code() == MaiErrorCode::NotFound);
    const nlohmann::json continuation = {
        {"action", "continue"}, {"message", "Check status"}, {"conversation_id", prior.id}};
    const MaiToolResult checked = video->execute(continuation.dump(), context);
    CHECK(checked.hasError());
    CHECK(checked.error().code() == MaiErrorCode::NotFound);

    MaiSpecialistTask imageTask = prior;
    imageTask.id = MaiIdGenerator::generate("spt_");
    imageTask.specialistName = "seedream_image";
    imageTask.outputPath = "/tmp/prior.png";
    CHECK(!store->insertSpecialistTask(imageTask));
    auto image = makeMaiSeedreamImageTool([] { return std::string("test-key"); });
    const nlohmann::json imageRevision = {
        {"action", "revise"}, {"message", "Make it brighter"}, {"parent_task_id", imageTask.id}};
    const MaiToolResult imageResult = image->execute(imageRevision.dump(), context);
    CHECK(imageResult.hasError());
    CHECK(imageResult.error().code() == MaiErrorCode::NotFound);
}

void testLiveArkWhenExplicitlyConfigured() {
    const char* key = std::getenv("MAI_ARK_LIVE_TEST_KEY");
    if (key == nullptr || *key == '\0') return;
    const char* firstPath = std::getenv("MAI_ARK_LIVE_FIRST_IMAGE");
    const char* lastPath = std::getenv("MAI_ARK_LIVE_LAST_IMAGE");
    CHECK(firstPath != nullptr && lastPath != nullptr);
    if (firstPath == nullptr || lastPath == nullptr) return;

    const MaiFilePath workspace = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_ark_live_")));
    CHECK(!MaiFileSystem::createDirectories(workspace));
    const auto copyImage = [&](const char* source, const char* name) {
        std::string bytes;
        CHECK(!MaiFileSystem::readFile(MaiFilePath::fromUtf8(source), bytes));
        CHECK(!MaiFileSystem::writeFile(workspace.append(MaiFilePath::fromUtf8(name)), bytes));
    };
    copyImage(firstPath, "first.png");
    copyImage(lastPath, "last.png");
    MaiToolContext context;
    context.root = workspace.toUtf8();
    const std::string apiKey = key;

    auto image = makeMaiSeedreamImageTool([apiKey] { return apiKey; });
    const MaiToolResult imageResult = image->execute(
        R"({"action":"delegate","message":"Create one abstract composition blending the colors of image 1 and image 2. No people or text.","image_paths":["first.png","last.png"],"output_path":"live-seedream.png","size":"1K"})",
        context);
    if (imageResult.hasError()) {
        std::printf("LIVE Seedream failed: %s\n", imageResult.error().message().c_str());
        ++failures;
    } else {
        const auto output = nlohmann::json::parse(imageResult.output());
        std::printf("LIVE Seedream output: %s (%lld bytes)\n",
                    output.at("path").get<std::string>().c_str(),
                    output.at("bytes").get<long long>());
        CHECK(MaiFileSystem::exists(MaiFilePath::fromUtf8(output.at("path").get<std::string>())));
    }

    auto video = makeMaiSeedanceVideoTool([apiKey] { return apiKey; });
    const MaiToolResult submitted = video->execute(
        R"({"action":"delegate","message":"Make a smooth abstract transition from the first frame to the last frame. No people or text.","mode":"create","image_path":"first.png","last_frame_path":"last.png","production":{"duration":4,"ratio":"16:9","resolution":"480p","generate_audio":false}})",
        context);
    if (submitted.hasError()) {
        std::printf("LIVE Seedance submit failed: %s\n", submitted.error().message().c_str());
        ++failures;
        return;
    }
    const auto task = nlohmann::json::parse(submitted.output());
    const std::string taskId = task.at("task_id").get<std::string>();
    std::printf("LIVE Seedance task: %s\n", taskId.c_str());
    const std::string continuation = nlohmann::json{
        {"action", "continue"},
        {"message", "Check the video task"},
        {"conversation_id", taskId}}.dump();
    for (int attempt = 0; attempt < 12; ++attempt) {
        const MaiToolResult progress = video->execute(continuation, context);
        if (progress.hasError()) {
            std::printf("LIVE Seedance failed: %s\n", progress.error().message().c_str());
            ++failures;
            return;
        }
        const auto output = nlohmann::json::parse(progress.output());
        if (output.value("status", std::string{}) == "succeeded") {
            std::printf("LIVE Seedance output: %s (%lld bytes)\n",
                        output.at("path").get<std::string>().c_str(),
                        output.at("bytes").get<long long>());
            CHECK(
                MaiFileSystem::exists(MaiFilePath::fromUtf8(output.at("path").get<std::string>())));
            return;
        }
        std::printf("LIVE Seedance status: %s\n", output.value("status", std::string{}).c_str());
    }
    std::printf("LIVE Seedance still running after bounded polling: %s\n", taskId.c_str());
    ++failures;
}

void testLiveRevisionWhenExplicitlyConfigured() {
    const char* key = std::getenv("MAI_ARK_LIVE_REVISION_KEY");
    if (key == nullptr || *key == '\0') return;
    const char* previousImage = std::getenv("MAI_ARK_LIVE_PREVIOUS_IMAGE");
    const char* previousVideoTask = std::getenv("MAI_ARK_LIVE_PREVIOUS_VIDEO_TASK");
    CHECK(previousImage != nullptr && previousVideoTask != nullptr);
    if (previousImage == nullptr || previousVideoTask == nullptr) return;
    const MaiFilePath workspace = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_ark_revision_")));
    CHECK(!MaiFileSystem::createDirectories(workspace));
    std::string priorImageBytes;
    CHECK(!MaiFileSystem::readFile(MaiFilePath::fromUtf8(previousImage), priorImageBytes));
    const MaiFilePath imagePath = workspace.append(MaiFilePath::fromUtf8("previous.png"));
    CHECK(!MaiFileSystem::writeFile(imagePath, priorImageBytes));
    auto store = makeMaiMemoryStore();
    MaiSession session;
    session.id = MaiIdGenerator::newSessionId();
    session.directory = workspace.toUtf8();
    store->putSession(session);
    MaiToolContext context;
    context.sessionId = session.id;
    context.root = workspace.toUtf8();
    context.specialistTasks = store.get();
    const std::string apiKey = key;

    MaiSpecialistTask priorImageTask;
    priorImageTask.id = MaiIdGenerator::generate("spt_");
    priorImageTask.ownerSessionId = session.id;
    priorImageTask.specialistName = "seedream_image";
    priorImageTask.intent = "Blend a red circle with a blue diamond";
    priorImageTask.contextSummary = "No people or text";
    priorImageTask.outputPath = imagePath.toUtf8();
    CHECK(!store->insertSpecialistTask(priorImageTask));
    auto image = makeMaiSeedreamImageTool([apiKey] { return apiKey; });
    const nlohmann::json imageRevision = {
        {"action", "revise"},
        {"message", "Make the background softer while preserving the circle and diamond"},
        {"parent_task_id", priorImageTask.id},
        {"output_path", "live-revised-image.png"},
        {"size", "1K"}};
    const MaiToolResult imageResult = image->execute(imageRevision.dump(), context);
    if (imageResult.hasError()) {
        std::printf("LIVE Seedream revision failed: %s\n", imageResult.error().message().c_str());
        ++failures;
    } else {
        const auto output = nlohmann::json::parse(imageResult.output());
        CHECK(output.value("revision_of", std::string{}) == priorImageTask.id);
        std::printf("LIVE Seedream revision output: %s\n",
                    output.at("path").get<std::string>().c_str());
    }

    MaiSpecialistTask priorVideoTask;
    priorVideoTask.id = MaiIdGenerator::generate("spt_");
    priorVideoTask.ownerSessionId = session.id;
    priorVideoTask.specialistName = "seedance_video";
    priorVideoTask.providerTaskId = previousVideoTask;
    priorVideoTask.intent = "Transition from a red circle to a blue diamond";
    priorVideoTask.contextSummary = "No people or text";
    CHECK(!store->insertSpecialistTask(priorVideoTask));
    auto video = makeMaiSeedanceVideoTool([apiKey] { return apiKey; });
    const nlohmann::json videoRevision = {
        {"action", "revise"},
        {"message", "Make the transition smoother while retaining the first and last shapes"},
        {"conversation_id", priorVideoTask.id},
        {"production",
         {{"duration", 4},
          {"ratio", "adaptive"},
          {"resolution", "480p"},
          {"generate_audio", false}}}};
    const MaiToolResult submitted = video->execute(videoRevision.dump(), context);
    if (submitted.hasError()) {
        std::printf("LIVE Seedance revision submit failed: %s\n",
                    submitted.error().message().c_str());
        ++failures;
        return;
    }
    const auto task = nlohmann::json::parse(submitted.output());
    CHECK(task.value("revision_of", std::string{}) == priorVideoTask.id);
    const std::string localId = task.at("conversation_id").get<std::string>();
    std::printf("LIVE Seedance revision task: %s\n", task.at("task_id").get<std::string>().c_str());
    const nlohmann::json query = {
        {"action", "continue"}, {"message", "Check progress"}, {"conversation_id", localId}};
    for (int attempt = 0; attempt < 12; ++attempt) {
        const MaiToolResult progress = video->execute(query.dump(), context);
        if (progress.hasError()) {
            std::printf("LIVE Seedance revision failed: %s\n", progress.error().message().c_str());
            ++failures;
            return;
        }
        const auto output = nlohmann::json::parse(progress.output());
        if (output.value("status", std::string{}) == "succeeded") {
            std::printf("LIVE Seedance revision output: %s\n",
                        output.at("path").get<std::string>().c_str());
            CHECK(
                MaiFileSystem::exists(MaiFilePath::fromUtf8(output.at("path").get<std::string>())));
            return;
        }
    }
    std::printf("LIVE Seedance revision still running: %s\n", localId.c_str());
    ++failures;
}

}  // namespace

int main() {
    testDynamicCredentialAndToolIdentity();
    testInvalidInputsDoNotReachNetwork();
    testRevisionRejectsAnotherConversation();
    testLiveArkWhenExplicitlyConfigured();
    testLiveRevisionWhenExplicitlyConfigured();
    return failures == 0 ? 0 : 1;
}
