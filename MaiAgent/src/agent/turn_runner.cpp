#include "agent/turn_runner.h"

#include "mai/id.h"

namespace mai::internal {
TurnRunner::TurnRunner(Deps deps, std::string session_id, Message assistant)
    : deps_(std::move(deps)),
      session_id_(std::move(session_id)),
      assistant_(std::move(assistant)) {}

void TurnRunner::run(const std::atomic<bool>& cancel) {
  if (!deps_.model) {
    error_ = Error::make(ErrorCode::NotConfigured, "没有配置模型客户端");
    finish(cancel);
    return;
  }

  Session session;
  if (!deps_.store->get_session(session_id_, session)) {
    error_ = Error::make(ErrorCode::NotFound, "会话不存在");
    finish(cancel);
    return;
  }

  const std::string model_name =
      session.model.empty() ? deps_.default_model : session.model;

  // ── 工具循环 ──────────────────────────────────────────────────
  // 这是 agent 之所以是 agent 的地方：模型说要调工具 → 我们执行 → 把结果
  // 回灌 → 再问一次 → 它可能还要调 → 直到它不再要调为止。
  //
  // 每一圈都重新组装上下文，因为上一圈的工具结果已经作为 part 落在
  // assistant_ 上了，ContextBuilder 会把它展开成模型认得的形状。
  for (int iteration = 0; iteration < deps_.max_iterations; ++iteration) {
    if (cancel.load(std::memory_order_relaxed)) break;

    const auto calls = request_completion(build_request(model_name), cancel);
    commit_streamed_parts();

    if (error_) break;
    if (calls.empty()) break;  // 模型不再要调工具，这一轮结束
    if (cancel.load(std::memory_order_relaxed)) break;

    execute_tools(calls, cancel);

    // 到达上限还没收手：明确告诉用户，而不是悄悄停在半路让人以为跑完了。
    if (iteration + 1 >= deps_.max_iterations) {
      error_ = Error::make(ErrorCode::Internal,
                           "工具调用达到上限（" + std::to_string(deps_.max_iterations) +
                               " 轮）后停止。可以让我换个思路再试。");
    }
  }

  finish(cancel);
}

ModelRequest TurnRunner::build_request(const std::string& model_name) const {
  ModelRequest req;
  req.model = model_name;

  // 历史里那条正在写的 assistant 消息，要用内存中最新的版本——
  // 存储里的那份可能还没包含刚执行完的工具结果。
  auto history = deps_.store->list_messages(session_id_);
  for (auto& m : history) {
    if (m.id == assistant_.id) m = assistant_;
  }
  req.messages = deps_.context->build(history);

  if (deps_.tools && !deps_.tools->empty()) req.tools = deps_.tools->specs();
  return req;
}

std::vector<ToolInvocation> TurnRunner::request_completion(const ModelRequest& req,
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
      deps_.emitter->emit_part(EventType::MessagePartUpdated, session_id_, assistant_.id,
                          text_part_id_);
    }
    text_.append(chunk);
    deps_.emitter->emit_delta(session_id_, assistant_.id, text_part_id_, "text", chunk);
  };
  sink.on_reasoning = [&](std::string_view chunk) {
    if (!reasoning_started) {
      reasoning_started = true;
      deps_.emitter->emit_part(EventType::MessagePartUpdated, session_id_, assistant_.id,
                          reasoning_part_id_);
    }
    reasoning_.append(chunk);
    deps_.emitter->emit_delta(session_id_, assistant_.id, reasoning_part_id_, "text", chunk);
  };
  sink.on_tool_call = [&](const ToolInvocation& call) { calls.push_back(call); };

  error_ = deps_.model->stream(req, sink, cancel);
  return calls;
}

void TurnRunner::execute_tools(const std::vector<ToolInvocation>& calls,
                           const std::atomic<bool>& cancel) {
  Session session;
  deps_.store->get_session(session_id_, session);

  ToolContext ctx;
  ctx.session_id = session_id_;
  ctx.root = session.directory;
  ctx.cancel = &cancel;

  for (const auto& call : calls) {
    if (cancel.load(std::memory_order_relaxed)) break;

    MessagePart part;
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
    deps_.emitter->emit_part(EventType::MessagePartUpdated, session_id_, assistant_.id, part.id);

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
    deps_.emitter->emit_part(EventType::MessagePartUpdated, session_id_, assistant_.id, part.id);

    // 每执行完一个工具就落一次库：工具可能跑很久，中途崩了不该丢掉已完成的部分。
    deps_.store->put_message(session_id_, assistant_);
  }
}

void TurnRunner::commit_streamed_parts() {
  // 落库只在这里做。每个 delta 落一次盘等于每秒几十次 fsync。
  if (!reasoning_.empty()) {
    MessagePart p;
    p.id = reasoning_part_id_;
    p.body = ReasoningPart{reasoning_};
    p.created = now_ms();
    assistant_.parts.push_back(std::move(p));
    reasoning_.clear();
  }
  if (!text_.empty()) {
    MessagePart p;
    p.id = text_part_id_;
    p.body = TextPart{text_};
    p.created = now_ms();
    assistant_.parts.push_back(std::move(p));
    text_.clear();
  }
}

void TurnRunner::finish(const std::atomic<bool>& cancel) {
  commit_streamed_parts();  // 中断时可能还有没落库的半截内容

  assistant_.completed = now_ms();
  deps_.store->put_message(session_id_, assistant_);
  deps_.emitter->emit_message(EventType::MessageUpdated, session_id_, assistant_.id);

  // 给会话起名不是"跑一轮"的职责，委托出去（见 session_titler.h）。
  if (deps_.titler) {
    const std::string title = deps_.titler->apply(*deps_.store, session_id_);
    if (!title.empty()) {
      deps_.emitter->emit_session(EventType::SessionUpdated, session_id_, title);
    }
  }

  // 主动中断不是故障：界面不该弹错误，已经吐出来的内容也照常保留。
  const bool canceled =
      cancel.load(std::memory_order_relaxed) || error_.code == ErrorCode::Canceled;
  if (error_ && !canceled) {
    deps_.emitter->emit_session(EventType::SessionError, session_id_, error_.message);
  }
}


}  // namespace mai::internal
