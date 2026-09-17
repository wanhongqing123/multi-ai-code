#include <cassert>
#include <cstdio>
#include <string>
#include <set>

#include "MaiIdGenerator.h"
#include "MaiAgent.h"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

static void test_id_prefix_and_monotonic() {
    CHECK(MaiIdGenerator::newSessionId().rfind("ses_", 0) == 0);
    CHECK(MaiIdGenerator::newMessageId().rfind("msg_", 0) == 0);
    CHECK(MaiIdGenerator::newPartId().rfind("prt_", 0) == 0);
    CHECK(MaiIdGenerator::newEventId().rfind("evt_", 0) == 0);

    // 同一毫秒内连发，必须仍然严格递增且不重复——消息顺序依赖它。
    std::string prev;
    std::set<std::string> seen;
    for (int i = 0; i < 2000; ++i) {
        const std::string cur = MaiIdGenerator::newMessageId();
        CHECK(seen.insert(cur).second);
        if (!prev.empty()) CHECK(cur > prev);
        prev = cur;
    }
}

static void test_session_crud_and_events() {
    MaiAgent agent(makeMaiMemoryStore(), nullptr, nullptr);

    int created = 0, updated = 0, deleted = 0;
    agent.eventBus().subscribe([&](const MaiEvent& e) {
        if (e.type == MaiEventType::SessionCreated) ++created;
        if (e.type == MaiEventType::SessionUpdated) ++updated;
        if (e.type == MaiEventType::SessionDeleted) ++deleted;
    });

    const std::string a = agent.submit(MaiCreateSession{"/tmp/a", "会话 A", "glm-5.3"}).value();
    CHECK(!a.empty());
    CHECK(created == 1);

    MaiSession s;
    CHECK(agent.getSession(a, s));
    CHECK(s.title == "会话 A");
    CHECK(s.model == "glm-5.3");
    CHECK(s.agent == "build");

    agent.submit(MaiUpdateSession{a, "改了标题", "", ""});
    CHECK(updated == 1);
    CHECK(agent.getSession(a, s));
    CHECK(s.title == "改了标题");

    const std::string b = agent.submit(MaiCreateSession{"", "", ""}).value();
    CHECK(agent.getSession(b, s));
    CHECK(s.title == "新会话");

    // 列表按 updated 倒序
    const auto list = agent.listSessions();
    CHECK(list.size() == 2);
    CHECK(list[0].updated >= list[1].updated);

    agent.submit(MaiDeleteSession{a});
    CHECK(deleted == 1);
    CHECK(!agent.getSession(a, s));
    CHECK(agent.listSessions().size() == 1);

    // 删不存在的不该发事件
    agent.submit(MaiDeleteSession{"ses_nope"});
    CHECK(deleted == 1);
}

static void test_unsubscribe() {
    MaiAgent agent(makeMaiMemoryStore(), nullptr, nullptr);
    int n = 0;
    const auto tok = agent.eventBus().subscribe([&](const MaiEvent&) { ++n; });
    agent.submit(MaiCreateSession{"", "x", ""});
    CHECK(n == 1);
    agent.eventBus().unsubscribe(tok);
    agent.submit(MaiCreateSession{"", "y", ""});
    CHECK(n == 1);
    CHECK(agent.eventBus().subscriberCount() == 0);
}

int main() {
    test_id_prefix_and_monotonic();
    test_session_crud_and_events();
    test_unsubscribe();
    if (failures == 0) std::printf("all tests passed\n");
    return failures == 0 ? 0 : 1;
}
