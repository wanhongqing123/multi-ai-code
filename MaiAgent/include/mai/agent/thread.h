#pragma once
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "mai/agent/event.h"
#include "mai/agent/llm.h"
#include "mai/agent/session.h"

namespace mai::agent {

// ── Op：能提交给核心的操作 ──────────────────────────────────────
// 形状抄 codex（protocol/src/protocol.rs 的 Op）。它那边 18 个变体，
// 跑通一次带工具的对话只用得上 4 个。这里按里程碑逐个补。
struct OpCreateSession {
  std::string directory;
  std::string title;
  std::string model;
};
struct OpUpdateSession {
  std::string session_id;
  std::string title;   // 空表示不改
  std::string model;
  std::string agent;
};
struct OpDeleteSession {
  std::string session_id;
};

// 发一轮消息。**立刻返回**，真正的输出全部走事件流——
// 这是 agent loop 的入口，也是整个核心唯一会跑很久的操作。
struct OpTurnInput {
  std::string session_id;
  std::string text;
};

// 中断正在跑的那一轮。
struct OpInterrupt {
  std::string session_id;
};

// M4: OpApprovePermission

using Op = std::variant<OpCreateSession, OpUpdateSession, OpDeleteSession,
                        OpTurnInput, OpInterrupt>;

// ── Agent：核心对外的全部入口 ───────────────────────────────────
// 查询走同步方法（它们是纯读，没必要塞进队列绕一圈）；
// 会产生流式工作的变更走 submit()，结果从事件流里出来。
//
// HTTP 适配器、Qt 桌面、iOS/Android、CLI 都是调这一个类。
// 这里不出现任何 HTTP 或 JSON 的概念——这是"库是边界"的落点。
class Agent {
 public:
  // llm 可以为空：那样 OpTurnInput 会立刻以 session.error 收场，
  // 但其余功能照常——M1 的空转服务端就是这么跑的。
  Agent(std::unique_ptr<Store> store, std::unique_ptr<LlmClient> llm,
        std::string default_model);
  explicit Agent(std::unique_ptr<Store> store);
  ~Agent();
  Agent(const Agent&) = delete;
  Agent& operator=(const Agent&) = delete;

  // 查询
  std::vector<Session> sessions() const;
  bool session(const std::string& id, Session& out) const;
  std::vector<Message> messages(const std::string& session_id) const;
  bool busy(const std::string& session_id) const;  // 这一轮还在跑吗

  // 变更。返回受影响的对象 id；OpTurnInput 返回新建的 assistant message id。
  std::string submit(const Op& op);

  // 等所有在跑的轮次结束。给测试和优雅退出用。
  void wait_idle();

  // 事件流。适配器订阅它翻译成 SSE；嵌入式直接订阅。
  EventBus& events();
  const EventBus& events() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mai::agent
