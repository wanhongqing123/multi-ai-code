// agent loop 的测试：从 submit(Prompt) 到事件流吐完的完整一圈。
//
// 用假的 Chat Completions 服务端当模型，所以不需要 API key、不花钱、
// 结果完全确定。真实 GLM 说的是同一个协议，切过去只改 base_url。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <json.hpp>

#include "mai/agent.h"

using namespace mai;
using nlohmann::json;

static int failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                 \
    }                                                             \
  } while (0)

namespace {

// ── 假模型：一个真的 Chat Completions 端点 ─────────────────────
struct FakeModel {
  httplib::Server srv;
  std::thread th;
  int port = 0;

  // 每个 content 片段之间的停顿，用来模拟"慢慢吐字"，好测中断。
  int delay_ms = 0;
  std::vector<std::string> chunks{"你好", "，我是", "测试模型"};
  std::string reasoning;

  // 收到的最后一个请求体，用来断言我们发出去的东西对不对。
  std::mutex mu;
  std::string last_body;

  void start() {
    srv.Post("/chat/completions", [this](const httplib::Request& req, httplib::Response& res) {
      {
        std::lock_guard<std::mutex> lock(mu);
        last_body = req.body;
      }
      auto idx = std::make_shared<std::size_t>(0);
      auto sent_reasoning = std::make_shared<bool>(reasoning.empty());
      const auto chunk_list = chunks;
      const auto reasoning_text = reasoning;
      const int delay = delay_ms;

      res.set_chunked_content_provider(
          "text/event-stream",
          [idx, sent_reasoning, chunk_list, reasoning_text, delay](
              std::size_t, httplib::DataSink& sink) {
            if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));

            if (!*sent_reasoning) {
              *sent_reasoning = true;
              json d;
              d["reasoning_content"] = reasoning_text;
              json c;
              c["delta"] = std::move(d);
              json root;
              root["choices"] = json::array({c});
              const std::string f = "data: " + root.dump() + "\n\n";
              return sink.write(f.data(), f.size());
            }
            if (*idx < chunk_list.size()) {
              json d;
              d["content"] = chunk_list[*idx];
              json c;
              c["delta"] = std::move(d);
              json root;
              root["choices"] = json::array({c});
              ++*idx;
              const std::string f = "data: " + root.dump() + "\n\n";
              return sink.write(f.data(), f.size());
            }
            const std::string done = "data: [DONE]\n\n";
            const bool ok = sink.write(done.data(), done.size());
            sink.done();
            return ok;
          });
    });
    port = srv.bind_to_any_port("127.0.0.1");
    th = std::thread([this] { srv.listen_after_bind(); });
    for (int i = 0; i < 200 && !srv.is_running(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  ~FakeModel() {
    srv.stop();
    if (th.joinable()) th.join();
  }

  std::string base() const { return "http://127.0.0.1:" + std::to_string(port); }
  std::string body() {
    std::lock_guard<std::mutex> lock(mu);
    return last_body;
  }
};

// 把事件流录下来，方便断言顺序和内容。
struct Recorder {
  std::mutex mu;
  std::vector<Event> events;

  void attach(Agent& a) {
    a.events().subscribe([this](const Event& e) {
      std::lock_guard<std::mutex> lock(mu);
      events.push_back(e);
    });
  }

  std::vector<Event> snapshot() {
    std::lock_guard<std::mutex> lock(mu);
    return events;
  }

  std::size_t count(EventType t) {
    std::lock_guard<std::mutex> lock(mu);
    std::size_t n = 0;
    for (const auto& e : events)
      if (e.type == t) ++n;
    return n;
  }

  // 把某个 part 上的所有 delta 按顺序拼起来——
  // 这正是界面做的事，拼出来应该等于完整正文。
  std::string assemble(const std::string& field_owner_part) {
    std::lock_guard<std::mutex> lock(mu);
    std::string out;
    for (const auto& e : events)
      if (e.type == EventType::MessagePartDelta && e.part_id == field_owner_part)
        out += e.delta;
    return out;
  }

  std::string first_delta_part_id() {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& e : events)
      if (e.type == EventType::MessagePartDelta) return e.part_id;
    return {};
  }
};

std::unique_ptr<Agent> make_agent(const FakeModel& model) {
  ModelConfig cfg;
  cfg.base_url = model.base();
  cfg.api_key = "test";
  Agent::Options opts;
  opts.default_model = "glm-5.3";
  return std::make_unique<Agent>(make_memory_store(), make_model_client(cfg), nullptr, opts);
}

// ── 用例 ────────────────────────────────────────────────────────

void test_full_turn() {
  FakeModel model;
  model.start();
  auto agent = make_agent(model);
  Recorder rec;
  rec.attach(*agent);

  const std::string sid = agent->submit(CreateSession{"/tmp", "", ""}).value();
  const std::string msg_id = agent->submit(Prompt{sid, "你是谁？"}).value();
  CHECK(!msg_id.empty());
  CHECK(msg_id.rfind("msg_", 0) == 0);

  agent->wait_idle();

  // 1. 事件顺序：至少要有 delta，最后要有 idle
  CHECK(rec.count(EventType::MessagePartDelta) == 3);  // 三个 chunk
  CHECK(rec.count(EventType::SessionIdle) == 1);
  CHECK(rec.count(EventType::SessionError) == 0);

  // 2. part id 全程稳定 —— 换 id 会让界面重绘甚至闪屏
  const auto all = rec.snapshot();
  std::string part_id;
  for (const auto& e : all) {
    if (e.type != EventType::MessagePartDelta) continue;
    if (part_id.empty()) part_id = e.part_id;
    CHECK(e.part_id == part_id);
    CHECK(e.message_id == msg_id);
    CHECK(e.field == "text");
  }
  CHECK(part_id.rfind("prt_", 0) == 0);

  // 3. 界面把 delta 拼起来 == 完整正文
  CHECK(rec.assemble(part_id) == "你好，我是测试模型");

  // 4. 落库的内容一致，刷新后不会变样
  const auto msgs = agent->messages(sid);
  CHECK(msgs.size() == 2);  // user + assistant
  if (msgs.size() == 2) {
    CHECK(msgs[0].role == Role::User);
    CHECK(msgs[1].role == Role::Assistant);
    CHECK(msgs[1].completed > 0);
    CHECK(msgs[1].parts.size() == 1);
    if (msgs[1].parts.size() == 1) {
      CHECK(msgs[1].parts[0].id == part_id);  // 落库的 id 和事件里的一致
      const auto* t = std::get_if<TextPart>(&msgs[1].parts[0].body);
      CHECK(t != nullptr);
      if (t) CHECK(t->text == "你好，我是测试模型");
    }
  }

  // 5. 标题自动从第一句话来，不再是"新会话"
  Session s;
  CHECK(agent->session(sid, s));
  CHECK(s.title == "你是谁？");
}

void test_request_body_is_correct() {
  FakeModel model;
  model.start();
  auto agent = make_agent(model);

  const std::string sid = agent->submit(CreateSession{"/tmp", "", "glm-4.6"}).value();
  agent->submit(Prompt{sid, "第一句"});
  agent->wait_idle();

  const json b = json::parse(model.body(), nullptr, false);
  CHECK(!b.is_discarded());
  CHECK(b.value("model", "") == "glm-4.6");  // 会话上的 model 覆盖默认值
  CHECK(b.value("stream", false) == true);
  CHECK(b["messages"].size() == 1);
  CHECK(b["messages"][0]["role"] == "user");
  CHECK(b["messages"][0]["content"] == "第一句");
}

void test_multi_turn_history() {
  FakeModel model;
  model.start();
  auto agent = make_agent(model);

  const std::string sid = agent->submit(CreateSession{"/tmp", "", ""}).value();
  agent->submit(Prompt{sid, "第一句"});
  agent->wait_idle();
  agent->submit(Prompt{sid, "第二句"});
  agent->wait_idle();

  // 第二轮必须带上完整历史，否则模型没有上下文
  const json b = json::parse(model.body(), nullptr, false);
  CHECK(b["messages"].size() == 3);  // user + assistant + user
  if (b["messages"].size() == 3) {
    CHECK(b["messages"][0]["content"] == "第一句");
    CHECK(b["messages"][1]["role"] == "assistant");
    CHECK(b["messages"][1]["content"] == "你好，我是测试模型");
    CHECK(b["messages"][2]["content"] == "第二句");
  }
  CHECK(agent->messages(sid).size() == 4);
}

void test_reasoning_goes_to_its_own_part() {
  FakeModel model;
  model.reasoning = "让我想想";
  model.start();
  auto agent = make_agent(model);
  Recorder rec;
  rec.attach(*agent);

  const std::string sid = agent->submit(CreateSession{"/tmp", "", ""}).value();
  agent->submit(Prompt{sid, "算一下"});
  agent->wait_idle();

  // reasoning 和 text 必须是两个不同的 part，界面才能分开显示
  const auto all = rec.snapshot();
  std::vector<std::string> part_ids;
  for (const auto& e : all) {
    if (e.type != EventType::MessagePartDelta) continue;
    if (std::find(part_ids.begin(), part_ids.end(), e.part_id) == part_ids.end())
      part_ids.push_back(e.part_id);
  }
  CHECK(part_ids.size() == 2);

  const auto msgs = agent->messages(sid);
  CHECK(msgs.size() == 2);
  if (msgs.size() == 2) {
    CHECK(msgs[1].parts.size() == 2);
    if (msgs[1].parts.size() == 2) {
      CHECK(std::get_if<ReasoningPart>(&msgs[1].parts[0].body) != nullptr);
      CHECK(std::get_if<TextPart>(&msgs[1].parts[1].body) != nullptr);
    }
  }

  // reasoning 不该回灌给模型——它是草稿，会污染下一轮上下文
  agent->submit(Prompt{sid, "继续"});
  agent->wait_idle();
  const json b = json::parse(model.body(), nullptr, false);
  bool leaked = false;
  for (const auto& m : b["messages"])
    if (m.value("content", std::string{}).find("让我想想") != std::string::npos) leaked = true;
  CHECK(!leaked);
}

void test_interrupt() {
  FakeModel model;
  model.delay_ms = 150;  // 慢慢吐，好让我们插进去
  model.chunks = {"一", "二", "三", "四", "五", "六", "七", "八"};
  model.start();
  auto agent = make_agent(model);
  Recorder rec;
  rec.attach(*agent);

  const std::string sid = agent->submit(CreateSession{"/tmp", "", ""}).value();
  agent->submit(Prompt{sid, "数数"});

  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  CHECK(agent->busy(sid));
  CHECK(agent->submit(Interrupt{sid}).ok());

  agent->wait_idle();
  CHECK(!agent->busy(sid));

  // 中断不算错误，而且已经吐出来的内容要保住
  CHECK(rec.count(EventType::SessionError) == 0);
  CHECK(rec.count(EventType::SessionIdle) == 1);
  const std::size_t got = rec.count(EventType::MessagePartDelta);
  CHECK(got > 0);
  CHECK(got < 8);  // 确实提前停了
  const auto msgs = agent->messages(sid);
  CHECK(msgs.size() == 2);
  if (msgs.size() == 2) CHECK(!msgs[1].parts.empty());  // 半截内容也落库
}

void test_busy_session_rejects_second_turn() {
  FakeModel model;
  model.delay_ms = 120;
  model.start();
  auto agent = make_agent(model);

  const std::string sid = agent->submit(CreateSession{"/tmp", "", ""}).value();
  CHECK(agent->submit(Prompt{sid, "第一句"}).ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  // 不排队而是拒绝：排队会让用户以为消息丢了，界面上看不出区别
  CHECK(!agent->submit(Prompt{sid, "第二句"}).ok());
  agent->wait_idle();
  CHECK(agent->messages(sid).size() == 2);
}

void test_unknown_session() {
  FakeModel model;
  model.start();
  auto agent = make_agent(model);
  CHECK(!agent->submit(Prompt{"ses_nope", "hi"}).ok());
  CHECK(!agent->submit(Interrupt{"ses_nope"}).ok());
}

void test_no_llm_configured() {
  // M1 的空转服务端就是这个配置：没有模型客户端，但不能崩
  Agent agent(make_memory_store(), nullptr);
  Recorder rec;
  rec.attach(agent);
  const std::string sid = agent.submit(CreateSession{"/tmp", "", ""}).value();
  CHECK(agent.submit(Prompt{sid, "hi"}).ok());
  agent.wait_idle();
  CHECK(rec.count(EventType::SessionError) == 1);
  CHECK(rec.count(EventType::SessionIdle) == 1);
}

void test_concurrent_sessions() {
  FakeModel model;
  model.delay_ms = 40;
  model.start();
  auto agent = make_agent(model);

  // 多个会话必须能同时跑——一个卡住不能拖累其它的
  std::vector<std::string> sids;
  for (int i = 0; i < 4; ++i)
    sids.push_back(agent->submit(CreateSession{"/tmp", "", ""}).value());
  for (const auto& sid : sids) CHECK(agent->submit(Prompt{sid, "并发测试"}).ok());
  agent->wait_idle();
  for (const auto& sid : sids) {
    const auto msgs = agent->messages(sid);
    CHECK(msgs.size() == 2);
    if (msgs.size() == 2 && !msgs[1].parts.empty()) {
      const auto* t = std::get_if<TextPart>(&msgs[1].parts[0].body);
      CHECK(t && t->text == "你好，我是测试模型");
    }
  }
}

}  // namespace

int main() {
  test_full_turn();
  test_request_body_is_correct();
  test_multi_turn_history();
  test_reasoning_goes_to_its_own_part();
  test_interrupt();
  test_busy_session_rejects_second_turn();
  test_unknown_session();
  test_no_llm_configured();
  test_concurrent_sessions();
  if (failures == 0) std::printf("loop tests passed\n");
  return failures == 0 ? 0 : 1;
}
