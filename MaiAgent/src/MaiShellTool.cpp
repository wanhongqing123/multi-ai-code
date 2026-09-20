#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "MaiProcess.h"
#include "MaiTool.h"

// shell 工具：跑一条命令，把它说的话给模型。
//
// 这是整套工具里最危险的一个，所以它的审批逻辑比别的都细：
// 不是「shell 要不要审批」，而是「**这一条命令**要不要审批」。

namespace {

using json = nlohmann::json;

constexpr int kDefaultTimeoutMs = 120000;
constexpr int kMaxTimeoutMs = 600000;
constexpr std::size_t kMaxOutputBytes = 48 * 1024;

json parseArguments(const std::string& raw) {
    json parsed = json::parse(raw, nullptr, false);
    return parsed.is_object() ? parsed : json::object();
}

// 命令行里第一个词，也就是要跑的程序。
//
// 只做最朴素的切分：按空白切，去掉引号。**不求精确**——它只用来生成审批的键
// 和给只读白名单查一下，判错的后果是多问一次用户，不是少问一次。
std::string programOf(const std::string& command) {
    std::size_t begin = command.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::string();
    std::size_t end = command.find_first_of(" \t\r\n", begin);
    std::string first = command.substr(begin, end == std::string::npos ? end : end - begin);
    if (first.size() >= 2 && (first.front() == '"' || first.front() == '\'') &&
        first.back() == first.front()) {
        first = first.substr(1, first.size() - 2);
    }
    // 带路径时只留最后一段：`/usr/bin/git` 和 `git` 是一回事。
    const std::size_t slash = first.find_last_of("/\\");
    if (slash != std::string::npos) first = first.substr(slash + 1);
    // Windows 上 `git.exe` 和 `git` 也是一回事。
    if (first.size() > 4) {
        std::string tail = first.substr(first.size() - 4);
        std::transform(tail.begin(), tail.end(), tail.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (tail == ".exe" || tail == ".cmd" || tail == ".bat") {
            first = first.substr(0, first.size() - 4);
        }
    }
    return first;
}

// 只读命令白名单：跑这些不用问。
//
// **白名单，不是黑名单。** 黑名单永远列不全——`rm` 挡住了还有 `find -delete`、
// `> file`、`python -c`。名单里只放「无论带什么参数都改不了东西」的程序。
//
// git 是个例外：`git status` 安全，`git push` 不安全，所以 git 整个不在名单里，
// 交给下面的子命令判断。
bool isReadOnlyProgram(const std::string& program) {
    static const char* const kReadOnly[] = {
        "ls",   "dir",  "pwd",  "echo", "cat",  "head", "tail", "wc",    "file",
        "stat", "find", "grep", "rg",   "which", "where", "whoami", "date", "uname",
        "df",   "du",   "env",  "printenv", "hostname", "tree", "diff", "cmp",
    };
    for (const char* candidate : kReadOnly) {
        if (program == candidate) return true;
    }
    return false;
}

// git 的只读子命令。`git status` 每一轮都要用，每次弹框会把用户训练成
// 条件反射点「允许」，那时候真正危险的那次也会被一起放过。
bool isReadOnlyGitCommand(const std::string& command) {
    static const char* const kReadOnlySubcommands[] = {
        "status", "log", "diff", "show", "branch", "remote", "blame", "describe", "rev-parse",
        "ls-files", "shortlog", "tag", "config",
    };
    const std::size_t programEnd = command.find_first_of(" \t");
    if (programEnd == std::string::npos) return false;
    std::size_t begin = command.find_first_not_of(" \t", programEnd);
    if (begin == std::string::npos) return false;
    const std::size_t end = command.find_first_of(" \t", begin);
    const std::string sub = command.substr(begin, end == std::string::npos ? end : end - begin);
    for (const char* candidate : kReadOnlySubcommands) {
        if (sub == candidate) {
            // `git config --global user.email x` 是写操作。带 --global / --system 就不算只读。
            return command.find("--global") == std::string::npos &&
                   command.find("--system") == std::string::npos &&
                   command.find("--replace-all") == std::string::npos;
        }
    }
    return false;
}

// 一条命令里有没有 shell 的控制字符。
//
// 有的话就不能只看第一个词了：`ls; rm -rf .` 的第一个词是 ls，但它真正干的是 rm。
// 这种一律要审批，不管第一个词多无害。
bool hasShellControlCharacters(const std::string& command) {
    return command.find_first_of(";&|><`$\n") != std::string::npos;
}

class ShellTool final : public MaiTool {
public:
    std::string name() const override {
        return "shell";
    }

    std::string description() const override {
        return "Run a shell command in the working directory and return its combined stdout and "
               "stderr. Use it to build, run tests, inspect git state, or anything else a "
               "terminal can do. Commands that only read are run immediately; anything else "
               "asks the user first.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("command":{"type":"string","description":"The command line to run"},)"
               R"("timeout_ms":{"type":"integer","description":"Give up after this many milliseconds, default 120000, max 600000"}},)"
               R"("required":["command"]})";
    }

    // 只有能确定是只读的才放行。看不懂、解析不了、带控制字符——一律问。
    // 这一层的兜底方向永远是「不放行」。
    bool requiresApproval(const std::string& argumentsJson) const override {
        const json args = parseArguments(argumentsJson);
        const std::string command = args.value("command", std::string{});
        if (command.empty()) return true;
        if (hasShellControlCharacters(command)) return true;

        const std::string program = programOf(command);
        if (program.empty()) return true;
        if (program == "git") return !isReadOnlyGitCommand(command);
        return !isReadOnlyProgram(program);
    }

    // 「这个会话以后都允许」只记到**程序**这一级。
    //
    // 记成 "shell" 的话，给 `npm test` 点一次「以后都允许」，后面的 `rm -rf` 也跟着放行了。
    std::string approvalKey(const std::string& argumentsJson) const override {
        const json args = parseArguments(argumentsJson);
        const std::string command = args.value("command", std::string{});
        const std::string program = programOf(command);
        // 带控制字符的命令不给会话级豁免：它真正跑什么不由第一个词决定，
        // 每一条都得单独问。给一个独一无二的键，等于这条永远进不了白名单。
        if (program.empty() || hasShellControlCharacters(command)) return "shell:<unsafe>";
        return "shell:" + program;
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const json args = parseArguments(raw);
        const std::string command = args.value("command", std::string{});
        if (command.empty()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "the command parameter is required");
        }
        if (context.root.empty()) {
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "this session has no working directory, so shell commands are unavailable");
        }

        MaiProcessOptions options;
        // 整行交给系统 shell 解释。
        //
        // 这里**刻意**打开 useShell：模型写的是终端里那种命令，管道和 && 是它表达
        // 意图的一部分，拆成 argv 数组反而会把意思改掉。代价是元字符会生效——
        // 所以上面的审批逻辑对带控制字符的命令一律要求用户点头。
        options.useShell = true;
        options.argv = {command};
        options.workingDirectory = context.root;
        options.maxOutputBytes = kMaxOutputBytes;
        options.cancel = context.cancel;
        const int requested = args.value("timeout_ms", kDefaultTimeoutMs);
        options.timeoutMs = std::max(1000, std::min(requested, kMaxTimeoutMs));

        MaiProcessResult process;
        const MaiError failure = maiRunProcess(options, process);
        if (failure.hasError()) {
            return MaiToolResult::failure(failure.code(), failure.message());
        }

        if (process.canceled) {
            return MaiToolResult::failure(MaiErrorCode::Canceled,
                                          "the command was interrupted by the user");
        }

        // 下面这些话都会原样进上下文，所以每一句都必须是真的——
        // 超时说成「命令失败了」会让模型以为是代码有问题，然后去改不该改的地方。
        std::string out = process.output;
        if (out.empty()) out = "(no output)";
        if (process.timedOut) {
            out += "\n\n[timed out after " + std::to_string(options.timeoutMs) +
                   " ms and was killed; it may have been waiting for input or running forever]";
        } else if (process.exitCode != 0) {
            out += "\n\n[exit code " + std::to_string(process.exitCode) + "]";
        }
        if (process.truncated) {
            out += "\n[output truncated; rerun with something that prints less]";
        }
        return MaiToolResult::success(std::move(out), process.truncated);
    }
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiShellTool() {
    return std::make_unique<ShellTool>();
}
