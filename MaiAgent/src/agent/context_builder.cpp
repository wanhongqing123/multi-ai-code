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
    Turn t;
    t.speaker = (m.role == Role::User) ? Turn::Speaker::User : Turn::Speaker::Assistant;

    for (const auto& p : m.parts) {
      if (const auto* text = std::get_if<TextPart>(&p.body)) {
        if (!t.content.empty()) t.content += "\n";
        t.content += text->text;
      } else if (const auto* r = std::get_if<ReasoningPart>(&p.body)) {
        // 默认不回灌。reasoning 是模型的草稿，喂回去会污染下一轮的判断。
        if (options_.include_reasoning) {
          if (!t.content.empty()) t.content += "\n";
          t.content += r->text;
        }
      }
      // ToolPart 等 M3 做工具时处理：它要展开成
      // assistant(invocations) + 一条 ToolResult。
    }

    if (t.content.empty() && t.invocations.empty()) continue;  // 空轮次不发给模型
    out.push_back(std::move(t));
  }
  return out;
}

}  // namespace mai::internal
