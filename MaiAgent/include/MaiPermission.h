#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "MaiTime.h"

// 用户对一次工具调用的裁决。
//
// 取值照抄 codex 的 ReviewDecision（codex-rs/protocol/src/protocol.rs）。
// 原来叫 Once / AlwaysInSession / Reject，"Once" 根本没说是批准还是拒绝，得看另外两个值反推。
enum class MaiPermissionDecision {
    Approved,            // 批准这一次
    ApprovedForSession,  // 批准，且这个会话里这个工具以后别再问
    Denied,              // 不许跑，但这一轮继续，让模型换个做法

    // 没人裁决就到点了。**不能和 Denied 合并**：合并之后回灌给模型的话是"用户拒绝了这次调用"，
    // 而用户可能根本没看见那个请求。对模型说假话，它下一步就会基于假前提行动。
    //
    // 这个值只会由闸门自己产生，不接受从线上传进来。
    TimedOut,
};

// codex 还有两个值我们暂时没有：Denied 带一句用户写的理由，以及 Abort（拒绝并且整轮停下）。
// Abort 现在由 MaiInterrupt 覆盖。

const char* maiPermissionDecisionToString(MaiPermissionDecision decision);

// 解析界面传来的字符串。认不出来返回 false，**不要默认成 Once**——把没看懂的输入当成"允许"，
// 是这一层最不该犯的错。
bool maiParsePermissionDecision(const std::string& text, MaiPermissionDecision& out);

// 一次待裁决的请求。
struct MaiPermissionRequest {
    std::string id;  // per_...
    std::string sessionId;
    std::string messageId;
    // 对应的工具 part。界面据此知道是哪次调用在等授权，而那个 part 里已经有 tool / input，
    // 不用再塞一份进事件。
    std::string partId;
    std::string toolName;
    std::string arguments;  // 参数 JSON 原文
    MaiMillis asked = 0;
};

// 权限闸门：会改东西的工具跑之前，在这里停一下等用户点头。
//
// **线程契约**：`ask()` 阻塞**调用它的那个线程**，直到有人裁决。
// 每一轮对话跑在自己的线程上（见 MaiAgent::ApprovalTable），所以一个会话卡在
// 等授权，不影响其它会话继续跑。
//
// 为什么是阻塞而不是把这一轮拆成状态机：拆开之后"跑一轮"的代码就不再是从上往下读的了，
// 每个工具调用点都要能保存和恢复现场，而收益只是省下一个挂起的线程。等以后真换成线程池，
// `ask()` 这个接口可以原样保留，换里面的实现就行。
//
// 这个类不认识事件总线：它要能被单独测，不用搭一整套 agent。要广播"有人在等授权"，
// 通过 `ask()` 的 announce 回调。
class MaiPermissionGate {
public:
    // 注册完成之后、开始等之前调用。**必须在这个时机**——先广播再注册会漏掉抢在中间到达的裁决，
    // 那次请求就永远醒不过来了。
    using Announce = std::function<void(const MaiPermissionRequest&)>;

    struct Options {
        // 0 = 无限等。
        //
        // 默认无限等，是因为超时自动拒绝等于替用户做了决定，而用户可能只是去倒了杯水。
        // 等待期间会话一直是"在跑"状态，界面上看得见，用户随时可以中断。
        // 无人值守的场景可以设一个值。
        MaiMillis timeoutMs = 0;
    };

    explicit MaiPermissionGate(Options options = {});
    ~MaiPermissionGate();
    MaiPermissionGate(const MaiPermissionGate&) = delete;
    MaiPermissionGate& operator=(const MaiPermissionGate&) = delete;

    // 问一次，阻塞到有结果。被中断或超时都返回 Reject。
    MaiPermissionDecision ask(const MaiPermissionRequest& request, const Announce& announce,
                              const std::atomic<bool>& cancel);

    // 裁决。找不到这个 id 返回 false——界面重复点、或者对着已经结束的请求点，都会走到这里，
    // 不是异常情况。
    bool reply(const std::string& permissionId, MaiPermissionDecision decision);

    // 用户之前选了"本会话都允许"。
    bool isAllowedInSession(const std::string& sessionId, const std::string& toolName) const;

    // 界面刷新后要能重新看到还在等什么。没有这个，
    // SSE 断开重连的那个窗口期里发出的 permission.asked 就永远看不到了，那一轮会一直挂着。
    std::vector<MaiPermissionRequest> listPending() const;

    // 中断这个会话时把它所有待裁决的请求叫醒并按拒绝处理。
    void cancelSession(const std::string& sessionId);

    // 会话删除时清掉它的"本会话都允许"记录。不清的话，重建一个同 id 的会话会继承上一个的授权。
    void forgetSession(const std::string& sessionId);

private:
    struct ApprovalTable;
    std::unique_ptr<ApprovalTable> mApprovals;
};
