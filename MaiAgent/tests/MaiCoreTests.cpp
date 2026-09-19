#include <cassert>
#include <cstdio>
#include <string>
#include <thread>
#include <set>

#include "MaiBlockingCheck.h"
#include "MaiIdGenerator.h"
#include "MaiThread.h"
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

    const std::string a = agent.submit(MaiCreateSession{"/tmp/a", "Session A", "glm-5.3"}).value();
    CHECK(!a.empty());
    CHECK(created == 1);

    MaiSession s;
    CHECK(agent.getSession(a, s));
    CHECK(s.title == "Session A");
    CHECK(s.model == "glm-5.3");
    CHECK(s.agent == "build");

    agent.submit(MaiUpdateSession{a, "Renamed", "", ""});
    CHECK(updated == 1);
    CHECK(agent.getSession(a, s));
    CHECK(s.title == "Renamed");

    const std::string b = agent.submit(MaiCreateSession{"", "", ""}).value();
    CHECK(agent.getSession(b, s));
    CHECK(s.title == kMaiDefaultSessionTitle);

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

// 事件处理函数里不许做慢活——而且这条要真的被装上，不能只是注释。
//
// 不能直接测"违反了会炸"：那会终止进程，测试框架接不住。所以退一步，测**守卫确实在生效**：
// 处理函数里问一句"现在允许慢活吗"，答案必须是否。
// 这就足以说明 MaiFileSystem 里那几个断言会在这条路径上被触发。
static void test_event_handlers_run_with_blocking_disallowed() {
    MaiEventBus bus;

    bool allowedInsideHandler = true;
    bus.subscribe([&](const MaiEvent&) { allowedInsideHandler = maiIsBlockingAllowed(); });

    // 发之前是允许的：工作线程本来就要读文件、写库
    CHECK(maiIsBlockingAllowed());

    MaiEvent event;
    event.type = MaiEventType::SessionIdle;
    bus.publish(event);

    CHECK(!allowedInsideHandler);
    // 发完要恢复，不能把整个线程一直摁住
    CHECK(maiIsBlockingAllowed());
}

static void test_blocking_scopes_nest() {
    CHECK(maiIsBlockingAllowed());
    {
        MaiScopedDisallowBlocking outer;
        CHECK(!maiIsBlockingAllowed());
        {
            // 明确开一个口子
            MaiScopedAllowBlocking inner;
            CHECK(maiIsBlockingAllowed());
        }
        // 内层退出时要恢复成**进来之前**的状态，不是无条件放开——无条件放开的话，
        // 一次嵌套就把外层的限制悄悄拆了。
        CHECK(!maiIsBlockingAllowed());
    }
    CHECK(maiIsBlockingAllowed());
}

static void test_thread_name() {
    MaiThread::setCurrentName("mai-test");
    CHECK(MaiThread::currentName() == "mai-test");

    // 别的线程互不影响（thread_local）
    std::string fromOtherThread = "not set";
    std::thread worker([&] {
        fromOtherThread = MaiThread::currentName();
        MaiThread::setCurrentName("mai-other");
    });
    worker.join();
    CHECK(fromOtherThread.empty());
    CHECK(MaiThread::currentName() == "mai-test");
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
    test_event_handlers_run_with_blocking_disallowed();
    test_blocking_scopes_nest();
    test_thread_name();
    test_unsubscribe();
    if (failures == 0) std::printf("all tests passed\n");
    return failures == 0 ? 0 : 1;
}
