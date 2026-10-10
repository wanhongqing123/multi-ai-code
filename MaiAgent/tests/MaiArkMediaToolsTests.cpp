#include <cstdlib>
#include <cstdio>
#include <string>

#include <json.hpp>

#include "MaiArkMediaTools.h"
#include "MaiArkAssetTools.h"
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
    CHECK(nlohmann::json::parse(video->parametersSchema()).at("required") ==
          nlohmann::json::array({"action"}));
    CHECK(nlohmann::json::parse(video->parametersSchema())
              .at("properties")
              .at("model")
              .at("enum")
              .size() == 3);
    const std::string deprecatedField = std::string("reference_image_") + "paths";
    const nlohmann::json deprecatedInput = {{"action", "discover"},
                                            {deprecatedField, nlohmann::json::array()}};
    const MaiToolResult rejectedOldField = video->execute(deprecatedInput.dump(), context);
    CHECK(rejectedOldField.hasError());
    CHECK(rejectedOldField.error().message().find("unsupported Seedance parameter") !=
          std::string::npos);
    CHECK(nlohmann::json::parse(image->parametersSchema()).at("required") ==
          nlohmann::json::array({"action"}));
    CHECK(video->description().find("not configured") != std::string::npos);
    CHECK(image->description().find("not configured") != std::string::npos);
    const std::string discover = R"({"action":"discover","message":"capabilities"})";
    CHECK(!video->requiresPerCallApproval(discover));
    CHECK(!image->requiresPerCallApproval(discover));
    CHECK(video->requiresPerCallApproval(R"({"action":"delegate","message":"create"})"));
    CHECK(!video->requiresPerCallApproval(
        R"({"action":"delegate","mode":"edit","video_path":"clip.mp4"})"));
    CHECK(!video->requiresPerCallApproval(
        R"({"action":"delegate","mode":"reference","local_image_paths":["first.jpg"]})"));
    CHECK(video->requiresPerCallApproval(
        R"({"action":"delegate","mode":"reference","video_url":"https://example.com/source.mp4"})"));
    CHECK(image->requiresPerCallApproval(R"({"action":"revise","message":"adjust"})"));
    CHECK(!video->requiresPerCallApproval(R"({"action":"continue"})"));
    auto videoInfo = nlohmann::json::parse(video->execute(discover, context).output());
    auto imageInfo = nlohmann::json::parse(image->execute(discover, context).output());
    CHECK(!video->execute(R"({"action":"discover"})", context).hasError());
    CHECK(!image->execute(R"({"action":"discover"})", context).hasError());
    CHECK(!videoInfo.at("configured").get<bool>());
    CHECK(!imageInfo.at("configured").get<bool>());
    CHECK(videoInfo.at("capabilities").is_array());
    CHECK(videoInfo.dump().find("platform_virtual_avatar") != std::string::npos);
    CHECK(videoInfo.dump().find("authorized_real_portrait") != std::string::npos);
    CHECK(videoInfo.dump().find("real_portrait_h5_registration") != std::string::npos);
    CHECK(videoInfo.dump().find("multi_reference_video") != std::string::npos);
    CHECK(videoInfo.dump().find("local_portrait_asset_registration") != std::string::npos);
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
    CHECK(videoInfo.at("bound_model") == "doubao-seedance-2-0-mini-260615");
    CHECK(videoInfo.at("models").size() == 3);
    CHECK(videoInfo.at("models")[0].at("id") == "doubao-seedance-2-0-mini-260615");
    CHECK(videoInfo.at("models")[1].at("id") == "doubao-seedance-2-0-fast-260128");
    CHECK(videoInfo.at("models")[2].at("id") == "doubao-seedance-2-0-260128");
    CHECK(videoInfo.at("models")[2].at("duration_seconds").at("maximum") == 15);
    CHECK(videoInfo.at("models")[2].at("resolutions").dump().find("4k") != std::string::npos);
    CHECK(videoInfo.at("models").dump().find("doubao-seedance-2-5-260628") == std::string::npos);
    CHECK(videoInfo.at("capabilities")[0].at("tool_status") == "implemented_unverified");
    CHECK(videoInfo.dump().find("nine reference images") != std::string::npos);
    // discover 中的每条能力都必须解释边界，避免主模型把空限制当作无限制。
    for (const auto& capability : videoInfo.at("capabilities"))
        CHECK(!capability.at("limitation").get<std::string>().empty());
    CHECK(videoInfo.dump().find("multiple_reference_videos") != std::string::npos);
    const auto videoSchema = nlohmann::json::parse(video->parametersSchema());
    CHECK(videoSchema["properties"]["local_image_paths"]["maxItems"] == 9);
    CHECK(videoSchema["properties"]["reference_asset_ids"]["description"].get<std::string>().find(
              "Active Ark asset") != std::string::npos);
    CHECK(videoSchema["properties"]["video_path"]["description"].get<std::string>().find(
              "2-15 seconds") != std::string::npos);
    CHECK(video->description().find("Seedance 2.0 Mini (default)") != std::string::npos);
    CHECK(video->parametersSchema().find("doubao-seedance-2-5-260628") == std::string::npos);
    CHECK(video->description().find("mode=create with local_image_paths") != std::string::npos);
    CHECK(video->parametersSchema().find("reference_asset_ids") != std::string::npos);
    const MaiToolResult invalidAsset = video->execute(
        R"({"action":"delegate","message":"test","reference_asset_ids":["asset://bad"]})", context);
    CHECK(invalidAsset.hasError());
    CHECK(invalidAsset.error().message().find("reference_asset_ids") != std::string::npos);
    const MaiToolResult validAsset = video->execute(
        R"({"action":"delegate","message":"test","reference_asset_ids":["asset://asset-1234567890"]})",
        context);
    CHECK(validAsset.hasError());
    CHECK(validAsset.error().message().find("confirm duration") != std::string::npos);
    CHECK(video->description().find("group_type=LivenessFace") != std::string::npos);
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
          MaiSpecialistCapabilityStatus::UploadNotConfigured);
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
    nlohmann::json tooManyReferences = {{"action", "delegate"},
                                        {"message", "Create a video"},
                                        {"local_image_paths", nlohmann::json::array()}};
    for (int index = 0; index < 10; ++index)
        tooManyReferences["local_image_paths"].push_back("image.png");
    const auto excess = video->execute(tooManyReferences.dump(), context);
    CHECK(excess.hasError());
    CHECK(excess.error().message().find("at most 9") != std::string::npos);
    const auto tooLong = video->execute(
        R"({"action":"delegate","message":"Create a video","production":{"duration":16,"ratio":"16:9","resolution":"720p"}})",
        context);
    CHECK(tooLong.hasError());
    CHECK(tooLong.error().message().find("4 to 15") != std::string::npos);
    const auto disabled = video->execute(
        R"({"action":"delegate","model":"doubao-seedance-2-5-260628","message":"Create a video","production":{"duration":4,"ratio":"16:9","resolution":"480p"}})",
        context);
    CHECK(disabled.hasError());
    CHECK(disabled.error().message().find("disabled") != std::string::npos);
    CHECK(!video->requiresPerCallApproval(
        R"({"action":"delegate","model":"doubao-seedance-2-5-260628","message":"Create a video"})"));
    const auto unsupported4k = video->execute(
        R"({"action":"delegate","message":"Create a video","production":{"duration":10,"ratio":"16:9","resolution":"4k"}})",
        context);
    CHECK(unsupported4k.hasError());
    CHECK(unsupported4k.error().message().find("480p or 720p") != std::string::npos);
    const auto standard4k = video->execute(
        R"({"action":"delegate","model":"doubao-seedance-2-0-260128","message":"Animate the frame","image_path":"missing.png","production":{"duration":15,"ratio":"adaptive","resolution":"4k"}})",
        context);
    CHECK(standard4k.hasError());
    CHECK(standard4k.error().message().find("image_path must contain") != std::string::npos);
    const auto invalidEditDuration = video->execute(
        R"({"action":"delegate","mode":"edit","message":"Change the scene","video_url":"https://example.com/source.mp4","production":{"duration":20,"ratio":"adaptive","resolution":"720p"}})",
        context);
    CHECK(invalidEditDuration.hasError());
    CHECK(invalidEditDuration.error().message().find("4 to 15") != std::string::npos);
    const auto fastTooLong = video->execute(
        R"({"action":"delegate","model":"doubao-seedance-2-0-fast-260128","message":"Create a video","production":{"duration":16,"ratio":"16:9","resolution":"720p"}})",
        context);
    CHECK(fastTooLong.hasError());
    CHECK(fastTooLong.error().message().find("4 to 15") != std::string::npos);
    const auto miniHighResolution = video->execute(
        R"({"action":"delegate","model":"doubao-seedance-2-0-mini-260615","message":"Create a video","production":{"duration":10,"ratio":"16:9","resolution":"1080p"}})",
        context);
    CHECK(miniHighResolution.hasError());
    CHECK(miniHighResolution.error().message().find("480p or 720p") != std::string::npos);
    nlohmann::json fastReferences = {{"action", "delegate"},
                                     {"model", "doubao-seedance-2-0-fast-260128"},
                                     {"message", "Create a video"},
                                     {"local_image_paths", nlohmann::json::array()}};
    for (int index = 0; index < 10; ++index)
        fastReferences["local_image_paths"].push_back("missing.png");
    const auto tooManyFastImages = video->execute(fastReferences.dump(), context);
    CHECK(tooManyFastImages.hasError());
    CHECK(tooManyFastImages.error().message().find("at most 9") != std::string::npos);
    const auto miniFifteenSeconds = video->execute(
        R"({"action":"delegate","model":"doubao-seedance-2-0-mini-260615","message":"Animate a still","image_path":"missing.png","production":{"duration":15,"ratio":"16:9","resolution":"720p"}})",
        context);
    CHECK(miniFifteenSeconds.hasError());
    CHECK(miniFifteenSeconds.error().message().find("image_path must contain") !=
          std::string::npos);
    CHECK(
        video
            ->execute(
                R"({"action":"delegate","message":"Create a video","image_path":"first.png","local_image_paths":["other.png"]})",
                context)
            .hasError());
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
    CHECK(
        video
            ->execute(
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
        {"action", "continue"}, {"message", ""}, {"conversation_id", prior.id}};
    const MaiToolResult checked = video->execute(continuation.dump(), context);
    CHECK(checked.hasError());
    CHECK(checked.error().code() == MaiErrorCode::NotFound);
    const nlohmann::json withoutMessage = {{"action", "continue"}, {"conversation_id", prior.id}};
    CHECK(video->execute(withoutMessage.dump(), context).error().code() == MaiErrorCode::NotFound);

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

void testLocalVideoUploadHandoffWithoutArkSubmission() {
    const MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai-ark-video-upload-")));
    CHECK(!MaiFileSystem::createDirectories(root));
    const MaiFilePath source = root.append(MaiFilePath::fromUtf8("clip.mp4"));
    CHECK(!MaiFileSystem::writeFile(source, "synthetic video fixture"));
    int uploads = 0;
    auto video = makeMaiSeedanceVideoTool(
        [] { return std::string("test-key"); }, {},
        [&](const std::string& localPath, const MaiToolContext&) -> MaiResult<std::string> {
            ++uploads;
            std::uint64_t bytes = 0;
            CHECK(MaiFileSystem::fileSize(MaiFilePath::fromUtf8(localPath), bytes));
            CHECK(bytes == std::string("synthetic video fixture").size());
            return std::string("http://example.com/video.mp4");
        });
    MaiToolContext context;
    context.root = root.toUtf8();
    const auto discovered =
        nlohmann::json::parse(video->execute(R"({"action":"discover"})", context).output());
    bool uploadListed = false;
    for (const auto& capability : discovered.at("capabilities")) {
        if (capability.value("id", std::string{}) == "video_edit_from_local_file") {
            uploadListed = true;
            CHECK(capability.at("tool_status") == "implemented_unverified");
        }
    }
    CHECK(uploadListed);
    CHECK(discovered.at("reply").get<std::string>().find("local video upload is not configured") ==
          std::string::npos);
    const nlohmann::json valid = {{"action", "delegate"},
                                  {"mode", "edit"},
                                  {"message", "Change the background"},
                                  {"video_path", "clip.mp4"}};
    auto missing = valid;
    missing["video_path"] = "missing.mp4";
    CHECK(video->execute(missing.dump(), context).hasError());
    CHECK(uploads == 0);
    const MaiToolResult result = video->execute(valid.dump(), context);
    CHECK(result.hasError());
    if (result.hasError())
        CHECK(result.error().message().find("public HTTPS URL") != std::string::npos);
    CHECK(uploads == 1);
    auto withPortrait = valid;
    withPortrait["message"] = "Replace the person using reference image 1";
    withPortrait["reference_asset_ids"] = {"asset-20261009130956-7x8lv"};
    withPortrait["production"] = {{"duration", -1}, {"ratio", "adaptive"}, {"resolution", "720p"}};
    const MaiToolResult editWithPortrait = video->execute(withPortrait.dump(), context);
    CHECK(editWithPortrait.hasError());
    if (editWithPortrait.hasError())
        CHECK(editWithPortrait.error().message().find("public HTTPS URL") != std::string::npos);
    CHECK(uploads == 2);
    MaiFileSystem::removeRecursively(root);
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
        R"({"action":"delegate","message":"Make a smooth abstract transition from the first frame to the last frame. No people or text.","mode":"create","image_path":"first.png","last_frame_path":"last.png","production":{"duration":4,"ratio":"adaptive","resolution":"480p","generate_audio":false}})",
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

void testVirtualAssetFlow() {
    const MaiFilePath workspace = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("ark_assets_")));
    CHECK(!MaiFileSystem::createDirectories(workspace));
    const MaiFilePath image = workspace.append(MaiFilePath::fromUtf8("portrait.jpg"));
    CHECK(!MaiFileSystem::writeFile(image, "test image bytes"));
    MaiToolContext context;
    context.root = workspace.toUtf8();
    int calls = 0;
    auto tool = makeMaiArkAssetTool([&](const std::string& raw,
                                        const MaiToolContext&) -> MaiResult<std::string> {
        const auto request = nlohmann::json::parse(raw);
        ++calls;
        if (request.at("action") == "upload_image") {
            const MaiFilePath submitted =
                MaiFilePath::fromUtf8(request.at("image_path").get<std::string>());
            CHECK(submitted.baseName().toUtf8() == "portrait.jpg");
            CHECK(MaiFileSystem::exists(submitted));
            CHECK(request.at("group_id") == "group-1234567890");
            return std::string{R"({"Id":"asset-1234567890","Status":"Processing"})"};
        }
        if (request.at("action") == "create_asset") {
            CHECK(request.at("url") == "https://example.com/portrait.jpg");
            return std::string{R"({"Id":"asset-1234567890","Status":"Processing"})"};
        }
        return std::string{
            R"({"Id":"asset-1234567890","Status":"Active","URL":"https://asset.example.com/image.jpg?X-Tos-Signature=secret"})"};
    });
    CHECK(tool->name() == "ark_assets");
    CHECK(!tool->requiresApproval(R"({"action":"get_asset","asset_id":"asset-1234567890"})"));
    CHECK(tool->requiresApproval(R"({"action":"upload_image"})"));
    const auto uploaded = tool->execute(
        R"({"action":"upload_image","group_id":"group-1234567890","image_path":"portrait.jpg"})",
        context);
    CHECK(!uploaded.hasError());
    CHECK(nlohmann::json::parse(uploaded.output()).at("Status") == "Processing");
    const auto fromUrl = tool->execute(
        R"({"action":"create_asset","group_id":"group-1234567890","url":"https://example.com/portrait.jpg"})",
        context);
    CHECK(!fromUrl.hasError());
    const auto ready =
        tool->execute(R"({"action":"get_asset","asset_id":"asset-1234567890"})", context);
    CHECK(!ready.hasError());
    CHECK(nlohmann::json::parse(ready.output()).at("asset_uri") == "asset://asset-1234567890");
    CHECK(!nlohmann::json::parse(ready.output()).contains("URL"));
    CHECK(calls == 3);
    CHECK(
        tool->execute(
                R"({"action":"upload_image","group_id":"group-1234567890","image_path":"missing.jpg"})",
                context)
            .hasError());
    CHECK(calls == 3);
    CHECK(!MaiFileSystem::removeFile(image));
}

void testRealPortraitValidationHandoff() {
    MaiToolContext context;
    int calls = 0;
    auto tool = makeMaiArkAssetTool(
        [&](const std::string& raw, const MaiToolContext&) -> MaiResult<std::string> {
            const auto request = nlohmann::json::parse(raw);
            ++calls;
            if (request.at("action") == "begin_real_validation")
                return std::string{
                    R"({"BytedToken":"vvs-test","H5Link":"https://ark.example.com/verify"})"};
            if (request.at("action") == "get_real_validation") {
                CHECK(request.at("byted_token") == "vvs-test");
                return std::string{R"({"GroupId":"group-real-123456"})"};
            }
            CHECK(request.at("group_type") == "LivenessFace");
            return std::string{R"({"Items":[]})"};
        });
    CHECK(tool->requiresApproval(R"({"action":"begin_real_validation"})"));
    CHECK(tool->requiresPerCallApproval(R"({"action":"begin_real_validation"})"));
    CHECK(!tool->requiresApproval(R"({"action":"get_real_validation"})"));
    const MaiToolResult begin = tool->execute(R"({"action":"begin_real_validation"})", context);
    CHECK(!begin.hasError());
    CHECK(nlohmann::json::parse(begin.output()).at("H5Link") == "https://ark.example.com/verify");
    const MaiToolResult finish =
        tool->execute(R"({"action":"get_real_validation","byted_token":"vvs-test"})", context);
    CHECK(!finish.hasError());
    CHECK(nlohmann::json::parse(finish.output()).at("GroupId") == "group-real-123456");
    const MaiToolResult groups =
        tool->execute(R"({"action":"list_groups","group_type":"LivenessFace"})", context);
    CHECK(!groups.hasError());
    CHECK(calls == 3);
}

void testLiveAssetServiceWhenExplicitlyConfigured() {
    const char* address = std::getenv("MAI_ARK_ASSET_SERVICE_TEST_URL");
    const char* token = std::getenv("MAI_ARK_ASSET_SERVICE_TEST_TOKEN");
    const char* certificate = std::getenv("MAI_ARK_ASSET_SERVICE_TEST_CA");
    if (address == nullptr || *address == '\0' || token == nullptr || *token == '\0') return;
    auto tool = makeMaiArkAssetTool(makeMaiArkAssetServiceProvider(
        [address, token] {
            return MaiResult<MaiArkAssetServiceSettings>(
                MaiArkAssetServiceSettings{address, token});
        },
        certificate == nullptr ? std::string{} : std::string(certificate)));
    MaiToolContext context;
    const MaiToolResult result = tool->execute(R"({"action":"list_groups"})", context);
    if (result.hasError())
        std::printf("LIVE Ark Assets list_groups failed: %s\n", result.error().message().c_str());
    CHECK(!result.hasError());
    if (!result.hasError()) {
        const auto groups = nlohmann::json::parse(result.output(), nullptr, false);
        CHECK(groups.is_object());
        CHECK(groups.value("Items", nlohmann::json{}).is_array());
    }
    if (std::getenv("MAI_ARK_ASSET_SERVICE_TEST_CHECK_WRITE_RETRY") != nullptr) {
        const MaiToolResult write =
            tool->execute(R"({"action":"create_group","name":"retry-check"})", context);
        CHECK(write.hasError());
        if (write.hasError()) {
            const auto error = nlohmann::json::parse(write.error().message(), nullptr, false);
            CHECK(error.is_object());
            if (error.is_object()) {
                CHECK(error.value("http_status", 0) == 503);
                CHECK(error.value("attempts", 0) == 1);
            }
        }
    }
    const char* image = std::getenv("MAI_ARK_ASSET_SERVICE_TEST_IMAGE");
    const char* group = std::getenv("MAI_ARK_ASSET_SERVICE_TEST_GROUP");
    if (image == nullptr || *image == '\0' || group == nullptr || *group == '\0') return;
    context.root = MaiFilePath::fromUtf8(image).dirName().toUtf8();
    const std::string input = nlohmann::json{{"action", "upload_image"},
                                             {"group_id", group},
                                             {"image_path", image},
                                             {"name", "MaiChat OSS integration check"}}
                                  .dump();
    const MaiToolResult uploaded = tool->execute(input, context);
    CHECK(!uploaded.hasError());
    if (!uploaded.hasError()) {
        const auto asset = nlohmann::json::parse(uploaded.output(), nullptr, false);
        CHECK(asset.value("Id", nlohmann::json{}).is_string());
        if (asset.value("Id", nlohmann::json{}).is_string())
            std::printf("LIVE Ark asset created: %s\n", asset["Id"].get<std::string>().c_str());
    }
}

void testLocalImagesStopBeforePaidSubmissionWhenOssFails() {
    const MaiFilePath source = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("oss_input_") + ".jpg"));
    CHECK(!MaiFileSystem::writeFile(source, "synthetic image fixture"));
    MaiToolContext context;
    context.root = source.dirName().toUtf8();
    int uploads = 0;
    const MaiCreativeMediaUploadProvider rejected =
        [&uploads](const std::string&, const MaiToolContext&) -> MaiResult<std::string> {
        ++uploads;
        return {MaiErrorCode::Network, "Synthetic OSS failure"};
    };
    auto video = makeMaiSeedanceVideoTool([] { return std::string("test-key"); }, {}, rejected);
    const nlohmann::json request = {
        {"action", "delegate"},
        {"message", "Animate the reference"},
        {"local_image_paths", nlohmann::json::array({source.baseName().toUtf8()})},
        {"production", {{"duration", 5}, {"ratio", "16:9"}, {"resolution", "720p"}}}};
    const MaiToolResult result = video->execute(request.dump(), context);
    CHECK(result.hasError());
    CHECK(result.error().message().find("upload_failed") != std::string::npos);
    CHECK(uploads == 1);
    (void)MaiFileSystem::removeFile(source);
}

void testPrivateOssUploaderAcceptsDocumentSourceWithoutBase64() {
    const MaiFilePath source = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("wan_document_") + ".pptx"));
    CHECK(!MaiFileSystem::writeFile(source, "synthetic presentation fixture"));
    MaiToolContext context;
    context.root = source.dirName().toUtf8();
    int configReads = 0;
    auto uploader = makeMaiPrivateOssMediaUploader([&]() -> MaiResult<MaiArkAssetServiceSettings> {
        ++configReads;
        return {MaiErrorCode::NotConfigured, "Synthetic signer unavailable"};
    });
    const auto document = uploader(source.baseName().toUtf8(), context);
    CHECK(!document);
    CHECK(configReads == 1);
    const auto unsupported = uploader("not-a-document.exe", context);
    CHECK(!unsupported);
    CHECK(unsupported.error().code() == MaiErrorCode::InvalidInput);
    CHECK(configReads == 1);
    (void)MaiFileSystem::removeFile(source);
}

void testSeedanceManagementRequiresOwnedConversation() {
    auto tool = makeMaiSeedanceVideoTool([] { return std::string("test-key"); });
    const auto schema = nlohmann::json::parse(tool->parametersSchema());
    CHECK(schema["properties"]["action"]["enum"].dump().find("cancel") != std::string::npos);
    CHECK(schema["properties"]["action"]["enum"].dump().find("delete") != std::string::npos);
    CHECK(tool->requiresPerCallApproval(R"({"action":"cancel"})"));
    CHECK(tool->requiresPerCallApproval(R"({"action":"delete"})"));
    MaiToolContext context;
    const MaiToolResult unknown =
        tool->execute(R"({"action":"delete","conversation_id":"provider-task-1"})", context);
    CHECK(unknown.hasError());
    CHECK(unknown.error().message().find("not_found") != std::string::npos);
}

}  // namespace

int main() {
    testDynamicCredentialAndToolIdentity();
    testInvalidInputsDoNotReachNetwork();
    testRevisionRejectsAnotherConversation();
    testLocalVideoUploadHandoffWithoutArkSubmission();
    testVirtualAssetFlow();
    testRealPortraitValidationHandoff();
    testLiveAssetServiceWhenExplicitlyConfigured();
    testLocalImagesStopBeforePaidSubmissionWhenOssFails();
    testPrivateOssUploaderAcceptsDocumentSourceWithoutBase64();
    testSeedanceManagementRequiresOwnedConversation();
    testLiveArkWhenExplicitlyConfigured();
    testLiveRevisionWhenExplicitlyConfigured();
    return failures == 0 ? 0 : 1;
}
