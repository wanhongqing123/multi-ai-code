#include "mai/tool.h"

#include "util/fs_utf8.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace mai {

namespace fs = std::filesystem;

void ToolRegistry::add(std::unique_ptr<Tool> tool) {
  if (!tool) return;
  const std::string name = tool->name();
  // 同名覆盖：注册表里不该出现两个同名工具，模型按名字调，撞了就不确定跑哪个。
  auto it = std::find_if(tools_.begin(), tools_.end(),
                         [&](const std::unique_ptr<Tool>& t) { return t->name() == name; });
  if (it != tools_.end()) {
    *it = std::move(tool);
    return;
  }
  tools_.push_back(std::move(tool));
}

Tool* ToolRegistry::find(const std::string& name) const {
  auto it = std::find_if(tools_.begin(), tools_.end(),
                         [&](const std::unique_ptr<Tool>& t) { return t->name() == name; });
  return it == tools_.end() ? nullptr : it->get();
}

std::vector<ToolSchema> ToolRegistry::schemas() const {
  std::vector<ToolSchema> out;
  out.reserve(tools_.size());
  for (const auto& t : tools_) {
    ToolSchema s;
    s.name = t->name();
    s.description = t->description();
    s.parameters_json = t->parameters_schema();
    out.push_back(std::move(s));
  }
  return out;
}

void register_builtin_tools(ToolRegistry& registry) {
  registry.add(make_read_tool());
  registry.add(make_write_tool());
  registry.add(make_glob_tool());
  registry.add(make_grep_tool());
}

// ── 路径安全 ────────────────────────────────────────────────────
//
// 这是工具层唯一的安全边界。模型会试着越界——有时是它自己想看看
// ../.env，有时是被提示词注入诱导的。所以这里按"默认拒绝"来写。
//
// 用 weakly_canonical 而不是 canonical：目标文件可能还不存在（write 要创建它），
// canonical 在那种情况下直接失败。weakly_canonical 会解析已存在的那一段，
// 对不存在的部分做词法规范化——`..` 仍然会被消掉，这是我们要的。
//
// 已存在的路径还要再用 canonical 过一遍：weakly_canonical 在某些平台上
// 不解析符号链接，而符号链接是最容易被忽略的越界手段——
// root 里放一个指向 C:\ 的链接就绕过去了。
std::string resolve_within_root(const std::string& root, const std::string& candidate) {
  if (root.empty() || candidate.empty()) return {};

  std::error_code ec;
  // 一律走 path_from_utf8 / path_to_utf8。直接用 fs::path(std::string) 会按
  // 当前 ANSI 代码页解释（Windows 中文环境是 GBK），中文路径立刻出问题。
  const fs::path root_path =
      fs::weakly_canonical(internal::path_from_utf8(root), ec);
  if (ec) return {};

  fs::path target = internal::path_from_utf8(candidate);
  // 相对路径按 root 解析；绝对路径原样拿去判断是否落在 root 内。
  if (target.is_relative()) target = root_path / target;

  fs::path resolved = fs::weakly_canonical(target, ec);
  if (ec) return {};

  // 存在的话再解一次符号链接：weakly_canonical 在某些平台上不解析它们，
  // 而 root 里放一个指向 C:\ 的链接就能绕过整个检查。
  if (fs::exists(resolved, ec) && !ec) {
    const fs::path real = fs::canonical(resolved, ec);
    if (!ec) resolved = real;
  }

  // 逐段比对，不能用字符串前缀——前缀判断会让 /srv/app-secrets 通过
  // 对 /srv/app 的检查。
  auto r = root_path.begin();
  auto t = resolved.begin();
  while (r != root_path.end()) {
    if (t == resolved.end()) return {};  // 比 root 还短，肯定在外面
    if (*r != *t) return {};
    ++r;
    ++t;
  }
  return internal::path_to_utf8(resolved);
}

}  // namespace mai
