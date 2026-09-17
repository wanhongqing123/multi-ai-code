#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>

#include <json.hpp>

#include "mai/tool.h"

#include "util/fs_utf8.h"

namespace mai {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// ── 输出上限 ────────────────────────────────────────────────────
// 工具输出会原样进上下文，所以每一个字节都要花 token、也都要占内存。
// grep 一个大仓库能返回几 MB —— 不设限的话内存、请求体、费用一起炸。
// 这是计划里的二号性能风险。
constexpr std::size_t kMaxOutputBytes = 64 * 1024;
constexpr std::size_t kMaxReadBytes = 256 * 1024;
constexpr int kMaxGrepMatches = 200;
constexpr int kMaxGlobResults = 300;

// 截断时要落在 UTF-8 字符边界上，不然会切出半个汉字，
// 后面 JSON 序列化会失败或者产生乱码。
void truncate_utf8(std::string& s, std::size_t max_bytes) {
  if (s.size() <= max_bytes) return;
  std::size_t cut = max_bytes;
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
  s.resize(cut);
}

json parse_args(const std::string& raw) {
  const json j = json::parse(raw, nullptr, /*allow_exceptions=*/false);
  return j.is_object() ? j : json::object();
}

// 把 root 之外的路径挡掉，并给模型一句它能据此改正的话。
struct Resolved {
  std::string path;
  ToolResult error;
  bool ok = false;
};

Resolved resolve_or_fail(const ToolContext& ctx, const std::string& raw_path) {
  if (raw_path.empty())
    return {{}, ToolResult::fail(ErrorCode::InvalidInput, "缺少 path 参数"), false};
  const std::string resolved = resolve_within_root(ctx.root, raw_path);
  if (resolved.empty()) {
    return {{},
            ToolResult::fail(ErrorCode::InvalidInput,
                             "路径超出了工作目录，被拒绝：" + raw_path +
                                 "。只能访问工作目录内的文件。"),
            false};
  }
  return {resolved, {}, true};
}

// glob 匹配。**不用 std::regex。**
//
// 第一版是把 glob 转成正则再交给 std::regex，结果整个进程崩了
// （STATUS_STACK_BUFFER_OVERRUN）。原因是 `**/*.cpp` 会转成
// `.*[^/\\]*\.cpp` 这种形状，两个贪婪量词挨着会产生灾难性回溯，
// 而 MSVC 的 std::regex 是递归实现的，回溯深度直接把栈打穿。
//
// 手写的这个是迭代式的：记住最近一次 `*` 的位置，失配就回到那里让 `*`
// 多吃一个字符。最坏 O(n*m)，不递归、不分配、没有栈风险。
//
// 语义：
//   *   匹配任意字符，但不跨路径分隔符
//   **  匹配任意字符，跨分隔符
//   ?   匹配单个非分隔符字符
// 大小写不敏感，只对 ASCII 做折叠——中文没有大小写，不需要处理。
bool is_sep(char c) { return c == '/' || c == '\\'; }

char fold(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool glob_match(const std::string& pattern, const std::string& text) {
  std::size_t p = 0, t = 0;
  std::size_t star_p = std::string::npos, star_t = 0;
  bool star_crosses_sep = false;

  while (t < text.size()) {
    if (p < pattern.size() && pattern[p] == '*') {
      const bool doubled = (p + 1 < pattern.size() && pattern[p + 1] == '*');
      star_p = p;
      star_crosses_sep = doubled;
      p += doubled ? 2 : 1;
      // `**/` 里的那个分隔符是可选的：`**/*.cpp` 也该匹配根目录下的 a.cpp
      if (doubled && p < pattern.size() && is_sep(pattern[p])) ++p;
      star_t = t;
      continue;
    }
    if (p < pattern.size() &&
        (fold(pattern[p]) == fold(text[t]) ||
         (pattern[p] == '?' && !is_sep(text[t])) ||
         (is_sep(pattern[p]) && is_sep(text[t])))) {
      ++p;
      ++t;
      continue;
    }
    // 失配：退回最近的 `*`，让它多吃一个字符。
    if (star_p != std::string::npos) {
      // 单星不跨分隔符，遇到分隔符就彻底失败。
      if (!star_crosses_sep && is_sep(text[star_t])) return false;
      ++star_t;
      t = star_t;
      p = star_p + (star_crosses_sep ? 2 : 1);
      if (star_crosses_sep && p < pattern.size() && is_sep(pattern[p])) ++p;
      continue;
    }
    return false;
  }
  // 文本吃完了，剩下的模式必须全是 `*`
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

// 跳过这些目录：它们体量巨大而且几乎肯定不是模型要找的东西。
// 不跳过的话 glob 一个前端仓库会在 node_modules 里走几十万个文件。
bool should_skip_dir(const std::string& name) {
  static const char* kSkip[] = {".git", "node_modules", "target", "build", "dist",
                                "out",  ".venv",        "__pycache__", ".cache"};
  for (const char* s : kSkip)
    if (name == s) return true;
  return false;
}

std::string to_relative(const std::string& root, const fs::path& p) {
  std::error_code ec;
  const fs::path rel = fs::relative(p, internal::path_from_utf8(root), ec);
  // 一律返回 UTF-8。generic 形式统一用 / 分隔，这样模型看到的路径
  // 在三个平台上长得一样，它给回来的路径我们也认。
  return ec ? internal::path_to_utf8(p) : internal::path_to_utf8_generic(rel);
}

// ── read ────────────────────────────────────────────────────────
class ReadTool final : public Tool {
 public:
  std::string name() const override { return "read"; }
  std::string description() const override {
    return "读取工作目录内某个文件的内容。返回带行号的文本，便于后续引用具体行。";
  }
  std::string parameters_schema() const override {
    return R"({"type":"object","properties":{)"
           R"("path":{"type":"string","description":"相对于工作目录的文件路径"},)"
           R"("offset":{"type":"integer","description":"从第几行开始，默认 1"},)"
           R"("limit":{"type":"integer","description":"最多读多少行，默认 500"}},)"
           R"("required":["path"]})";
  }

  ToolResult execute(const std::string& raw, const ToolContext& ctx) override {
    const json args = parse_args(raw);
    auto r = resolve_or_fail(ctx, args.value("path", std::string{}));
    if (!r.ok) return r.error;

    std::error_code ec;
    const fs::path target = internal::path_from_utf8(r.path);
    if (!fs::exists(target, ec) || ec)
      return ToolResult::fail(ErrorCode::NotFound, "文件不存在：" + args.value("path", std::string{}));
    if (fs::is_directory(target, ec))
      return ToolResult::fail(ErrorCode::InvalidInput,
                              "这是一个目录，不是文件。用 glob 列目录内容。");

    std::ifstream in(internal::path_from_utf8(r.path), std::ios::binary);
    if (!in) return ToolResult::fail(ErrorCode::Internal, "打不开文件");

    const int offset = std::max(1, args.value("offset", 1));
    const int limit = std::max(1, args.value("limit", 500));

    std::string out;
    std::string line;
    int lineno = 0;
    int emitted = 0;
    bool truncated = false;
    while (std::getline(in, line)) {
      ++lineno;
      if (lineno < offset) continue;
      if (emitted >= limit) {
        truncated = true;
        break;
      }
      if (!line.empty() && line.back() == '\r') line.pop_back();
      out += std::to_string(lineno);
      out += "\t";
      out += line;
      out += "\n";
      ++emitted;
      if (out.size() > kMaxReadBytes) {
        truncated = true;
        break;
      }
    }

    if (out.empty()) {
      return ToolResult::ok(lineno == 0 ? "（空文件）"
                                        : "（从第 " + std::to_string(offset) + " 行起没有内容）");
    }
    truncate_utf8(out, kMaxReadBytes);
    if (truncated) out += "\n…内容被截断。用 offset 参数继续读后面的部分。";
    return ToolResult::ok(std::move(out), truncated);
  }
};

// ── write ───────────────────────────────────────────────────────
class WriteTool final : public Tool {
 public:
  std::string name() const override { return "write"; }
  std::string description() const override {
    return "把内容写入工作目录内的某个文件，覆盖原有内容。父目录会自动创建。";
  }
  std::string parameters_schema() const override {
    return R"({"type":"object","properties":{)"
           R"("path":{"type":"string","description":"相对于工作目录的文件路径"},)"
           R"("content":{"type":"string","description":"要写入的完整内容"}},)"
           R"("required":["path","content"]})";
  }
  // 会改文件。M4 的闸门就位后这里会真正拦一道。
  bool requires_approval() const override { return true; }

  ToolResult execute(const std::string& raw, const ToolContext& ctx) override {
    const json args = parse_args(raw);
    auto r = resolve_or_fail(ctx, args.value("path", std::string{}));
    if (!r.ok) return r.error;
    if (!args.contains("content") || !args["content"].is_string())
      return ToolResult::fail(ErrorCode::InvalidInput, "缺少 content 参数");

    const std::string content = args["content"].get<std::string>();

    std::error_code ec;
    const fs::path p = internal::path_from_utf8(r.path);
    if (p.has_parent_path()) {
      fs::create_directories(p.parent_path(), ec);
      if (ec) return ToolResult::fail(ErrorCode::Internal, "创建父目录失败：" + ec.message());
    }
    std::ofstream out(internal::path_from_utf8(r.path), std::ios::binary | std::ios::trunc);
    if (!out) return ToolResult::fail(ErrorCode::Internal, "打不开文件用于写入");
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) return ToolResult::fail(ErrorCode::Internal, "写入失败");
    out.close();

    return ToolResult::ok("已写入 " + to_relative(ctx.root, p) + "（" +
                          std::to_string(content.size()) + " 字节）");
  }
};

// ── glob ────────────────────────────────────────────────────────
class GlobTool final : public Tool {
 public:
  std::string name() const override { return "glob"; }
  std::string description() const override {
    return "按文件名模式查找工作目录内的文件。支持 * ? 和 **（跨目录）。";
  }
  std::string parameters_schema() const override {
    return R"({"type":"object","properties":{)"
           R"("pattern":{"type":"string","description":"如 **/*.cpp 或 src/*.h"},)"
           R"("path":{"type":"string","description":"从哪个子目录开始找，默认工作目录根"}},)"
           R"("required":["pattern"]})";
  }

  ToolResult execute(const std::string& raw, const ToolContext& ctx) override {
    const json args = parse_args(raw);
    const std::string pattern = args.value("pattern", std::string{});
    if (pattern.empty())
      return ToolResult::fail(ErrorCode::InvalidInput, "缺少 pattern 参数");

    std::string base = ctx.root;
    if (args.contains("path") && args["path"].is_string() &&
        !args["path"].get<std::string>().empty()) {
      auto r = resolve_or_fail(ctx, args["path"].get<std::string>());
      if (!r.ok) return r.error;
      base = r.path;
    }

    std::vector<std::string> hits;
    std::error_code ec;
    fs::recursive_directory_iterator it(internal::path_from_utf8(base),
                                       fs::directory_options::skip_permission_denied, ec);
    if (ec) return ToolResult::fail(ErrorCode::NotFound, "目录读不了：" + ec.message());

    for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      if (ctx.canceled()) break;
      if (it->is_directory(ec)) {
        if (should_skip_dir(internal::path_to_utf8(it->path().filename()))) it.disable_recursion_pending();
        continue;
      }
      const std::string rel = to_relative(ctx.root, it->path());
      if (glob_match(pattern, rel) ||
          glob_match(pattern, internal::path_to_utf8(it->path().filename()))) {
        hits.push_back(rel);
        if (static_cast<int>(hits.size()) >= kMaxGlobResults) break;
      }
    }

    if (hits.empty()) return ToolResult::ok("没有匹配 " + pattern + " 的文件");
    std::sort(hits.begin(), hits.end());
    std::string out;
    for (const auto& h : hits) {
      out += h;
      out += "\n";
    }
    const bool truncated = static_cast<int>(hits.size()) >= kMaxGlobResults;
    if (truncated)
      out += "…只显示前 " + std::to_string(kMaxGlobResults) + " 条，用更精确的模式缩小范围。";
    return ToolResult::ok(std::move(out), truncated);
  }
};

// ── grep ────────────────────────────────────────────────────────
class GrepTool final : public Tool {
 public:
  std::string name() const override { return "grep"; }
  std::string description() const override {
    return "在工作目录内按正则搜索文件内容，返回匹配的文件、行号和整行。";
  }
  std::string parameters_schema() const override {
    return R"({"type":"object","properties":{)"
           R"("pattern":{"type":"string","description":"正则表达式"},)"
           R"("path":{"type":"string","description":"从哪个子目录开始搜，默认工作目录根"},)"
           R"("glob":{"type":"string","description":"只搜匹配此模式的文件，如 *.cpp"}},)"
           R"("required":["pattern"]})";
  }

  ToolResult execute(const std::string& raw, const ToolContext& ctx) override {
    const json args = parse_args(raw);
    const std::string pattern = args.value("pattern", std::string{});
    if (pattern.empty()) return ToolResult::fail(ErrorCode::InvalidInput, "缺少 pattern 参数");

    std::string base = ctx.root;
    if (args.contains("path") && args["path"].is_string() &&
        !args["path"].get<std::string>().empty()) {
      auto r = resolve_or_fail(ctx, args["path"].get<std::string>());
      if (!r.ok) return r.error;
      base = r.path;
    }

    std::regex re;
    try {
      re = std::regex(pattern, std::regex::ECMAScript);
    } catch (const std::regex_error& e) {
      return ToolResult::fail(ErrorCode::InvalidInput,
                              std::string("正则无法解析：") + e.what());
    }

    const std::string glob_pat = args.value("glob", std::string{});
    const bool has_filter = !glob_pat.empty();

    std::string out;
    int matches = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(internal::path_from_utf8(base),
                                       fs::directory_options::skip_permission_denied, ec);
    if (ec) return ToolResult::fail(ErrorCode::NotFound, "目录读不了：" + ec.message());

    for (; it != fs::recursive_directory_iterator() && matches < kMaxGrepMatches;
         it.increment(ec)) {
      if (ec) break;
      if (ctx.canceled()) break;
      if (it->is_directory(ec)) {
        if (should_skip_dir(internal::path_to_utf8(it->path().filename()))) it.disable_recursion_pending();
        continue;
      }
      if (!it->is_regular_file(ec)) continue;
      const std::string rel = to_relative(ctx.root, it->path());
      if (has_filter && !glob_match(glob_pat, rel) &&
          !glob_match(glob_pat, internal::path_to_utf8(it->path().filename())))
        continue;

      // 太大的文件跳过：多半是二进制或产物，搜了也没意义还很慢。
      const auto size = fs::file_size(it->path(), ec);
      if (ec || size > 2u * 1024 * 1024) continue;

      std::ifstream in(it->path(), std::ios::binary);
      if (!in) continue;
      std::string line;
      int lineno = 0;
      while (std::getline(in, line) && matches < kMaxGrepMatches) {
        ++lineno;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // 含 NUL 的当二进制跳过整个文件。
        if (line.find('\0') != std::string::npos) break;
        // 先截断再匹配。用户给的正则可能有灾难性回溯，行越长越容易把栈打穿——
        // glob 那边已经因此崩过一次，这里不重蹈覆辙。
        if (line.size() > 400) {
          std::size_t cut = 400;
          while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) --cut;
          line.resize(cut);
        }
        if (!std::regex_search(line, re)) continue;
        out += rel;
        out += ":";
        out += std::to_string(lineno);
        out += ": ";
        out += line;
        out += "\n";
        ++matches;
        if (out.size() > kMaxOutputBytes) break;
      }
      if (out.size() > kMaxOutputBytes) break;
    }

    if (matches == 0) return ToolResult::ok("没有匹配 " + pattern + " 的内容");
    const bool truncated = matches >= kMaxGrepMatches || out.size() > kMaxOutputBytes;
    truncate_utf8(out, kMaxOutputBytes);
    if (truncated)
      out += "\n…匹配太多，只显示前面一部分。用更精确的正则或加 glob 参数缩小范围。";
    return ToolResult::ok(std::move(out), truncated);
  }
};

}  // namespace

std::unique_ptr<Tool> make_read_tool() { return std::make_unique<ReadTool>(); }
std::unique_ptr<Tool> make_write_tool() { return std::make_unique<WriteTool>(); }
std::unique_ptr<Tool> make_glob_tool() { return std::make_unique<GlobTool>(); }
std::unique_ptr<Tool> make_grep_tool() { return std::make_unique<GrepTool>(); }

}  // namespace mai
