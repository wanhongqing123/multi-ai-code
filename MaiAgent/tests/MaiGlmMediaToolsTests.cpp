#include <cstdio>
#include <string>

#include <json.hpp>

#include "MaiFileSystem.h"
#include "MaiGlmMediaTools.h"
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

void testDiscoveryAndApproval() {
    std::string key;
    auto video = makeMaiGlmVideoTool([&] { return key; });
    auto image = makeMaiGlmImageTool([&] { return key; });
    MaiToolContext context;
    CHECK(video->name() == "glm_video");
    CHECK(image->name() == "glm_image");
    CHECK(nlohmann::json::parse(video->parametersSchema()).is_object());
    CHECK(nlohmann::json::parse(image->parametersSchema()).is_object());
    CHECK(!video->requiresPerCallApproval(R"({"action":"discover"})"));
    CHECK(!video->requiresPerCallApproval(R"({"action":"continue"})"));
    CHECK(video->requiresPerCallApproval(R"({"action":"delegate"})"));
    CHECK(image->requiresPerCallApproval(R"({"action":"delegate"})"));
    CHECK(video->requiresPerCallApproval("not json"));
    auto info = nlohmann::json::parse(video->execute(R"({"action":"discover"})", context).output());
    CHECK(info.at("bound_model") == "cogvideox-3");
    CHECK(!info.at("configured").get<bool>());
    CHECK(info.at("capabilities")[0].at("tool_status") == "not_configured");
    CHECK(info.dump().find("existing_video_edit") != std::string::npos);
    key = "test-key";
    info = nlohmann::json::parse(video->execute(R"({"action":"discover"})", context).output());
    CHECK(info.at("configured").get<bool>());
    CHECK(info.at("capabilities")[0].at("tool_status") == "implemented_unverified");
    CHECK(info.dump().find(key) == std::string::npos);
    info = nlohmann::json::parse(image->execute(R"({"action":"discover"})", context).output());
    CHECK(info.at("bound_model") == "glm-image");
    CHECK(info.dump().find("existing_image_edit") != std::string::npos);
}

void testRejectsInvalidInputsBeforeNetwork() {
    auto video = makeMaiGlmVideoTool([] { return std::string("test-key"); });
    auto image = makeMaiGlmImageTool([] { return std::string("test-key"); });
    MaiToolContext context;
    context.root = MaiFileSystem::temporaryDirectory().toUtf8();
    CHECK(video->execute(R"({"action":"delegate","message":"A cat runs"})", context).hasError());
    CHECK(video
              ->execute(
                  R"({"action":"delegate","message":"A cat runs","duration":7,"size":"1280x720"})",
                  context)
              .hasError());
    CHECK(
        video
            ->execute(
                R"({"action":"delegate","message":"A cat runs","duration":4294967301,"size":"1280x720"})",
                context)
            .hasError());
    CHECK(video
              ->execute(
                  R"({"action":"delegate","message":"A cat runs","duration":5,"size":"640x480"})",
                  context)
              .hasError());
    CHECK(
        video
            ->execute(
                R"({"action":"delegate","message":"A cat runs","duration":5,"size":"1280x720","last_frame_path":"last.png"})",
                context)
            .hasError());
    CHECK(
        video
            ->execute(
                R"({"action":"delegate","message":"A cat runs","duration":5,"size":"1280x720","image_path":"missing.png"})",
                context)
            .hasError());
    CHECK(
        video
            ->execute(
                R"({"action":"delegate","message":"A cat runs","duration":5,"size":"1280x720","with_audio":"yes"})",
                context)
            .hasError());
    CHECK(
        video
            ->execute(
                R"({"action":"delegate","message":"Edit","duration":5,"size":"1280x720","video_path":"old.mp4"})",
                context)
            .hasError());
    CHECK(
        image->execute(R"({"action":"delegate","message":"Edit","image_path":"old.png"})", context)
            .hasError());
    CHECK(
        image->execute(R"({"action":"delegate","message":"Draw a cat","size":"512x512"})", context)
            .hasError());
    CHECK(
        image
            ->execute(R"({"action":"delegate","message":"Draw a cat","size":"1281x1280"})", context)
            .hasError());
    CHECK(
        image->execute(R"({"action":"continue","conversation_id":"bad/id"})", context).hasError());
    CHECK(image->execute("not json", context).hasError());
}

void testCannotReadAnotherConversationTask() {
    auto store = makeMaiMemoryStore();
    MaiSession owner;
    owner.id = MaiIdGenerator::newSessionId();
    store->putSession(owner);
    MaiSession other;
    other.id = MaiIdGenerator::newSessionId();
    store->putSession(other);
    MaiSpecialistTask task;
    task.id = MaiIdGenerator::generate("spt_");
    task.ownerSessionId = owner.id;
    task.specialistName = "glm_video";
    task.providerTaskId = "provider-task";
    task.intent = "Create a video";
    CHECK(!store->insertSpecialistTask(task));
    auto video = makeMaiGlmVideoTool([] { return std::string("test-key"); });
    MaiToolContext context;
    context.sessionId = other.id;
    context.specialistTasks = store.get();
    const auto result = video->execute(
        nlohmann::json{{"action", "continue"}, {"conversation_id", task.id}}.dump(), context);
    CHECK(result.hasError());
    CHECK(result.error().code() == MaiErrorCode::NotFound);
}

}  // namespace

int main() {
    testDiscoveryAndApproval();
    testRejectsInvalidInputsBeforeNetwork();
    testCannotReadAnotherConversationTask();
    return failures == 0 ? 0 : 1;
}
