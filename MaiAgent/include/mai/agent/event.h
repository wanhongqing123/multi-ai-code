#pragma once
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "mai/agent/session.h"

namespace mai::agent {

// ── 事件 ────────────────────────────────────────────────────────
// 只做第一阶段要的 13 种。openapi.json 里有 89 种 type，其余 76 种属于
// pty / tui / lsp / mcp / vcs / workspace 这些第一阶段不做的子系统。
//
// 另外 spec 里有两套并行的流式协议：session.next.* 和 message.part.*。
// 翻过 UI 的消费端，它吃的是后者（session.next.* 全仓只有 2 处引用），
// 所以这里只产出 message.part.* 那一套。
enum class EventType {
  SessionCreated,
  SessionUpdated,
  SessionDeleted,
  SessionIdle,
  SessionError,
  SessionStatus,
  MessageUpdated,
  MessageRemoved,
  MessagePartUpdated,
  MessagePartDelta,
  MessagePartRemoved,
  PermissionAsked,
  PermissionReplied,
};

const char* to_wire(EventType t);  // 转成 "message.part.delta" 这类线上字符串

// 事件载荷。刻意做成扁平结构而不是一个 JSON 对象：
// 流式期间每秒几十条，走 JSON DOM 等于每条一堆堆分配。
struct Event {
  std::string id;          // evt_...
  EventType type = EventType::SessionStatus;
  std::string session_id;
  std::string message_id;
  std::string part_id;

  // MessagePartDelta 用：field 说明增量打在哪个字段上（"text" / "input" / "output"），
  // delta 是这次追加的内容。**只带增量，不带全量**——全量重推是 CPU 杀手。
  std::string field;
  std::string delta;

  // 其它事件的附带信息（权限 id、错误文本、标题等）。
  std::string detail;
  Millis time = 0;
};

// ── 事件总线 ────────────────────────────────────────────────────
// 核心里它是观察者接口，不是 SSE。HTTP 适配器订阅之后翻译成 SSE；
// 移动端 / 嵌入式直接订阅，完全不经过 HTTP。
class EventBus {
 public:
  using Handler = std::function<void(const Event&)>;
  using Token = std::uint64_t;

  EventBus();
  ~EventBus();
  EventBus(const EventBus&) = delete;
  EventBus& operator=(const EventBus&) = delete;

  Token subscribe(Handler h);
  void unsubscribe(Token t);
  void publish(const Event& e);

  std::size_t subscriber_count() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mai::agent
