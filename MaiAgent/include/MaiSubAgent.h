#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "MaiError.h"
#include "MaiTime.h"

// 子 Agent：让一个会话去起另一个会话干活，自己接着干别的。
//
// ── 子 Agent 到底是什么 ─────────────────────────────────────────
//
// **就是一个会话。** 它有自己的消息、自己的一轮、自己的工作线程、自己的授权记录。
// 父子关系只是会话上的一个 parentId 字段。codex 那边也是这么做的——
// 它的 sub-agent 就是一个 thread。
//
// 这样做的好处是几乎不用新东西：MaiAgent 本来就是一个会话一个 turn 线程、
// Busy 按会话判，多会话并发跑是现成的。
//
// ── 为什么值得有 ────────────────────────────────────────────────
//
// 一件事：**上下文隔离**。让子 Agent 去翻四十个文件找一处实现，回来只说结论，
// 那四十个文件的内容从来没进过父的上下文。父那边省下的不是时间，是"还能记住多少"。
//
// 附带的一件事是并行，但那是次要的——真正贵的是上下文，不是墙上时钟。
//
// ── 三条硬规矩 ──────────────────────────────────────────────────
//
// 1. **深度有上限。** 不封的话模型能把自己 fork 到爆——每一层都觉得"这活该交出去"。
// 2. **总数有上限。** 同上，而且每个子 Agent 都在烧钱。
// 3. **子只能比父弱，不能比父强。** 现在落在两条上：
//      工作目录  子**继承父的**，没有参数能改它。所以子看得见的文件是父的子集。
//      授权      会话级豁免是按 sessionId 记的，子是新会话，**天然不继承**。
//                父点过一次「以后都允许跑 rm」，子照样要重新问。这条不是刻意写的代码，
//                是数据结构自带的——但正因为不显眼，这里写清楚，免得以后有人
//                "顺手"把豁免改成按用户记。
//
// ── 这个接口为什么单独存在 ──────────────────────────────────────
//
// 工具层在 MaiAgent 下面，不能反过来认识那个门面（会成环，而且工具不该看见
// 整个应用）。所以只暴露子 Agent 需要的这几个动作，由 MaiAgent 实现。

struct MaiSubAgentInfo {
    std::string sessionId;
    // 起它的时候给的名字，报状态时用它指代，比一串 ses_ 好认。
    std::string taskName;
    // "running" / "idle" / "closed"。取值是给模型看的，别改字面量。
    std::string status;
    int depth = 0;
};

// 子 Agent 的宿主。MaiAgent 实现它。
class MaiSubAgentHost {
public:
    virtual ~MaiSubAgentHost() = default;

    // 起一个子 Agent 并把第一句话交给它。返回新会话的 id。
    //
    // 失败的两种都要**说清楚是哪一种**：深度到顶和数量到顶，模型的应对不一样
    //（一个是"别再往下分了"，一个是"先收掉几个"）。
    virtual MaiResult<std::string> spawnSubAgent(const std::string& parentSessionId,
                                                 const std::string& taskName,
                                                 const std::string& prompt) = 0;

    // 再给一个已经起来的子 Agent 派一句话。它必须是 parentSessionId 的孩子——
    // **不能拿它去戳别人的会话**，那等于横向越权。
    virtual MaiError sendToSubAgent(const std::string& parentSessionId,
                                    const std::string& childSessionId,
                                    const std::string& prompt) = 0;

    // 阻塞等这个子 Agent 这一轮跑完。等到了返回 true，超时或被中断返回 false。
    //
    // **这是阻塞调用**，只能在那一轮的工作线程上调，绝不能在事件处理函数里调——
    // 那里跑着网络读线程，MaiBlockingCheck 会当场炸。
    virtual bool waitForSubAgent(const std::string& parentSessionId,
                                 const std::string& childSessionId, MaiMillis timeoutMs,
                                 const std::atomic<bool>& cancel) = 0;

    // 这个会话起过哪些子 Agent，含已经收掉的。
    virtual std::vector<MaiSubAgentInfo> listSubAgents(const std::string& parentSessionId) = 0;

    // 收掉一个子 Agent：中断它并从「还开着」的账上划掉，腾出名额。
    // 消息不删——父之后还要能翻它说过什么。
    virtual MaiError closeSubAgent(const std::string& parentSessionId,
                                   const std::string& childSessionId) = 0;

    // 这个子 Agent 最后说的那段正文。等完之后拿它当"结论"回给父。
    // 没有就返回空串。
    virtual std::string subAgentReport(const std::string& childSessionId) = 0;
};
