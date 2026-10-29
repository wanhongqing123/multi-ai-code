// 文件作用：验证当前会话改名的普通 API、真实模型工具链、自动命名竞争及 SQLite 持久化。
// 改名不换会话、不动工作区/模型/历史；显式占位名称也不能在收尾或重启后被覆盖。
#include "MaiSessionTools.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <json.hpp>
#include <sqlite3.h>

#include "MaiAgent.h"
#include "MaiFakeModelClient.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiMemoryStore.h"
#include "MaiSqliteStore.h"

static int failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

namespace {

struct MaiRenameTestDirectory {
    MaiRenameTestDirectory()
        : path(MaiFileSystem::temporaryDirectory().append(
              MaiFilePath::fromUtf8(MaiIdGenerator::generate("session_rename_")))) {
        CHECK(!MaiFileSystem::createDirectories(path));
    }
    ~MaiRenameTestDirectory() {
        MaiFileSystem::removeRecursively(path);
    }
    MaiFilePath path;
};

std::string repeated(const std::string& scalar, int count) {
    std::string text;
    for (int index = 0; index < count; ++index) text += scalar;
    return text;
}

void test_current_only_validation_and_noop() {
    auto store = makeMaiMemoryStore();
    auto* observed = store.get();
    auto registry = std::make_unique<MaiToolRegistry>();
    registerMaiSessionTools(*registry, false);
    MaiTool* rename = registry->find("session_rename");
    CHECK(rename != nullptr);
    if (!rename) return;
    CHECK(rename->requiresApproval("{}"));
    CHECK(!rename->requiresPerCallApproval("{}"));
    CHECK(!rename->requiresPaidApproval("{}"));
    CHECK(registry->find("session_set_workspace") == nullptr);
    MaiAgent agent(std::move(store), nullptr, std::move(registry));
    const auto id = agent.submit(MaiCreateSession{"/tmp/work", "Current", "test-model"}).value();
    const auto other = agent.submit(MaiCreateSession{"/tmp/other", "Other", "other-model"}).value();
    MaiToolContext context;
    context.sessionAgent = &agent;
    context.sessionId = id;
    int updated = 0;
    bool persistedBeforeEvent = false;
    agent.eventBus().subscribe([&](const MaiEvent& event) {
        if (event.type != MaiEventType::SessionUpdated || event.sessionId != id) return;
        ++updated;
        MaiSession stored;
        persistedBeforeEvent =
            agent.getSession(id, stored) && stored.title == event.detail && stored.titleEdited;
    });
    const auto renamed = rename->execute(R"({"title":"Changed current"})", context);
    CHECK(!renamed.hasError());
    if (!renamed.hasError()) {
        const auto output = nlohmann::json::parse(renamed.output());
        CHECK(output.at("session_id") == id && output.at("title") == "Changed current");
        CHECK(output.at("changed") == true);
    }
    CHECK(updated == 1 && persistedBeforeEvent);
    MaiSession current, untouched;
    CHECK(agent.getSession(id, current) && agent.getSession(other, untouched));
    CHECK(current.title == "Changed current" && current.titleEdited);
    CHECK(current.directory == "/tmp/work" && current.model == "test-model" &&
          current.agent == "build");
    CHECK(untouched.title == "Other" && untouched.directory == "/tmp/other");
    CHECK(agent.listMessages(id).empty() && agent.listSessions().size() == 2);
    const auto unchanged = rename->execute(R"({"title":"Changed current"})", context);
    CHECK(!unchanged.hasError());
    if (!unchanged.hasError())
        CHECK(nlohmann::json::parse(unchanged.output()).at("changed") == false);
    CHECK(updated == 1);
    MaiSession afterNoop;
    CHECK(agent.getSession(id, afterNoop) && afterNoop.updated == current.updated);

    for (const auto& bad : std::vector<std::string>{
             "", "  ", "line\nbreak", "\t", "bad\x7f", u8"\u0085", u8"\u00a0\u3000", u8"\u2028",
             u8"\u2029", std::string(121, 'a'), repeated(u8"\U0001f600", 121)}) {
        const auto result = rename->execute(nlohmann::json{{"title", bad}}.dump(), context);
        CHECK(result.hasError() && result.error().code() == MaiErrorCode::InvalidInput);
        CHECK(!agent.submit(MaiUpdateSession{id, bad.empty() ? "\n" : bad, "", "", ""}));
    }
    CHECK(maiValidateSessionTitle(std::string("\xff")));
    CHECK(maiValidateSessionTitle(std::string("\xed\xa0\x80")));
    CHECK(maiValidateSessionTitle(std::string("\xf8\x80\x90\x80\x80")));
    CHECK(!maiValidateSessionTitle(repeated(u8"\u540d", 120)));
    CHECK(!maiValidateSessionTitle(repeated(u8"\U0001f600", 120)));
    CHECK(!maiValidateSessionTitle("  Name with preserved spaces  "));
    CHECK(rename
              ->execute(nlohmann::json{{"title", "Wrong target"}, {"session_id", other}}.dump(),
                        context)
              .hasError());
    CHECK(rename->execute(R"({"title":false})", context).hasError());
    CHECK(rename->execute("{}", context).hasError());
    CHECK(updated == 1);
    CHECK(agent.getSession(id, current) && current.title == "Changed current");
    CHECK(agent.getSession(other, untouched) && untouched.title == "Other");

    MaiSession child;
    child.id = MaiIdGenerator::newSessionId();
    child.parentId = id;
    child.title = "Child";
    observed->putSession(child);
    context.sessionId = child.id;
    CHECK(rename->execute(R"({"title":"Not a user conversation"})", context).hasError());
    context.sessionId = "ses_missing";
    CHECK(rename->execute(R"({"title":"Missing"})", context).error().code() ==
          MaiErrorCode::NotFound);
    context.sessionAgent = nullptr;
    CHECK(rename->execute(R"({"title":"Unavailable"})", context).error().code() ==
          MaiErrorCode::NotSupported);
}

void test_running_rename_keeps_explicit_placeholder_title() {
    MaiFakeModelClient::Turn rename;
    rename.invocations.push_back({"call_rename", "session_rename", R"({"title":"New session"})"});
    MaiFakeModelClient::Turn done;
    done.textChunks = {"Renamed."};
    MaiAgent::Options options;
    options.approvalPolicy = MaiApprovalPolicy::Never;
    auto tools = std::make_unique<MaiToolRegistry>();
    // 移动端关闭工作区切换时仍必须获得当前会话改名能力。
    registerMaiSessionTools(*tools, false);
    CHECK(tools->find("session_rename") != nullptr);
    auto model = std::make_unique<MaiFakeModelClient>(
        std::vector<MaiFakeModelClient::Turn>{rename, done, done});
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools), options);
    const std::string session = agent.submit(MaiCreateSession{"/tmp", "", "test"}).value();
    int permissionAsked = 0;
    agent.eventBus().subscribe([&](const MaiEvent& event) {
        if (event.type == MaiEventType::PermissionAsked) ++permissionAsked;
    });
    CHECK(agent.submit(MaiSendPrompt{session, "Please preserve my explicit placeholder name"}));
    agent.waitIdle();
    MaiSession stored;
    CHECK(agent.getSession(session, stored));
    CHECK(stored.title == kMaiDefaultSessionTitle);
    CHECK(stored.titleEdited && !stored.isUntitled());
    CHECK(permissionAsked == 0);
    CHECK(agent.submit(MaiSendPrompt{session, "A second turn must preserve the name too"}));
    agent.waitIdle();
    CHECK(agent.getSession(session, stored));
    CHECK(stored.title == kMaiDefaultSessionTitle);
}

void test_sqlite_v5_migration_explicit_name_reopen_and_clear() {
    MaiRenameTestDirectory temp;
    const auto path = temp.path.append(MaiFilePath::fromUtf8("sessions.db"));
    const std::string id = MaiIdGenerator::newSessionId();
    {
        auto opened = makeMaiSqliteStore(path.toUtf8());
        CHECK(opened.isOk());
        if (!opened) return;
        MaiSession legacy;
        legacy.id = id;
        legacy.title = kMaiDefaultSessionTitle;
        legacy.directory = "/tmp/legacy";
        legacy.model = "old-model";
        legacy.created = 123;
        legacy.updated = 456;
        opened.value()->putSession(legacy);
    }
    // 构造完整且自洽的 V5 旧库：移除 V6 列并保留真实的会话/消息/专用模型表。
    sqlite3* database = nullptr;
    CHECK(sqlite3_open(path.toUtf8().c_str(), &database) == SQLITE_OK);
    if (!database) return;
    CHECK(sqlite3_exec(database,
                       "ALTER TABLE sessions DROP COLUMN title_edited;PRAGMA user_version=5;",
                       nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(database);
    {
        auto opened = makeMaiSqliteStore(path.toUtf8());
        CHECK(opened.isOk());
        if (!opened) return;
        MaiSession migrated;
        CHECK(opened.value()->getSession(id, migrated));
        CHECK(!migrated.titleEdited && migrated.isUntitled());
        CHECK(migrated.directory == "/tmp/legacy" && migrated.model == "old-model");
        CHECK(migrated.created == 123 && migrated.updated == 456);
        MaiAgent agent(std::move(opened.value()), nullptr, nullptr);
        CHECK(agent.submit(MaiUpdateSession{id, kMaiDefaultSessionTitle, "", "", ""}));
        CHECK(agent.getSession(id, migrated) && migrated.titleEdited && !migrated.isUntitled());
    }
    {
        auto opened = makeMaiSqliteStore(path.toUtf8());
        CHECK(opened.isOk());
        if (!opened) return;
        MaiFakeModelClient::Turn reply;
        reply.textChunks = {"Continued."};
        auto model = std::make_unique<MaiFakeModelClient>(
            std::vector<MaiFakeModelClient::Turn>{reply, reply});
        MaiAgent agent(std::move(opened.value()), std::move(model), nullptr);
        CHECK(agent.submit(MaiSendPrompt{id, "Do not replace my name after reopening"}));
        agent.waitIdle();
        MaiSession current;
        CHECK(agent.getSession(id, current));
        CHECK(current.title == kMaiDefaultSessionTitle && current.titleEdited);
        CHECK(agent.listSessions().front().titleEdited);
        // 空 title 仍是普通更新的“不改”，不会解除显式命名或破坏其它更新。
        CHECK(agent.submit(MaiUpdateSession{id, "", "next-model", "", ""}));
        CHECK(agent.getSession(id, current) && current.titleEdited &&
              current.model == "next-model");
        CHECK(agent.submit(MaiClearMessages{id}));
        CHECK(agent.getSession(id, current) && current.title.empty() && !current.titleEdited);
        CHECK(agent.submit(MaiSendPrompt{id, "Fresh automatic title"}));
        agent.waitIdle();
        CHECK(agent.getSession(id, current) && current.title == "Fresh automatic title" &&
              !current.titleEdited);
    }
    auto reopened = makeMaiSqliteStore(path.toUtf8());
    CHECK(reopened.isOk());
    if (reopened) {
        MaiSession stored;
        CHECK(reopened.value()->getSession(id, stored));
        CHECK(stored.title == "Fresh automatic title" && !stored.titleEdited);
    }
}

}  // namespace

int main() {
    test_current_only_validation_and_noop();
    test_running_rename_keeps_explicit_placeholder_title();
    test_sqlite_v5_migration_explicit_name_reopen_and_clear();
    if (failures == 0) std::puts("session rename tests passed");
    return failures == 0 ? 0 : 1;
}
