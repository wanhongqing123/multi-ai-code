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
      assistant_(std::move(assistant)) {}

void TurnRunner::run(const std::atomic<bool>& cancel) {
  if (!deps_.model) {
    error_ = Error::make(ErrorCode::NotConfigured, "没有配置模型客户端");
    persist_and_finish(cancel);
    return;
  }

  Session session;
  if (!deps_.store->get_session(session_id_, session)) {
    error_ = Error::make(ErrorCode::NotFound, "会话不存在");
    persist_and_finish(cancel);
    return;
  }

  const std::string model_name =
      session.model.empty() ? deps_.default_model : session.model;
  const std::string root = session.directory;

  // ── 工具循环 ──────────────────────────────────────────────────
  // 这是 agent 之所以是 agent 的地方：模型说要调工具 → 我们执行 → 把结果
  // 回灌 → 再问一次 → 它可能还要调 → 直到它不再要调为止。
  //
  // 每一圈都重新组装上下文，因为上一圈的工具结果已经作为 part 落在
  // assistant_ 上了，ContextBuilder 会把它展开成模型认得的形状。
  for (int iteration = 0; iteration < deps_.max_iterations; ++iteration) {
    if (cancel.load(std::memory_order_relaxed)) break;

    Completion req;
    req.model = model_name;
    // 历史 + 本轮已经产生的内容（工具调用和结果都在里面）
    auto history = deps_.store->list_messages(session_id_);
    for (auto& m : history) {
      if (m.id == assistant_.id) m = assistant_;  // 用内存里最新的那份
    }
    req.turns = deps_.context->build(history);
    if (deps_.tools && !deps_.tools->empty()) req.tools = deps_.tools->schemas();

    const auto calls = stream_once(req, cancel);
    flush_text_parts();

    if (error_) break;
    if (calls.empty()) break;  // 模型不再要调工具，这一轮结束
    if (cancel.load(std::memory_order_relaxed)) break;

    run_tools(calls, cancel);

    // 到达上限还没收手：明确告诉用户，而不是悄悄停在半路让人以为跑完了。
    if (iteration + 1 >= deps_.max_iterations) {
      error_ = Error::make(ErrorCode::Internal,
                           "工具调用达到上限（" + std::to_string(deps_.max_iterations) +
                               " 轮）后停止。可以让我换个思路再试。");
    }
  }

  persist_and_finish(cancel);
}

std::vector<ToolInvocation> TurnRunner::stream_once(const Completion& req,
                                                    const std::atomic<bool>& cancel) {
  // 每一圈的文本是独立的 part：模型在调工具前后说的话是两段发言，
  // 混成一个 part 会让界面把工具卡夹在一段文字中间。
  text_.clear();
  reasoning_.clear();
  text_part_id_ = id::part();
  reasoning_part_id_ = id::part();

  bool text_started = false;
  bool reasoning_started = false;
  std::vector<ToolInvocation> calls;

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
  sink.on_tool_call = [&](const ToolInvocation& call) { calls.push_back(call); };

  error_ = deps_.model->stream(req, sink, cancel);
  return calls;
}

void TurnRunner::run_tools(const std::vector<ToolInvocation>& calls,
                           const std::atomic<bool>& cancel) {
  Session session;
  deps_.store->get_session(session_id_, session);

  ToolContext ctx;
  ctx.session_id = session_id_;
  ctx.root = session.directory;
  ctx.cancel = &cancel;

  for (const auto& call : calls) {
    if (cancel.load(std::memory_order_relaxed)) break;

    Part part;
    part.id = id::part();
    part.created = now_ms();
    ToolPart body;
    body.tool = call.name;
    body.call_id = call.id;
    body.input = call.arguments;
    body.state = ToolState::Running;
    part.body = body;
    assistant_.parts.push_back(part);

    // 先广播"开始跑了"，界面立刻能显示工具卡，而不是等它跑完才蹦出来。
    deps_.emitter->part(EventType::MessagePartUpdated, session_id_, assistant_.id, part.id);

    Tool* tool = deps_.tools ? deps_.tools->find(call.name) : nullptr;
    ToolResult result;
    if (!tool) {
      // 模型有时会编一个不存在的工具名。告诉它事实，它下一圈通常会改对；
      // 直接失败整轮反而更糟。
      result = ToolResult::fail(ErrorCode::NotFound, "没有名为 " + call.name + " 的工具");
    } else if (ctx.root.empty()) {
      result = ToolResult::fail(ErrorCode::InvalidInput,
                                "这个会话没有设置工作目录，文件类工具无法使用");
    } else {
      result = tool->execute(call.arguments, ctx);
    }

    auto& stored = std::get<ToolPart>(assistant_.parts.back().body);
    if (result.error) {
      stored.state = ToolState::Error;
      stored.error = result.error.message;
      // 错误也要回灌给模型——它需要知道失败了才能换个做法。
      stored.output = result.error.message;
    } else {
      stored.state = ToolState::Completed;
      stored.output = result.output;
      if (result.truncated) stored.output += "\n（输出已截断）";
    }
    deps_.emitter->part(EventType::MessagePartUpdated, session_id_, assistant_.id, part.id);

    // 每执行完一个工具就落一次库：工具可能跑很久，中途崩了不该丢掉已完成的部分。
    deps_.store->put_message(session_id_, assistant_);
  }
}

void TurnRunner::flush_text_parts() {
  // 落库只在这里做。每个 delta 落一次盘等于每秒几十次 fsync。
  if (!reasoning_.empty()) {
    Part p;
    p.id = reasoning_part_id_;
    p.body = ReasoningPart{reasoning_};
    p.created = now_ms();
    assistant_.parts.push_back(std::move(p));
    reasoning_.clear();
  }
  if (!text_.empty()) {
    Part p;
    p.id = text_part_id_;
    p.body = TextPart{text_};
    p.created = now_ms();
    assistant_.parts.push_back(std::move(p));
    text_.clear();
  }
}

void TurnRunner::persist_and_finish(const std::atomic<bool>& cancel) {
  flush_text_parts();  // 中断时可能还有没落库的半截内容

  assistant_.completed = now_ms();
  deps_.store->put_message(session_id_, assistant_);
  deps_.emitter->message(EventType::MessageUpdated, session_id_, assistant_.id);

  maybe_name_session();

  // 主动中断不是故障：界面不该弹错误，已经吐出来的内容也照常保留。
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
