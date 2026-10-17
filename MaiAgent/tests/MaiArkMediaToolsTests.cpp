// 文件作用：验证“封装火山方舟媒体任务的提交、查询和结果取回”的正确行为和边界条件。
// 验证入口：testDynamicCredentialAndToolIdentity、testInvalidInputsDoNotReachNetwork、
// testRevisionRejectsAnotherConversation；检查可观察结果而非实现细节。
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(MAI_ARK_ASSET_TLS_TESTS)
#include <httplib.h>
#endif
#include <json.hpp>

#include "MaiArkMediaTools.h"
#include "MaiArkAssetTools.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiMemoryStore.h"

namespace {

int failures = 0;
std::string fakeProbeOutput;

int writeFakeProbeOutput(int argumentCount, char** arguments) {
    for (int index = 0; index + 1 < argumentCount; ++index) {
        if (std::string(arguments[index]) != "-o") continue;
        const MaiError written =
            MaiFileSystem::writeFile(MaiFilePath::fromUtf8(arguments[index + 1]), fakeProbeOutput);
        return written.hasError() ? 1 : 0;
    }
    return 1;
}

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

// 验证方舟媒体工具在每次调用时读取最新凭据，同时保持对模型公开的工具身份稳定。
// 使用本地假响应核对参数，不提交真实付费任务。
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
    CHECK(!video->requiresPerCallApproval(R"({"action":"delegate","mode":"reference",)"
                                          R"("video_url":"https://example.com/source.mp4"})"));
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
    CHECK(videoInfo.dump().find("nine Active Image assets") != std::string::npos);
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
    CHECK(video->description().find("local_image_paths") != std::string::npos);
    CHECK(!videoSchema["properties"].contains("video_url"));
    CHECK(video->parametersSchema().find("reference_asset_ids") != std::string::npos);
    const MaiToolResult invalidAsset = video->execute(
        R"({"action":"delegate","message":"test","reference_asset_ids":["asset://bad"]})", context);
    CHECK(invalidAsset.hasError());
    CHECK(invalidAsset.error().message().find("reference_asset_ids") != std::string::npos);
    const MaiToolResult validAsset = video->execute(
        R"({"action":"delegate","message":"test","reference_asset_ids":["asset://asset-1234567890")"
        R"(]})",
        context);
    CHECK(validAsset.hasError());
    CHECK(validAsset.error().message().find("confirm duration") != std::string::npos);
    CHECK(video->description().find("asset_group_type") != std::string::npos);
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
    MaiToolRegistry internal;
    internal.addInternal(makeMaiSeedanceVideoTool([&] { return key; }));
    internal.addInternal(makeMaiSeedreamImageTool([&] { return key; }));
    CHECK(internal.specs().empty());
    CHECK(internal.specialists().empty());
    CHECK(internal.find("seedance_video") != nullptr);
    CHECK(internal.findForModel("seedance_video") == nullptr);
    CHECK(internal.findForModel("seedream_image") == nullptr);
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
    nlohmann::json tooManyReferences = {{"action", "prepare"},
                                        {"message", "Create a video"},
                                        {"local_image_paths", nlohmann::json::array()}};
    for (int index = 0; index < 10; ++index)
        tooManyReferences["local_image_paths"].push_back("image.png");
    const auto excess = video->execute(tooManyReferences.dump(), context);
    CHECK(excess.hasError());
    CHECK(excess.error().message().find("at most 9") != std::string::npos);
    const auto tooLong = video->execute(
        R"({"action":"delegate","message":"Create a video","production":{"duration":16,)"
        R"("ratio":"16:9","resolution":"720p"}})",
        context);
    CHECK(tooLong.hasError());
    CHECK(tooLong.error().message().find("4 to 15") != std::string::npos);
    const auto disabled = video->execute(
        R"({"action":"delegate","model":"doubao-seedance-2-5-260628","message":"Create a video",)"
        R"("production":{"duration":4,"ratio":"16:9","resolution":"480p"}})",
        context);
    CHECK(disabled.hasError());
    CHECK(disabled.error().message().find("disabled") != std::string::npos);
    CHECK(!video->requiresPerCallApproval(
        R"({"action":"delegate","model":"doubao-seedance-2-5-260628",)"
        R"("message":"Create a video"})"));
    const auto unsupported4k = video->execute(
        R"({"action":"delegate","message":"Create a video","production":{"duration":10,)"
        R"("ratio":"16:9","resolution":"4k"}})",
        context);
    CHECK(unsupported4k.hasError());
    CHECK(unsupported4k.error().message().find("480p or 720p") != std::string::npos);
    const auto standard4k = video->execute(
        R"({"action":"prepare","model":"doubao-seedance-2-0-260128","message":"Animate the frame")"
        R"(,"image_path":"missing.png","asset_group_id":"group-test-123456",)"
        R"("asset_group_type":"AIGC","production":{"duration":15,"ratio":"adaptive",)"
        R"("resolution":"4k"}})",
        context);
    CHECK(standard4k.hasError());
    CHECK(standard4k.error().message().find("Local image must be") != std::string::npos);
    const auto invalidEditDuration =
        video->execute(R"({"action":"delegate","mode":"edit","message":"Change the scene",)"
                       R"("video_asset_id":"asset-video-123456","production":{"duration":20,)"
                       R"("ratio":"adaptive","resolution":"720p"}})",
                       context);
    CHECK(invalidEditDuration.hasError());
    CHECK(invalidEditDuration.error().message().find("4 to 15") != std::string::npos);
    const auto fastTooLong =
        video->execute(R"({"action":"delegate","model":"doubao-seedance-2-0-fast-260128",)"
                       R"("message":"Create a video","production":{"duration":16,"ratio":"16:9",)"
                       R"("resolution":"720p"}})",
                       context);
    CHECK(fastTooLong.hasError());
    CHECK(fastTooLong.error().message().find("4 to 15") != std::string::npos);
    const auto miniHighResolution =
        video->execute(R"({"action":"delegate","model":"doubao-seedance-2-0-mini-260615",)"
                       R"("message":"Create a video","production":{"duration":10,"ratio":"16:9",)"
                       R"("resolution":"1080p"}})",
                       context);
    CHECK(miniHighResolution.hasError());
    CHECK(miniHighResolution.error().message().find("480p or 720p") != std::string::npos);
    nlohmann::json fastReferences = {{"action", "prepare"},
                                     {"model", "doubao-seedance-2-0-fast-260128"},
                                     {"message", "Create a video"},
                                     {"local_image_paths", nlohmann::json::array()}};
    for (int index = 0; index < 10; ++index)
        fastReferences["local_image_paths"].push_back("missing.png");
    const auto tooManyFastImages = video->execute(fastReferences.dump(), context);
    CHECK(tooManyFastImages.hasError());
    CHECK(tooManyFastImages.error().message().find("at most 9") != std::string::npos);
    const auto miniFifteenSeconds =
        video->execute(R"({"action":"prepare","model":"doubao-seedance-2-0-mini-260615",)"
                       R"("message":"Animate a still","image_path":"missing.png",)"
                       R"("asset_group_id":"group-test-123456","asset_group_type":"AIGC",)"
                       R"("production":{"duration":15,)"
                       R"("ratio":"16:9","resolution":"720p"}})",
                       context);
    CHECK(miniFifteenSeconds.hasError());
    CHECK(miniFifteenSeconds.error().message().find("Local image must be") != std::string::npos);
    CHECK(
        video
            ->execute(R"({"action":"delegate","message":"Create a video","image_path":"first.png",)"
                      R"("local_image_paths":["other.png"]})",
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
        R"({"action":"delegate","message":"test","virtual_avatar_asset_id":"https://example.com/fa)"
        R"(ce.jpg"})",
        context);
    CHECK(invalidAvatar.hasError());
    CHECK(invalidAvatar.error().message().find("asset ID") != std::string::npos);
    const auto authorized =
        video->execute(R"({"action":"delegate","message":"Keep the authorized person's face",)"
                       R"("authorized_portrait_asset_id":"asset://asset-20261005230545-tb2zr"})",
                       context);
    CHECK(authorized.hasError());
    CHECK(authorized.error().message().find("confirm duration") != std::string::npos);
    CHECK(
        video
            ->execute(
                R"({"action":"delegate","message":"test","virtual_avatar_asset_id":"asset-20260401)"
                R"(123823-6d4x2","authorized_portrait_asset_id":"asset-20261005230545-tb2zr"})",
                context)
            .hasError());
    CHECK(video
              ->execute(
                  R"({"action":"delegate","message":"test","mode":"edit",)"
                  R"("video_url":"https://example.com/clip.mp4","virtual_avatar_asset_id":"asset)"
                  R"(-20260401123823-6d4x2"})",
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
    CHECK(image
              ->execute(R"({"action":"delegate","message":"test","image_paths":["a.png","b.png"],)"
                        R"("image_path":"c.png"})",
                        context)
              .hasError());
}

// 新资产入口只验证 ID、模式和互斥关系；有效请求停在付费规格校验前。
// 测试不发起 Ark 视频任务，content[] 的 role 映射另由源码核对。
void testSeedanceAssetInputBoundaries() {
    auto video = makeMaiSeedanceVideoTool([] { return std::string("test-key"); });
    MaiToolContext context;
    const auto schema = nlohmann::json::parse(video->parametersSchema());
    for (const char* field : {"first_frame_asset_id", "last_frame_asset_id", "video_asset_id"})
        CHECK(schema["properties"].contains(field));
    CHECK(video->description().find("video_asset_id") != std::string::npos);

    const auto rejected = [&](const nlohmann::json& input, const std::string& reason) {
        const MaiToolResult result = video->execute(input.dump(), context);
        CHECK(result.hasError());
        if (result.hasError() && result.error().message().find(reason) == std::string::npos) {
            std::printf("Unexpected Seedance rejection: %s\n", result.error().message().c_str());
            CHECK(false);
        }
    };
    const nlohmann::json create = {{"action", "delegate"}, {"message", "Create a video"}};
    const std::string first = "asset://asset-first-123456";
    const std::string last = "asset-last-123456";
    const std::string source = "asset://asset-video-123456";

    auto request = create;
    request["first_frame_asset_id"] = first;
    rejected(request, "confirm duration");
    request["last_frame_asset_id"] = last;
    rejected(request, "confirm duration");
    request["last_frame_asset_id"] = "last-bad";
    rejected(request, "last_frame_asset_id must be a valid");
    request = create;
    request["first_frame_asset_id"] = "https://example.com/first.jpg";
    rejected(request, "first_frame_asset_id must be a valid");
    request = create;
    request["last_frame_asset_id"] = last;
    rejected(request, "last frame requires create mode and a first frame");
    request["image_path"] = "first.jpg";
    rejected(request, "Run action=prepare");
    request["action"] = "prepare";
    rejected(request, "Local image must be");
    request = create;
    request["action"] = "prepare";
    request["first_frame_asset_id"] = first;
    request["last_frame_path"] = "last.jpg";
    rejected(request, "Local image must be");
    request["last_frame_asset_id"] = last;
    rejected(request, "choose either last_frame_path or last_frame_asset_id");
    request = create;
    request["action"] = "prepare";
    request["first_frame_asset_id"] = first;
    request["image_path"] = "first.jpg";
    rejected(request, "choose either image_path or first_frame_asset_id");
    request = create;
    request["first_frame_asset_id"] = first;
    request["reference_asset_ids"] = nlohmann::json::array({"asset-reference-123456"});
    rejected(request, "reference images cannot be mixed");
    request = create;
    request["first_frame_asset_id"] = first;
    request["authorized_portrait_asset_id"] = "asset-person-123456";
    rejected(request, "portrait asset cannot be mixed");
    request = create;
    request["first_frame_asset_id"] = 7;
    rejected(request, "first_frame_asset_id must be a string");

    request = create;
    request["mode"] = "reference";
    request["video_asset_id"] = source;
    rejected(request, "confirm duration");
    CHECK(video->requiresPerCallApproval(request.dump()));
    request["video_url"] = "https://example.com/source.mp4";
    rejected(request, "unsupported Seedance parameter: video_url");
    CHECK(!video->requiresPerCallApproval(request.dump()));
    request.erase("video_url");
    request["video_task_id"] = "previous-task-123456";
    rejected(request, "provide one video source");
    CHECK(!video->requiresPerCallApproval(request.dump()));
    request.erase("video_task_id");
    request["video_path"] = "source.mp4";
    rejected(request, "Run action=prepare");
    request["action"] = "prepare";
    rejected(request, "provide one video source");
    CHECK(!video->requiresPerCallApproval(request.dump()));
    request = create;
    request["mode"] = "edit";
    request["video_asset_id"] = "asset-video-123456";
    request["production"] = {{"duration", 20}};
    rejected(request, "duration must be -1 or 4 to 15");
    CHECK(video->requiresPerCallApproval(request.dump()));
    request["mode"] = "extend";
    rejected(request, "duration must be -1 or 4 to 15");
    request = create;
    request["mode"] = "reference";
    request["video_asset_id"] = "asset://bad";
    rejected(request, "video_asset_id must be a valid");
    request = create;
    request["video_asset_id"] = source;
    rejected(request, "provide one video source");
    request = create;
    request["mode"] = "edit";
    request["video_asset_id"] = source;
    request["first_frame_asset_id"] = first;
    rejected(request, "first_frame_asset_id requires create mode");
    request = create;
    request["mode"] = "reference";
    request["video_url"] = source;
    request["production"] = {{"duration", 4}, {"ratio", "adaptive"}, {"resolution", "480p"}};
    rejected(request, "unsupported Seedance parameter: video_url");
}

void testSeedanceChecksAssetTypeAndPortraitGroup() {
    int assetReads = 0;
    int groupReads = 0;
    const MaiArkAssetProvider assets = [&](const std::string& raw,
                                           const MaiToolContext&) -> MaiResult<std::string> {
        const auto request = nlohmann::json::parse(raw);
        if (request.at("action") == "get_group") {
            ++groupReads;
            const std::string id = request.at("group_id").get<std::string>();
            if (id == "group-real-123456")
                return std::string(R"({"Id":"group-real-123456","GroupType":"LivenessFace"})");
            CHECK(id == "group-virtual-123456");
            return std::string(R"({"Id":"group-virtual-123456","GroupType":"AIGC"})");
        }
        CHECK(request.at("action") == "get_asset");
        ++assetReads;
        const std::string id = request.at("asset_id").get<std::string>();
        if (id == "asset-real-123456")
            return std::string(R"({"Id":"asset-real-123456","Status":"Active","AssetType":"Image",)"
                               R"("GroupId":"group-real-123456"})");
        if (id == "asset-video-123456")
            return std::string(
                R"({"Id":"asset-video-123456","Status":"Active","AssetType":"Image",)"
                R"("GroupId":"group-virtual-123456"})");
        CHECK(id == "asset-virtual-123456");
        return std::string(R"({"Id":"asset-virtual-123456","Status":"Active","AssetType":"Image",)"
                           R"("GroupId":"group-virtual-123456"})");
    };
    auto video = makeMaiSeedanceVideoTool([] { return std::string("test-key"); }, {}, assets);
    MaiToolContext context;
    const nlohmann::json virtualRequest = {{"action", "prepare"},
                                           {"virtual_avatar_asset_id", "asset-virtual-123456"}};
    const MaiToolResult virtualResult = video->execute(virtualRequest.dump(), context);
    CHECK(!virtualResult.hasError());
    CHECK(nlohmann::json::parse(virtualResult.output()).at("status") == "ready");
    auto request = virtualRequest;
    request.erase("virtual_avatar_asset_id");
    request["authorized_portrait_asset_id"] = "asset-real-123456";
    const MaiToolResult authorized = video->execute(request.dump(), context);
    CHECK(!authorized.hasError());
    CHECK(nlohmann::json::parse(authorized.output()).at("status") == "ready");
    request["authorized_portrait_asset_id"] = "asset-virtual-123456";
    const MaiToolResult wrongPortrait = video->execute(request.dump(), context);
    CHECK(wrongPortrait.hasError());
    CHECK(wrongPortrait.error().message().find("does not match") != std::string::npos);
    request = virtualRequest;
    request["virtual_avatar_asset_id"] = "asset-real-123456";
    const MaiToolResult wrongAvatar = video->execute(request.dump(), context);
    CHECK(wrongAvatar.hasError());
    CHECK(wrongAvatar.error().message().find("does not match") != std::string::npos);
    request = {{"action", "prepare"},
               {"reference_asset_ids", nlohmann::json::array({"asset-virtual-123456"})},
               {"asset_group_id", "group-real-123456"},
               {"asset_group_type", "LivenessFace"}};
    const MaiToolResult wrongGroup = video->execute(request.dump(), context);
    CHECK(wrongGroup.hasError());
    CHECK(wrongGroup.error().message().find("not in the requested asset group") !=
          std::string::npos);
    request = {
        {"action", "prepare"}, {"mode", "reference"}, {"video_asset_id", "asset-video-123456"}};
    const MaiToolResult wrongType = video->execute(request.dump(), context);
    CHECK(wrongType.hasError());
    CHECK(wrongType.error().message().find("not a Video asset") != std::string::npos);
    CHECK(assetReads > 0);
    CHECK(groupReads > 0);
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
    int assetUploads = 0;
    int groupChecks = 0;
    int assetChecks = 0;
    MaiVideoMetadata measuredVideo{5.0, 30.0};
    int videoProbes = 0;
    const MaiVideoMetadataProvider videoMetadata =
        [&](const std::string& path, const MaiToolContext&) -> MaiResult<MaiVideoMetadata> {
        ++videoProbes;
        CHECK(path == "clip.mp4");
        return measuredVideo;
    };
    const MaiArkAssetProvider assets = [&](const std::string& raw,
                                           const MaiToolContext&) -> MaiResult<std::string> {
        const auto request = nlohmann::json::parse(raw);
        const std::string action = request.at("action").get<std::string>();
        if (action == "get_group") {
            ++groupChecks;
            CHECK(request.at("group_id") == "group-test-123456");
            return std::string(R"({"Id":"group-test-123456","GroupType":"AIGC"})");
        }
        if (action == "upload_video") {
            ++assetUploads;
            const MaiFilePath submitted =
                MaiFilePath::fromUtf8(request.at("video_path").get<std::string>());
            CHECK(submitted.baseName().toUtf8() == "clip.mp4");
            CHECK(MaiFileSystem::exists(submitted));
            CHECK(request.at("expected_group_type") == "AIGC");
            return std::string(R"({"Id":"asset-video-123456","Status":"Processing"})");
        }
        CHECK(action == "get_asset");
        ++assetChecks;
        CHECK(request.at("asset_id") == "asset-video-123456");
        return std::string(R"({"Id":"asset-video-123456","Status":"Failed","AssetType":"Video"})");
    };
    auto video =
        makeMaiSeedanceVideoTool([] { return std::string("test-key"); }, {}, assets, videoMetadata);
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
    const nlohmann::json request = {{"action", "prepare"},
                                    {"mode", "edit"},
                                    {"message", "Change the background"},
                                    {"video_path", "clip.mp4"},
                                    {"asset_group_id", "group-test-123456"},
                                    {"asset_group_type", "AIGC"}};
    CHECK(!video->requiresPerCallApproval(request.dump()));
    auto withoutGroup = request;
    withoutGroup.erase("asset_group_id");
    CHECK(!video->requiresPerCallApproval(withoutGroup.dump()));
    CHECK(video->execute(withoutGroup.dump(), context).hasError());
    CHECK(groupChecks == 0);
    auto withoutProbe =
        makeMaiSeedanceVideoTool([] { return std::string("test-key"); }, {}, assets);
    CHECK(withoutProbe->execute(request.dump(), context).error().message().find("FFprobe") !=
          std::string::npos);
    measuredVideo.durationSeconds = 15.1;
    CHECK(video->execute(request.dump(), context).error().message().find("2-15") !=
          std::string::npos);
    measuredVideo = {5.0, 23.9};
    CHECK(video->execute(request.dump(), context).error().message().find("24-60") !=
          std::string::npos);
    measuredVideo = {5.0, 30.0};
    CHECK(groupChecks == 0);
    auto wrongType = request;
    wrongType["asset_group_type"] = "LivenessFace";
    const MaiToolResult mismatched = video->execute(wrongType.dump(), context);
    CHECK(mismatched.hasError());
    CHECK(mismatched.error().message().find("does not match") != std::string::npos);
    CHECK(groupChecks == 1);
    CHECK(assetUploads == 0);
    const MaiToolResult started = video->execute(request.dump(), context);
    CHECK(!started.hasError());
    const auto pending = nlohmann::json::parse(started.output());
    CHECK(pending.at("status") == "processing");
    const auto retry = pending.at("prepared_arguments");
    CHECK(retry.at("video_asset_id") == "asset-video-123456");
    CHECK(retry.at("source_duration_seconds") == 5.0);
    CHECK(retry.at("source_fps") == 30.0);
    CHECK(!retry.contains("video_path"));
    const MaiToolResult result = video->execute(retry.dump(), context);
    CHECK(result.hasError());
    CHECK(result.error().message().find("asset_failed") != std::string::npos);
    CHECK(groupChecks == 3);
    CHECK(assetUploads == 1);
    CHECK(assetChecks == 1);
    CHECK(videoProbes == 5);
    MaiFileSystem::removeRecursively(root);
}

// 验证强类型探测回调解析 FFprobe 的容器时长和分数帧率，避免 Seedance 接受估算值。
void testFfprobeVideoMetadataProviderUsesMeasuredValues() {
    const MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai-video-metadata-")));
    CHECK(!MaiFileSystem::createDirectories(root));
    const MaiFilePath source = root.append(MaiFilePath::fromUtf8("clip.mp4"));
    CHECK(!MaiFileSystem::writeFile(source, "synthetic video fixture"));
    MaiFfmpegEngine engine;
    engine.runFfprobe = writeFakeProbeOutput;
    const MaiVideoMetadataProvider probe = makeMaiFfprobeVideoMetadataProvider(engine);
    CHECK(static_cast<bool>(probe));
    MaiToolContext context;
    context.root = root.toUtf8();
    fakeProbeOutput =
        R"({"streams":[{"avg_frame_rate":"30000/1001"}],"format":{"duration":"14.700"}})";
    const MaiResult<MaiVideoMetadata> measured = probe("clip.mp4", context);
    CHECK(measured);
    if (measured) {
        CHECK(std::abs(measured.value().durationSeconds - 14.7) < 0.001);
        CHECK(std::abs(measured.value().framesPerSecond - 29.97003) < 0.001);
    }
    fakeProbeOutput = R"({"streams":[{}],"format":{"duration":"14.700"}})";
    const MaiResult<MaiVideoMetadata> missingRate = probe("clip.mp4", context);
    CHECK(!missingRate);
    if (!missingRate) CHECK(missingRate.error().message().find("frame rate") != std::string::npos);
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
        R"({"action":"delegate","message":"Create one abstract composition blending the colors of )"
        R"(image 1 and image 2. No people or text.","image_paths":["first.png","last.png"],)"
        R"("output_path":"live-seedream.png","size":"1K"})",
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
        R"({"action":"delegate","message":"Make a smooth abstract transition from the first frame )"
        R"(to the last frame. No people or text.","mode":"create","image_path":"first.png",)"
        R"("last_frame_path":"last.png","production":{"duration":4,"ratio":"adaptive",)"
        R"("resolution":"480p","generate_audio":false}})",
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
            R"({"Id":"asset-1234567890","Status":"Active","URL":"https://asset.example.com/image.j)"
            R"(pg?X-Tos-Signature=secret"})"};
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
        R"({"action":"create_asset","group_id":"group-1234567890","url":"https://example.com/portr)"
        R"(ait.jpg"})",
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
                R"({"action":"upload_image","group_id":"group-1234567890","image_path":"missing.jp)"
                R"(g"})",
                context)
            .hasError());
    CHECK(calls == 3);
    CHECK(!MaiFileSystem::removeFile(image));
}

void testImageAssetGroupPreflight() {
    const MaiFilePath workspace = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("ark_image_group_")));
    CHECK(!MaiFileSystem::createDirectories(workspace));
    const MaiFilePath image = workspace.append(MaiFilePath::fromUtf8("portrait.jpg"));
    CHECK(!MaiFileSystem::writeFile(image, "synthetic image bytes"));
    MaiToolContext context;
    context.root = workspace.toUtf8();
    int lookups = 0;
    int uploads = 0;
    auto tool = makeMaiArkAssetTool(
        [&](const std::string& raw, const MaiToolContext&) -> MaiResult<std::string> {
            const auto request = nlohmann::json::parse(raw);
            if (request.at("action") == "get_group") {
                ++lookups;
                CHECK(request.at("group_id") == "group-verified-123456");
                return std::string{R"({"Id":"group-verified-123456","GroupType":"LivenessFace"})"};
            }
            ++uploads;
            CHECK(request.at("action") == "upload_image");
            CHECK(request.at("expected_group_type") == "LivenessFace");
            return std::string{R"({"Id":"asset-image-123456","Status":"Processing"})"};
        });
    const auto request = nlohmann::json{{"action", "upload_image"},
                                        {"group_id", "group-verified-123456"},
                                        {"image_path", "portrait.jpg"},
                                        {"expected_group_type", "AIGC"}};
    CHECK(tool->execute(request.dump(), context).hasError());
    CHECK(lookups == 1);
    CHECK(uploads == 0);
    auto invalid = request;
    invalid["expected_group_type"] = true;
    CHECK(tool->execute(invalid.dump(), context).hasError());
    CHECK(lookups == 1);
    auto matched = request;
    matched["expected_group_type"] = "LivenessFace";
    const MaiToolResult result = tool->execute(matched.dump(), context);
    CHECK(!result.hasError());
    CHECK(lookups == 2);
    CHECK(uploads == 1);
    CHECK(!MaiFileSystem::removeFile(image));
}

void testLocalVideoAssetRegistration() {
    const MaiFilePath workspace = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("ark_video_asset_")));
    CHECK(!MaiFileSystem::createDirectories(workspace));
    const MaiFilePath video = workspace.append(MaiFilePath::fromUtf8("reference.MOV"));
    const MaiFilePath other = workspace.append(MaiFilePath::fromUtf8("invalid.txt"));
    const MaiFilePath empty = workspace.append(MaiFilePath::fromUtf8("empty.mp4"));
    CHECK(!MaiFileSystem::writeFile(video, "synthetic video bytes"));
    CHECK(!MaiFileSystem::writeFile(other, "synthetic text bytes"));
    CHECK(!MaiFileSystem::writeFile(empty, ""));
    MaiToolContext context;
    context.root = workspace.toUtf8();
    int calls = 0;
    const MaiArkAssetProvider provider = [&](const std::string& raw,
                                             const MaiToolContext&) -> MaiResult<std::string> {
        const auto request = nlohmann::json::parse(raw);
        ++calls;
        if (request.at("action") == "get_group") {
            CHECK(request.at("group_id") == "group-verified-123456");
            return std::string{R"({"Id":"group-verified-123456","GroupType":"LivenessFace"})"};
        }
        if (request.at("action") == "upload_video") {
            CHECK(request.at("group_id") == "group-verified-123456");
            CHECK(request.at("expected_group_type") == "LivenessFace");
            CHECK(request.at("name") == "Reference clip");
            const MaiFilePath submitted =
                MaiFilePath::fromUtf8(request.at("video_path").get<std::string>());
            CHECK(submitted.baseName().toUtf8() == "reference.MOV");
            CHECK(MaiFileSystem::exists(submitted));
            return std::string{R"({"Id":"asset-video-123456","Status":"Processing",)"
                               R"("URL":"https://example.com/video.mov?X-Tos-Signature=secret"})"};
        }
        CHECK(request.at("asset_id") == "asset-video-123456");
        return std::string{R"({"Id":"asset-video-123456","AssetType":"Video","Status":"Active",)"
                           R"("URL":"https://example.com/video.mov?X-Tos-Signature=secret"})"};
    };
    auto tool = makeMaiArkAssetTool(provider);
    const auto schema = nlohmann::json::parse(tool->parametersSchema());
    CHECK(schema.at("properties").at("action").at("enum").dump().find("upload_video") !=
          std::string::npos);
    CHECK(schema.at("properties").contains("video_path"));
    CHECK(schema.at("properties").contains("expected_group_type"));
    CHECK(tool->requiresApproval(R"({"action":"upload_video"})"));
    CHECK(!tool->requiresApproval(R"({"action":"get_group","group_id":"group-verified-123456"})"));
    const auto discovered =
        nlohmann::json::parse(tool->execute(R"({"action":"discover"})", context).output());
    CHECK(discovered.at("actions").dump().find("upload_video") != std::string::npos);
    const auto request = nlohmann::json{{"action", "upload_video"},
                                        {"group_id", "group-verified-123456"},
                                        {"video_path", "reference.MOV"},
                                        {"name", "Reference clip"},
                                        {"expected_group_type", "LivenessFace"}};
    const MaiToolResult uploaded = tool->execute(request.dump(), context);
    CHECK(!uploaded.hasError());
    if (!uploaded.hasError()) {
        const auto result = nlohmann::json::parse(uploaded.output());
        CHECK(result.at("Id") == "asset-video-123456");
        CHECK(result.at("Status") == "Processing");
        CHECK(!result.contains("URL"));
    }
    const MaiResult<MaiArkVideoAsset> direct =
        uploadMaiArkVideoAsset(provider, "group-verified-123456", "reference.MOV", "Reference clip",
                               "LivenessFace", context);
    CHECK(direct);
    if (direct) {
        CHECK(direct.value().id == "asset-video-123456");
        CHECK(direct.value().status == "Processing");
    }
    const MaiToolResult ready =
        tool->execute(R"({"action":"get_asset","asset_id":"asset-video-123456"})", context);
    CHECK(!ready.hasError());
    if (!ready.hasError()) {
        const auto result = nlohmann::json::parse(ready.output());
        CHECK(result.at("asset_uri") == "asset://asset-video-123456");
        CHECK(!result.contains("URL"));
    }
    CHECK(calls == 5);
    const MaiToolResult group =
        tool->execute(R"({"action":"get_group","group_id":"group-verified-123456"})", context);
    CHECK(!group.hasError());
    if (!group.hasError())
        CHECK(nlohmann::json::parse(group.output()).at("GroupType") == "LivenessFace");
    auto invalidRequest = request;
    invalidRequest["expected_group_type"] = "AIGC";
    CHECK(tool->execute(invalidRequest.dump(), context).hasError());
    CHECK(calls == 7);
    invalidRequest["group_id"] = "not-a-group";
    CHECK(tool->execute(invalidRequest.dump(), context).hasError());
    invalidRequest = request;
    invalidRequest["video_path"] = "invalid.txt";
    CHECK(tool->execute(invalidRequest.dump(), context).hasError());
    invalidRequest["video_path"] = "empty.mp4";
    CHECK(tool->execute(invalidRequest.dump(), context).hasError());
    invalidRequest["video_path"] = "missing.mp4";
    CHECK(tool->execute(invalidRequest.dump(), context).hasError());
    invalidRequest = request;
    invalidRequest["name"] = std::string(65, 'x');
    CHECK(tool->execute(invalidRequest.dump(), context).hasError());
    invalidRequest = request;
    invalidRequest.erase("expected_group_type");
    CHECK(tool->execute(invalidRequest.dump(), context).hasError());
    CHECK(calls == 7);
    int blockedCalls = 0;
    const MaiArkAssetProvider unavailableGroup =
        [&](const std::string& raw, const MaiToolContext&) -> MaiResult<std::string> {
        ++blockedCalls;
        CHECK(nlohmann::json::parse(raw).at("action") == "get_group");
        return {MaiErrorCode::Network, "Group lookup is unavailable"};
    };
    const auto blocked =
        uploadMaiArkVideoAsset(unavailableGroup, "group-verified-123456", "reference.MOV",
                               "Reference clip", "LivenessFace", context);
    CHECK(!blocked);
    CHECK(blockedCalls == 1);
    int invalidIdCalls = 0;
    const MaiArkAssetProvider invalidId = [&](const std::string& raw,
                                              const MaiToolContext&) -> MaiResult<std::string> {
        ++invalidIdCalls;
        if (nlohmann::json::parse(raw).at("action") == "get_group")
            return std::string{R"({"Id":"group-verified-123456","GroupType":"LivenessFace"})"};
        return std::string{R"({"Id":"asset-"})"};
    };
    CHECK(!uploadMaiArkVideoAsset(invalidId, "group-verified-123456", "reference.MOV",
                                  "Reference clip", "LivenessFace", context));
    CHECK(invalidIdCalls == 2);
    CHECK(!MaiFileSystem::removeFile(video));
    CHECK(!MaiFileSystem::removeFile(other));
    CHECK(!MaiFileSystem::removeFile(empty));
}

#if defined(MAI_ARK_ASSET_TLS_TESTS)
// 本地 TLS 服务模拟签发、OSS PUT 和 Ark 登记，验证视频入库请求的实际线格式。
void testLocalVideoAssetServiceFlow() {
    const std::string certificateRoot =
        std::string(MAI_AGENT_SOURCE_DIR) + "/third_party/curl/tests/certs/";
    const std::string certificate = certificateRoot + "Server-localhost-sv.crt";
    const std::string privateKey = certificateRoot + "Server-localhost-sv.key";
    const std::string authority = certificateRoot + "EdelCurlRoot-ca.crt";
    httplib::SSLServer server(certificate.c_str(), privateKey.c_str());
    CHECK(server.is_valid());
    if (!server.is_valid()) return;
    std::string address;
    std::mutex mutex;
    nlohmann::json signRequest;
    nlohmann::json groupRequest;
    nlohmann::json assetRequest;
    std::string uploadedBytes;
    std::vector<std::string> stages;
    server.Post("/sign-upload", [&](const httplib::Request& request, httplib::Response& response) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stages.push_back("sign_upload");
            signRequest = nlohmann::json::parse(request.body, nullptr, false);
        }
        const std::string objectUrl = address + "/object/reference.mp4";
        response.set_content(nlohmann::json{{"upload_url", objectUrl + "?sig=put"},
                                            {"read_url", objectUrl + "?sig=get"},
                                            {"upload_headers", {{"Content-Type", "video/mp4"}}}}
                                 .dump(),
                             "application/json");
    });
    server.Put("/object/reference.mp4",
               [&](const httplib::Request& request, httplib::Response& response) {
                   {
                       std::lock_guard<std::mutex> lock(mutex);
                       stages.push_back("oss_put");
                       uploadedBytes = request.body;
                   }
                   response.set_content("", "text/plain");
               });
    server.Post("/ark-assets", [&](const httplib::Request& request, httplib::Response& response) {
        const auto body = nlohmann::json::parse(request.body, nullptr, false);
        {
            std::lock_guard<std::mutex> lock(mutex);
            stages.push_back(body.value("action", std::string{}));
            if (body.value("action", std::string{}) == "get_group")
                groupRequest = body;
            else if (body.value("action", std::string{}) == "create_asset")
                assetRequest = body;
        }
        if (body.value("action", std::string{}) == "get_group")
            response.set_content(
                R"({"result":{"Id":"group-verified-123456","GroupType":"LivenessFace",)"
                R"("ProjectName":"test-project"}})",
                "application/json");
        else if (body.value("action", std::string{}) == "create_asset")
            response.set_content(
                R"({"result":{"Id":"asset-video-123456",)"
                R"("Status":"Processing","URL":"https://secret.example/?sig=read"}})",
                "application/json");
        else
            response.set_content(R"({"result":{"Id":"asset-video-123456",)"
                                 R"("Status":"Active","AssetType":"Video",)"
                                 R"("URL":"https://secret.example/?sig=read"}})",
                                 "application/json");
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    CHECK(port > 0);
    if (port <= 0) return;
    address = "https://localhost:" + std::to_string(port);
    std::thread worker([&] { server.listen_after_bind(); });
    for (int attempt = 0; attempt < 200 && !server.is_running(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(server.is_running());
    const MaiFilePath workspace = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("ark_video_service_")));
    CHECK(!MaiFileSystem::createDirectories(workspace));
    const MaiFilePath video = workspace.append(MaiFilePath::fromUtf8("reference.mp4"));
    CHECK(!MaiFileSystem::writeFile(video, "synthetic video bytes"));
    MaiToolContext context;
    context.root = workspace.toUtf8();
    auto tool = makeMaiArkAssetTool(makeMaiArkAssetServiceProvider(
        [address]() -> MaiResult<MaiArkAssetServiceSettings> {
            return MaiArkAssetServiceSettings{address, std::string(32, 't')};
        },
        authority));
    const auto request = nlohmann::json{{"action", "upload_video"},
                                        {"group_id", "group-verified-123456"},
                                        {"video_path", "reference.mp4"},
                                        {"name", "Reference clip"},
                                        {"expected_group_type", "LivenessFace"}};
    auto mismatched = request;
    mismatched["expected_group_type"] = "AIGC";
    CHECK(tool->execute(mismatched.dump(), context).hasError());
    {
        std::lock_guard<std::mutex> lock(mutex);
        CHECK(stages == std::vector<std::string>({"get_group"}));
        CHECK(signRequest.is_null());
        stages.clear();
    }
    const MaiToolResult registered = tool->execute(request.dump(), context);
    CHECK(!registered.hasError());
    if (!registered.hasError()) {
        const auto result = nlohmann::json::parse(registered.output());
        CHECK(result.at("Id") == "asset-video-123456");
        CHECK(result.at("Status") == "Processing");
        CHECK(!result.contains("URL"));
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        CHECK(signRequest.is_object());
        CHECK(groupRequest.is_object());
        CHECK(assetRequest.is_object());
        CHECK(stages ==
              std::vector<std::string>({"get_group", "sign_upload", "oss_put", "create_asset"}));
        if (groupRequest.is_object()) {
            CHECK(groupRequest.at("action") == "get_group");
            CHECK(groupRequest.at("group_id") == "group-verified-123456");
        }
        if (signRequest.is_object()) {
            CHECK(signRequest.at("filename") == "reference.mp4");
            CHECK(signRequest.at("size_bytes") == 21);
            CHECK(signRequest.at("purpose") == "ark_asset_input");
        }
        CHECK(uploadedBytes == "synthetic video bytes");
        if (assetRequest.is_object()) {
            CHECK(assetRequest.at("action") == "create_asset");
            CHECK(assetRequest.at("asset_type") == "Video");
            CHECK(assetRequest.at("group_id") == "group-verified-123456");
            CHECK(assetRequest.at("expected_group_type") == "LivenessFace");
            CHECK(assetRequest.at("url") == address + "/object/reference.mp4?sig=get");
            CHECK(!assetRequest.contains("video_path"));
        }
    }
    const MaiToolResult ready =
        tool->execute(R"({"action":"get_asset","asset_id":"asset-video-123456"})", context);
    CHECK(!ready.hasError());
    if (!ready.hasError()) {
        const auto result = nlohmann::json::parse(ready.output());
        CHECK(result.at("asset_uri") == "asset://asset-video-123456");
        CHECK(!result.contains("URL"));
    }
    server.stop();
    worker.join();
    CHECK(!MaiFileSystem::removeFile(video));
}

// 本地 TLS 服务只观察签发请求并拒绝它，因此测试不会执行 OSS 上传或模型调用。
void testMediaSignUploadPurposes() {
    const std::string certificateRoot =
        std::string(MAI_AGENT_SOURCE_DIR) + "/third_party/curl/tests/certs/";
    const std::string certificate = certificateRoot + "Server-localhost-sv.crt";
    const std::string privateKey = certificateRoot + "Server-localhost-sv.key";
    const std::string authority = certificateRoot + "EdelCurlRoot-ca.crt";
    httplib::SSLServer server(certificate.c_str(), privateKey.c_str());
    CHECK(server.is_valid());
    if (!server.is_valid()) return;
    std::mutex mutex;
    std::vector<nlohmann::json> requests;
    server.Post("/sign-upload", [&](const httplib::Request& request, httplib::Response& response) {
        std::lock_guard<std::mutex> lock(mutex);
        requests.push_back(nlohmann::json::parse(request.body, nullptr, false));
        response.status = 403;
        response.set_content(R"({"error":"mock signer rejection"})", "application/json");
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    CHECK(port > 0);
    if (port <= 0) return;
    std::thread worker([&] { server.listen_after_bind(); });
    for (int attempt = 0; attempt < 200 && !server.is_running(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(server.is_running());
    const MaiFilePath workspace = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("oss_purpose_")));
    CHECK(!MaiFileSystem::createDirectories(workspace));
    const MaiFilePath image = workspace.append(MaiFilePath::fromUtf8("portrait.JPG"));
    const MaiFilePath video = workspace.append(MaiFilePath::fromUtf8("reference.MOV"));
    const MaiFilePath document = workspace.append(MaiFilePath::fromUtf8("script.pdf"));
    CHECK(!MaiFileSystem::writeFile(image, "mock image"));
    CHECK(!MaiFileSystem::writeFile(video, "mock video"));
    CHECK(!MaiFileSystem::writeFile(document, "mock pdf"));
    MaiToolContext context;
    context.root = workspace.toUtf8();
    const std::string address = "https://localhost:" + std::to_string(port);
    const auto settings = [address]() -> MaiResult<MaiArkAssetServiceSettings> {
        return MaiArkAssetServiceSettings{address, std::string(32, 't')};
    };
    const auto seedreamUpload =
        makeMaiPrivateOssMediaUploader(settings, authority, MaiOssUploadPurpose::SeedreamInput);
    const auto miniMaxUpload =
        makeMaiPrivateOssMediaUploader(settings, authority, MaiOssUploadPurpose::MiniMaxInput);
    const auto klingUpload =
        makeMaiPrivateOssMediaUploader(settings, authority, MaiOssUploadPurpose::KlingInput);
    const auto wanUpload =
        makeMaiPrivateOssMediaUploader(settings, authority, MaiOssUploadPurpose::WanInput);
    auto assetTool = makeMaiArkAssetTool(makeMaiArkAssetServiceProvider(settings, authority));
    const MaiCreativeMediaUploadProvider assetImageUpload =
        [&](const std::string& path,
            const MaiToolContext& uploadContext) -> MaiResult<std::string> {
        const nlohmann::json request = {
            {"action", "upload_image"}, {"group_id", "group-1234567890"}, {"image_path", path}};
        const MaiToolResult result = assetTool->execute(request.dump(), uploadContext);
        if (result.hasError()) return result.error();
        return result.output();
    };
    std::size_t expectedRequests = 0;
    const auto checkSignRequest = [&](const MaiCreativeMediaUploadProvider& uploader,
                                      const char* fileName, const char* expectedName,
                                      std::uint64_t expectedBytes, const char* expectedPurpose) {
        const auto uploaded = uploader(fileName, context);
        CHECK(!uploaded);
        std::lock_guard<std::mutex> lock(mutex);
        CHECK(requests.size() == ++expectedRequests);
        if (requests.size() != expectedRequests) return;
        const auto& body = requests.back();
        CHECK(body.is_object());
        if (!body.is_object()) return;
        CHECK(body.at("filename") == expectedName);
        CHECK(body.at("size_bytes") == expectedBytes);
        CHECK(body.at("purpose") == expectedPurpose);
    };
    checkSignRequest(seedreamUpload, "portrait.JPG", "portrait.jpg", 10, "seedream_input");
    checkSignRequest(miniMaxUpload, "portrait.JPG", "portrait.jpg", 10, "minimax_input");
    checkSignRequest(miniMaxUpload, "reference.MOV", "reference.MOV", 10, "minimax_input");
    checkSignRequest(klingUpload, "portrait.JPG", "portrait.jpg", 10, "kling_input");
    checkSignRequest(klingUpload, "reference.MOV", "reference.MOV", 10, "kling_input");
    checkSignRequest(wanUpload, "portrait.JPG", "portrait.jpg", 10, "wan_input");
    checkSignRequest(wanUpload, "reference.MOV", "reference.MOV", 10, "wan_input");
    checkSignRequest(wanUpload, "script.pdf", "script.pdf", 8, "wan_input");
    checkSignRequest(assetImageUpload, "portrait.JPG", "portrait.jpg", 10, "ark_asset_input");
    {
        std::lock_guard<std::mutex> lock(mutex);
        CHECK(requests.size() == 9);
    }
    server.stop();
    worker.join();
    CHECK(!MaiFileSystem::removeFile(image));
    CHECK(!MaiFileSystem::removeFile(video));
    CHECK(!MaiFileSystem::removeFile(document));
}
#endif

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
    int assetUploads = 0;
    const MaiArkAssetProvider assets = [&](const std::string& raw,
                                           const MaiToolContext&) -> MaiResult<std::string> {
        const auto request = nlohmann::json::parse(raw);
        if (request.at("action") == "get_group")
            return std::string(R"({"Id":"group-test-123456","GroupType":"AIGC"})");
        CHECK(request.at("action") == "upload_image");
        CHECK(request.at("group_id") == "group-test-123456");
        ++assetUploads;
        return {MaiErrorCode::Network, "Synthetic asset upload failure"};
    };
    auto video = makeMaiSeedanceVideoTool([] { return std::string("test-key"); }, {}, assets);
    const nlohmann::json request = {
        {"action", "prepare"},
        {"message", "Animate the reference"},
        {"local_image_paths", nlohmann::json::array({source.baseName().toUtf8()})},
        {"asset_group_id", "group-test-123456"},
        {"asset_group_type", "AIGC"},
        {"production", {{"duration", 5}, {"ratio", "16:9"}, {"resolution", "720p"}}}};
    const MaiToolResult result = video->execute(request.dump(), context);
    CHECK(result.hasError());
    CHECK(result.error().message().find("asset_upload_failed") != std::string::npos);
    CHECK(assetUploads == 1);
    (void)MaiFileSystem::removeFile(source);
}

void testLocalImagePrepareResumesWithoutReupload() {
    const MaiFilePath source = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("ark_wait_") + ".jpg"));
    CHECK(!MaiFileSystem::writeFile(source, "synthetic image fixture"));
    MaiToolContext context;
    context.root = source.dirName().toUtf8();
    int checks = 0;
    int uploads = 0;
    const MaiArkAssetProvider assets = [&](const std::string& raw,
                                           const MaiToolContext&) -> MaiResult<std::string> {
        const auto request = nlohmann::json::parse(raw);
        if (request.at("action") == "get_group")
            return std::string(R"({"Id":"group-test-123456","GroupType":"AIGC"})");
        if (request.at("action") == "upload_image") {
            ++uploads;
            return std::string(R"({"Id":"asset-image-123456","Status":"Processing"})");
        }
        CHECK(request.at("action") == "get_asset");
        CHECK(request.at("asset_id") == "asset-image-123456");
        ++checks;
        if (checks == 1)
            return std::string(
                R"({"Id":"asset-image-123456","Status":"Processing","AssetType":"Image"})");
        return std::string(R"({"Id":"asset-image-123456","Status":"Active","AssetType":"Image",)"
                           R"("GroupId":"group-test-123456"})");
    };
    auto video = makeMaiSeedanceVideoTool([] { return std::string("test-key"); }, {}, assets);
    const nlohmann::json request = {
        {"action", "prepare"},
        {"message", "Animate the reference"},
        {"local_image_paths", nlohmann::json::array({source.baseName().toUtf8()})},
        {"asset_group_id", "group-test-123456"},
        {"asset_group_type", "AIGC"},
        {"poll_once", true}};
    CHECK(!video->requiresPerCallApproval(request.dump()));
    const MaiToolResult first = video->execute(request.dump(), context);
    CHECK(!first.hasError());
    const auto pending = nlohmann::json::parse(first.output());
    CHECK(pending.at("status") == "processing");
    CHECK(pending.at("pending_asset_ids") == nlohmann::json::array({"asset-image-123456"}));
    const auto retry = pending.at("prepared_arguments");
    CHECK(retry.at("action") == "prepare");
    CHECK(!retry.contains("local_image_paths"));
    CHECK(retry.at("reference_asset_ids") == nlohmann::json::array({"asset-image-123456"}));
    CHECK(checks == 0);
    const MaiToolResult second = video->execute(retry.dump(), context);
    CHECK(!second.hasError());
    const auto stillPending = nlohmann::json::parse(second.output());
    CHECK(stillPending.at("status") == "processing");
    CHECK(uploads == 1);
    const MaiToolResult third =
        video->execute(stillPending.at("prepared_arguments").dump(), context);
    CHECK(!third.hasError());
    const auto ready = nlohmann::json::parse(third.output());
    CHECK(ready.at("status") == "ready");
    CHECK(ready.at("prepared_arguments").at("action") == "delegate");
    CHECK(!ready.at("prepared_arguments").contains("poll_once"));
    CHECK(uploads == 1);
    CHECK(checks == 3);
    (void)MaiFileSystem::removeFile(source);
}

void testPrivateOssUploaderAcceptsDocumentSourceWithoutBase64() {
    const MaiFilePath source = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("wan_document_") + ".pptx"));
    CHECK(!MaiFileSystem::writeFile(source, "synthetic presentation fixture"));
    MaiToolContext context;
    context.root = source.dirName().toUtf8();
    int configReads = 0;
    auto uploader = makeMaiPrivateOssMediaUploader(
        [&]() -> MaiResult<MaiArkAssetServiceSettings> {
            ++configReads;
            return {MaiErrorCode::NotConfigured, "Synthetic signer unavailable"};
        },
        {}, MaiOssUploadPurpose::WanInput);
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
    testSeedanceAssetInputBoundaries();
    testSeedanceChecksAssetTypeAndPortraitGroup();
    testRevisionRejectsAnotherConversation();
    testLocalVideoUploadHandoffWithoutArkSubmission();
    testFfprobeVideoMetadataProviderUsesMeasuredValues();
    testVirtualAssetFlow();
    testImageAssetGroupPreflight();
    testLocalVideoAssetRegistration();
#if defined(MAI_ARK_ASSET_TLS_TESTS)
    testLocalVideoAssetServiceFlow();
    testMediaSignUploadPurposes();
#endif
    testRealPortraitValidationHandoff();
    testLiveAssetServiceWhenExplicitlyConfigured();
    testLocalImagesStopBeforePaidSubmissionWhenOssFails();
    testLocalImagePrepareResumesWithoutReupload();
    testPrivateOssUploaderAcceptsDocumentSourceWithoutBase64();
    testSeedanceManagementRequiresOwnedConversation();
    testLiveArkWhenExplicitlyConfigured();
    testLiveRevisionWhenExplicitlyConfigured();
    return failures == 0 ? 0 : 1;
}
