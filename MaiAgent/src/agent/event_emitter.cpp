#include "agent/event_emitter.h"

#include "mai/id.h"

namespace mai::internal {
namespace {

// 所有事件共有的字段只在这一处填。加新事件类型时不会漏掉 id 或 time——
// 漏了不会编译报错，只会在界面上表现成某些更新莫名其妙丢了。
Event base(EventType type, const std::string& session_id) {
  Event e;
  e.id = id::event();
  e.type = type;
  e.session_id = session_id;
  e.time = now_ms();
  return e;
}

}  // namespace

void EventEmitter::session(EventType type, const std::string& session_id,
                           const std::string& detail) {
  Event e = base(type, session_id);
  e.detail = detail;
  bus_.publish(e);
}

void EventEmitter::message(EventType type, const std::string& session_id,
                           const std::string& message_id) {
  Event e = base(type, session_id);
  e.message_id = message_id;
  bus_.publish(e);
}

void EventEmitter::part(EventType type, const std::string& session_id,
                        const std::string& message_id, const std::string& part_id) {
  Event e = base(type, session_id);
  e.message_id = message_id;
  e.part_id = part_id;
  bus_.publish(e);
}

void EventEmitter::delta(const std::string& session_id, const std::string& message_id,
                         const std::string& part_id, const char* field,
                         std::string_view chunk) {
  Event e = base(EventType::MessagePartDelta, session_id);
  e.message_id = message_id;
  e.part_id = part_id;
  e.field = field;
  e.delta.assign(chunk);
  bus_.publish(e);
}

}  // namespace mai::internal
