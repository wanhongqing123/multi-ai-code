#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "MaiTime.h"

// 模型中途问用户一句，等回答。
//
// ── 和权限闸门的区别 ────────────────────────────────────────────
//
// 形状是一样的（登记 → 广播 → 阻塞等 → 有人回 → 醒），但语义不同，所以没有合并：
//
//   权限闸门  答案是**枚举**（准 / 本会话都准 / 不准），兜底方向必须是「不放行」，
//             它是安全边界。
//   这个      答案是**一段文字**，兜底是「没人回答」，不是安全问题。
//
// 把两者塞进一个类，最危险的后果是有人为了让问答更好用而放松了兜底方向，
// 而那条规则是权限闸门存在的全部理由。
//
// ── 为什么要有这个工具 ──────────────────────────────────────────
//
// 不要它其实也能凑合：模型把问题写在回答里，这一轮结束，用户回一句，下一轮继续。
// 差别在于**模型丢了现场**——它已经读了十个文件、跑了三次测试，那些结论都在上下文里，
// 但它得重新判断进行到哪一步了。有这个工具的话那一轮不结束，问完接着干。
//
// 所以它的适用面很窄：**只在「这一步选错了后面全白做」的时候用**。
// 什么都问一句的模型比什么都不问的更烦人，描述里写清楚了这一点。

struct MaiQuestionRequest {
    std::string id;  // qst_...
    std::string sessionId;
    std::string messageId;
    // 对应的工具 part。界面据此知道是哪次调用在等回答。
    std::string partId;
    std::string question;
    // 建议选项，可以为空。给了的话界面可以摆成按钮，用户不用打字。
    // **不是强制的**：用户照样可以回一句别的。
    std::vector<std::string> options;
    MaiMillis asked = 0;
};

// 问答闸门。
//
// **线程契约**和权限闸门一样：`ask()` 阻塞调用它的那个线程，直到有人回答、
// 被中断、或者超时。每一轮跑在自己的线程上，所以一个会话在等回答不影响别的会话。
class MaiQuestionGate {
public:
    // 登记完成之后、开始等之前调用。**必须在这个时机**——先广播再登记会漏掉
    // 抢在中间到达的回答，那次提问就永远醒不过来了。
    using Announce = std::function<void(const MaiQuestionRequest&)>;

    struct Options {
        // 0 = 无限等。
        //
        // 默认无限等：超时等于替用户做了决定，而用户可能只是走开了。
        // 等待期间会话一直是「在跑」状态，界面上看得见，随时可以中断。
        MaiMillis timeoutMs = 0;
    };

    explicit MaiQuestionGate(Options options = {});
    ~MaiQuestionGate();
    MaiQuestionGate(const MaiQuestionGate&) = delete;
    MaiQuestionGate& operator=(const MaiQuestionGate&) = delete;

    // 问一次，阻塞到有人回答。
    //
    // 回答为空表示**没等到**（被中断或超时）。调用方要把这两种情况如实告诉模型，
    // 不能编一个答案——编出来的答案会让它基于一个用户从没说过的决定往下做。
    std::string ask(const MaiQuestionRequest& request, const Announce& announce,
                    const std::atomic<bool>& cancel);

    // 回答。找不到这个 id 返回 false——界面重复点、或者对着已经结束的提问回答，
    // 都会走到这里，不是异常情况。
    bool reply(const std::string& questionId, const std::string& answer);

    // 界面刷新后要能重新看到还在等什么。没有这个，断线重连的窗口期里发出的
    // 提问就永远看不到了，那一轮会一直挂着。
    std::vector<MaiQuestionRequest> listPending() const;

    // 中断这个会话时把它所有待回答的提问叫醒。
    void cancelSession(const std::string& sessionId);

private:
    struct QuestionTable;
    std::unique_ptr<QuestionTable> mQuestions;
};
