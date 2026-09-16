#include "mai/agent/session.h"

#include <algorithm>
#include <chrono>

namespace mai::agent {

Millis now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

namespace {

class MemoryStore final : public Store {
 public:
  void put_session(const Session& s) override {
    std::lock_guard<std::mutex> lock(mu_);
    sessions_[s.id] = s;
  }

  bool get_session(const std::string& id, Session& out) const override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return false;
    out = it->second;
    return true;
  }

  std::vector<Session> list_sessions() const override {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Session> out;
    out.reserve(sessions_.size());
    for (const auto& [_, s] : sessions_) out.push_back(s);
    // UI 左侧列表按最近活跃排序，这里就排好，省得每个调用方各排一遍。
    std::sort(out.begin(), out.end(),
              [](const Session& a, const Session& b) { return a.updated > b.updated; });
    return out;
  }

  bool remove_session(const std::string& id) override {
    std::lock_guard<std::mutex> lock(mu_);
    messages_.erase(id);
    return sessions_.erase(id) > 0;
  }

  void put_message(const std::string& session_id, const Message& m) override {
    std::lock_guard<std::mutex> lock(mu_);
    auto& list = messages_[session_id];
    // 同 id 视为更新（流式期间同一条消息会被反复写回）。
    auto it = std::find_if(list.begin(), list.end(),
                           [&](const Message& x) { return x.id == m.id; });
    if (it != list.end()) {
      *it = m;
    } else {
      list.push_back(m);
    }
  }

  std::vector<Message> list_messages(const std::string& session_id) const override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = messages_.find(session_id);
    if (it == messages_.end()) return {};
    return it->second;
  }

 private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, Session> sessions_;
  std::unordered_map<std::string, std::vector<Message>> messages_;
};

}  // namespace

std::unique_ptr<Store> make_memory_store() { return std::make_unique<MemoryStore>(); }

}  // namespace mai::agent
