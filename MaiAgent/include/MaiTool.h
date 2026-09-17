#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "MaiError.h"
#include "MaiModelClient.h"

// 工具执行时能看到的环境。
struct MaiToolContext {
    std::string sessionId;
    // 所有文件操作的根。**工具不得访问这个目录之外的任何路径**——
    // 模型会试（有意或被提示词注入诱导），这是安全边界不是建议。
    std::string root;
    const std::atomic<bool>* cancel = nullptr;

    bool isCanceled() const;
};

class MaiToolResult {
public:
    static MaiToolResult success(std::string output, bool truncated = false);
    static MaiToolResult failure(MaiErrorCode code, std::string message);

    const std::string& output() const;
    const MaiError& error() const;
    // 输出被截断过。要告诉模型，否则它会把"看到的就是全部"当真——
    // 比如 grep 只回了前 200 行，它会以为只有 200 处匹配。
    bool isTruncated() const;
    bool hasError() const;

private:
    std::string output_;
    MaiError error_;
    bool truncated_ = false;
};

// 一个工具。
//
// 参数以 JSON 原文传入，由工具自己解析——核心不认识任何具体工具的
// 参数结构，加新工具不用动核心。
class MaiTool {
public:
    virtual ~MaiTool() = default;

    virtual std::string name() const = 0;
    virtual std::string description() const = 0;
    // 给模型看的 JSON Schema 原文。
    virtual std::string parametersSchema() const = 0;

    // 需要用户点头才能跑吗。read / glob / grep 这类只读的不需要；
    // write / edit / shell 会改东西或执行命令，需要。权限闸门读这个标志。
    virtual bool requiresApproval() const;

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
    std::vector<std::unique_ptr<MaiTool>> tools_;
};

// 第一批工具。read / glob / grep 是只读的；write 会改文件，
// 标了需要审批，等权限闸门就位后会真正拦一道。
std::unique_ptr<MaiTool> makeMaiReadTool();
std::unique_ptr<MaiTool> makeMaiWriteTool();
std::unique_ptr<MaiTool> makeMaiGlobTool();
std::unique_ptr<MaiTool> makeMaiGrepTool();

void registerMaiBuiltinTools(MaiToolRegistry& registry);

// 把用户给的路径解析成绝对路径，并确认它在 root 之内。失败返回空字符串。
//
// 单独暴露出来是为了能被单独测：它是整个工具层唯一的安全边界，
// 混在各个工具里就没法保证每个都做对，也没法集中测 `..`、符号链接、
// 同前缀同级目录这些绕过手法。
std::string maiResolvePathWithinRoot(const std::string& root, const std::string& candidate);
