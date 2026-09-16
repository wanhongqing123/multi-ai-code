#include "mai/agent/thread.h"

#include "mai/agent/id.h"

namespace mai::agent {

struct Agent::Impl {
  std::unique_ptr<Store> store;
  EventBus bus;

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
};

Agent::Agent(std::unique_ptr<Store> store) : impl_(std::make_unique<Impl>()) {
  impl_->store = std::move(store);
}

Agent::~Agent() = default;

std::vector<Session> Agent::sessions() const { return impl_->store->list_sessions(); }

bool Agent::session(const std::string& id, Session& out) const {
  return impl_->store->get_session(id, out);
}

std::vector<Message> Agent::messages(const std::string& session_id) const {
  return impl_->store->list_messages(session_id);
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

        } else {
          return {};
        }
      },
      op);
}

}  // namespace mai::agent
