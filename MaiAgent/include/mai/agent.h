#pragma once
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "mai/event.h"
#include "mai/llm.h"
#include "mai/message.h"
#include "mai/session.h"
#include "mai/store.h"
#include "mai/types.h"

namespace mai {

// ── Op：能提交给核心的操作 ──────────────────────────────────────
// 形状抄 codex（它的 core 对外就 submit(Op) / next_event() 两个方法）。
// 做成一个封闭的 variant 而不是一堆方法，是为了让"核心能做什么"可枚举：
// 加能力时编译器会强制每个 visit 都处理到，不会有人忘了改某个分支。
struct CreateSession {
  std::string directory;
  std::string title;
  std::string model;
};

struct UpdateSession {
  std::string session_id;
  std::string title;  // 空表示不改
  std::string model;
  std::string agent;
};

struct DeleteSession {
  std::string session_id;
};

// 发一轮消息。**立刻返回**，真正的输出全部走事件流。
struct Prompt {
  std::string session_id;
  std::string text;
};

// 中断正在跑的那一轮。
struct Interrupt {
  std::string session_id;
};

// M4: ApprovePermission

using Op = std::variant<CreateSession, UpdateSession, DeleteSession, Prompt, Interrupt>;

// ── Agent：核心的门面 ───────────────────────────────────────────
//
// 它只做四件事：接收 Op、转发查询、持有依赖、暴露事件流。
// **跑一轮对话的逻辑不在这里**——那在 TurnRunner（src/agent/turn_runner.h），
// 上下文组装在 ContextBuilder。之前这三件事挤在一个 Impl 里，
// 加工具和权限之后会彻底失控。
//
// Qt 桌面、iOS/Android、CLI、HTTP 适配器都只认这一个类。
// 这里不出现任何 HTTP 或 JSON 的概念——这是"库是边界"的落点。
class Agent {
 public:
  struct Options {
    std::string default_model = "glm-5.3";
    // 每个会话同时只跑一轮。第二条来了直接拒绝而不是排队——
    // 排队会让用户以为消息丢了，界面上看不出区别。
    bool reject_when_busy = true;
  };

  // model 可以为空：那样 Prompt 会以 NotConfigured 收场，
  // 但其余功能照常——空转模式就是这么跑的。
  Agent(std::unique_ptr<SessionStore> store, std::unique_ptr<ModelClient> model,
        Options options = {});
  ~Agent();
  Agent(const Agent&) = delete;
  Agent& operator=(const Agent&) = delete;

  // ── 查询（纯读，不经过 Op）────────────────────────────────────
  std::vector<Session> sessions() const;
  bool session(const std::string& id, Session& out) const;
  std::vector<Message> messages(const std::string& session_id) const;
  bool busy(const std::string& session_id) const;

  // ── 变更 ────────────────────────────────────────────────────
  // 返回受影响的对象 id；Prompt 返回新建的 assistant message id。
  // 失败时 Result 里带 ErrorCode，调用方能区分"会话不存在"和"正忙"——
  // 之前只返回空字符串，上层只能猜。
  Result<std::string> submit(const Op& op);

  // 等所有在跑的轮次结束。给测试和优雅退出用。
  void wait_idle();

  EventBus& events();
  const EventBus& events() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mai
