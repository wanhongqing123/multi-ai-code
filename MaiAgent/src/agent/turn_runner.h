#pragma once
#include <atomic>
#include <memory>
#include <string>

#include "mai/event.h"
#include "mai/llm.h"
#include "mai/message.h"
#include "mai/store.h"
#include "mai/types.h"

#include "agent/context_builder.h"
#include "agent/event_emitter.h"

namespace mai::internal {

// 跑一轮对话：组装上下文 → 流式请求 → 把增量变成事件 → 落库。
//
// 从 Agent 里分出来的理由是职责，不是代码长度：
// Agent 是门面（接 Op、转发查询、持有依赖），一轮对话的生命周期是另一回事。
// M3 的工具循环、M4 的权限挂起都长在这里，留在 Agent 里那个类会失控。
//
// 一个 TurnRunner 实例只跑一轮，跑完就扔——没有可复用的状态，
// 也就没有"上一轮残留"这类 bug。
class TurnRunner {
 public:
  struct Deps {
    SessionStore* store = nullptr;
    ModelClient* model = nullptr;
    EventEmitter* emitter = nullptr;
    const ContextBuilder* context = nullptr;
    std::string default_model;
  };

  TurnRunner(Deps deps, std::string session_id, Message assistant);

  // 阻塞跑完一轮。调用方负责把它放到自己的线程上。
  void run(const std::atomic<bool>& cancel);

 private:
  void persist_and_finish(const std::atomic<bool>& cancel);
  void maybe_name_session();

  Deps deps_;
  std::string session_id_;
  Message assistant_;

  // part id 在这里一次性定死，之后所有 delta 都引用它。
  // 中途换 id 会让界面重绘甚至闪屏。
  std::string text_part_id_;
  std::string reasoning_part_id_;

  std::string text_;
  std::string reasoning_;
  Error error_;
};

}  // namespace mai::internal
