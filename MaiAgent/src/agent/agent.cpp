#include "mai/agent.h"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "mai/id.h"

#include "agent/context_builder.h"
#include "agent/event_emitter.h"
#include "agent/turn_runner.h"

namespace mai {
namespace {

// 一轮对话的在跑状态。TurnRunner 负责跑，这个只负责"还在不在跑"和"叫停"。
struct ActiveTurn {
  std::thread worker;
  std::atomic<bool> cancel{false};
};

}  // namespace

struct Agent::Impl {
  std::unique_ptr<SessionStore> store;
  std::unique_ptr<ModelClient> model;
  Options options;

  EventBus bus;
  internal::EventEmitter emitter{bus};
  internal::ContextBuilder context;

  mutable std::mutex mu;
  std::condition_variable cv;
  std::unordered_map<std::string, std::shared_ptr<ActiveTurn>> active;

  ~Impl() {
    // 析构时把所有在跑的轮次叫停并等它们退出，
    // 否则工作线程会访问已经销毁的 store/emitter。
    std::vector<std::shared_ptr<ActiveTurn>> pending;
    {
      std::lock_guard<std::mutex> lock(mu);
      for (auto& [_, t] : active) {
        t->cancel.store(true, std::memory_order_relaxed);
        pending.push_back(t);
      }
    }
    for (auto& t : pending) {
      if (t->worker.joinable()) t->worker.join();
    }
  }

  internal::TurnRunner::Deps deps() {
    internal::TurnRunner::Deps d;
    d.store = store.get();
    d.model = model.get();
    d.emitter = &emitter;
    d.context = &context;
    d.default_model = options.default_model;
    return d;
  }

  void retire(const std::string& session_id, const std::shared_ptr<ActiveTurn>& turn) {
    emitter.session(EventType::SessionIdle, session_id);
    {
      std::lock_guard<std::mutex> lock(mu);
      auto it = active.find(session_id);
      // 只有还是自己这一轮时才移除：可能已经有新的一轮顶上来了。
      if (it != active.end() && it->second == turn) {
        // 正在执行的就是这个线程，不能 join 自己。
        it->second->worker.detach();
        active.erase(it);
      }
    }
    cv.notify_all();
  }
};

Agent::Agent(std::unique_ptr<SessionStore> store, std::unique_ptr<ModelClient> model,
             Options options)
    : impl_(std::make_unique<Impl>()) {
  impl_->store = std::move(store);
  impl_->model = std::move(model);
  impl_->options = std::move(options);
}

Agent::~Agent() = default;

std::vector<Session> Agent::sessions() const { return impl_->store->list_sessions(); }

bool Agent::session(const std::string& id, Session& out) const {
  return impl_->store->get_session(id, out);
}

std::vector<Message> Agent::messages(const std::string& session_id) const {
  return impl_->store->list_messages(session_id);
}

bool Agent::busy(const std::string& session_id) const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->active.count(session_id) > 0;
}

void Agent::wait_idle() {
  std::unique_lock<std::mutex> lock(impl_->mu);
  impl_->cv.wait(lock, [this] { return impl_->active.empty(); });
}

EventBus& Agent::events() { return impl_->bus; }
const EventBus& Agent::events() const { return impl_->bus; }

Result<std::string> Agent::submit(const Op& op) {
  return std::visit(
      [this](const auto& o) -> Result<std::string> {
        using T = std::decay_t<decltype(o)>;

        if constexpr (std::is_same_v<T, CreateSession>) {
          Session s;
          s.id = id::session();
          s.directory = o.directory;
          s.title = o.title.empty() ? "新会话" : o.title;
          s.model = o.model;
          s.created = now_ms();
          s.updated = s.created;
          impl_->store->put_session(s);
          impl_->emitter.session(EventType::SessionCreated, s.id, s.title);
          return s.id;

        } else if constexpr (std::is_same_v<T, UpdateSession>) {
          std::string title;
          const bool found = impl_->store->mutate_session(o.session_id, [&](Session& s) {
            if (!o.title.empty()) s.title = o.title;
            if (!o.model.empty()) s.model = o.model;
            if (!o.agent.empty()) s.agent = o.agent;
            s.updated = now_ms();
            title = s.title;
          });
          if (!found) return {ErrorCode::NotFound, "会话不存在"};
          impl_->emitter.session(EventType::SessionUpdated, o.session_id, title);
          return o.session_id;

        } else if constexpr (std::is_same_v<T, DeleteSession>) {
          if (!impl_->store->remove_session(o.session_id))
            return {ErrorCode::NotFound, "会话不存在"};
          impl_->emitter.session(EventType::SessionDeleted, o.session_id);
          return o.session_id;

        } else if constexpr (std::is_same_v<T, Prompt>) {
          if (o.text.empty()) return {ErrorCode::InvalidInput, "消息不能为空"};

          Session s;
          if (!impl_->store->get_session(o.session_id, s))
            return {ErrorCode::NotFound, "会话不存在"};

          {
            std::lock_guard<std::mutex> lock(impl_->mu);
            if (impl_->options.reject_when_busy && impl_->active.count(o.session_id))
              return {ErrorCode::Busy, "这个会话已经有一轮在跑"};
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
          impl_->emitter.message(EventType::MessageUpdated, o.session_id, user.id);

          // assistant 消息此刻就建好，后续 delta 都挂在它下面。
          Message assistant;
          assistant.id = id::message();
          assistant.role = Role::Assistant;
          assistant.created = now_ms();
          impl_->store->put_message(o.session_id, assistant);
          impl_->emitter.message(EventType::MessageUpdated, o.session_id, assistant.id);

          impl_->store->mutate_session(o.session_id,
                                       [](Session& sess) { sess.updated = now_ms(); });

          auto turn = std::make_shared<ActiveTurn>();
          {
            std::lock_guard<std::mutex> lock(impl_->mu);
            impl_->active[o.session_id] = turn;
          }

          // 单独线程跑，submit 立刻返回——同步等会让 HTTP 请求挂几十秒。
          const std::string sid = o.session_id;
          auto deps = impl_->deps();
          turn->worker = std::thread([this, sid, turn, deps, assistant] {
            internal::TurnRunner runner(deps, sid, assistant);
            runner.run(turn->cancel);
            impl_->retire(sid, turn);
          });
          return assistant.id;

        } else if constexpr (std::is_same_v<T, Interrupt>) {
          std::lock_guard<std::mutex> lock(impl_->mu);
          auto it = impl_->active.find(o.session_id);
          if (it == impl_->active.end()) return {ErrorCode::NotFound, "没有正在跑的轮次"};
          it->second->cancel.store(true, std::memory_order_relaxed);
          return o.session_id;

        } else {
          return {ErrorCode::Internal, "未处理的操作"};
        }
      },
      op);
}

}  // namespace mai
