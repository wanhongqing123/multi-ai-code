#pragma once
#include <string>
#include <variant>
#include <vector>

#include "mai/types.h"

// 纯数据，没有行为，不知道存储、不知道网络、不知道 JSON。
// 领域模型独立于一切基础设施——换存储、换模型供应商、换 UI 都不该动这里。

namespace mai {

// ── MessagePart：一条消息里的一个片段 ──────────────────────────────────
// 用 std::variant 而不是继承 + 虚函数：part 在流式期间高频读写，
// variant 是值语义、无堆分配、cache 友好，而且穷尽 visit 时编译器会
// 提醒漏掉的分支——加新 part 类型时不会悄悄漏处理。
struct TextPart {
  std::string text;
};

// 模型的思考过程。**不回灌给模型**——它是草稿，回灌会污染下一轮上下文。
struct ReasoningPart {
  std::string text;
};

enum class ToolState { Pending, Running, Completed, Error };
const char* to_string(ToolState s);

struct ToolPart {
  std::string tool;      // read / write / bash / glob / grep ...
  std::string call_id;   // 模型给的 tool_call_id，回灌结果时要原样带回
  std::string input;     // 参数的 JSON 原文。核心不解析，交给工具实现
  std::string output;
  std::string error;
  ToolState state = ToolState::Pending;
};

using MessagePartBody = std::variant<TextPart, ReasoningPart, ToolPart>;

struct MessagePart {
  std::string id;  // prt_...，**创建后永不改变**——界面靠它做增量更新
  MessagePartBody body;
  Millis created = 0;
};

// ── Message ────────────────────────────────────────────────────
enum class Role { User, Assistant };
const char* to_string(Role r);

struct Message {
  std::string id;  // msg_...
  Role role = Role::User;
  std::vector<MessagePart> parts;
  Millis created = 0;
  Millis completed = 0;  // 0 表示还在进行中

  bool in_progress() const { return completed == 0; }

  // 把所有文本片段拼起来。reasoning 和 tool 不算。
  std::string text() const;
};

}  // namespace mai
