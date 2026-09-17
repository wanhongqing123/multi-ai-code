#include "mai/agent/thread.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "mai/agent/id.h"

namespace mai::agent {
namespace {

// 一轮对话的运行状态。每个会话最多一个。
struct Turn {
  std::thread worker;
  std::atomic<bool> cancel{false};
  std::atomic<bool> running{true};
};

}  // namespace

struct Agent::Impl {
  std::unique_ptr<Store> store;
  std::unique_ptr<LlmClient> llm;
  std::string default_model;
  EventBus bus;

  mutable std::mutex turns_mu;
  std::condition_variable turns_cv;
  std::unordered_map<std::string, std::shared_ptr<Turn>> turns;

  ~Impl() {
    // 析构时把所有在跑的轮次叫停并等它们退出，否则线程会访问已销毁的成员。
    std::vector<std::shared_ptr<Turn>> pending;
    {
      std::lock_guard<std::mutex> lock(turns_mu);
      for (auto& [_, t] : turns) {
        t->cancel.store(true, std::memory_order_relaxed);
        pending.push_back(t);
      }
    }
    for (auto& t : pending) {
      if (t->worker.joinable()) t->worker.join();
    }
  }

  void emit(EventType type, const std::string& session_id,
            const std::string& detail = {}) {
    Event e;
    e.id = id::event();
    e.type = type;
    e.session_id = session_id;
    e.detail = detail;
    e.time = now_ms();
    bus.publish(e);
  }

  // part 级别的增量。**只带这次新增的内容**，不重推全量——
  // 流式期间每秒几十条，全量重推是纯烧 CPU。
  void emit_delta(const std::string& session_id, const std::string& message_id,
                  const std::string& part_id, const char* field,
                  std::string_view delta) {
    Event e;
    e.id = id::event();
    e.type = EventType::MessagePartDelta;
    e.session_id = session_id;
    e.message_id = message_id;
    e.part_id = part_id;
    e.field = field;
    e.delta.assign(delta);
    e.time = now_ms();
    bus.publish(e);
  }

  void emit_part_updated(const std::string& session_id, const std::string& message_id,
                         const std::string& part_id) {
    Event e;
    e.id = id::event();
    e.type = EventType::MessagePartUpdated;
    e.session_id = session_id;
    e.message_id = message_id;
    e.part_id = part_id;
    e.time = now_ms();
    bus.publish(e);
  }

  void emit_message_updated(const std::string& session_id, const std::string& message_id) {
    Event e;
    e.id = id::event();
    e.type = EventType::MessageUpdated;
    e.session_id = session_id;
    e.message_id = message_id;
    e.time = now_ms();
    bus.publish(e);
  }

  // 把历史消息摊平成给大模型的 messages 数组。
  //
  // 这里是 O(n) 重建。当前实现先求正确；上下文长了之后要改成
  // 保留已序列化的前缀、每轮只追加增量，否则整体是 O(n²)。
  // 这是计划里标出来的第一号性能风险。
  std::vector<ChatMessage> build_history(const std::string& session_id) const {
    std::vector<ChatMessage> out;
    for (const auto& m : store->list_messages(session_id)) {
      ChatMessage cm;
      cm.role = (m.role == Role::User) ? "user" : "assistant";
      for (const auto& p : m.parts) {
        if (const auto* t = std::get_if<TextPart>(&p.body)) {
          if (!cm.content.empty()) cm.content += "\n";
          cm.content += t->text;
        }
        // reasoning 不回灌：它是模型的草稿，回灌会污染下一轮的上下文。
        // tool part 等 M3 做工具时再处理。
      }
      if (cm.content.empty()) continue;  // 空消息不发给模型
      out.push_back(std::move(cm));
    }
    return out;
  }

  void run_turn(const std::string& session_id, std::shared_ptr<Turn> turn,
                Message assistant);
  void finish_turn(const std::string& session_id, std::shared_ptr<Turn> turn);
};

void Agent::Impl::finish_turn(const std::string& session_id, std::shared_ptr<Turn> turn) {
  turn->running.store(false, std::memory_order_relaxed);
  emit(EventType::SessionIdle, session_id);
  {
    std::lock_guard<std::mutex> lock(turns_mu);
    auto it = turns.find(session_id);
    // 只有还是自己这一轮时才移除：可能已经有新的一轮顶上来了。
    if (it != turns.end() && it->second == turn) {
      // worker 线程正在执行 detach 后的收尾，不能在这里 join 自己。
      it->second->worker.detach();
      turns.erase(it);
    }
  }
  turns_cv.notify_all();
}

void Agent::Impl::run_turn(const std::string& session_id, std::shared_ptr<Turn> turn,
                           Message assistant) {
  if (!llm) {
    emit(EventType::SessionError, session_id, "没有配置模型客户端");
    finish_turn(session_id, turn);
    return;
  }

  Session s;
  if (!store->get_session(session_id, s)) {
    finish_turn(session_id, turn);
    return;
  }

  ChatRequest req;
  req.model = s.model.empty() ? default_model : s.model;
  req.messages = build_history(session_id);

  // part id 必须在第一次创建时就定下来，之后所有 delta 都引用它。
  // 中途换 id 会让界面重绘甚至闪屏——这是计划 09 节第 2 条。
  const std::string text_part_id = id::part();
  const std::string reasoning_part_id = id::part();

  // 累积的正文。落库只在轮次结束时做一次——
  // 每个 delta 落一次盘等于每秒几十次 fsync。
  auto text = std::make_shared<std::string>();
  auto reasoning = std::make_shared<std::string>();
  auto text_started = std::make_shared<bool>(false);
  auto reasoning_started = std::make_shared<bool>(false);
  auto error = std::make_shared<std::string>();

  StreamHandler h;
  h.on_text = [&](std::string_view chunk) {
    if (!*text_started) {
      *text_started = true;
      emit_part_updated(session_id, assistant.id, text_part_id);
    }
    text->append(chunk);
    emit_delta(session_id, assistant.id, text_part_id, "text", chunk);
  };
  h.on_reasoning = [&](std::string_view chunk) {
    if (!*reasoning_started) {
      *reasoning_started = true;
      emit_part_updated(session_id, assistant.id, reasoning_part_id);
    }
    reasoning->append(chunk);
    emit_delta(session_id, assistant.id, reasoning_part_id, "text", chunk);
  };
  h.on_error = [&](const std::string& msg) { *error = msg; };
  // 工具调用留到 M3。现在收到也只是忽略，不会假装执行。

  llm->stream(req, h, turn->cancel);

  // 收尾：把累积的内容写成 part 落库。
  if (!reasoning->empty()) {
    Part p;
    p.id = reasoning_part_id;
    p.body = ReasoningPart{*reasoning};
    p.created = now_ms();
    assistant.parts.push_back(std::move(p));
  }
  if (!text->empty()) {
    Part p;
    p.id = text_part_id;
    p.body = TextPart{*text};
    p.created = now_ms();
    assistant.parts.push_back(std::move(p));
  }
  assistant.completed = now_ms();
  store->put_message(session_id, assistant);
  emit_message_updated(session_id, assistant.id);

  // 会话标题：第一轮结束时用用户那句话的开头当标题，
  // 否则列表里全是"新会话"，分不出谁是谁。
  if (s.title.empty() || s.title == "新会话") {
    const auto msgs = store->list_messages(session_id);
    for (const auto& m : msgs) {
      if (m.role != Role::User) continue;
      if (const auto* t = std::get_if<TextPart>(&m.parts.front().body)) {
        std::string title = t->text;
        // 截断按字节找边界会切坏 UTF-8，往前退到字符起始处。
        if (title.size() > 40) {
          std::size_t cut = 40;
          while (cut > 0 && (static_cast<unsigned char>(title[cut]) & 0xC0) == 0x80) --cut;
          title.resize(cut);
          title += "…";
        }
        s.title = title;
        s.updated = now_ms();
        store->put_session(s);
        emit(EventType::SessionUpdated, session_id, s.title);
      }
      break;
    }
  }

  if (!error->empty()) emit(EventType::SessionError, session_id, *error);
  finish_turn(session_id, turn);
}

Agent::Agent(std::unique_ptr<Store> store, std::unique_ptr<LlmClient> llm,
             std::string default_model)
    : impl_(std::make_unique<Impl>()) {
  impl_->store = std::move(store);
  impl_->llm = std::move(llm);
  impl_->default_model = std::move(default_model);
}

Agent::Agent(std::unique_ptr<Store> store) : Agent(std::move(store), nullptr, {}) {}

Agent::~Agent() = default;

std::vector<Session> Agent::sessions() const { return impl_->store->list_sessions(); }

bool Agent::session(const std::string& id, Session& out) const {
  return impl_->store->get_session(id, out);
}

std::vector<Message> Agent::messages(const std::string& session_id) const {
  return impl_->store->list_messages(session_id);
}

bool Agent::busy(const std::string& session_id) const {
  std::lock_guard<std::mutex> lock(impl_->turns_mu);
  return impl_->turns.count(session_id) > 0;
}

void Agent::wait_idle() {
  std::unique_lock<std::mutex> lock(impl_->turns_mu);
  impl_->turns_cv.wait(lock, [this] { return impl_->turns.empty(); });
}

EventBus& Agent::events() { return impl_->bus; }
const EventBus& Agent::events() const { return impl_->bus; }

std::string Agent::submit(const Op& op) {
  return std::visit(
      [this](const auto& o) -> std::string {
        using T = std::decay_t<decltype(o)>;

        if constexpr (std::is_same_v<T, OpCreateSession>) {
          Session s;
          s.id = id::session();
          s.directory = o.directory;
          s.title = o.title.empty() ? "新会话" : o.title;
          s.model = o.model;
          s.created = now_ms();
          s.updated = s.created;
          impl_->store->put_session(s);
          impl_->emit(EventType::SessionCreated, s.id, s.title);
          return s.id;

        } else if constexpr (std::is_same_v<T, OpUpdateSession>) {
          Session s;
          if (!impl_->store->get_session(o.session_id, s)) return {};
          if (!o.title.empty()) s.title = o.title;
          if (!o.model.empty()) s.model = o.model;
          if (!o.agent.empty()) s.agent = o.agent;
          s.updated = now_ms();
          impl_->store->put_session(s);
          impl_->emit(EventType::SessionUpdated, s.id, s.title);
          return s.id;

        } else if constexpr (std::is_same_v<T, OpDeleteSession>) {
          if (!impl_->store->remove_session(o.session_id)) return {};
          impl_->emit(EventType::SessionDeleted, o.session_id);
          return o.session_id;

        } else if constexpr (std::is_same_v<T, OpTurnInput>) {
          Session s;
          if (!impl_->store->get_session(o.session_id, s)) return {};

          // 同一会话已经在跑就拒绝，不排队。
          // 排队会让用户以为消息丢了——界面上看不出区别。
          {
            std::lock_guard<std::mutex> lock(impl_->turns_mu);
            if (impl_->turns.count(o.session_id)) return {};
          }

          // 用户消息先落库并广播，界面立刻看到自己发的话。
          Message user;
          user.id = id::message();
          user.role = Role::User;
          user.created = now_ms();
          user.completed = user.created;
          Part up;
          up.id = id::part();
          up.body = TextPart{o.text};
          up.created = user.created;
          user.parts.push_back(std::move(up));
          impl_->store->put_message(o.session_id, user);
          impl_->emit_message_updated(o.session_id, user.id);

          // assistant 消息此刻就建好，后续 delta 都挂在它下面。
          Message assistant;
          assistant.id = id::message();
          assistant.role = Role::Assistant;
          assistant.created = now_ms();
          impl_->store->put_message(o.session_id, assistant);
          impl_->emit_message_updated(o.session_id, assistant.id);

          s.updated = now_ms();
          impl_->store->put_session(s);

          auto turn = std::make_shared<Turn>();
          {
            std::lock_guard<std::mutex> lock(impl_->turns_mu);
            impl_->turns[o.session_id] = turn;
          }
          // 单独线程跑，submit 立刻返回——否则 HTTP 请求会挂在这儿几十秒。
          const std::string sid = o.session_id;
          turn->worker = std::thread(
              [this, sid, turn, assistant] { impl_->run_turn(sid, turn, assistant); });
          return assistant.id;

        } else if constexpr (std::is_same_v<T, OpInterrupt>) {
          std::lock_guard<std::mutex> lock(impl_->turns_mu);
          auto it = impl_->turns.find(o.session_id);
          if (it == impl_->turns.end()) return {};
          it->second->cancel.store(true, std::memory_order_relaxed);
          return o.session_id;

        } else {
          return {};
        }
      },
      op);
}

}  // namespace mai::agent
