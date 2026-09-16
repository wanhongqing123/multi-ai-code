#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

// 刻意不 include 任何 JSON 头。
// 铁律：JSON 只出现在边界（adapters/http）。核心里一律用原生结构体，
// 一是性能（nlohmann 的 DOM 每个节点一次堆分配），二是换 JSON 库时不用翻全身。

namespace mai::agent {

using Millis = std::int64_t;
Millis now_ms();

// ── Part：一条消息里的一个片段 ──────────────────────────────────
// 对应 opencode 的 message part，也对应 codex 的 TurnItem。
// 用 std::variant 而不是继承 + 虚函数：part 在流式期间会被高频读写，
// variant 是值语义、无堆分配、cache 友好。
struct TextPart {
  std::string text;
};

struct ReasoningPart {
  std::string text;
};

enum class ToolState { Pending, Running, Completed, Error };

struct ToolPart {
  std::string tool;      // read / write / bash / glob / grep ...
  std::string call_id;   // 大模型给的 tool_call_id，回灌结果时要原样带回
  std::string input;     // 参数的 JSON 原文。核心不解析它，转给工具实现去解
  std::string output;
  std::string error;
  ToolState state = ToolState::Pending;
};

using PartBody = std::variant<TextPart, ReasoningPart, ToolPart>;

struct Part {
  std::string id;        // prt_...
  PartBody body;
  Millis created = 0;
};

// ── Message ────────────────────────────────────────────────────
enum class Role { User, Assistant };

struct Message {
  std::string id;        // msg_...
  Role role = Role::User;
  std::vector<Part> parts;
  Millis created = 0;
  Millis completed = 0;  // 0 表示还在进行中
};

// ── Session ────────────────────────────────────────────────────
struct Session {
  std::string id;        // ses_...
  std::string title;
  std::string directory;
  std::string model;
  std::string agent = "build";
  Millis created = 0;
  Millis updated = 0;
};

// ── Store：存储接口 ─────────────────────────────────────────────
// 先给内存实现跑通 M1，SQLite 实现按同一个接口补上（M5）。
// 之所以第一天就抽接口：嵌入式上可能根本不落盘，或者换成别的 KV。
class Store {
 public:
  virtual ~Store() = default;

  virtual void put_session(const Session& s) = 0;
  virtual bool get_session(const std::string& id, Session& out) const = 0;
  virtual std::vector<Session> list_sessions() const = 0;  // 按 updated 倒序
  virtual bool remove_session(const std::string& id) = 0;

  virtual void put_message(const std::string& session_id, const Message& m) = 0;
  virtual std::vector<Message> list_messages(const std::string& session_id) const = 0;
};

std::unique_ptr<Store> make_memory_store();

}  // namespace mai::agent
