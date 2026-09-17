#pragma once
#include <string>
#include <vector>

#include "mai/llm.h"
#include "mai/message.h"

namespace mai::internal {

// 把历史消息组装成给模型看的上下文。
//
// 单独抽出来，是因为这里将来会长出一堆策略，而它们都不该塞进 Agent：
//   - 增量缓存：现在每轮 O(n) 重建整个历史，聊长了整体就是 O(n²)。
//     要改成保留已序列化的前缀、每轮只追加——这是计划里的一号性能风险。
//   - 上下文压缩：超长时把前面的对话摘要掉。
//   - 裁剪：按 token 预算丢最老的。
//   - 系统提示词、项目规则文件的注入。
// 混在 Agent 里就没地方放这些，也没法单独测。
class ContextBuilder {
 public:
  struct Options {
    // reasoning 不回灌。它是模型的草稿，回灌会污染下一轮上下文。
    bool include_reasoning = false;
    std::string system_prompt;
  };

  explicit ContextBuilder(Options options = {}) : options_(std::move(options)) {}

  std::vector<ModelMessage> build(const std::vector<Message>& history) const;

 private:
  Options options_;
};

}  // namespace mai::internal
