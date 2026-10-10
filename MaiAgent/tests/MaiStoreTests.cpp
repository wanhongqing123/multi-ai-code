// 存储层测试。
//
// 核心是一组**契约用例**：同一套断言对内存存储和 SQLite 存储各跑一遍。
// 这是这个文件存在的主要理由——MaiSessionStore 有两个实现，
// 而上层（MaiAgent / MaiTurnRunner）只认接口。两个实现行为不一致的话，
// 换存储就会冒出一堆"只在落盘时才出现"的怪毛病，而且很难联想到存储层。
//
// 契约之外还有两类：
//   - 只有 SQLite 才有的性质：关掉再打开、坏文件、目录自动创建。
//   - 顺序依赖：listMessages 按 id 排，而 id 的单调性是 MaiIdGenerator
//     许诺的。这里盯着这个许诺，免得哪天换 id 方案时静默出错。
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <json.hpp>
#include <sqlite3.h>

#include "MaiIdGenerator.h"
#include "MaiSessionStore.h"
#include "MaiMemoryStore.h"
#include "MaiSqliteStore.h"
#include "MaiTime.h"

namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

namespace {

// 临时目录。SQLite 的用例要真的落盘才能验"关掉再打开还在"。
struct TempDir {
    fs::path root;
    TempDir() {
        root = fs::temp_directory_path() / ("maiagent-store-" + std::to_string(std::rand()));
        fs::create_directories(root);
    }
    ~TempDir() {
        std::error_code errorCode;
        fs::remove_all(root, errorCode);
    }
    std::string file(const char* name) const {
        return (root / name).u8string();
    }
};

MaiSession makeSession(const std::string& id, const std::string& title, MaiMillis updated) {
    MaiSession session;
    session.id = id;
    session.title = title;
    session.directory = "/tmp/work";
    session.model = "glm-5.3";
    session.created = 1000;
    session.updated = updated;
    return session;
}

MaiMessage makeMessage(const std::string& id, MaiRole role) {
    MaiMessage message;
    message.id = id;
    message.role = role;
    message.created = 2000;
    message.completed = 2001;
    return message;
}

MaiMessagePart textPart(const std::string& id, const std::string& text) {
    MaiMessagePart part;
    part.id = id;
    part.created = 2000;
    part.body = MaiTextPart{text};
    return part;
}

// ── 契约用例：对每个实现都必须成立 ─────────────────────────────

void contract_session_crud(MaiSessionStore& store, const char* which) {
    std::printf("   [%s] session create/read/update/delete\n", which);

    const std::string id = MaiIdGenerator::newSessionId();
    MaiSession loaded;
    CHECK(!store.getSession(id, loaded));  // 还没存

    store.putSession(makeSession(id, "first title", 5000));
    CHECK(store.getSession(id, loaded));
    CHECK(loaded.title == "first title");
    CHECK(loaded.directory == "/tmp/work");
    CHECK(loaded.model == "glm-5.3");
    CHECK(loaded.agent == "build");  // 默认值也要存下来
    CHECK(loaded.created == 1000);
    CHECK(loaded.updated == 5000);

    // 同 id 再 put 是覆盖，不是插一条新的
    store.putSession(makeSession(id, "second title", 6000));
    CHECK(store.getSession(id, loaded));
    CHECK(loaded.title == "second title");

    CHECK(store.removeSession(id));
    CHECK(!store.getSession(id, loaded));
    CHECK(!store.removeSession(id));  // 删第二次要说"没这条"
}

void contract_list_sessions_ordering(MaiSessionStore& store, const char* which) {
    std::printf("   [%s] sessions listed newest-updated first\n", which);

    const std::string oldest = MaiIdGenerator::newSessionId();
    const std::string newest = MaiIdGenerator::newSessionId();
    const std::string middle = MaiIdGenerator::newSessionId();
    store.putSession(makeSession(oldest, "oldest", 100));
    store.putSession(makeSession(newest, "newest", 900));
    store.putSession(makeSession(middle, "middle", 500));

    const auto listed = store.listSessions();
    CHECK(listed.size() == 3);
    if (listed.size() == 3) {
        // 界面左侧列表靠这个顺序，排错了看起来就像"新会话跑到下面去了"
        CHECK(listed[0].id == newest);
        CHECK(listed[1].id == middle);
        CHECK(listed[2].id == oldest);
    }

    for (const auto& session : listed) store.removeSession(session.id);
    CHECK(store.listSessions().empty());
}

void contract_messages_and_parts(MaiSessionStore& store, const char* which) {
    std::printf("   [%s] messages and parts round-trip\n", which);

    const std::string sessionId = MaiIdGenerator::newSessionId();
    store.putSession(makeSession(sessionId, "chat", 1));
    CHECK(store.listMessages(sessionId).empty());

    MaiMessage user = makeMessage(MaiIdGenerator::newMessageId(), MaiRole::User);
    user.parts.push_back(textPart(MaiIdGenerator::newPartId(), "hello"));
    MaiMessagePart image;
    image.id = MaiIdGenerator::newPartId();
    image.created = 2000;
    image.body = MaiImagePart{"photo.png", "image/png"};
    user.parts.push_back(image);
    MaiMessagePart video;
    video.id = MaiIdGenerator::newPartId();
    video.body = MaiVideoPart{"clip.mp4", "video/mp4"};
    user.parts.push_back(video);
    MaiMessagePart quote;
    quote.id = MaiIdGenerator::newPartId();
    quote.body = MaiQuotePart{"msg_source", "Earlier video"};
    user.parts.push_back(quote);
    store.putMessage(sessionId, user);

    // assistant 消息把三种 part 都带上：文本、推理、工具
    MaiMessage assistant = makeMessage(MaiIdGenerator::newMessageId(), MaiRole::Assistant);
    MaiMessagePart reasoning;
    reasoning.id = MaiIdGenerator::newPartId();
    reasoning.body = MaiReasoningPart{"thinking"};
    assistant.parts.push_back(reasoning);

    MaiMessagePart toolCall;
    toolCall.id = MaiIdGenerator::newPartId();
    MaiToolPart tool;
    tool.tool = "write";
    tool.callId = "call_1";
    tool.input = R"({"path":"a.txt"})";
    tool.output = "wrote a.txt";
    tool.error = "";
    tool.state = MaiToolState::Completed;
    toolCall.body = tool;
    assistant.parts.push_back(toolCall);

    MaiMessagePart canceledCall;
    canceledCall.id = MaiIdGenerator::newPartId();
    tool.callId = "call_2";
    tool.state = MaiToolState::Canceled;
    tool.error = "The user canceled this tool call.";
    tool.output = tool.error;
    canceledCall.body = tool;
    assistant.parts.push_back(canceledCall);

    assistant.parts.push_back(textPart(MaiIdGenerator::newPartId(), "done"));
    store.putMessage(sessionId, assistant);

    const auto loaded = store.listMessages(sessionId);
    CHECK(loaded.size() == 2);
    if (loaded.size() != 2) return;

    CHECK(loaded[0].id == user.id);
    CHECK(loaded[0].role == MaiRole::User);
    CHECK(loaded[0].parts.size() == 4);
    if (loaded[0].parts.size() == 4) {
        const auto* readImage = std::get_if<MaiImagePart>(&loaded[0].parts[1].body);
        CHECK(readImage && readImage->path == "photo.png");
        CHECK(readImage && readImage->mimeType == "image/png");
        const auto* readVideo = std::get_if<MaiVideoPart>(&loaded[0].parts[2].body);
        CHECK(readVideo && readVideo->path == "clip.mp4");
        CHECK(readVideo && readVideo->mimeType == "video/mp4");
        const auto* readQuote = std::get_if<MaiQuotePart>(&loaded[0].parts[3].body);
        CHECK(readQuote && readQuote->messageId == "msg_source");
        CHECK(readQuote && readQuote->preview == "Earlier video");
    }

    CHECK(loaded[1].id == assistant.id);
    CHECK(loaded[1].role == MaiRole::Assistant);
    CHECK(loaded[1].created == 2000);
    CHECK(loaded[1].completed == 2001);
    CHECK(loaded[1].parts.size() == 4);
    if (loaded[1].parts.size() != 4) return;

    // part 的顺序必须和放进去时一致：界面就按这个顺序渲染，
    // 乱了的话工具卡会跑到它触发的那段文字前面去。
    const auto* readReasoning = std::get_if<MaiReasoningPart>(&loaded[1].parts[0].body);
    CHECK(readReasoning && readReasoning->text == "thinking");

    const auto* readTool = std::get_if<MaiToolPart>(&loaded[1].parts[1].body);
    CHECK(readTool != nullptr);
    if (readTool) {
        CHECK(readTool->tool == "write");
        CHECK(readTool->callId == "call_1");
        CHECK(readTool->input == R"({"path":"a.txt"})");
        CHECK(readTool->output == "wrote a.txt");
        CHECK(readTool->state == MaiToolState::Completed);
    }

    const auto* readCanceled = std::get_if<MaiToolPart>(&loaded[1].parts[2].body);
    CHECK(readCanceled && readCanceled->state == MaiToolState::Canceled);
    CHECK(readCanceled && readCanceled->callId == "call_2");

    const auto* readText = std::get_if<MaiTextPart>(&loaded[1].parts[3].body);
    CHECK(readText && readText->text == "done");
}

void contract_message_pages(MaiSessionStore& store, const char* which) {
    std::printf("   [%s] message pages keep session and cursor boundaries\n", which);
    const std::string sessionId = MaiIdGenerator::newSessionId();
    const std::string otherSessionId = MaiIdGenerator::newSessionId();
    store.putSession(makeSession(sessionId, "paged", 1));
    store.putSession(makeSession(otherSessionId, "other", 1));

    std::vector<std::string> ids;
    for (int index = 0; index < 5; ++index) {
        MaiMessage message = makeMessage(MaiIdGenerator::newMessageId(), MaiRole::User);
        message.parts.push_back(textPart(MaiIdGenerator::newPartId(), "page text"));
        ids.push_back(message.id);
        store.putMessage(sessionId, message);
    }
    store.putMessage(otherSessionId, makeMessage(MaiIdGenerator::newMessageId(), MaiRole::User));

    const auto newest = store.listMessagesPage(sessionId, "", 2);
    CHECK(newest.size() == 2);
    if (newest.size() != 2) return;
    CHECK(newest[0].id == ids[4]);
    CHECK(newest[1].id == ids[3]);
    CHECK(newest[0].parts.size() == 1);

    const auto older = store.listMessagesPage(sessionId, newest.back().id, 2);
    CHECK(older.size() == 2);
    if (older.size() != 2) return;
    CHECK(older[0].id == ids[2]);
    CHECK(older[1].id == ids[1]);
    const auto oldest = store.listMessagesPage(sessionId, older.back().id, 2);
    CHECK(oldest.size() == 1);
    if (oldest.size() == 1) CHECK(oldest[0].id == ids[0]);
    CHECK(store.listMessagesPage(sessionId, ids[0], 2).empty());
    CHECK(store.listMessagesPage(sessionId, "", 0).empty());

    MaiMessage later = makeMessage(MaiIdGenerator::newMessageId(), MaiRole::Assistant);
    store.putMessage(sessionId, later);
    const auto afterAppend = store.listMessagesPage(sessionId, newest.back().id, 2);
    CHECK(afterAppend.size() == 2);
    if (afterAppend.size() == 2) CHECK(afterAppend[0].id == ids[2]);
}

void contract_recovers_interrupted_tools(MaiSessionStore& store, const char* which) {
    std::printf("   [%s] interrupted tools recover without changing completed calls\n", which);
    const std::string sessionId = MaiIdGenerator::newSessionId();
    const std::string otherSessionId = MaiIdGenerator::newSessionId();
    store.putSession(makeSession(sessionId, "recover", 1));
    store.putSession(makeSession(otherSessionId, "other", 1));

    auto saveTool = [&](const std::string& session, MaiToolState state, bool finished) {
        MaiMessage message = makeMessage(MaiIdGenerator::newMessageId(), MaiRole::Assistant);
        if (!finished) message.completed = 0;
        MaiMessagePart part;
        part.id = MaiIdGenerator::newPartId();
        MaiToolPart tool;
        tool.tool = "shell";
        tool.state = state;
        part.body = tool;
        message.parts.push_back(part);
        store.putMessage(session, message);
        return message.id;
    };

    const std::string pendingId = saveTool(sessionId, MaiToolState::Pending, false);
    const std::string completedId = saveTool(sessionId, MaiToolState::Completed, true);
    const std::string runningId = saveTool(otherSessionId, MaiToolState::Running, false);
    store.recoverInterruptedTools("interrupted", 3000);

    auto findMessage = [&](const std::string& session, const std::string& id) {
        for (const auto& message : store.listMessages(session))
            if (message.id == id) return message;
        return MaiMessage{};
    };
    const auto pending = findMessage(sessionId, pendingId);
    const auto completed = findMessage(sessionId, completedId);
    const auto running = findMessage(otherSessionId, runningId);
    CHECK(pending.completed == 3000);
    CHECK(running.completed == 3000);
    CHECK(completed.completed == 2001);
    CHECK(pending.parts.size() == 1);
    CHECK(running.parts.size() == 1);
    CHECK(completed.parts.size() == 1);
    if (pending.parts.size() != 1 || running.parts.size() != 1 || completed.parts.size() != 1)
        return;
    for (const MaiMessage* message : {&pending, &running}) {
        const auto* tool = std::get_if<MaiToolPart>(&message->parts[0].body);
        CHECK(tool && tool->state == MaiToolState::Error);
        CHECK(tool && tool->error == "interrupted" && tool->output == "interrupted");
    }
    const auto* finishedTool = std::get_if<MaiToolPart>(&completed.parts[0].body);
    CHECK(finishedTool && finishedTool->state == MaiToolState::Completed);
    store.recoverInterruptedTools("second pass", 4000);
    CHECK(findMessage(sessionId, pendingId).completed == 3000);
}

void contract_put_message_replaces_parts(MaiSessionStore& store, const char* which) {
    std::printf("   [%s] re-putting a message replaces it\n", which);

    // 跑一轮的过程中，assistant 消息会被反复 put：先一个 part，再两个，再三个。
    // 要是每次都追加而不是覆盖，界面上就会看到内容重复。
    const std::string sessionId = MaiIdGenerator::newSessionId();
    store.putSession(makeSession(sessionId, "growing", 1));

    MaiMessage message = makeMessage(MaiIdGenerator::newMessageId(), MaiRole::Assistant);
    message.parts.push_back(textPart(MaiIdGenerator::newPartId(), "one"));
    store.putMessage(sessionId, message);

    message.parts.push_back(textPart(MaiIdGenerator::newPartId(), "two"));
    store.putMessage(sessionId, message);

    const auto loaded = store.listMessages(sessionId);
    CHECK(loaded.size() == 1);
    if (loaded.size() == 1) CHECK(loaded[0].parts.size() == 2);
}

void contract_remove_session_takes_messages(MaiSessionStore& store, const char* which) {
    std::printf("   [%s] deleting a session takes its messages\n", which);

    const std::string sessionId = MaiIdGenerator::newSessionId();
    store.putSession(makeSession(sessionId, "doomed", 1));
    MaiMessage message = makeMessage(MaiIdGenerator::newMessageId(), MaiRole::User);
    message.parts.push_back(textPart(MaiIdGenerator::newPartId(), "bye"));
    store.putMessage(sessionId, message);
    CHECK(store.listMessages(sessionId).size() == 1);

    CHECK(store.removeSession(sessionId));
    // 留着孤儿消息的话，建一个同 id 的新会话会诡异地带着上一个的历史
    CHECK(store.listMessages(sessionId).empty());
}

void contract_clear_messages_keeps_the_session(MaiSessionStore& store, const char* which) {
    std::printf("   [%s] clearMessages drops the history, keeps the session\n", which);

    const std::string sessionId = MaiIdGenerator::newSessionId();
    store.putSession(makeSession(sessionId, "keep me", 7));
    for (int i = 0; i < 3; ++i) {
        MaiMessage message = makeMessage(MaiIdGenerator::newMessageId(), MaiRole::User);
        message.parts.push_back(textPart(MaiIdGenerator::newPartId(), "say something"));
        store.putMessage(sessionId, message);
    }
    CHECK(store.listMessages(sessionId).size() == 3);

    CHECK(store.clearMessages(sessionId));
    CHECK(store.listMessages(sessionId).empty());

    // 和 removeSession 的**全部区别**就在下面这两条：会话还在，还能接着用。
    MaiSession still;
    CHECK(store.getSession(sessionId, still));
    CHECK(still.title == "keep me");

    // 清完之后还能正常写。SQLite 那边是在一个事务里逐级删的，
    // 少了 COMMIT 的话后面的写会一直卡在锁上——这条盯着那个。
    MaiMessage fresh = makeMessage(MaiIdGenerator::newMessageId(), MaiRole::User);
    fresh.parts.push_back(textPart(MaiIdGenerator::newPartId(), "starting over"));
    store.putMessage(sessionId, fresh);
    CHECK(store.listMessages(sessionId).size() == 1);

    // 不存在的会话要明确说"没有"，不能假装清成功了。
    CHECK(!store.clearMessages("ses_does_not_exist"));
}

void contract_mutate_session(MaiSessionStore& store, const char* which) {
    std::printf("   [%s] mutateSession does read-modify-write\n", which);

    const std::string sessionId = MaiIdGenerator::newSessionId();
    store.putSession(makeSession(sessionId, "before", 10));

    const bool changed = store.mutateSession(sessionId, [](MaiSession& session) {
        session.title = "after";
        session.updated = 20;
    });
    CHECK(changed);

    MaiSession loaded;
    CHECK(store.getSession(sessionId, loaded));
    CHECK(loaded.title == "after");
    CHECK(loaded.updated == 20);
    CHECK(loaded.directory == "/tmp/work");  // 没动的字段不能被清掉

    // 会话不存在时返回 false，而且不能凭空造一条出来
    CHECK(!store.mutateSession("ses_nope", [](MaiSession&) {}));
    MaiSession ghost;
    CHECK(!store.getSession("ses_nope", ghost));

    store.removeSession(sessionId);
}

// 切模型：M5 明确要的能力之一。改 model 不能碰到别的字段。
void contract_switch_model(MaiSessionStore& store, const char* which) {
    std::printf("   [%s] switching model touches only model\n", which);

    const std::string sessionId = MaiIdGenerator::newSessionId();
    store.putSession(makeSession(sessionId, "keep this title", 10));

    CHECK(store.mutateSession(sessionId, [](MaiSession& session) {
        session.model = "glm-4.6";
        session.updated = 30;
    }));

    MaiSession loaded;
    CHECK(store.getSession(sessionId, loaded));
    CHECK(loaded.model == "glm-4.6");
    CHECK(loaded.title == "keep this title");  // 标题不能被顺手覆盖
    CHECK(loaded.created == 1000);

    store.removeSession(sessionId);
}

void contract_concurrent_writes(MaiSessionStore& store, const char* which) {
    std::printf("   [%s] concurrent writes\n", which);

    // 多个会话各跑各的线程，都会写同一个 store。这条不是测性能，是测"同时写不会丢行、不会崩"。
    constexpr int kThreads = 4;
    constexpr int kPerThread = 20;
    std::vector<std::string> sessionIds;
    for (int i = 0; i < kThreads; ++i) {
        const std::string id = MaiIdGenerator::newSessionId();
        sessionIds.push_back(id);
        store.putSession(makeSession(id, "concurrent", 1));
    }

    std::vector<std::thread> workers;
    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&store, id = sessionIds[static_cast<std::size_t>(i)]] {
            for (int n = 0; n < kPerThread; ++n) {
                MaiMessage message = makeMessage(MaiIdGenerator::newMessageId(), MaiRole::User);
                message.parts.push_back(textPart(MaiIdGenerator::newPartId(), "x"));
                store.putMessage(id, message);
                store.mutateSession(id, [](MaiSession& session) { session.updated += 1; });
            }
        });
    }
    for (auto& worker : workers) worker.join();

    for (const auto& id : sessionIds) {
        CHECK(store.listMessages(id).size() == kPerThread);
        MaiSession loaded;
        CHECK(store.getSession(id, loaded));
        // 读-改-写必须原子，不然这里会少于 1 + kPerThread
        CHECK(loaded.updated == 1 + kPerThread);
    }
    for (const auto& id : sessionIds) store.removeSession(id);

    CHECK(!store.lastWriteError().hasError());
}

void contract_specialist_reply(MaiSessionStore& store, const char* which) {
    std::printf("   [%s] specialist text stream notifies parent once without an artifact\n", which);
    const std::string owner = MaiIdGenerator::newSessionId();
    const std::string stranger = MaiIdGenerator::newSessionId();
    store.putSession(makeSession(owner, "owner", 1));
    store.putSession(makeSession(stranger, "stranger", 1));
    MaiSpecialistTask task;
    task.id = MaiIdGenerator::generate("spt_");
    task.ownerSessionId = owner;
    task.specialistName = "text_specialist";
    task.providerTaskId = "provider-task";
    task.intent = "Write a short summary";
    task.created = 1000;
    CHECK(!store.insertSpecialistTask(task));
    CHECK(!store.listActiveSpecialistTasks(10).empty());
    CHECK(
        store.finishSpecialistTask(task.id, owner, MaiSpecialistTaskStatus::Succeeded, "", "", 1000)
            .hasError());
    CHECK(!store.appendSpecialistText(task.id, owner, "hello ", 1001));
    CHECK(!store.appendSpecialistText(task.id, owner, "world", 1002));
    CHECK(!store.finishSpecialistTask(task.id, owner, MaiSpecialistTaskStatus::Succeeded, "", "",
                                      1003));
    MaiSpecialistTask loaded;
    CHECK(store.getSpecialistTask(task.id, owner, loaded));
    CHECK(loaded.status == MaiSpecialistTaskStatus::Succeeded);
    CHECK(loaded.finalText == "hello world");
    CHECK(loaded.outputPath.empty());
    CHECK(store.listActiveSpecialistTasks(10).empty());
    auto pending = store.listUnnotifiedSpecialistTasks(10);
    CHECK(pending.size() == 1);
    if (pending.size() == 1) CHECK(pending[0].finalText == "hello world");
    const std::string firstMessage = MaiIdGenerator::newMessageId();
    const std::string secondMessage = MaiIdGenerator::newMessageId();
    const auto reserved = store.reserveSpecialistNotification(task.id, owner, firstMessage, 2000);
    CHECK(reserved);
    if (reserved) CHECK(reserved.value() == firstMessage);
    const auto retried = store.reserveSpecialistNotification(task.id, owner, secondMessage, 3000);
    CHECK(retried);
    if (retried) CHECK(retried.value() == firstMessage);
    CHECK(store.markSpecialistNotified(task.id, stranger, firstMessage, 4000).hasError());
    CHECK(store.markSpecialistNotified(task.id, owner, secondMessage, 4000).hasError());
    CHECK(!store.markSpecialistNotified(task.id, owner, firstMessage, 4000));
    CHECK(store.listUnnotifiedSpecialistTasks(10).empty());
    CHECK(store.appendSpecialistText(task.id, owner, "again", 4001).hasError());
    MaiSpecialistTask failed = task;
    failed.id = MaiIdGenerator::generate("spt_");
    CHECK(!store.insertSpecialistTask(failed));
    CHECK(!store.appendSpecialistText(failed.id, owner, "partial result", 4002));
    CHECK(!store.finishSpecialistTask(failed.id, owner, MaiSpecialistTaskStatus::Failed, "", "",
                                      4003));
    CHECK(store.getSpecialistTask(failed.id, owner, loaded));
    CHECK(loaded.errorText == "partial result");
    CHECK(store.removeSession(owner));
    CHECK(!store.getSpecialistTask(task.id, owner, loaded));
    CHECK(!store.lastWriteError().hasError());
}

void contract_specialist_delete(MaiSessionStore& store, const char* which) {
    std::printf("   [%s] only delivered terminal specialist tasks can be deleted\n", which);
    const std::string owner = MaiIdGenerator::newSessionId();
    const std::string foreign = MaiIdGenerator::newSessionId();
    store.putSession(makeSession(owner, "task owner", 1));
    store.putSession(makeSession(foreign, "other owner", 1));
    MaiSpecialistTask task;
    task.id = MaiIdGenerator::generate("spt_");
    task.ownerSessionId = owner;
    task.specialistName = "wan_video";
    task.providerTaskId = "provider-1";
    task.intent = "Create a video";
    task.created = 1000;
    CHECK(!store.insertSpecialistTask(task));
    CHECK(store.deleteCompletedSpecialistTask(task.id, foreign).hasError());
    CHECK(store.deleteCompletedSpecialistTask(task.id, owner).hasError());
    CHECK(!store.finishSpecialistTask(task.id, owner, MaiSpecialistTaskStatus::Canceled,
                                      "Canceled by provider", {}, 1001));
    CHECK(store.deleteCompletedSpecialistTask(task.id, owner).hasError());
    const auto reservation =
        store.reserveSpecialistNotification(task.id, owner, "notification", 1002);
    CHECK(reservation.isOk());
    if (!reservation.isOk()) return;
    CHECK(!store.markSpecialistNotified(task.id, owner, reservation.value(), 1003));
    MaiSpecialistTask child = task;
    child.id = MaiIdGenerator::generate("spt_");
    child.parentTaskId = task.id;
    CHECK(!store.insertSpecialistTask(child));
    CHECK(store.deleteCompletedSpecialistTask(task.id, owner).hasError());
    CHECK(!store.finishSpecialistTask(child.id, owner, MaiSpecialistTaskStatus::Failed,
                                      "Synthetic failure", {}, 1004));
    const auto childNotice =
        store.reserveSpecialistNotification(child.id, owner, "child-notification", 1005);
    CHECK(childNotice.isOk());
    if (!childNotice.isOk()) return;
    CHECK(!store.markSpecialistNotified(child.id, owner, childNotice.value(), 1006));
    CHECK(!store.deleteCompletedSpecialistTask(child.id, owner));
    CHECK(!store.deleteCompletedSpecialistTask(task.id, owner));
    MaiSpecialistTask loaded;
    CHECK(!store.getSpecialistTask(task.id, owner, loaded));
}

void runContract(MaiSessionStore& store, const char* which) {
    contract_session_crud(store, which);
    contract_list_sessions_ordering(store, which);
    contract_messages_and_parts(store, which);
    contract_message_pages(store, which);
    contract_recovers_interrupted_tools(store, which);
    contract_put_message_replaces_parts(store, which);
    contract_remove_session_takes_messages(store, which);
    contract_clear_messages_keeps_the_session(store, which);
    contract_mutate_session(store, which);
    contract_switch_model(store, which);
    contract_concurrent_writes(store, which);
    contract_specialist_reply(store, which);
    contract_specialist_delete(store, which);
    const std::string owner = MaiIdGenerator::newSessionId();
    const std::string other = MaiIdGenerator::newSessionId();
    store.putSession(makeSession(owner, "specialist owner", 1));
    store.putSession(makeSession(other, "other owner", 1));
    MaiSpecialistTask first;
    first.id = MaiIdGenerator::generate("spt_");
    first.ownerSessionId = owner;
    first.specialistName = "seedance_video";
    first.providerTaskId = "provider-1";
    first.intent = "Replace the background";
    first.contextSummary = "Keep the subject unchanged";
    first.outputPath = "/tmp/result.mp4";
    first.created = 1234;
    CHECK(!store.insertSpecialistTask(first));
    MaiSpecialistTask loaded;
    CHECK(store.getSpecialistTask(first.id, owner, loaded));
    CHECK(loaded.intent == first.intent);
    CHECK(loaded.contextSummary == first.contextSummary);
    CHECK(loaded.providerTaskId == first.providerTaskId);
    CHECK(!store.getSpecialistTask(first.id, other, loaded));
    CHECK(store.insertSpecialistTask(first).hasError());
    MaiSpecialistTask revision = first;
    revision.id = MaiIdGenerator::generate("spt_");
    revision.parentTaskId = first.id;
    revision.intent = "Fix the white halo";
    CHECK(!store.insertSpecialistTask(revision));
    CHECK(store.getSpecialistTask(revision.id, owner, loaded));
    CHECK(loaded.parentTaskId == first.id);
    MaiSpecialistTask crossOwner = revision;
    crossOwner.id = MaiIdGenerator::generate("spt_");
    crossOwner.ownerSessionId = other;
    CHECK(store.insertSpecialistTask(crossOwner).hasError());
    CHECK(store.clearMessages(owner));
    CHECK(!store.getSpecialistTask(first.id, owner, loaded));
    CHECK(!store.getSpecialistTask(revision.id, owner, loaded));
    CHECK(!store.lastWriteError().hasError());
}

// ── 只有 SQLite 才有的性质 ─────────────────────────────────────

void test_survives_reopen() {
    std::printf("-> test_survives_reopen\n");
    TempDir temp;
    const std::string path = temp.file("agent.db");

    std::string sessionId;
    std::string messageId;
    std::string specialistTaskId;
    {
        auto opened = makeMaiSqliteStore(path);
        CHECK(opened.isOk());
        if (!opened.isOk()) return;
        auto store = std::move(opened.value());

        sessionId = MaiIdGenerator::newSessionId();
        store->putSession(makeSession(sessionId, "persisted", 42));

        MaiMessage message = makeMessage(MaiIdGenerator::newMessageId(), MaiRole::Assistant);
        messageId = message.id;
        message.parts.push_back(textPart(MaiIdGenerator::newPartId(), "still here"));
        store->putMessage(sessionId, message);
        MaiSpecialistTask task;
        task.id = MaiIdGenerator::generate("spt_");
        task.ownerSessionId = sessionId;
        task.specialistName = "seedream_image";
        task.intent = "Make a blue background";
        task.outputPath = "/tmp/result.png";
        task.created = 42;
        specialistTaskId = task.id;
        CHECK(!store->insertSpecialistTask(task));
        CHECK(!store->lastWriteError().hasError());
    }  // 析构 = 关库

    // 重新打开：这才是"落库"这两个字的全部意义
    auto reopened = makeMaiSqliteStore(path);
    CHECK(reopened.isOk());
    if (!reopened.isOk()) return;
    auto store = std::move(reopened.value());

    MaiSession loaded;
    CHECK(store->getSession(sessionId, loaded));
    CHECK(loaded.title == "persisted");
    CHECK(loaded.updated == 42);
    MaiSpecialistTask loadedTask;
    CHECK(store->getSpecialistTask(specialistTaskId, sessionId, loadedTask));
    CHECK(loadedTask.intent == "Make a blue background");
    CHECK(loadedTask.outputPath == "/tmp/result.png");

    const auto messages = store->listMessages(sessionId);
    CHECK(messages.size() == 1);
    if (messages.size() == 1) {
        CHECK(messages[0].id == messageId);
        CHECK(messages[0].parts.size() == 1);
        if (messages[0].parts.size() == 1) {
            const auto* text = std::get_if<MaiTextPart>(&messages[0].parts[0].body);
            CHECK(text && text->text == "still here");
        }
    }
}

void test_specialist_reply_survives_reopen() {
    std::printf("-> test_specialist_reply_survives_reopen\n");
    TempDir temp;
    const std::string path = temp.file("specialist-reply.db");
    const std::string sessionId = MaiIdGenerator::newSessionId();
    const std::string taskId = MaiIdGenerator::generate("spt_");
    const std::string messageId = MaiIdGenerator::newMessageId();
    {
        auto opened = makeMaiSqliteStore(path);
        CHECK(opened.isOk());
        if (!opened.isOk()) return;
        auto store = std::move(opened.value());
        store->putSession(makeSession(sessionId, "owner", 1));
        MaiSpecialistTask task;
        task.id = taskId;
        task.ownerSessionId = sessionId;
        task.specialistName = "text_specialist";
        task.intent = "summarize";
        task.created = 1000;
        CHECK(!store->insertSpecialistTask(task));
        CHECK(!store->finishSpecialistTask(taskId, sessionId, MaiSpecialistTaskStatus::Succeeded,
                                           "text-only reply", "", 2000));
        const auto reserved =
            store->reserveSpecialistNotification(taskId, sessionId, messageId, 3000);
        CHECK(reserved);
    }
    {
        auto opened = makeMaiSqliteStore(path);
        CHECK(opened.isOk());
        if (!opened.isOk()) return;
        auto store = std::move(opened.value());
        const auto pending = store->listUnnotifiedSpecialistTasks(10);
        CHECK(pending.size() == 1);
        if (pending.size() == 1) {
            CHECK(pending[0].notificationMessageId == messageId);
            CHECK(pending[0].finalText == "text-only reply");
            CHECK(pending[0].outputPath.empty());
        }
        const auto reserved = store->reserveSpecialistNotification(
            taskId, sessionId, MaiIdGenerator::newMessageId(), 4000);
        CHECK(reserved);
        if (reserved) CHECK(reserved.value() == messageId);
        CHECK(!store->markSpecialistNotified(taskId, sessionId, messageId, 5000));
    }
    auto opened = makeMaiSqliteStore(path);
    CHECK(opened.isOk());
    if (opened.isOk()) CHECK(opened.value()->listUnnotifiedSpecialistTasks(10).empty());
}

void test_migrates_unfinished_tool_index() {
    std::printf("-> test_migrates_unfinished_tool_index\n");
    TempDir temp;
    const std::string path = temp.file("migration.db");
    const std::string sessionId = MaiIdGenerator::newSessionId();
    {
        auto opened = makeMaiSqliteStore(path);
        CHECK(opened.isOk());
        if (!opened.isOk()) return;
        auto store = std::move(opened.value());
        store->putSession(makeSession(sessionId, "before migration", 1));
    }

    sqlite3* database = nullptr;
    CHECK(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
    if (!database) return;
    CHECK(sqlite3_exec(database,
                       "DROP INDEX parts_unfinished_tool;"
                       "DROP TABLE specialist_tasks;"
                       "PRAGMA user_version=2;",
                       nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(database);

    auto reopened = makeMaiSqliteStore(path);
    CHECK(reopened.isOk());
    if (!reopened.isOk()) return;
    MaiSession loaded;
    CHECK(reopened.value()->getSession(sessionId, loaded));
    reopened.value().reset();

    database = nullptr;
    CHECK(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
    if (!database) return;
    int foundIndex = 0;
    const auto countIndex = [](void* result, int, char**, char**) -> int {
        ++*static_cast<int*>(result);
        return 0;
    };
    CHECK(sqlite3_exec(database,
                       "SELECT name FROM sqlite_master WHERE type='index' "
                       "AND name='parts_unfinished_tool'",
                       countIndex, &foundIndex, nullptr) == SQLITE_OK);
    CHECK(foundIndex == 1);
    sqlite3_close(database);
}

void test_creates_parent_directory() {
    std::printf("-> test_creates_parent_directory\n");
    TempDir temp;
    // 父目录不存在。少了自动创建这一步，第一次跑的人只会看到"unable to open database file"，
    // 猜不到是目录的问题。
    const std::string path = (temp.root / "deep" / "nested" / "agent.db").u8string();
    auto opened = makeMaiSqliteStore(path);
    CHECK(opened.isOk());
    if (!opened.isOk()) std::printf("   %s\n", opened.error().message().c_str());
}

void test_bad_path_reports_error() {
    std::printf("-> test_bad_path_reports_error\n");
    CHECK(!makeMaiSqliteStore("").isOk());

    // 指到一个不是数据库的文件上——这是最常见的手误
    TempDir temp;
    const std::string path = temp.file("not-a-database.txt");
    {
        std::ofstream out(temp.root / "not-a-database.txt", std::ios::binary);
        out << "not a database, just text long enough to get past SQLite's header check "
               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    }
    auto opened = makeMaiSqliteStore(path);
    // 关键是"明确失败"，不是返回一个用起来处处出错的半死对象
    CHECK(!opened.isOk());
    if (!opened.isOk()) CHECK(opened.error().code() == MaiErrorCode::Internal);
}

void test_message_order_follows_id_order() {
    std::printf("-> test_message_order_follows_id_order\n");
    // SQLite 那边靠 ORDER BY id 还原插入顺序，前提是 MaiIdGenerator 产出的 id 单调递增。
    // 这条用例盯着那个许诺——换 id 方案时它会先红。
    auto opened = makeMaiSqliteStore(":memory:");
    CHECK(opened.isOk());
    if (!opened.isOk()) return;
    auto store = std::move(opened.value());

    const std::string sessionId = MaiIdGenerator::newSessionId();
    store->putSession(makeSession(sessionId, "ordered", 1));

    std::vector<std::string> expected;
    for (int i = 0; i < 30; ++i) {
        MaiMessage message = makeMessage(MaiIdGenerator::newMessageId(), MaiRole::User);
        expected.push_back(message.id);
        store->putMessage(sessionId, message);
    }

    const auto loaded = store->listMessages(sessionId);
    CHECK(loaded.size() == expected.size());
    if (loaded.size() != expected.size()) return;
    for (std::size_t i = 0; i < expected.size(); ++i) CHECK(loaded[i].id == expected[i]);
}

void test_non_ascii_round_trip() {
    std::printf("-> test_non_ascii_round_trip\n");
    TempDir temp;
    // 路径和内容都用非 ASCII。SQLite 的文件名参数是 UTF-8，
    // 而 MSVC 的 fs::path 会把 narrow 字符串按 ANSI 代码页解释——
    // 这两件事撞在一起，中文路径当初是让进程直接挂掉的。
    //
    // 被测数据，不是文案：这条用例测的就是非 ASCII，字节一个都不能改。
    //   \u4e2d\u6587  中文
    const std::string directory = "\u4e2d\u6587";
    const std::string path =
        (temp.root / std::filesystem::u8path(directory) / "db.sqlite").u8string();
    auto opened = makeMaiSqliteStore(path);
    CHECK(opened.isOk());
    if (!opened.isOk()) {
        std::printf("   %s\n", opened.error().message().c_str());
        return;
    }
    auto store = std::move(opened.value());

    //   \u4f60\u597d  你好
    //   \U0001F642  emoji
    const std::string body = "\u4f60\u597d \U0001F642";
    const std::string sessionId = MaiIdGenerator::newSessionId();
    MaiSession session = makeSession(sessionId, body, 1);
    session.directory = directory;
    store->putSession(session);

    MaiMessage message = makeMessage(MaiIdGenerator::newMessageId(), MaiRole::User);
    message.parts.push_back(textPart(MaiIdGenerator::newPartId(), body));
    store->putMessage(sessionId, message);

    MaiSession loaded;
    CHECK(store->getSession(sessionId, loaded));
    CHECK(loaded.title == body);
    CHECK(loaded.directory == directory);

    const auto messages = store->listMessages(sessionId);
    CHECK(messages.size() == 1);
    if (messages.size() == 1 && messages[0].parts.size() == 1) {
        const auto* text = std::get_if<MaiTextPart>(&messages[0].parts[0].body);
        CHECK(text && text->text == body);
    }
}

void test_sqlite_sanitizes_invalid_utf8_before_write() {
    std::printf("-> test_sqlite_sanitizes_invalid_utf8_before_write\n");
    TempDir temp;
    const std::string path = temp.file("utf8.db");
    const std::string invalid("\xD5\xD2", 2);
    std::string sessionId;
    std::string messageId;

    {
        auto opened = makeMaiSqliteStore(path);
        CHECK(opened.isOk());
        if (!opened.isOk()) return;
        auto store = std::move(opened.value());

        sessionId = MaiIdGenerator::newSessionId();
        MaiSession session = makeSession(sessionId, invalid, 1);
        store->putSession(session);

        MaiMessage message = makeMessage(MaiIdGenerator::newMessageId(), MaiRole::Assistant);
        messageId = message.id;
        message.parts.push_back(textPart(MaiIdGenerator::newPartId(), invalid));
        MaiMessagePart toolPart;
        toolPart.id = MaiIdGenerator::newPartId();
        MaiToolPart tool;
        tool.tool = "shell";
        tool.callId = "call_invalid";
        tool.input = invalid;
        tool.output = invalid;
        tool.error = invalid;
        tool.state = MaiToolState::Error;
        toolPart.body = std::move(tool);
        message.parts.push_back(std::move(toolPart));
        store->putMessage(sessionId, message);
    }

    auto reopened = makeMaiSqliteStore(path);
    CHECK(reopened.isOk());
    if (!reopened.isOk()) return;
    auto store = std::move(reopened.value());
    const auto isUtf8 = [](const std::string& value) {
        try {
            (void)nlohmann::json(value).dump();
            return true;
        } catch (...) {
            return false;
        }
    };

    MaiSession session;
    CHECK(store->getSession(sessionId, session));
    CHECK(isUtf8(session.title));
    CHECK(session.title.find("\xEF\xBF\xBD") != std::string::npos);

    const auto messages = store->listMessages(sessionId);
    CHECK(messages.size() == 1);
    if (messages.size() != 1 || messages[0].id != messageId || messages[0].parts.size() != 2)
        return;
    const auto* text = std::get_if<MaiTextPart>(&messages[0].parts[0].body);
    CHECK(text != nullptr);
    if (text == nullptr) return;
    CHECK(isUtf8(text->text));
    CHECK(text->text.find("\xEF\xBF\xBD") != std::string::npos);

    const auto* tool = std::get_if<MaiToolPart>(&messages[0].parts[1].body);
    CHECK(tool != nullptr);
    if (tool == nullptr) return;
    CHECK(isUtf8(tool->input));
    CHECK(isUtf8(tool->output));
    CHECK(isUtf8(tool->error));
    CHECK(tool->input.find("\xEF\xBF\xBD") != std::string::npos);
    CHECK(tool->output.find("\xEF\xBF\xBD") != std::string::npos);
    CHECK(tool->error.find("\xEF\xBF\xBD") != std::string::npos);
}

}  // namespace

int main() {
    std::printf("-> contract: memory store\n");
    {
        auto store = makeMaiMemoryStore();
        runContract(*store, "memory");
    }

    std::printf("-> contract: sqlite (:memory:, full SQL path, no disk)\n");
    {
        auto opened = makeMaiSqliteStore(":memory:");
        CHECK(opened.isOk());
        if (opened.isOk()) {
            auto store = std::move(opened.value());
            runContract(*store, "sqlite");
        }
    }

    std::printf("-> contract: sqlite (real file)\n");
    {
        TempDir temp;
        auto opened = makeMaiSqliteStore(temp.file("contract.db"));
        CHECK(opened.isOk());
        if (opened.isOk()) {
            auto store = std::move(opened.value());
            runContract(*store, "sqlite-file");
        }
    }

    test_survives_reopen();
    test_specialist_reply_survives_reopen();
    test_migrates_unfinished_tool_index();
    test_creates_parent_directory();
    test_bad_path_reports_error();
    test_message_order_follows_id_order();
    test_non_ascii_round_trip();
    test_sqlite_sanitizes_invalid_utf8_before_write();

    if (failures) {
        std::printf("\n%d checks failed\n", failures);
        return 1;
    }
    std::printf("\nstore tests passed\n");
    return 0;
}
