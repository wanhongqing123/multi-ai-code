#pragma once
#include <string>
#include <string_view>

#include "mai/event.h"
#include "mai/types.h"

namespace mai::internal {

// 把"构造事件并发布"这件事收在一处。
//
// 之前 Agent::Impl 里散着四个 emit_xxx 私有方法，每个都在手工填
// id/time/type 那几个字段——加一种事件就要再抄一遍，抄漏一个字段
// 不会编译报错，只会在界面上表现成某些更新丢了。
//
// 方法名都带 emit_ 前缀：不带的话 `emitter->session(...)` 读起来
// 像在取 session，而它其实是在发事件。
class EventEmitter {
 public:
  explicit EventEmitter(EventBus& bus) : bus_(bus) {}

  void emit_session(EventType type, const std::string& session_id,
                    const std::string& detail = {});

  void emit_message(EventType type, const std::string& session_id,
                    const std::string& message_id);

  void emit_part(EventType type, const std::string& session_id,
                 const std::string& message_id, const std::string& part_id);

  // 增量。**只带这次新增的内容**，不重推全量——
  // 流式期间每秒几十条，全量重推是纯烧 CPU。
  void emit_delta(const std::string& session_id, const std::string& message_id,
                  const std::string& part_id, const char* field,
                  std::string_view chunk);

 private:
  EventBus& bus_;
};

}  // namespace mai::internal
