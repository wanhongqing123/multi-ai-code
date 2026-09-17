#include "http/server.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <httplib.h>
#include <json.hpp>

#include "mai/id.h"

namespace mai::http {
namespace {

using json = nlohmann::json;

const char* role_wire(Role r) { return r == Role::User ? "user" : "assistant"; }

// 核心的错误码 -> HTTP 状态码。映射只在这一处，核心本身不知道 HTTP。
int http_status(ErrorCode c) {
  switch (c) {
    case ErrorCode::Ok:            return 200;
    case ErrorCode::NotFound:      return 404;
    case ErrorCode::Busy:          return 409;
    case ErrorCode::InvalidInput:  return 400;
    case ErrorCode::NotConfigured: return 503;
    case ErrorCode::Network:
    case ErrorCode::Protocol:      return 502;
    case ErrorCode::Canceled:      return 200;
    case ErrorCode::Internal:      return 500;
  }
  return 500;
}

const char* tool_state_wire(ToolState s) {
  switch (s) {
    case ToolState::Pending:   return "pending";
    case ToolState::Running:   return "running";
    case ToolState::Completed: return "completed";
    case ToolState::Error:     return "error";
  }
  return "pending";
}

// ── 序列化：只在这里把原生结构体变成 JSON ─────────────────────
json to_json(const Session& s) {
  return json{
      {"id", s.id},
      {"title", s.title},
      {"directory", s.directory},
      {"model", s.model},
      {"agent", s.agent},
      {"time", {{"created", s.created}, {"updated", s.updated}}},
  };
}

json to_json(const Part& p) {
  json j{{"id", p.id}};
  std::visit(
      [&j](const auto& body) {
        using T = std::decay_t<decltype(body)>;
        if constexpr (std::is_same_v<T, TextPart>) {
          j["type"] = "text";
          j["text"] = body.text;
        } else if constexpr (std::is_same_v<T, ReasoningPart>) {
          j["type"] = "reasoning";
          j["text"] = body.text;
        } else if constexpr (std::is_same_v<T, ToolPart>) {
          j["type"] = "tool";
          j["tool"] = body.tool;
          j["callID"] = body.call_id;
          j["state"] = {{"status", tool_state_wire(body.state)},
                        {"input", body.input},
                        {"output", body.output},
                        {"error", body.error}};
        }
      },
      p.body);
  return j;
}

json to_json(const Message& m) {
  json parts = json::array();
  for (const auto& p : m.parts) parts.push_back(to_json(p));
  return json{
      {"id", m.id},
      {"role", role_wire(m.role)},
      {"parts", std::move(parts)},
      {"time", {{"created", m.created}, {"completed", m.completed}}},
  };
}

// 事件的线上形状照 openapi.json 里的 MessagePartDelta 等 schema：
// { id, type, data: { sessionID, messageID, partID, field, delta } }
std::string serialize_event(const Event& e) {
  json data{{"sessionID", e.session_id}};
  if (!e.message_id.empty()) data["messageID"] = e.message_id;
  if (!e.part_id.empty()) data["partID"] = e.part_id;
  if (!e.field.empty()) data["field"] = e.field;
  if (!e.delta.empty()) data["delta"] = e.delta;
  if (!e.detail.empty()) data["detail"] = e.detail;

  const json envelope{{"id", e.id}, {"type", to_wire(e.type)}, {"data", std::move(data)}};

  // SSE 帧：data: <json>\n\n
  std::string frame = "data: ";
  frame += envelope.dump();
  frame += "\n\n";
  return frame;
}

// ── SSE 连接 ────────────────────────────────────────────────────
// 每个连接一个队列。事件在适配器层**只序列化一次**，再把同一份字符串
// 分发给所有连接——N 个客户端时不做 N 次 dump()。
struct SseConn {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::string> queue;
  bool closed = false;

  void push(const std::string& frame) {
    {
      std::lock_guard<std::mutex> lock(mu);
      if (closed) return;
      // 背压：客户端卡住时不要无限堆积。丢最老的，UI 重连后会重新拉全量。
      if (queue.size() > 2048) queue.pop_front();
      queue.push_back(frame);
    }
    cv.notify_one();
  }

  void close() {
    {
      std::lock_guard<std::mutex> lock(mu);
      closed = true;
    }
    cv.notify_all();
  }
};

}  // namespace

struct Server::Impl {
  Agent& agent;
  ServerOptions opts;
  httplib::Server srv;
  std::atomic<int> bound_port{0};

  std::mutex conns_mu;
  std::unordered_map<std::uint64_t, std::shared_ptr<SseConn>> conns;
  std::atomic<std::uint64_t> next_conn{1};
  EventBus::Token bus_token = 0;

  explicit Impl(Agent& a, ServerOptions o) : agent(a), opts(std::move(o)) {}

  void broadcast(const Event& e) {
    const std::string frame = serialize_event(e);  // 只 dump 一次
    std::vector<std::shared_ptr<SseConn>> targets;
    {
      std::lock_guard<std::mutex> lock(conns_mu);
      targets.reserve(conns.size());
      for (const auto& [_, c] : conns) targets.push_back(c);
    }
    for (const auto& c : targets) c->push(frame);
  }

  void routes();
};

void Server::Impl::routes() {
  srv.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(json{{"status", "ok"}, {"service", "maiagent"}}.dump(),
                    "application/json");
  });

  // UI 有几处会读它。M1 先给个能让 UI 跑起来的最小骨架。
  srv.Get("/config", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(json{{"provider", json::object()}, {"model", ""}}.dump(),
                    "application/json");
  });

  srv.Get("/api/session", [this](const httplib::Request&, httplib::Response& res) {
    json arr = json::array();
    for (const auto& s : agent.sessions()) arr.push_back(to_json(s));
    res.set_content(arr.dump(), "application/json");
  });

  srv.Post("/api/session", [this](const httplib::Request& req, httplib::Response& res) {
    CreateSession op;
    if (!req.body.empty()) {
      // 客户端给的 JSON 可能缺字段甚至不是合法 JSON，不能让它把服务端搞崩。
      const json body = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
      if (body.is_object()) {
        op.directory = body.value("directory", std::string{});
        op.title = body.value("title", std::string{});
        op.model = body.value("model", std::string{});
      }
    }
    const auto created = agent.submit(op);
    if (!created) {
      res.status = 400;
      res.set_content(json{{"error", created.error().message},
                           {"code", to_string(created.error().code)}}.dump(),
                      "application/json");
      return;
    }
    Session s;
    agent.session(created.value(), s);
    res.set_content(to_json(s).dump(), "application/json");
  });

  srv.Get(R"(/api/session/([^/]+))",
          [this](const httplib::Request& req, httplib::Response& res) {
            Session s;
            if (!agent.session(req.matches[1], s)) {
              res.status = 404;
              res.set_content(json{{"error", "session not found"}}.dump(),
                              "application/json");
              return;
            }
            res.set_content(to_json(s).dump(), "application/json");
          });

  srv.Get(R"(/api/session/([^/]+)/message)",
          [this](const httplib::Request& req, httplib::Response& res) {
            json arr = json::array();
            for (const auto& m : agent.messages(req.matches[1])) arr.push_back(to_json(m));
            res.set_content(arr.dump(), "application/json");
          });

  // 发一轮消息。**立刻返回**——真正的输出全部走 /api/event 的事件流。
  // 这里如果同步等 agent 跑完，HTTP 请求会挂几十秒，界面就卡死了。
  srv.Post(R"(/api/session/([^/]+)/prompt)",
           [this](const httplib::Request& req, httplib::Response& res) {
             const std::string sid = req.matches[1];
             std::string text;
             const json body = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
             if (body.is_object()) {
               // UI 可能给 {"text": "..."}，也可能给 opencode 那种
               // {"parts":[{"type":"text","text":"..."}]}，两种都认。
               if (body.contains("text") && body["text"].is_string()) {
                 text = body["text"].get<std::string>();
               } else if (body.contains("parts") && body["parts"].is_array()) {
                 for (const auto& part : body["parts"]) {
                   if (part.is_object() && part.value("type", "") == "text") {
                     if (!text.empty()) text += "\n";
                     text += part.value("text", std::string{});
                   }
                 }
               }
             }
             const auto sent = agent.submit(Prompt{sid, text});
             if (!sent) {
               // 核心给的是结构化错误码，这里只做一次映射。
               // 之前核心只返回空字符串，适配器得靠 busy() 反猜是哪种失败——
               // 那是个竞态：猜的时候状态可能已经变了。
               res.status = http_status(sent.error().code);
               res.set_content(json{{"error", sent.error().message},
                                    {"code", to_string(sent.error().code)}}.dump(),
                               "application/json");
               return;
             }
             res.set_content(json{{"messageID", sent.value()}}.dump(), "application/json");
           });

  srv.Post(R"(/api/session/([^/]+)/interrupt)",
           [this](const httplib::Request& req, httplib::Response& res) {
             const std::string sid = req.matches[1];
             const auto r = agent.submit(Interrupt{sid});
             res.set_content(json{{"interrupted", r.ok()}}.dump(), "application/json");
           });

  // ── SSE 事件流 ────────────────────────────────────────────────
  srv.Get("/api/event", [this](const httplib::Request&, httplib::Response& res) {
    auto conn = std::make_shared<SseConn>();
    const std::uint64_t cid = next_conn.fetch_add(1);
    {
      std::lock_guard<std::mutex> lock(conns_mu);
      conns.emplace(cid, conn);
    }

    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    res.set_header("X-Accel-Buffering", "no");

    res.set_chunked_content_provider(
        "text/event-stream",
        [conn](std::size_t, httplib::DataSink& sink) {
          std::unique_lock<std::mutex> lock(conn->mu);
          // 15 秒没事件就发一个注释行当心跳，防止中间的代理把连接掐了。
          const bool got = conn->cv.wait_for(lock, std::chrono::seconds(15), [&] {
            return conn->closed || !conn->queue.empty();
          });
          if (conn->closed) {
            sink.done();
            return false;
          }
          if (!got) {
            lock.unlock();
            return sink.write(":\n\n", 3);
          }
          std::string batch;
          while (!conn->queue.empty()) {
            batch += conn->queue.front();
            conn->queue.pop_front();
          }
          lock.unlock();
          return sink.write(batch.data(), batch.size());
        },
        [this, cid, conn](bool) {
          conn->close();
          std::lock_guard<std::mutex> lock(conns_mu);
          conns.erase(cid);
        });
  });
}

Server::Server(Agent& agent, ServerOptions opts)
    : impl_(std::make_unique<Impl>(agent, std::move(opts))) {
  impl_->routes();
  impl_->bus_token =
      impl_->agent.events().subscribe([this](const Event& e) { impl_->broadcast(e); });
}

Server::~Server() {
  if (impl_->bus_token) impl_->agent.events().unsubscribe(impl_->bus_token);
  stop();
}

bool Server::bind() {
  // port=0 走 bind_to_any_port 让系统挑；指定端口就直接绑。
  const int p = impl_->opts.port > 0
                    ? (impl_->srv.bind_to_port(impl_->opts.host.c_str(), impl_->opts.port)
                           ? impl_->opts.port
                           : 0)
                    : impl_->srv.bind_to_any_port(impl_->opts.host.c_str());
  if (p <= 0) return false;
  impl_->bound_port.store(p);
  return true;
}

bool Server::serve() { return impl_->srv.listen_after_bind(); }

void Server::stop() {
  {
    std::lock_guard<std::mutex> lock(impl_->conns_mu);
    for (auto& [_, c] : impl_->conns) c->close();
  }
  impl_->srv.stop();
}

int Server::port() const { return impl_->bound_port.load(); }

std::string Server::base_url() const {
  return "http://" + impl_->opts.host + ":" + std::to_string(port());
}

}  // namespace mai::http
