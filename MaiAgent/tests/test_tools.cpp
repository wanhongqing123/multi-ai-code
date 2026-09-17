// 工具层测试。
//
// 顺序是有意的：先测路径边界，再测功能。边界错了后面全都不重要——
// 模型会主动试着越界（有时是它自己想看 ../.env，有时是被提示词注入诱导），
// 这不是假想威胁。
//
// 写这些用例时踩到的坑，记在这里免得重犯：
// **在 Windows 上不要写 `some_path / "中文目录"`。** 源码里的字面量是 UTF-8，
// 而 MSVC 的 fs::path 把 narrow 字符串按当前 ANSI 代码页（中文机器是 GBK）
// 解释，拼出来是乱码路径——实测直接让进程挂掉，不是返回错误。
// 测试里要用 std::filesystem::u8path("中文")，产品代码里用
// internal::path_from_utf8()。
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <json.hpp>

#include "mai/tool.h"

using namespace mai;
namespace fs = std::filesystem;
using nlohmann::json;

static int failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                 \
    }                                                             \
  } while (0)

namespace {

// 造一个临时工作区，里面放好几种文件。
struct Workspace {
  fs::path root;
  fs::path outside;  // root 之外的一个目录，用来验证越界被挡住

  Workspace() {
    const auto base = fs::temp_directory_path() /
                      ("maiagent-tools-" + std::to_string(std::rand()));
    root = base / "workspace";
    outside = base / "outside";
    fs::create_directories(root / "src");
    fs::create_directories(root / "node_modules" / "junk");
    fs::create_directories(outside);

    write(root / "README.md", "第一行\n第二行 hello\n第三行\n");
    write(root / "src" / "main.cpp", "#include <cstdio>\nint main() { return 0; }\n");
    write(root / "src" / "util.cpp", "void helper() {}\n// TODO: 补实现\n");
    write(root / "src" / "util.h", "void helper();\n");
    // node_modules 里也放一个能匹配的，验证它被跳过
    write(root / "node_modules" / "junk" / "main.cpp", "should not be found\n");
    write(outside / "secret.txt", "这是工作目录之外的机密\n");
  }

  ~Workspace() {
    std::error_code ec;
    fs::remove_all(root.parent_path(), ec);
  }

  static void write(const fs::path& p, const std::string& body) {
    std::ofstream out(p, std::ios::binary);
    out << body;
  }

  ToolContext ctx() const {
    ToolContext c;
    c.session_id = "ses_test";
    c.root = root.string();
    return c;
  }
};

std::string args(const json& j) { return j.dump(); }

// ── 1. 路径边界 ─────────────────────────────────────────────────

void test_path_escape_is_blocked() {
  Workspace ws;
  auto read = make_read_tool();
  const auto ctx = ws.ctx();

  // 各种越界写法都要被挡住
  const char* escapes[] = {
      "../outside/secret.txt",
      "../../etc/passwd",
      "src/../../outside/secret.txt",
      "./src/./../../outside/secret.txt",
  };
  for (const char* p : escapes) {
    const auto r = read->execute(args({{"path", p}}), ctx);
    CHECK(r.error.code == ErrorCode::InvalidInput);
    if (r.error.code != ErrorCode::InvalidInput)
      std::printf("  越界没被挡住: %s -> %s\n", p, r.output.c_str());
    // 而且不能把机密内容漏出去
    CHECK(r.output.find("机密") == std::string::npos);
  }

  // 绝对路径指向外面，同样挡住
  const auto abs = read->execute(
      args({{"path", (ws.outside / "secret.txt").string()}}), ctx);
  CHECK(abs.error.code == ErrorCode::InvalidInput);
  CHECK(abs.output.find("机密") == std::string::npos);
}

void test_write_cannot_escape() {
  Workspace ws;
  auto write = make_write_tool();
  const auto r = write->execute(
      args({{"path", "../outside/pwned.txt"}, {"content", "x"}}), ws.ctx());
  CHECK(r.error.code == ErrorCode::InvalidInput);
  CHECK(!fs::exists(ws.outside / "pwned.txt"));
}

void test_resolve_within_root_directly() {
  Workspace ws;
  const std::string root = ws.root.string();

  // 合法的
  CHECK(!resolve_within_root(root, "README.md").empty());
  CHECK(!resolve_within_root(root, "src/main.cpp").empty());
  CHECK(!resolve_within_root(root, "还不存在的文件.txt").empty());  // write 要能创建

  // 非法的
  CHECK(resolve_within_root(root, "../outside/secret.txt").empty());
  CHECK(resolve_within_root(root, "").empty());
  CHECK(resolve_within_root("", "README.md").empty());

  // 前缀不等于包含：同级的 workspace-evil 不能通过 workspace 的检查。
  // 用字符串前缀判断的实现会在这里漏。
  const fs::path sibling = ws.root.parent_path() / "workspace-evil";
  fs::create_directories(sibling);
  Workspace::write(sibling / "x.txt", "evil");
  CHECK(resolve_within_root(root, (sibling / "x.txt").string()).empty());
}

// ── 2. read ─────────────────────────────────────────────────────

void test_read() {
  Workspace ws;
  auto read = make_read_tool();
  const auto ctx = ws.ctx();

  const auto r = read->execute(args({{"path", "README.md"}}), ctx);
  CHECK(!r.error);
  CHECK(r.output.find("第二行 hello") != std::string::npos);
  CHECK(r.output.find("1\t") != std::string::npos);  // 带行号

  const auto off = read->execute(args({{"path", "README.md"}, {"offset", 2}, {"limit", 1}}), ctx);
  CHECK(!off.error);
  CHECK(off.output.find("第二行") != std::string::npos);
  CHECK(off.output.find("第一行") == std::string::npos);
  CHECK(off.output.find("第三行") == std::string::npos);

  const auto missing = read->execute(args({{"path", "不存在.txt"}}), ctx);
  CHECK(missing.error.code == ErrorCode::NotFound);

  // 目录要给一句有用的话，而不是一个看不懂的失败
  const auto dir = read->execute(args({{"path", "src"}}), ctx);
  CHECK(dir.error.code == ErrorCode::InvalidInput);
  CHECK(dir.error.message.find("glob") != std::string::npos);

  const auto no_path = read->execute("{}", ctx);
  CHECK(no_path.error.code == ErrorCode::InvalidInput);

  // 畸形 JSON 不能让工具崩
  const auto junk = read->execute("这不是 JSON", ctx);
  CHECK(junk.error.code == ErrorCode::InvalidInput);
}

// ── 3. write ────────────────────────────────────────────────────

void test_write() {
  Workspace ws;
  auto write = make_write_tool();
  const auto ctx = ws.ctx();

  const auto r = write->execute(
      args({{"path", "新目录/新文件.txt"}, {"content", "写入的内容 🙂"}}), ctx);
  CHECK(!r.error);
  // 测试里自己拼路径时也要走 UTF-8：ws.root 是 fs::path，但字面量
  // "新目录" 是 UTF-8 的 char*，`path / "中文"` 同样会按 ANSI 代码页解释。
  const fs::path written = ws.root / std::filesystem::u8path("新目录") /
                           std::filesystem::u8path("新文件.txt");
  CHECK(fs::exists(written));

  std::ifstream in(written, std::ios::binary);
  std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(body == "写入的内容 🙂");

  // 覆盖
  write->execute(args({{"path", "新目录/新文件.txt"}, {"content", "覆盖了"}}), ctx);
  std::ifstream in2(written, std::ios::binary);
  std::string body2((std::istreambuf_iterator<char>(in2)), std::istreambuf_iterator<char>());
  CHECK(body2 == "覆盖了");

  CHECK(write->execute(args({{"path", "x.txt"}}), ctx).error.code == ErrorCode::InvalidInput);
  // write 会改东西，必须标记为需要审批（M4 的闸门靠这个标志）
  CHECK(write->requires_approval());
  CHECK(!make_read_tool()->requires_approval());
}

// ── 4. glob ─────────────────────────────────────────────────────

void test_glob() {
  Workspace ws;
  auto glob = make_glob_tool();
  const auto ctx = ws.ctx();

  const auto cpp = glob->execute(args({{"pattern", "**/*.cpp"}}), ctx);
  CHECK(!cpp.error);
  CHECK(cpp.output.find("src/main.cpp") != std::string::npos);
  CHECK(cpp.output.find("src/util.cpp") != std::string::npos);
  // node_modules 必须被跳过，否则在真实仓库里会走几十万个文件
  CHECK(cpp.output.find("node_modules") == std::string::npos);

  const auto h = glob->execute(args({{"pattern", "*.h"}}), ctx);
  CHECK(!h.error);
  CHECK(h.output.find("util.h") != std::string::npos);
  CHECK(h.output.find(".cpp") == std::string::npos);

  const auto none = glob->execute(args({{"pattern", "**/*.rs"}}), ctx);
  CHECK(!none.error);
  CHECK(none.output.find("没有匹配") != std::string::npos);

  CHECK(glob->execute("{}", ctx).error.code == ErrorCode::InvalidInput);
}

// ── 5. grep ─────────────────────────────────────────────────────

void test_grep() {
  Workspace ws;
  auto grep = make_grep_tool();
  const auto ctx = ws.ctx();

  const auto todo = grep->execute(args({{"pattern", "TODO"}}), ctx);
  CHECK(!todo.error);
  CHECK(todo.output.find("src/util.cpp") != std::string::npos);
  CHECK(todo.output.find(":2:") != std::string::npos);  // 带行号

  const auto filtered = grep->execute(
      args({{"pattern", "helper"}, {"glob", "*.h"}}), ctx);
  CHECK(!filtered.error);
  CHECK(filtered.output.find("util.h") != std::string::npos);
  CHECK(filtered.output.find("util.cpp") == std::string::npos);

  const auto none = grep->execute(args({{"pattern", "绝对找不到的字符串xyzzy"}}), ctx);
  CHECK(!none.error);
  CHECK(none.output.find("没有匹配") != std::string::npos);

  // 坏正则要给一句能改的话，而不是崩掉
  const auto bad = grep->execute(args({{"pattern", "([unclosed"}}), ctx);
  CHECK(bad.error.code == ErrorCode::InvalidInput);

  // 中文能搜到
  const auto cn = grep->execute(args({{"pattern", "第二行"}}), ctx);
  CHECK(!cn.error);
  CHECK(cn.output.find("README.md") != std::string::npos);
}

// ── 6. 注册表 ───────────────────────────────────────────────────

void test_registry() {
  ToolRegistry reg;
  CHECK(reg.empty());
  register_builtin_tools(reg);
  CHECK(!reg.empty());

  for (const char* n : {"read", "write", "glob", "grep"}) {
    CHECK(reg.find(n) != nullptr);
  }
  CHECK(reg.find("不存在的工具") == nullptr);

  const auto schemas = reg.schemas();
  CHECK(schemas.size() == 4);
  for (const auto& s : schemas) {
    CHECK(!s.name.empty());
    CHECK(!s.description.empty());
    // schema 必须是合法 JSON，否则请求体构造时会被悄悄换成空对象，
    // 模型就不知道参数怎么填了
    const auto parsed = json::parse(s.parameters_json, nullptr, false);
    CHECK(!parsed.is_discarded());
    if (!parsed.is_discarded()) {
      CHECK(parsed.contains("properties"));
      CHECK(parsed.contains("required"));
    }
  }

  // 同名覆盖而不是并存
  reg.add(make_read_tool());
  CHECK(reg.schemas().size() == 4);
}

void test_cancel_stops_traversal() {
  Workspace ws;
  auto glob = make_glob_tool();
  auto ctx = ws.ctx();
  std::atomic<bool> canceled{true};
  ctx.cancel = &canceled;
  // 已取消时不该继续遍历。这里只验证它不崩、能立刻返回。
  const auto r = glob->execute(args({{"pattern", "**/*"}}), ctx);
  CHECK(!r.error || r.error.code != ErrorCode::Internal);
}

}  // namespace

// 每个用例跑之前先打一行，崩了的话能立刻看出停在哪一个。
// 这次就是靠它定位的：上一版整个进程直接挂掉，一条输出都没有。
#define RUN(f)                  \
  do {                          \
    std::printf("-> %s\n", #f); \
    std::fflush(stdout);        \
    f();                        \
  } while (0)

int main() {
  RUN(test_path_escape_is_blocked);
  RUN(test_write_cannot_escape);
  RUN(test_resolve_within_root_directly);
  RUN(test_read);
  RUN(test_write);
  RUN(test_glob);
  RUN(test_grep);
  RUN(test_registry);
  RUN(test_cancel_stops_traversal);
  if (failures == 0) std::printf("tool tests passed\n");
  return failures == 0 ? 0 : 1;
}
