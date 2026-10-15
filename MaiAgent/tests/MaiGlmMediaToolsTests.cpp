// 文件作用：验证“封装智谱图片媒体能力的请求与结果处理”的正确行为和边界条件。
// 验证入口：testImageDiscoveryAndApproval、testImageRejectsUnsupportedInputsBeforeNetwork、
// testCannotReadAnotherConversationImageTask；检查可观察结果而非实现细节。
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

void testImageDiscoveryAndApproval() {
    std::string key;
    auto image = makeMaiGlmImageTool([&] { return key; });
    MaiToolContext context;
    CHECK(image->name() == "glm_image");
    const auto schema = nlohmann::json::parse(image->parametersSchema());
    CHECK(schema.is_object());
    CHECK(!schema["properties"].contains("video_path"));
    CHECK(!schema["properties"].contains("image_path"));
    CHECK(schema["properties"]["action"]["enum"].dump().find("delete") == std::string::npos);
    CHECK(!image->requiresPerCallApproval(R"({"action":"discover"})"));
    CHECK(!image->requiresPerCallApproval(R"({"action":"continue"})"));
    CHECK(image->requiresPerCallApproval(R"({"action":"delegate"})"));
    auto info = nlohmann::json::parse(image->execute(R"({"action":"discover"})", context).output());
    CHECK(info.at("bound_model") == "glm-image");
    CHECK(!info.at("configured").get<bool>());
    CHECK(info.at("capabilities")[0].at("tool_status") == "not_configured");
    CHECK(info.dump().find("existing_image_edit") != std::string::npos);
    CHECK(info.dump().find("cogvideox") == std::string::npos);
    key = "test-key";
    info = nlohmann::json::parse(image->execute(R"({"action":"discover"})", context).output());
    CHECK(info.at("configured").get<bool>());
    CHECK(info.at("capabilities")[0].at("tool_status") == "implemented_unverified");
    CHECK(info.dump().find(key) == std::string::npos);
}

void testImageRejectsUnsupportedInputsBeforeNetwork() {
    auto image = makeMaiGlmImageTool([] { return std::string("test-key"); });
    MaiToolContext context;
    context.root = MaiFileSystem::temporaryDirectory().toUtf8();
    CHECK(
        image->execute(R"({"action":"delegate","message":"Edit","image_path":"old.png"})", context)
            .hasError());
    CHECK(
        image->execute(R"({"action":"delegate","message":"Edit","video_path":"old.mp4"})", context)
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

void testCannotReadAnotherConversationImageTask() {
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
    task.specialistName = "glm_image";
    task.providerTaskId = "provider-task";
    task.intent = "Create an image";
    CHECK(!store->insertSpecialistTask(task));
    auto image = makeMaiGlmImageTool([] { return std::string("test-key"); });
    MaiToolContext context;
    context.sessionId = other.id;
    context.specialistTasks = store.get();
    const auto result = image->execute(
        nlohmann::json{{"action", "continue"}, {"conversation_id", task.id}}.dump(), context);
    CHECK(result.hasError());
    CHECK(result.error().code() == MaiErrorCode::NotFound);
}

}  // namespace

int main() {
    testImageDiscoveryAndApproval();
    testImageRejectsUnsupportedInputsBeforeNetwork();
    testCannotReadAnotherConversationImageTask();
    return failures == 0 ? 0 : 1;
}
