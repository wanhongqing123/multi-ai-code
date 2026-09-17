#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mai/message.h"
#include "mai/session.h"

namespace mai {

// 持久化接口。**从领域模型里分出来了**——之前 Store 和 Session/Message
// 挤在同一个头里，任何人 include 领域模型就把存储接口也拉了进来，
// 依赖方向是反的：领域模型不该知道持久化的存在。
//
// 第一天就抽接口的另一个原因：嵌入式上可能根本不落盘，或者换成别的 KV。
class SessionStore {
 public:
  virtual ~SessionStore() = default;

  virtual void put_session(const Session& s) = 0;
  virtual bool get_session(const std::string& id, Session& out) const = 0;
  virtual std::vector<Session> list_sessions() const = 0;  // 按 updated 倒序
  virtual bool remove_session(const std::string& id) = 0;

  virtual void put_message(const std::string& session_id, const Message& m) = 0;
  virtual std::vector<Message> list_messages(const std::string& session_id) const = 0;

  // 原子地改会话的一部分字段。
  //
  // 存在的理由是个真 bug：之前跑一轮的流程是"开头读 Session、结尾改 title 写回"，
  // 中间如果别人改了 model 或 agent，收尾那一写会把人家的改动盖掉（lost update）。
  // 让存储层在锁内做读-改-写，调用方就不会碰到这个窗口。
  // 返回 false 表示会话不存在。
  virtual bool mutate_session(const std::string& id,
                              const std::function<void(Session&)>& fn) = 0;
};

std::unique_ptr<SessionStore> make_memory_store();

}  // namespace mai
