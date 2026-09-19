#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "MaiError.h"
#include "MaiModelClient.h"

// 工具执行时能看到的环境。
//
// 每次调用现场构造，工具实现**不要持有它**——里面的 cancel 指针指向那一轮对话的取消标志，
// 轮次结束后就悬空了。
struct MaiToolContext {
    std::string sessionId;

    // 所有文件操作的根，UTF-8 绝对路径。
    //
    // **工具不得访问这个目录之外的任何路径。** 模型会试——有时是它自己想看看 ../.env，
    // 有时是被提示词注入诱导的。这是安全边界，不是建议。
    //
    // 别自己拼路径判断，一律走 maiResolvePathWithinRoot()：那里会解析符号链接再逐段比对，
    // 而"消掉 .. 之后比字符串前缀"这种直觉写法有两个洞（符号链接、同前缀兄弟目录），都有用例守着。
    //
    // 会话没设工作目录时这里是空的，
    // 文件类工具要明确拒绝而不是退回到当前目录——那会让模型读到进程的工作目录。
    std::string root;

    // 指向这一轮的取消标志。**可能为空**，用 isCanceled() 别直接解引用。
    //
    // 跑得久的工具（遍历大目录、搜大仓库）要在循环里定期查它，否则用户按了停还要等它跑完。
    const std::atomic<bool>* cancel = nullptr;

    bool isCanceled() const;
};

// 一次工具执行的结果。
//
// **失败也是正常结果，不是异常。** 工具失败了要把原因告诉模型，
// 它才能换个做法——直接让整轮对话挂掉反而更糟。所以这里没有抛异常的路径。
class MaiToolResult {
public:
    // output 会原样进上下文喂给模型，所以：写给模型看，不是写给人看。
    // 长了要自己截断并把 truncated 置位。
    static MaiToolResult success(std::string output, bool truncated = false);

    // message 同样是给模型看的，要写成**它能据此改正**的话。比如"路径超出工作目录，
    // 只能访问工作目录内的文件"比"权限不足"有用。
    static MaiToolResult failure(MaiErrorCode code, std::string message);

    const std::string& output() const;
    const MaiError& error() const;

    // 输出被截断过。**一定要如实置位**：
    // 不置的话模型会把"看到的就是全部"当真——grep 只回了前 200 行，它会以为总共就 200 处匹配，
    // 然后基于这个错误前提往下做。
    bool isTruncated() const;
    bool hasError() const;

private:
    std::string mOutput;
    MaiError mError;
    bool mTruncated = false;
};

// 一个工具。
//
// 参数以 JSON 原文传入，由工具自己解析——核心不认识任何具体工具的参数结构，加新工具不用动核心。
class MaiTool {
public:
    virtual ~MaiTool() = default;

    // 工具名。模型按这个名字调，所以**不能改**——改了等于换了个工具，
    // 而历史里存着的旧调用记录会对不上。
    virtual std::string name() const = 0;

    // 给模型看的说明。它靠这个决定什么时候用这个工具。写得含糊模型会用错，
    // 写得啰嗦白烧 token（每次请求都原样发过去）。
    virtual std::string description() const = 0;

    // 给模型看的 JSON Schema 原文。
    virtual std::string parametersSchema() const = 0;

    // 需要用户点头才能跑吗。
    //
    // read / glob / grep 这类只读的返回 false；write / edit / shell 会改东西或执行命令，返回 true。
    // 权限闸门（MaiPermission.h）读这个标志决定要不要停下来问。
    //
    // 默认 false。**加会改东西的新工具时别忘了覆盖它**——忘了的后果是模型可以不经用户同意改文件，
    // 而且没有任何报错。
    virtual bool requiresApproval() const;

    // 真正干活。
    //
    // 线程：在那一轮对话的工作线程上调用，
    // 同一个工具实例**可能被多个会话并发调用**（注册表是共享的），所以实现要么无状态，
    // 要么自己加锁。现有的内置工具都是无状态的。
    //
    // argumentsJson 是模型给的原文，**可能不是合法 JSON**（尤其被截断时），
    // 解析失败要返回 InvalidInput，不能崩。
    //
    // 跑得久的实现要定期查 context.isCanceled()，否则用户按了停还得等。
    virtual MaiToolResult execute(const std::string& argumentsJson,
                                  const MaiToolContext& context) = 0;
};

// 工具注册表。
class MaiToolRegistry {
public:
    void add(std::unique_ptr<MaiTool> tool);
    MaiTool* find(const std::string& name) const;
    bool isEmpty() const;

    // 给模型的工具清单。
    std::vector<MaiToolSpec> specs() const;

private:
    std::vector<std::unique_ptr<MaiTool>> mTools;
};

// 第一批工具。read / glob / grep 是只读的；write 会改文件，标了需要审批，
// 等权限闸门就位后会真正拦一道。
std::unique_ptr<MaiTool> makeMaiReadTool();
std::unique_ptr<MaiTool> makeMaiWriteTool();
std::unique_ptr<MaiTool> makeMaiGlobTool();
std::unique_ptr<MaiTool> makeMaiGrepTool();

void registerMaiBuiltinTools(MaiToolRegistry& registry);

// 把用户给的路径解析成绝对路径，并确认它在 root 之内。失败返回空字符串。
//
// 单独暴露出来是为了能被单独测：它是整个工具层唯一的安全边界，混在各个工具里就没法保证每个都做对，
// 也没法集中测 `..`、符号链接、同前缀同级目录这些绕过手法。
std::string maiResolvePathWithinRoot(const std::string& root, const std::string& candidate);
