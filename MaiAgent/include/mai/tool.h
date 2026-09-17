#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "mai/llm.h"
#include "mai/types.h"

namespace mai {

// 工具执行时能看到的环境。
struct ToolContext {
  std::string session_id;
  // 所有文件操作的根。**工具不得访问这个目录之外的任何路径**——
  // 模型会试（有意或无意），这是安全边界不是建议。
  std::string root;
  const std::atomic<bool>* cancel = nullptr;

  bool canceled() const {
    return cancel && cancel->load(std::memory_order_relaxed);
  }
};

struct ToolResult {
  std::string output;
  Error error;
  // 输出被截断过。要告诉模型，否则它会把"看到的就是全部"当真——
  // 比如 grep 只回了前 200 行，它会以为只有 200 处匹配。
  bool truncated = false;

  static ToolResult ok(std::string out, bool truncated = false) {
    return ToolResult{std::move(out), Error::ok(), truncated};
  }
  static ToolResult fail(ErrorCode code, std::string msg) {
    return ToolResult{{}, Error::make(code, std::move(msg)), false};
  }
};

// 一个工具。
//
// 参数以 JSON 原文传入，由工具自己解析——核心不认识任何具体工具的参数结构，
// 加新工具不用动核心。这也是为什么 ToolInvocation::arguments 一路保持原文。
class Tool {
 public:
  virtual ~Tool() = default;

  virtual std::string name() const = 0;
  virtual std::string description() const = 0;
  // 给模型看的 JSON Schema 原文。
  virtual std::string parameters_schema() const = 0;

  // 需要用户点头才能跑吗。read/glob/grep 这类只读的不需要；
  // write/edit/shell 会改东西或执行命令，需要。M4 的权限闸门读这个标志。
  virtual bool requires_approval() const { return false; }

  virtual ToolResult execute(const std::string& arguments_json,
                             const ToolContext& ctx) = 0;
};

// 工具注册表。
class ToolRegistry {
 public:
  void add(std::unique_ptr<Tool> tool);
  Tool* find(const std::string& name) const;
  bool empty() const { return tools_.empty(); }

  // 给模型的工具清单。
  std::vector<ToolSchema> schemas() const;

 private:
  std::vector<std::unique_ptr<Tool>> tools_;
};

// 第一批：只读的四个 + 待办。都不需要审批，所以可以先于权限闸门落地。
// write 虽然会改文件，但它的破坏面比 shell/edit 小得多，先放进来；
// 真正危险的 shell 和 edit 等 M4 的闸门就位再加。
std::unique_ptr<Tool> make_read_tool();
std::unique_ptr<Tool> make_write_tool();
std::unique_ptr<Tool> make_glob_tool();
std::unique_ptr<Tool> make_grep_tool();

// 把第一批工具都装上。
void register_builtin_tools(ToolRegistry& registry);

// ── 路径安全 ────────────────────────────────────────────────────
// 把用户给的路径解析成绝对路径，并确认它在 root 之内。
// 失败返回空字符串。
//
// 单独暴露出来是为了能被单独测：它是整个工具层唯一的安全边界，
// 混在各个工具里就没法保证每个都做对，也没法集中测 `..`、符号链接、
// Windows 短名、UNC 路径这些绕过手法。
std::string resolve_within_root(const std::string& root, const std::string& candidate);

}  // namespace mai
