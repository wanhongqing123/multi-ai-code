#pragma once

#include <string>
#include <variant>
#include <vector>

#include "MaiTime.h"

// 纯数据，没有行为，不知道存储、不知道网络、不知道 JSON。
// 领域模型独立于一切基础设施——换存储、换模型供应商、换界面都不该动这里。

// ── 消息片段 ────────────────────────────────────────────────────
// 用 std::variant 而不是继承 + 虚函数：片段在流式期间高频读写，
// variant 是值语义、无堆分配、cache 友好，而且穷尽 visit 时编译器会
// 提醒漏掉的分支——加新片段类型时不会悄悄漏处理。
struct MaiTextPart {
    std::string text;
};

// 模型的思考过程。**不回灌给模型**——它是草稿，回灌会污染下一轮上下文。
struct MaiReasoningPart {
    std::string text;
};

enum class MaiToolState {
    Pending,
    Running,
    Completed,
    Error,
};

const char* maiToolStateToString(MaiToolState state);

struct MaiToolPart {
    std::string tool;    // read / write / glob / grep ...
    std::string callId;  // 模型给的 tool_call_id，回灌结果时要原样带回
    std::string input;   // 参数的 JSON 原文。核心不解析，交给工具实现
    std::string output;
    std::string error;
    MaiToolState state = MaiToolState::Pending;
};

using MaiMessagePartBody = std::variant<MaiTextPart, MaiReasoningPart, MaiToolPart>;

struct MaiMessagePart {
    std::string id;  // prt_...，**创建后永不改变**——界面靠它做增量更新
    MaiMessagePartBody body;
    MaiMillis created = 0;
};

// ── 消息 ────────────────────────────────────────────────────────
enum class MaiRole {
    User,
    Assistant,
};

const char* maiRoleToString(MaiRole role);

struct MaiMessage {
    std::string id;  // msg_...
    MaiRole role = MaiRole::User;
    std::vector<MaiMessagePart> parts;
    MaiMillis created = 0;
    MaiMillis completed = 0;  // 0 表示还在进行中

    bool isInProgress() const;

    // 把所有文本片段拼起来。reasoning 和 tool 不算。
    std::string text() const;
};
