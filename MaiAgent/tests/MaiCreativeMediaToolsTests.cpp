#include <cstdio>
#include <string>

#include <json.hpp>

#include "MaiKlingMediaTools.h"
#include "MaiMiniMaxMediaTools.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiMemoryStore.h"
#include "MaiTime.h"

namespace {

int failures = 0;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

void checkTool(MaiTool& tool, const std::string& expectedName, const std::string& expectedModel,
               bool video) {
    MaiToolContext context;
    context.root = MaiFileSystem::temporaryDirectory().toUtf8();
    CHECK(tool.name() == expectedName);
    CHECK(nlohmann::json::parse(tool.parametersSchema()).is_object());
    CHECK(!tool.requiresPerCallApproval(R"({"action":"discover"})"));
    CHECK(!tool.requiresPerCallApproval(R"({"action":"continue"})"));
    CHECK(tool.requiresPerCallApproval(R"({"action":"delegate"})"));
    CHECK(tool.requiresPerCallApproval("broken json"));
    const auto discovered =
        nlohmann::json::parse(tool.execute(R"({"action":"discover"})", context).output());
    CHECK(discovered.at("bound_model") == expectedModel);
    CHECK(discovered.at("configured") == false);
    CHECK(discovered.at("capabilities")[0].at("tool_status") == "not_configured");
    CHECK(discovered.dump().find("secret") == std::string::npos);
    if (expectedName == "minimax_video") {
        CHECK(discovered.dump().find("Hailuo-2.3") == std::string::npos);
        CHECK(tool.parametersSchema().find("MiniMax-Hailuo-2.3") == std::string::npos);
    }
    CHECK(tool.execute(R"({"action":"delegate","message":"A running cat"})", context).hasError());
    CHECK(tool.execute(R"({"action":"continue","conversation_id":"missing"})", context).hasError());
    if (video && expectedName == "kling_video")
        CHECK(discovered.dump().find("existing_video_edit") != std::string::npos);
    if (video && expectedName == "minimax_video")
        CHECK(discovered.dump().find("multi_reference_video") != std::string::npos);
}

void testFailedTaskIsStableWithoutProviderRequest() {
    auto store = makeMaiMemoryStore();
    MaiSession session;
    session.id = "session";
    session.directory = "/tmp";
    session.model = "test-model";
    store->putSession(session);
    MaiSpecialistTask task;
    task.id = MaiIdGenerator::generate("spt_");
    task.ownerSessionId = "session";
    task.specialistName = "minimax_video";
    task.providerTaskId = "h3_12345";
    task.intent = "A child waves";
    task.inputReference = "first.jpg";
    task.created = MaiTime::getCurrentTime();
    CHECK(!store->insertSpecialistTask(task));
    CHECK(
        !store->finishSpecialistTask(task.id, task.ownerSessionId, MaiSpecialistTaskStatus::Failed,
                                     "input text sensitive (1026)", {}, MaiTime::getCurrentTime()));
    MaiToolContext context;
    context.sessionId = task.ownerSessionId;
    context.specialistTasks = store.get();
    auto video = makeMaiMiniMaxVideoTool([] { return std::string("test-key"); });
    const MaiToolResult result = video->execute(
        nlohmann::json{{"action", "continue"}, {"conversation_id", task.id}}.dump(), context);
    CHECK(!result.hasError());
    const auto reply = nlohmann::json::parse(result.output());
    CHECK(reply.at("status") == "failed");
    CHECK(reply.at("input_reference_present") == true);
    CHECK(reply.at("reply") == "input text sensitive (1026)");
}

}  // namespace

int main() {
    testFailedTaskIsStableWithoutProviderRequest();
    std::string key;
    auto klingVideo = makeMaiKlingVideoTool([&] { return key; });
    auto klingImage = makeMaiKlingImageTool([&] { return key; });
    auto miniMaxVideo = makeMaiMiniMaxVideoTool([&] { return key; });
    auto miniMaxImage = makeMaiMiniMaxImageTool([&] { return key; });
    checkTool(*klingVideo, "kling_video", "kling-3.0-turbo", true);
    checkTool(*klingImage, "kling_image", "kling-v3-omni", false);
    checkTool(*miniMaxVideo, "minimax_video", "MiniMax-H3", true);
    checkTool(*miniMaxImage, "minimax_image", "image-01", false);
    key = "secret-test-key";
    MaiToolContext context;
    context.root = MaiFileSystem::temporaryDirectory().toUtf8();
    for (MaiTool* tool :
         {klingVideo.get(), klingImage.get(), miniMaxVideo.get(), miniMaxImage.get()}) {
        const auto discovered =
            nlohmann::json::parse(tool->execute(R"({"action":"discover"})", context).output());
        CHECK(discovered.at("configured") == true);
        CHECK(discovered.at("capabilities")[0].at("tool_status") == "implemented_unverified");
        CHECK(discovered.dump().find(key) == std::string::npos);
        CHECK(tool->execute(R"({"action":"delegate","message":"A cat","video_path":"old.mp4"})",
                            context)
                  .hasError());
    }
    CHECK(
        klingVideo
            ->execute(R"({"action":"delegate","message":"A cat","duration":7,"resolution":"720p"})",
                      context)
            .hasError());
    CHECK(
        klingVideo
            ->execute(
                R"({"action":"delegate","message":"A cat","duration":5,"resolution":"720p","last_frame_path":"last.jpg"})",
                context)
            .hasError());
    CHECK(miniMaxVideo
              ->execute(
                  R"({"action":"delegate","message":"A cat","duration":10,"resolution":"1080P"})",
                  context)
              .hasError());
    CHECK(
        miniMaxVideo
            ->execute(
                R"({"action":"delegate","model":"MiniMax-H3","message":"A cat","duration":5,"resolution":"1080P","ratio":"9:16"})",
                context)
            .hasError());
    CHECK(
        miniMaxVideo
            ->execute(
                R"({"action":"delegate","model":"MiniMax-H3","message":"A cat","duration":10,"resolution":"768P","ratio":"9:16","content":[{"type":"image_url","role":"first_frame","path":"first.jpg"},{"type":"image_url","role":"reference_image","path":"reference.jpg"}]})",
                context)
            .hasError());
    CHECK(
        miniMaxVideo
            ->execute(
                R"({"action":"delegate","model":"MiniMax-H3","message":"A cat","duration":10,"resolution":"768P","ratio":"9:16","content":[{"type":"image_url","role":"reference_image","path":"missing.jpg"}]})",
                context)
            .hasError());
    CHECK(klingImage->execute(R"({"action":"delegate","message":"A cat","ratio":"3:4"})", context)
              .hasError());
    return failures == 0 ? 0 : 1;
}
