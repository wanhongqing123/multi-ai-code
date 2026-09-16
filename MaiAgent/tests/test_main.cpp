#include <cassert>
#include <cstdio>
#include <string>
#include <set>

#include "mai/agent/id.h"
#include "mai/agent/thread.h"

using namespace mai::agent;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
      ++failures;                                                       \
    }                                                                   \
  } while (0)

static void test_id_prefix_and_monotonic() {
  CHECK(id::session().rfind("ses_", 0) == 0);
  CHECK(id::message().rfind("msg_", 0) == 0);
  CHECK(id::part().rfind("prt_", 0) == 0);
  CHECK(id::event().rfind("evt_", 0) == 0);

  // 同一毫秒内连发，必须仍然严格递增且不重复——消息顺序依赖它。
  std::string prev;
  std::set<std::string> seen;
  for (int i = 0; i < 2000; ++i) {
    const std::string cur = id::message();
    CHECK(seen.insert(cur).second);
    if (!prev.empty()) CHECK(cur > prev);
    prev = cur;
  }
}

static void test_session_crud_and_events() {
  Agent agent(make_memory_store());

  int created = 0, updated = 0, deleted = 0;
  agent.events().subscribe([&](const Event& e) {
    if (e.type == EventType::SessionCreated) ++created;
    if (e.type == EventType::SessionUpdated) ++updated;
    if (e.type == EventType::SessionDeleted) ++deleted;
  });

  const std::string a = agent.submit(OpCreateSession{"/tmp/a", "会话 A", "glm-5.3"});
  CHECK(!a.empty());
  CHECK(created == 1);

  Session s;
  CHECK(agent.session(a, s));
  CHECK(s.title == "会话 A");
  CHECK(s.model == "glm-5.3");
  CHECK(s.agent == "build");

  agent.submit(OpUpdateSession{a, "改了标题", "", ""});
  CHECK(updated == 1);
  CHECK(agent.session(a, s));
  CHECK(s.title == "改了标题");

  const std::string b = agent.submit(OpCreateSession{"", "", ""});
  CHECK(agent.session(b, s));
  CHECK(s.title == "新会话");

  // 列表按 updated 倒序
  const auto list = agent.sessions();
  CHECK(list.size() == 2);
  CHECK(list[0].updated >= list[1].updated);

  agent.submit(OpDeleteSession{a});
  CHECK(deleted == 1);
  CHECK(!agent.session(a, s));
  CHECK(agent.sessions().size() == 1);

  // 删不存在的不该发事件
  agent.submit(OpDeleteSession{"ses_nope"});
  CHECK(deleted == 1);
}

static void test_unsubscribe() {
  Agent agent(make_memory_store());
  int n = 0;
  const auto tok = agent.events().subscribe([&](const Event&) { ++n; });
  agent.submit(OpCreateSession{"", "x", ""});
  CHECK(n == 1);
  agent.events().unsubscribe(tok);
  agent.submit(OpCreateSession{"", "y", ""});
  CHECK(n == 1);
  CHECK(agent.events().subscriber_count() == 0);
}

int main() {
  test_id_prefix_and_monotonic();
  test_session_crud_and_events();
  test_unsubscribe();
  if (failures == 0) std::printf("all tests passed\n");
  return failures == 0 ? 0 : 1;
}
