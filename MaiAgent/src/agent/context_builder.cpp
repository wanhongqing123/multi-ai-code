#include "agent/context_builder.h"

namespace mai::internal {

std::vector<Turn> ContextBuilder::build(const std::vector<Message>& history) const {
  std::vector<Turn> out;
  out.reserve(history.size() + 1);

  if (!options_.system_prompt.empty()) {
    Turn t;
    t.speaker = Turn::Speaker::System;
    t.content = options_.system_prompt;
    out.push_back(std::move(t));
  }

  for (const auto& m : history) {
    if (m.role == Role::User) {
      Turn t;
      t.speaker = Turn::Speaker::User;
      t.content = m.text();
      if (!t.content.empty()) out.push_back(std::move(t));
      continue;
    }

    // ── assistant 这一侧要按工具调用切段 ────────────────────────
    //
    // 一条 assistant 消息里可能是：说几句 → 调工具 → 拿到结果 → 再说几句。
    // 而 OpenAI 协议要求的形状是：
    //   assistant（带 tool_calls）
    //   tool（结果，一个调用一条）
    //   assistant（后续文本）
    // 所以不能把整条消息压成一个 turn——那样模型看不到调用和结果的对应关系，
    // 下一轮会重复调同一个工具。
    Turn pending;
    pending.speaker = Turn::Speaker::Assistant;
    std::vector<const ToolPart*> batch;

    auto flush_batch = [&] {
      if (batch.empty()) return;
      // 先发 assistant + 它发起的这批调用
      for (const auto* tp : batch) {
        ToolInvocation inv;
        inv.id = tp->call_id;
        inv.name = tp->tool;
        inv.arguments = tp->input;
        pending.invocations.push_back(std::move(inv));
      }
      out.push_back(pending);
      pending = Turn{};
      pending.speaker = Turn::Speaker::Assistant;

      // 再发每个调用的结果。tool_call_id 必须对得上，否则模型认不出
      // 这是哪次调用的结果。
      for (const auto* tp : batch) {
        Turn r;
        r.speaker = Turn::Speaker::ToolResult;
        r.tool_call_id = tp->call_id;
        r.content = tp->output.empty() ? "（无输出）" : tp->output;
        out.push_back(std::move(r));
      }
      batch.clear();
    };

    for (const auto& p : m.parts) {
      if (const auto* text = std::get_if<TextPart>(&p.body)) {
        // 工具调用之后又开口说话了，说明上一批已经结束，先结算。
        flush_batch();
        if (!pending.content.empty()) pending.content += "\n";
        pending.content += text->text;
      } else if (const auto* r = std::get_if<ReasoningPart>(&p.body)) {
        // 默认不回灌。reasoning 是模型的草稿，喂回去会污染下一轮的判断。
        if (options_.include_reasoning) {
          flush_batch();
          if (!pending.content.empty()) pending.content += "\n";
          pending.content += r->text;
        }
      } else if (const auto* tp = std::get_if<ToolPart>(&p.body)) {
        // 还没跑完的不回灌：模型看到一个没有结果的调用会以为它失败了。
        if (tp->state == ToolState::Completed || tp->state == ToolState::Error) {
          batch.push_back(tp);
        }
      }
    }
    flush_batch();

    if (!pending.content.empty() || !pending.invocations.empty()) {
      out.push_back(std::move(pending));
    }
  }
  return out;
}

}  // namespace mai::internal
