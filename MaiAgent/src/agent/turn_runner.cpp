#include "agent/turn_runner.h"

#include "mai/id.h"

namespace mai::internal {
namespace {

// 标题从第一句话截一段。按字节硬切会切出半个汉字，所以要退到字符边界。
std::string make_title(const std::string& text, std::size_t max_bytes = 40) {
  if (text.size() <= max_bytes) return text;
  std::size_t cut = max_bytes;
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
  return text.substr(0, cut) + "…";
}

}  // namespace

TurnRunner::TurnRunner(Deps deps, std::string session_id, Message assistant)
    : deps_(std::move(deps)),
      session_id_(std::move(session_id)),
      assistant_(std::move(assistant)),
      // part id 一次性定死。界面靠它做增量更新，中途换会重绘甚至闪屏。
      text_part_id_(id::part()),
      reasoning_part_id_(id::part()) {}

void TurnRunner::run(const std::atomic<bool>& cancel) {
  if (!deps_.model) {
    error_ = Error::make(ErrorCode::NotConfigured, "没有配置模型客户端");
    persist_and_finish(cancel);
    return;
  }

  Session s;
  if (!deps_.store->get_session(session_id_, s)) {
    error_ = Error::make(ErrorCode::NotFound, "会话不存在");
    persist_and_finish(cancel);
    return;
  }

  Completion req;
  req.model = s.model.empty() ? deps_.default_model : s.model;
  req.turns = deps_.context->build(deps_.store->list_messages(session_id_));

  bool text_started = false;
  bool reasoning_started = false;

  StreamSink sink;
  sink.on_text = [&](std::string_view chunk) {
    if (!text_started) {
      text_started = true;
      deps_.emitter->part(EventType::MessagePartUpdated, session_id_, assistant_.id,
                          text_part_id_);
    }
    text_.append(chunk);
    deps_.emitter->delta(session_id_, assistant_.id, text_part_id_, "text", chunk);
  };
  sink.on_reasoning = [&](std::string_view chunk) {
    if (!reasoning_started) {
      reasoning_started = true;
      deps_.emitter->part(EventType::MessagePartUpdated, session_id_, assistant_.id,
                          reasoning_part_id_);
    }
    reasoning_.append(chunk);
    deps_.emitter->delta(session_id_, assistant_.id, reasoning_part_id_, "text", chunk);
  };
  // on_tool_call 留到 M3。现在不设回调，收到也只是被忽略——
  // 不会假装执行，也不会静默丢弃后让用户以为模型没调。

  error_ = deps_.model->stream(req, sink, cancel);
  persist_and_finish(cancel);
}

void TurnRunner::persist_and_finish(const std::atomic<bool>& cancel) {
  // 落库只在这里做一次。每个 delta 落一次盘等于每秒几十次 fsync。
  if (!reasoning_.empty()) {
    Part p;
    p.id = reasoning_part_id_;
    p.body = ReasoningPart{reasoning_};
    p.created = now_ms();
    assistant_.parts.push_back(std::move(p));
  }
  if (!text_.empty()) {
    Part p;
    p.id = text_part_id_;
    p.body = TextPart{text_};
    p.created = now_ms();
    assistant_.parts.push_back(std::move(p));
  }
  assistant_.completed = now_ms();
  deps_.store->put_message(session_id_, assistant_);
  deps_.emitter->message(EventType::MessageUpdated, session_id_, assistant_.id);

  maybe_name_session();

  // 主动中断不是故障：界面不该弹错误，已经吐出来的半截内容也照常保留。
  const bool canceled =
      cancel.load(std::memory_order_relaxed) || error_.code == ErrorCode::Canceled;
  if (error_ && !canceled) {
    deps_.emitter->session(EventType::SessionError, session_id_, error_.message);
  }
}

void TurnRunner::maybe_name_session() {
  Session snapshot;
  if (!deps_.store->get_session(session_id_, snapshot)) return;
  if (!snapshot.untitled()) return;

  std::string title;
  for (const auto& m : deps_.store->list_messages(session_id_)) {
    if (m.role != Role::User) continue;
    title = make_title(m.text());
    break;
  }
  if (title.empty()) return;

  // 通过 mutate_session 而不是"读出来改完写回去"：
  // 那样会把这期间别人对 model/agent 的修改盖掉（lost update）。
  std::string applied;
  deps_.store->mutate_session(session_id_, [&](Session& s) {
    if (!s.untitled()) return;  // 锁内再查一次，可能已经被别人命名了
    s.title = title;
    s.updated = now_ms();
    applied = title;
  });
  if (!applied.empty()) {
    deps_.emitter->session(EventType::SessionUpdated, session_id_, applied);
  }
}

}  // namespace mai::internal
