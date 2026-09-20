// shell 工具的测试。
//
// 重点全在**审批粒度**上，那是这个工具唯一的安全阀。执行本身反而简单：
// 起个进程、收输出、到点杀掉。判错一次审批的后果是模型不经用户同意跑了命令，
// 而且没有任何报错——所以这部分的用例比执行部分多。
//
// 这些用例真的会起子进程。挑的都是三个平台都有的命令（echo / 一个不存在的名字），
// 不挑 ls 或 dir——那俩各平台只有一个。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#include <json.hpp>

#include "MaiProcess.h"
#include "MaiTool.h"
#include "MaiShellTool.h"

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

std::string args(const json& j) {
    return j.dump();
}

// 一条跑很久的命令，用来验超时和取消。
//
// **不能用 ping 或 timeout 拖时间**：它们在 System32 里，而测试跑在什么 PATH 下不好说——
// 实测这台机器上 ping 就不在 PATH 上，命令瞬间失败，于是「超时」那条用例
// 变成在测「命令找不到」，恒真通过。busy 循环只用 cmd / sh 的内置语法，不依赖任何 PATH。
#if defined(_WIN32)
const char* kSlowCommand = "for /L %i in (1,1,2000000000) do @rem";
#else
const char* kSlowCommand = "i=0; while [ $i -lt 2000000000 ]; do i=$((i+1)); done";
#endif

struct Workspace {
    fs::path root;

    Workspace() {
        root = fs::temp_directory_path() / ("maiagent-shell-" + std::to_string(std::rand()));
        fs::create_directories(root);
    }

    ~Workspace() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    MaiToolContext context() const {
        MaiToolContext c;
        c.sessionId = "ses_shell";
        c.root = root.string();
        return c;
    }
};

// ── 审批：只读的放行，别的都问 ──────────────────────────────────

void test_read_only_commands_run_without_asking() {
    auto tool = makeMaiShellTool();
    const char* const safe[] = {
        "ls -la", "pwd", "echo hello", "cat README.md", "grep -rn foo src", "which git",
        "git status", "git log --oneline -5", "git diff", "git branch",
    };
    for (const char* command : safe) {
        CHECK(!tool->requiresApproval(args({{"command", command}})));
    }
}

void test_anything_that_can_change_things_asks_first() {
    auto tool = makeMaiShellTool();
    const char* const risky[] = {
        "rm -rf build",
        "npm install",
        "make",
        "python setup.py install",
        // git 不是整个放行的：只读子命令放行，写操作照样问。
        "git push",
        "git commit -m x",
        "git reset --hard",
        // 改配置是写操作，即使子命令名在只读名单里。
        "git config --global user.email a@b.c",
    };
    for (const char* command : risky) {
        CHECK(tool->requiresApproval(args({{"command", command}})));
    }
}

void test_shell_metacharacters_always_ask() {
    // **这条是白名单能成立的前提。**
    //
    // `ls; rm -rf .` 的第一个词是 ls，可它真正干的是 rm。只看第一个词的话
    // 白名单就成了绕过通道——随便找个只读程序打头，后面接什么都放行。
    auto tool = makeMaiShellTool();
    const char* const sneaky[] = {
        "ls; rm -rf .",
        "echo hi && rm x",
        "pwd | sh",
        "cat x > y",
        "echo `rm -rf .`",
        "echo $(rm -rf .)",
        "ls\nrm -rf .",
    };
    for (const char* command : sneaky) {
        CHECK(tool->requiresApproval(args({{"command", command}})));
    }
}

void test_unparsable_arguments_ask() {
    // 兜底方向永远是「不放行」。看不懂的东西不能当成安全的。
    auto tool = makeMaiShellTool();
    CHECK(tool->requiresApproval("not json at all"));
    CHECK(tool->requiresApproval("{}"));
    CHECK(tool->requiresApproval(args({{"command", ""}})));
    CHECK(tool->requiresApproval(args({{"command", "   "}})));
}

void test_session_approval_is_scoped_to_the_program() {
    // 给 `npm test` 点一次「以后都允许」，不该连 `rm -rf` 一起放行。
    auto tool = makeMaiShellTool();
    CHECK(tool->approvalKey(args({{"command", "npm test"}})) == "shell:npm");
    CHECK(tool->approvalKey(args({{"command", "npm run build"}})) == "shell:npm");
    CHECK(tool->approvalKey(args({{"command", "rm -rf x"}})) == "shell:rm");
    CHECK(tool->approvalKey(args({{"command", "npm test"}})) !=
          tool->approvalKey(args({{"command", "rm -rf x"}})));

    // 带路径和扩展名的要归一化成同一个键，否则 `/usr/bin/git` 和 `git`
    // 会各自攒一份豁免，用户以为放行过了其实没有。
    CHECK(tool->approvalKey(args({{"command", "/usr/bin/git push"}})) == "shell:git");
    CHECK(tool->approvalKey(args({{"command", "C:\\\\tools\\\\git.exe push"}})) == "shell:git");

    // 带控制字符的命令**永远进不了白名单**：它真正跑什么不由第一个词决定。
    CHECK(tool->approvalKey(args({{"command", "ls; rm -rf ."}})) == "shell:<unsafe>");
}

// ── 执行 ────────────────────────────────────────────────────────

void test_runs_a_command_and_returns_its_output() {
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    auto tool = makeMaiShellTool();
    const MaiToolResult result =
        tool->execute(args({{"command", "echo maiagent-marker"}}), workspace.context());
    CHECK(!result.hasError());
    CHECK(result.output().find("maiagent-marker") != std::string::npos);
}

void test_a_failing_command_is_a_normal_result_not_an_error() {
    // 命令跑失败不是工具失败：模型要看到退出码和 stderr 才知道下一步怎么办。
    // 当成错误抛上去的话，那一轮直接断掉，模型连失败原因都看不到。
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    auto tool = makeMaiShellTool();
    const MaiToolResult result =
        tool->execute(args({{"command", "maiagent-no-such-command-xyz"}}), workspace.context());
    CHECK(!result.hasError());
    CHECK(result.output().find("exit code") != std::string::npos);
}

void test_stderr_is_interleaved_with_stdout() {
    // 编译器把错误写在 stderr、进度写在 stdout。分开收的话模型对不上号，
    // 所以两股合成一股。
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    auto tool = makeMaiShellTool();
#if defined(_WIN32)
    const char* command = "echo out-marker && maiagent-no-such-command-xyz";
#else
    const char* command = "echo out-marker; maiagent-no-such-command-xyz";
#endif
    const MaiToolResult result = tool->execute(args({{"command", command}}), workspace.context());
    CHECK(!result.hasError());
    CHECK(result.output().find("out-marker") != std::string::npos);
    // 找不到命令这句话是 shell 写到 stderr 上的。收不到就说明两股没合。
    CHECK(result.output().size() > std::string("out-marker").size() + 4);
}

void test_the_command_runs_in_the_working_directory() {
    // 不设工作目录的话模型会在**进程的当前目录**里跑命令，而那通常不是用户的项目。
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    auto tool = makeMaiShellTool();
#if defined(_WIN32)
    const char* command = "cd";
#else
    const char* command = "pwd";
#endif
    const MaiToolResult result = tool->execute(args({{"command", command}}), workspace.context());
    CHECK(!result.hasError());
    // 临时目录可能有符号链接（macOS 的 /var -> /private/var），所以只比最后一段。
    const std::string leaf = workspace.root.filename().string();
    CHECK(result.output().find(leaf) != std::string::npos);
}

void test_without_a_working_directory_it_refuses() {
    auto tool = makeMaiShellTool();
    MaiToolContext bare;
    bare.sessionId = "ses_shell";
    const MaiToolResult result = tool->execute(args({{"command", "echo hi"}}), bare);
    CHECK(result.hasError());
}

void test_a_hanging_command_is_killed_at_the_timeout() {
    // 模型写出 `tail -f` 这种命令是常事。没有超时的话那一轮就永远不结束。
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    auto tool = makeMaiShellTool();
    const char* command = kSlowCommand;
    const auto began = std::chrono::steady_clock::now();
    const MaiToolResult result =
        tool->execute(args({{"command", command}, {"timeout_ms", 1200}}), workspace.context());
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - began)
                          .count();
    CHECK(!result.hasError());
    // 到点就杀，不能等它自己跑完 20 秒。
    CHECK(took < 8000);
    // 回给模型的话必须说清是**超时被杀**，不能说成「命令失败了」——
    // 后者会让它以为是代码有问题，然后去改不该改的地方。
    CHECK(result.output().find("timed out") != std::string::npos);
}

void test_cancel_stops_a_running_command() {
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    auto tool = makeMaiShellTool();
    std::atomic<bool> cancel{false};
    MaiToolContext context = workspace.context();
    context.cancel = &cancel;

    std::thread stopper([&cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        cancel.store(true);
    });
    const char* command = kSlowCommand;
    const auto began = std::chrono::steady_clock::now();
    const MaiToolResult result =
        tool->execute(args({{"command", command}, {"timeout_ms", 60000}}), context);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - began)
                          .count();
    stopper.join();

    CHECK(took < 8000);
    CHECK(result.hasError());
    CHECK(result.error().code() == MaiErrorCode::Canceled);
}

void test_output_is_capped_and_says_so() {
    // 不封顶的话一条 `find /` 就能把整个上下文顶掉。
    // 封了就必须如实说截断过——不说的话模型会把看到的当成全部。
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    auto tool = makeMaiShellTool();
#if defined(_WIN32)
    const char* command = "for /L %i in (1,1,20000) do @echo aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
#else
    const char* command = "for i in $(seq 1 20000); do echo aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa; done";
#endif
    const MaiToolResult result =
        tool->execute(args({{"command", command}, {"timeout_ms", 30000}}), workspace.context());
    CHECK(!result.hasError());
    CHECK(result.isTruncated());
    CHECK(result.output().find("truncated") != std::string::npos);
    // 上限是 48KB，加上尾巴那几句提示，总长不该失控。
    CHECK(result.output().size() < 64 * 1024);
}

void test_shell_is_only_registered_where_processes_are_allowed() {
    // iOS 上跑不了外部进程。摆一个永远失败的工具比不摆更糟：模型会反复试，
    // 而它收到的「失败」听起来像临时故障，于是换个写法再试一遍。
    MaiToolRegistry registry;
    registerMaiBuiltinTools(registry);
    const bool listed = registry.find("shell") != nullptr;
    CHECK(listed == maiIsProcessExecutionSupported());

    // 其余工具不受平台影响，哪儿都该在。
    CHECK(registry.find("read") != nullptr);
    CHECK(registry.find("edit") != nullptr);
    CHECK(registry.find("current_time") != nullptr);
}

}  // namespace

#define RUN(f)                      \
    do {                            \
        std::printf("-> %s\n", #f); \
        std::fflush(stdout);        \
        f();                        \
    } while (0)

int main() {
    RUN(test_read_only_commands_run_without_asking);
    RUN(test_anything_that_can_change_things_asks_first);
    RUN(test_shell_metacharacters_always_ask);
    RUN(test_unparsable_arguments_ask);
    RUN(test_session_approval_is_scoped_to_the_program);
    RUN(test_runs_a_command_and_returns_its_output);
    RUN(test_a_failing_command_is_a_normal_result_not_an_error);
    RUN(test_stderr_is_interleaved_with_stdout);
    RUN(test_the_command_runs_in_the_working_directory);
    RUN(test_without_a_working_directory_it_refuses);
    RUN(test_a_hanging_command_is_killed_at_the_timeout);
    RUN(test_cancel_stops_a_running_command);
    RUN(test_output_is_capped_and_says_so);
    RUN(test_shell_is_only_registered_where_processes_are_allowed);
    if (failures == 0) std::printf("shell tool tests passed\n");
    return failures == 0 ? 0 : 1;
}
