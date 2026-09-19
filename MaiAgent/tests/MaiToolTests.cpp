// 工具层测试。
//
// 顺序是有意的：先测路径边界，再测功能。
// 边界错了后面全都不重要——模型会主动试着越界（有时是它自己想看 ../.env，有时是被提示词注入诱导），
// 这不是假想威胁。
//
// 写这些用例时踩到的坑，记在这里免得重犯：**在 Windows 上不要写 `some_path / "中文目录"`。
// ** 源码里的字面量是 UTF-8，
// 而 MSVC 的 fs::path 把 narrow 字符串按当前 ANSI 代码页（中文机器是 GBK）
// 解释，拼出来是乱码路径——实测直接让进程挂掉，不是返回错误。
// 测试里要用 std::filesystem::u8path("中文")，产品代码里用
// MaiPathUtf8::fromUtf8()。
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiTool.h"

namespace fs = std::filesystem;
using nlohmann::json;

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

namespace {

// 造一个临时工作区，里面放好几种文件。
// ── 这一组常量是非 ASCII 用例的素材，不是普通测试数据 ──────────
//
// 写成 \u 转义而不是直接的汉字：规范要求代码里除注释外不出现中文。
// 但这几条用例测的**就是**非 ASCII 路径和内容——MSVC 的 fs::path 会把
// narrow 字符串按当前 ANSI 代码页（中文机器上是 GBK）解释，而我们的路径全来自 JSON、是 UTF-8。
// 当初这个 bug 不是返回错误，是进程直接挂掉（STATUS_STACK_BUFFER_OVERRUN）。
// 所以字节本身一个都不能改。
//
//   kCjkDir      \u65b0\u76ee\u5f55                新目录
//   kCjkFile     \u65b0\u6587\u4ef6.txt            新文件.txt
//   kCjkContent  \u5199\u5165\u7684\u5185\u5bb9 🙂  写入的内容 🙂
//   kCjkLine2    \u7b2c\u4e8c\u884c                第二行
const char* kCjkDir = "\u65b0\u76ee\u5f55";
const char* kCjkFile = "\u65b0\u6587\u4ef6.txt";
const char* kCjkContent = "\u5199\u5165\u7684\u5185\u5bb9 \U0001F642";
const char* kCjkOverwrite = "\u8986\u76d6\u4e86";
const char* kCjkLine1 = "\u7b2c\u4e00\u884c";
const char* kCjkLine2 = "\u7b2c\u4e8c\u884c";
const char* kCjkLine3 = "\u7b2c\u4e09\u884c";
const char* kCjkMissingFile = "\u4e0d\u5b58\u5728.txt";         // 不存在.txt
const char* kCjkNotYetExists = "\u8fd8\u4e0d\u5b58\u5728.txt";  // 还不存在的文件.txt
const char* kSecretMarker = "TOP-SECRET";

struct Workspace {
    fs::path root;
    fs::path outside;  // root 之外的一个目录，用来验证越界被挡住

    Workspace() {
        const auto base =
            fs::temp_directory_path() / ("maiagent-tools-" + std::to_string(std::rand()));
        root = base / "workspace";
        outside = base / "outside";
        fs::create_directories(root / "src");
        fs::create_directories(root / "node_modules" / "junk");
        fs::create_directories(outside);

        write(root / "README.md",
              std::string(kCjkLine1) + "\n" + kCjkLine2 + " hello\n" + kCjkLine3 + "\n");
        write(root / "src" / "main.cpp", "#include <cstdio>\nint main() { return 0; }\n");
        write(root / "src" / "util.cpp", "void helper() {}\n// TODO: fill this in\n");
        write(root / "src" / "util.h", "void helper();\n");
        // node_modules 里也放一个能匹配的，验证它被跳过
        write(root / "node_modules" / "junk" / "main.cpp", "should not be found\n");
        write(outside / "secret.txt", std::string(kSecretMarker) + " outside the workspace\n");
    }

    ~Workspace() {
        std::error_code ec;
        fs::remove_all(root.parent_path(), ec);
    }

    static void write(const fs::path& p, const std::string& body) {
        std::ofstream out(p, std::ios::binary);
        out << body;
    }

    MaiToolContext context() const {
        MaiToolContext c;
        c.sessionId = "ses_test";
        c.root = root.string();
        return c;
    }
};

std::string args(const json& j) {
    return j.dump();
}

// ── 1. 路径边界 ─────────────────────────────────────────────────

void test_path_escape_is_blocked() {
    Workspace workspace;
    auto read = makeMaiReadTool();
    const auto context = workspace.context();

    // 各种越界写法都要被挡住
    const char* escapes[] = {
        "../outside/secret.txt",
        "../../etc/passwd",
        "src/../../outside/secret.txt",
        "./src/./../../outside/secret.txt",
    };
    for (const char* p : escapes) {
        const auto r = read->execute(args({{"path", p}}), context);
        CHECK(r.error().code() == MaiErrorCode::InvalidInput);
        if (r.error().code() != MaiErrorCode::InvalidInput)
            std::printf("  escape was not blocked: %s -> %s\n", p, r.output().c_str());
        // 而且不能把机密内容漏出去
        CHECK(r.output().find(kSecretMarker) == std::string::npos);
    }

    // 绝对路径指向外面，同样挡住
    const auto abs =
        read->execute(args({{"path", (workspace.outside / "secret.txt").string()}}), context);
    CHECK(abs.error().code() == MaiErrorCode::InvalidInput);
    CHECK(abs.output().find(kSecretMarker) == std::string::npos);
}

// 同前缀的兄弟目录不能算"在里面"。
//
// 这条是冲着一类经典实现错误来的：用字符串前缀判断包含关系。
// "/server/app-secrets" 确实以 "/server/app" 开头，但它是**另一个目录**。真按前缀判，
// root 旁边随便放一个同前缀的目录就全漏了。
//
// MaiFilePath::isParentOf 是逐段比的，所以这里必须被挡住。
void test_sibling_with_shared_prefix_is_outside() {
    Workspace workspace;

    // workspace.root 叫 "workspace"，在它旁边造一个 "workspace-secrets"
    const MaiFilePath rootPath = MaiFilePath::fromUtf8(workspace.root.u8string());
    const MaiFilePath sibling =
        rootPath.dirName().append(MaiFilePath::fromUtf8("workspace-secrets"));
    MaiFileSystem::createDirectories(sibling);
    const MaiFilePath leaked = sibling.append(MaiFilePath::fromUtf8("secret.txt"));
    MaiFileSystem::writeFile(leaked, std::string(kSecretMarker) + " sibling directory\n");

    // 用绝对路径直接指过去
    CHECK(maiResolvePathWithinRoot(workspace.root.u8string(), leaked.toUtf8()).empty());

    // 用相对路径绕过去（..\workspace-secrets\secret.txt）
    const std::string relative = "../workspace-secrets/secret.txt";
    CHECK(maiResolvePathWithinRoot(workspace.root.u8string(), relative).empty());

    // 走真正的工具，确认内容没漏出去
    auto read = makeMaiReadTool();
    const auto viaAbsolute = read->execute(args({{"path", leaked.toUtf8()}}), workspace.context());
    CHECK(viaAbsolute.error().code() == MaiErrorCode::InvalidInput);
    CHECK(viaAbsolute.output().find(kSecretMarker) == std::string::npos);

    const auto viaRelative = read->execute(args({{"path", relative}}), workspace.context());
    CHECK(viaRelative.error().code() == MaiErrorCode::InvalidInput);
    CHECK(viaRelative.output().find(kSecretMarker) == std::string::npos);

    MaiFileSystem::removeRecursively(sibling);
}

void test_write_cannot_escape() {
    Workspace workspace;
    auto write = makeMaiWriteTool();
    const auto r = write->execute(args({{"path", "../outside/pwned.txt"}, {"content", "x"}}),
                                  workspace.context());
    CHECK(r.error().code() == MaiErrorCode::InvalidInput);
    CHECK(!fs::exists(workspace.outside / "pwned.txt"));
}

void test_maiResolvePathWithinRoot_directly() {
    Workspace workspace;
    const std::string root = workspace.root.string();

    // 合法的
    CHECK(!maiResolvePathWithinRoot(root, "README.md").empty());
    CHECK(!maiResolvePathWithinRoot(root, "src/main.cpp").empty());
    CHECK(!maiResolvePathWithinRoot(root, kCjkNotYetExists).empty());  // write 要能创建

    // 非法的
    CHECK(maiResolvePathWithinRoot(root, "../outside/secret.txt").empty());
    CHECK(maiResolvePathWithinRoot(root, "").empty());
    CHECK(maiResolvePathWithinRoot("", "README.md").empty());

    // 前缀不等于包含：同级的 workspace-evil 不能通过 workspace 的检查。
    // 用字符串前缀判断的实现会在这里漏。
    const fs::path sibling = workspace.root.parent_path() / "workspace-evil";
    fs::create_directories(sibling);
    Workspace::write(sibling / "x.txt", "evil");
    CHECK(maiResolvePathWithinRoot(root, (sibling / "x.txt").string()).empty());
}

// ── 2. read ─────────────────────────────────────────────────────

void test_read() {
    Workspace workspace;
    auto read = makeMaiReadTool();
    const auto context = workspace.context();

    const auto r = read->execute(args({{"path", "README.md"}}), context);
    CHECK(!r.hasError());
    CHECK(r.output().find(std::string(kCjkLine2) + " hello") != std::string::npos);
    CHECK(r.output().find("1\t") != std::string::npos);  // 带行号

    const auto off =
        read->execute(args({{"path", "README.md"}, {"offset", 2}, {"limit", 1}}), context);
    CHECK(!off.hasError());
    CHECK(off.output().find(kCjkLine2) != std::string::npos);
    CHECK(off.output().find(kCjkLine1) == std::string::npos);
    CHECK(off.output().find(kCjkLine3) == std::string::npos);

    const auto missing = read->execute(args({{"path", kCjkMissingFile}}), context);
    CHECK(missing.error().code() == MaiErrorCode::NotFound);

    // 目录要给一句有用的话，而不是一个看不懂的失败
    const auto dir = read->execute(args({{"path", "src"}}), context);
    CHECK(dir.error().code() == MaiErrorCode::InvalidInput);
    CHECK(dir.error().message().find("glob") != std::string::npos);

    const auto no_path = read->execute("{}", context);
    CHECK(no_path.error().code() == MaiErrorCode::InvalidInput);

    // 畸形 JSON 不能让工具崩
    const auto junk = read->execute("this is not json", context);
    CHECK(junk.error().code() == MaiErrorCode::InvalidInput);
}

// ── 3. write ────────────────────────────────────────────────────

void test_write() {
    Workspace workspace;
    auto write = makeMaiWriteTool();
    const auto context = workspace.context();

    const std::string cjkPath = std::string(kCjkDir) + "/" + kCjkFile;
    const auto r = write->execute(args({{"path", cjkPath}, {"content", kCjkContent}}), context);
    CHECK(!r.hasError());
    // 测试里自己拼路径时也要走 UTF-8：workspace.root 是 fs::path，但字面量
    // "新目录" 是 UTF-8 的 char*，`path / "中文"` 同样会按 ANSI 代码页解释。
    const fs::path written =
        workspace.root / std::filesystem::u8path(kCjkDir) / std::filesystem::u8path(kCjkFile);
    CHECK(fs::exists(written));

    std::ifstream in(written, std::ios::binary);
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(body == kCjkContent);

    // 覆盖
    write->execute(args({{"path", cjkPath}, {"content", kCjkOverwrite}}), context);
    std::ifstream in2(written, std::ios::binary);
    std::string body2((std::istreambuf_iterator<char>(in2)), std::istreambuf_iterator<char>());
    CHECK(body2 == kCjkOverwrite);

    CHECK(write->execute(args({{"path", "x.txt"}}), context).error().code() ==
          MaiErrorCode::InvalidInput);
    // write 会改东西，必须标记为需要审批（M4 的闸门靠这个标志）
    CHECK(write->requiresApproval());
    CHECK(!makeMaiReadTool()->requiresApproval());
}

// ── 4. glob ─────────────────────────────────────────────────────

void test_glob() {
    Workspace workspace;
    auto glob = makeMaiGlobTool();
    const auto context = workspace.context();

    const auto cpp = glob->execute(args({{"pattern", "**/*.cpp"}}), context);
    CHECK(!cpp.hasError());
    CHECK(cpp.output().find("src/main.cpp") != std::string::npos);
    CHECK(cpp.output().find("src/util.cpp") != std::string::npos);
    // node_modules 必须被跳过，否则在真实仓库里会走几十万个文件
    CHECK(cpp.output().find("node_modules") == std::string::npos);

    const auto h = glob->execute(args({{"pattern", "*.h"}}), context);
    CHECK(!h.hasError());
    CHECK(h.output().find("util.h") != std::string::npos);
    CHECK(h.output().find(".cpp") == std::string::npos);

    const auto none = glob->execute(args({{"pattern", "**/*.rs"}}), context);
    CHECK(!none.hasError());
    CHECK(none.output().find("No files match") != std::string::npos);

    CHECK(glob->execute("{}", context).error().code() == MaiErrorCode::InvalidInput);
}

// ── 5. grep ─────────────────────────────────────────────────────

void test_grep() {
    Workspace workspace;
    auto grep = makeMaiGrepTool();
    const auto context = workspace.context();

    const auto todo = grep->execute(args({{"pattern", "TODO"}}), context);
    CHECK(!todo.hasError());
    CHECK(todo.output().find("src/util.cpp") != std::string::npos);
    CHECK(todo.output().find(":2:") != std::string::npos);  // 带行号

    const auto filtered = grep->execute(args({{"pattern", "helper"}, {"glob", "*.h"}}), context);
    CHECK(!filtered.hasError());
    CHECK(filtered.output().find("util.h") != std::string::npos);
    CHECK(filtered.output().find("util.cpp") == std::string::npos);

    const auto none = grep->execute(args({{"pattern", "definitely-not-there-xyzzy"}}), context);
    CHECK(!none.hasError());
    // grep 说的是 "No content matches"，glob 说的是 "No files match"——两边措辞不同是有意的：
    // 以前都叫"没有匹配"，断言根本分不出是谁产出的。
    CHECK(none.output().find("No content matches") != std::string::npos);

    // 坏正则要给一句能改的话，而不是崩掉
    const auto bad = grep->execute(args({{"pattern", "([unclosed"}}), context);
    CHECK(bad.error().code() == MaiErrorCode::InvalidInput);

    // 中文能搜到
    const auto cn = grep->execute(args({{"pattern", kCjkLine2}}), context);
    CHECK(!cn.hasError());
    CHECK(cn.output().find("README.md") != std::string::npos);
}

// ── 6. 注册表 ───────────────────────────────────────────────────

void test_registry() {
    MaiToolRegistry reg;
    CHECK(reg.isEmpty());
    registerMaiBuiltinTools(reg);
    CHECK(!reg.isEmpty());

    for (const char* n : {"read", "write", "glob", "grep"}) {
        CHECK(reg.find(n) != nullptr);
    }
    CHECK(reg.find("no-such-tool") == nullptr);

    const auto schemas = reg.specs();
    CHECK(schemas.size() == 4);
    for (const auto& s : schemas) {
        CHECK(!s.name.empty());
        CHECK(!s.description.empty());
        // schema 必须是合法 JSON，否则请求体构造时会被悄悄换成空对象，模型就不知道参数怎么填了
        const auto parsed = json::parse(s.parametersJson, nullptr, false);
        CHECK(!parsed.is_discarded());
        if (!parsed.is_discarded()) {
            CHECK(parsed.contains("properties"));
            CHECK(parsed.contains("required"));
        }
    }

    // 同名覆盖而不是并存
    reg.add(makeMaiReadTool());
    CHECK(reg.specs().size() == 4);
}

void test_cancel_stops_traversal() {
    Workspace workspace;
    auto glob = makeMaiGlobTool();
    auto context = workspace.context();
    std::atomic<bool> canceled{true};
    context.cancel = &canceled;
    // 已取消时不该继续遍历。这里只验证它不崩、能立刻返回。
    const auto r = glob->execute(args({{"pattern", "**/*"}}), context);
    CHECK(!r.hasError() || r.error().code() != MaiErrorCode::Internal);
}

}  // namespace

// 每个用例跑之前先打一行，崩了的话能立刻看出停在哪一个。这次就是靠它定位的：
// 上一版整个进程直接挂掉，一条输出都没有。
#define RUN(f)                      \
    do {                            \
        std::printf("-> %s\n", #f); \
        std::fflush(stdout);        \
        f();                        \
    } while (0)

int main() {
    RUN(test_path_escape_is_blocked);
    RUN(test_sibling_with_shared_prefix_is_outside);
    RUN(test_write_cannot_escape);
    RUN(test_maiResolvePathWithinRoot_directly);
    RUN(test_read);
    RUN(test_write);
    RUN(test_glob);
    RUN(test_grep);
    RUN(test_registry);
    RUN(test_cancel_stops_traversal);
    if (failures == 0) std::printf("tool tests passed\n");
    return failures == 0 ? 0 : 1;
}
