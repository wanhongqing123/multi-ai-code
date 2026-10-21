// 文件作用：验证“把 Agent 生成的媒体文件交付给聊天或宿主界面”的正确行为和边界条件。
// 验证入口：阅读 main 或测试注册处，核对输入、预期结果与失败输出。
#include "MaiAgentSendMediaTool.h"

#include <cstdio>
#include <string>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiMemoryStore.h"

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

int main() {
    const MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-agent-send-media-tests"));
    MaiFileSystem::removeRecursively(root);
    CHECK(!MaiFileSystem::createDirectories(root));
    const MaiFilePath image = root.append(MaiFilePath::fromUtf8("result.png"));
    const std::string png =
        "\x89PNG\r\n\x1a\n"
        "test-image-bytes";
    CHECK(!MaiFileSystem::writeFile(image, png));
    MaiToolContext context;
    context.root = root.toUtf8();
    auto tool = makeMaiAgentSendMediaTool();
    CHECK(tool && tool->name() == "agent_send_media");
    const MaiToolResult delivered = tool->execute(
        R"({"file_path":"result.png","type":"image","caption":"Edited photo"})", context);
    CHECK(!delivered.hasError());
    const auto output = nlohmann::json::parse(delivered.output());
    CHECK(output["delivery"] == "current_ai_session");
    CHECK(output["path"] == "result.png");
    CHECK(output["mime_type"] == "image/png");
    CHECK(output["caption"] == "Edited photo");
    CHECK(MaiFileSystem::exists(image));
    CHECK(tool->execute(R"({"file_path":"../result.png","type":"image"})", context).hasError());
    CHECK(tool->execute(R"({"file_path":"result.png","type":"video"})", context).hasError());
    CHECK(tool->execute(R"({"file_path":"result.png","type":"image","peer_id":"someone"})", context)
              .hasError());
    const MaiFilePath video = root.append(MaiFilePath::fromUtf8("generated.mp4"));
    CHECK(!MaiFileSystem::writeFile(video, std::string("\x00\x00\x00\x18"
                                                       "ftypisom",
                                                       12)));
    auto store = makeMaiMemoryStore();
    MaiSession session;
    session.id = "video-session";
    session.directory = root.toUtf8();
    session.model = "test-model";
    store->putSession(session);
    MaiSpecialistTask task;
    task.id = "spt_generated_video";
    task.ownerSessionId = session.id;
    task.specialistName = "wan_video";
    task.providerTaskId = "provider-task-1";
    task.created = 1;
    CHECK(!store->insertSpecialistTask(task));
    CHECK(!store->finishSpecialistTask(task.id, session.id, MaiSpecialistTaskStatus::Succeeded,
                                       "Ready", video.toUtf8(), 2));
    context.sessionId = session.id;
    context.specialistTasks = store.get();
    const auto sourceRequest = nlohmann::json{
        {"file_path", "generated.mp4"}, {"type", "video"}, {"source_task_id", task.id}};
    const MaiToolResult sourced = tool->execute(sourceRequest.dump(), context);
    CHECK(!sourced.hasError());
    const auto sourcedOutput = nlohmann::json::parse(sourced.output());
    CHECK(sourcedOutput["source_task_id"] == task.id);
    CHECK(sourcedOutput["source_tool"] == "wan_video");
    CHECK(tool->execute(nlohmann::json{{"file_path", "generated.mp4"},
                                       {"type", "video"},
                                       {"source_task_id", "spt_other_task"}}
                            .dump(),
                        context)
              .hasError());
    context.sessionId = "other-session";
    CHECK(tool->execute(sourceRequest.dump(), context).hasError());
    context.sessionId = session.id;
    const MaiFilePath unrelated = root.append(MaiFilePath::fromUtf8("unrelated.mp4"));
    CHECK(!MaiFileSystem::writeFile(unrelated, std::string("\x00\x00\x00\x18"
                                                           "ftypisom",
                                                           12)));
    CHECK(tool->execute(nlohmann::json{{"file_path", "unrelated.mp4"},
                                       {"type", "video"},
                                       {"source_task_id", task.id}}
                            .dump(),
                        context)
              .hasError());
    CHECK(tool->execute(
                  nlohmann::json{
                      {"file_path", "result.png"}, {"type", "image"}, {"source_task_id", task.id}}
                      .dump(),
                  context)
              .hasError());
    MaiFileSystem::removeRecursively(root);
    return 0;
}
