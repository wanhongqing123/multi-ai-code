#pragma once

#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "MaiError.h"
#include "MaiEventBus.h"
#include "MaiMessage.h"
#include "MaiModelClient.h"
#include "MaiPermission.h"
#include "MaiQuestion.h"
#include "MaiSubAgent.h"
#include "MaiSession.h"
#include "MaiSessionStore.h"
#include "MaiTool.h"

// ── 能提交给核心的操作 ──────────────────────────────────────────
// 形状抄 codex（它的 core 对外就 submit(Op) / next_event() 两个方法）。
// 做成一个封闭的 variant 而不是一堆方法，是为了让"核心能做什么"可枚举：
// 加能力时编译器会强制每个 visit 都处理到，不会有人忘了改某个分支。
struct MaiCreateSession {
    std::string directory;
    std::string title;
    std::string model;
};

struct MaiUpdateSession {
    std::string sessionId;
    std::string title;  // 空表示不改
    std::string model;
    std::string agent;
    // 移动端更新后，应用沙箱的绝对路径可能改变。调用方传入当前工作目录时，
    // 必须同步修正持久化会话，否则历史图片仍会从旧容器路径读取。
    std::string directory;
};

struct MaiDeleteSession {
    std::string sessionId;
};

// 清掉这个会话的聊天记录，**会话本身留着**。
//
// 为什么不用"删了再建一个"顶替：
//
//   1. 会换 id。界面上只有一个固定的 AI 助手会话，换 id 意味着它要重新去认。
//   2. **会把"本会话都允许"那份授权记录一起丢掉**（MaiDeleteSession 里明确清了它）。
//      用户点"清空重来"想丢的是聊天记录，不是他刚给过的授权——而这件事他感觉不到，
//      只会发现它又开始一个一个问了。
//
// 正在跑的时候不让清（返回 Busy）：那一轮的 assistant 消息还在往库里写，
// 清了它下一次 putMessage 又会把消息塞回来，最后剩一条来历不明的半截记录。
// 界面应该先 MaiInterrupt，再清。
struct MaiClearMessages {
    std::string sessionId;
};

// 发一轮消息。**立刻返回**，真正的输出全部走事件流。
struct MaiSendPrompt {
    std::string sessionId;
    std::string text;
    std::vector<MaiModelImage> images;

    MaiSendPrompt(std::string sessionId, std::string text);
    MaiSendPrompt(std::string sessionId, std::string text, std::vector<MaiModelImage> images);
};

// 中断正在跑的那一轮。
struct MaiInterrupt {
    std::string sessionId;
};

// 对一次工具调用的授权请求作出裁决。
//
// 做成操作而不是直接调闸门，是为了让它和别的变更走同一条路：一样的错误码、一样的返回形状，
// 调用方不用为它单开一套处理。
struct MaiReplyPermission {
    std::string permissionId;
    MaiPermissionDecision decision = MaiPermissionDecision::Denied;
};

// 回答模型中途问的那句话。
//
// 和裁决授权分开而不是共用一个「回复请求」：一个的答案是枚举、另一个是自由文字，
// 合成一个操作的话调用方得先判断类型才知道该填哪个字段。
struct MaiReplyQuestion {
    std::string questionId;
    std::string answer;
};

using MaiOperation =
    std::variant<MaiCreateSession, MaiUpdateSession, MaiDeleteSession, MaiClearMessages,
                 MaiSendPrompt, MaiInterrupt, MaiReplyPermission, MaiReplyQuestion>;

// ── 核心的门面 ──────────────────────────────────────────────────
//
// 它只做四件事：接收操作、转发查询、持有依赖、暴露事件流。
// **跑一轮对话的逻辑不在这里**——那在 MaiTurnRunner，上下文组装在 MaiContextBuilder，
// 给会话起名在 MaiSessionTitler。
//
// Qt 桌面、iOS / Android、命令行都只认这一个类。
// 这里不出现任何 HTTP 或 JSON 的概念——这是"库是边界"的落点。
//
// 这条边界已经受过一次检验：给 Electron 界面用的那层 REST + SSE 适配器整个删掉时，
// 这个文件一行都没改。
//
// ── 怎么用 ──────────────────────────────────────────────────────
//
//   auto store = makeMaiMemoryStore();               // 或 makeMaiSqliteStore(path)
//   MaiModelConfig config;
//   config.baseUrl = "https://open.bigmodel.cn/api/paas/v4";
//   config.apiKey  = apiKey;
//   auto tools = std::make_unique<MaiToolRegistry>();
//   registerMaiBuiltinTools(*tools);
//
//   MaiAgent agent(std::move(store), makeMaiModelClient(config), std::move(tools));
//
//   // 先订阅，再发消息——否则会漏掉最前面的事件
//   agent.eventBus().subscribe([](const MaiEvent& event) { /* 刷界面 */ });
//
//   const std::string sessionId = agent.submit(MaiCreateSession{workdir, "", ""}).value();
//   agent.submit(MaiSendPrompt{sessionId, "你好"});   // 立刻返回，不等模型
//
// ── submit 是异步的 ─────────────────────────────────────────────
//
// **这是用这个类最容易搞错的地方。** MaiSendPrompt 提交之后立刻返回，
// 返回的只是"新建的 assistant 消息 id"，不是回答。
// 真正的输出全部通过事件流出来——模型每吐一点就是一条 MessagePartDelta。
//
// 同步等模型答完会让界面卡几十秒，所以这一层不提供那种接口。测试里要等结果用 waitIdle()。
//
// ── 线程 ────────────────────────────────────────────────────────
//
// **所有公开方法都可以从任意线程调用**，内部自己加锁。
//
// 每一轮对话跑在**自己的工作线程**上（一轮一个），所以：
//   - 多个会话真的并行，一个卡在等授权不影响别的；
//   - 跑着的时候照样可以查询（listMessages 等）；
//   - 事件处理函数在**发布事件的那个线程**上同步跑，流式期间那是网络
//     读线程——里面不要做慢活，有守卫盯着（见 MaiBlockingCheck.h）。
//
// ── 析构 ────────────────────────────────────────────────────────
//
// 析构会把所有在跑的轮次叫停并**等它们退出**，所以析构可能阻塞若干秒（要等 HTTP 传输真的断掉）。
// 不这么做的话工作线程会去访问已经销毁的 store 和 emitter。
//
// 推论：**事件订阅者必须活得比 MaiAgent 久**，否则析构过程中最后几条事件会调到悬空的处理函数上。
class MaiAgent final : public MaiSubAgentHost {
public:
    struct Options {
        // 会话没指定模型时用这个。会话上配了就用会话的（MaiUpdateSession）。
        std::string defaultModel = "glm-5.3";
        // 模型可以连着调工具，一轮对话因此会有多次请求。
        // 设上限是因为模型会绕圈——拿同样的参数反复调同一个工具，没上限就一直烧钱。
        int maxToolIterations = 12;
        // 每个会话同时只跑一轮。第二条来了直接拒绝而不是排队——排队会让用户以为消息丢了，
        // 界面上看不出区别。
        bool rejectWhenBusy = true;
        // 等用户授权的超时。0 = 无限等，理由见 MaiPermissionGate::Options。
        MaiMillis permissionTimeoutMs = 0;
        MaiApprovalPolicy approvalPolicy = MaiApprovalPolicy::OnRequest;
        // 模型的基础指令。和 Codex 的 base_instructions 一样独立于消息历史，空表示不注入。
        std::string baseInstructions;

        // 子 Agent 最多能套多少层。根会话是 0，所以 2 表示「孙子辈就到头了」。
        //
        // **必须有上限。** 不封的话模型能把自己 fork 到爆：每一层都觉得
        // 「这活该交出去」，而每一层都在烧钱。codex 也是这么卡的。
        int maxSubAgentDepth = 2;
        // 一棵会话树里同时开着的子 Agent 上限。收掉的不算。
        int maxOpenSubAgents = 8;
    };

    // 三个依赖**都被接管所有权**，活到 MaiAgent 析构为止。
    //
    // store 不能为空。model 可以为空：那样发消息会以 NotConfigured 收场，但建会话、
    // 查历史这些照常（M1 的空转骨架就是这个配置）。tools 可以为空：那是纯对话模式，
    // 模型收不到任何工具声明。
    MaiAgent(std::unique_ptr<MaiSessionStore> store, std::unique_ptr<MaiModelClient> model,
             std::unique_ptr<MaiToolRegistry> tools = nullptr);
    MaiAgent(std::unique_ptr<MaiSessionStore> store, std::unique_ptr<MaiModelClient> model,
             std::unique_ptr<MaiToolRegistry> tools, Options options);
    ~MaiAgent();
    MaiAgent(const MaiAgent&) = delete;
    MaiAgent& operator=(const MaiAgent&) = delete;

    // ── 查询 ────────────────────────────────────────────────────
    // 纯读，直接问存储，不经过操作队列。轮次跑着的时候也能查，
    // 查到的是**那一刻已经落库的内容**（流式期间 assistant 消息会随着每次工具调用逐步变长）。

    // 按 updated 倒序——界面左侧列表直接用这个顺序。
    std::vector<MaiSession> listSessions() const;
    // 会话不存在返回 false，out 不动。
    bool getSession(const std::string& id, MaiSession& out) const;
    // 按生成顺序。会话不存在时返回空 vector，和"会话存在但没有消息"分不开——要分清先用 getSession。
    std::vector<MaiMessage> listMessages(const std::string& sessionId) const;
    // 这个会话现在有没有一轮在跑。注意这是**那一瞬间**的答案，
    // 拿它去做"没跑就发消息"的判断是有竞态的——直接 submit，忙的话会返回 Busy，那个判断在锁里做。
    bool isBusy(const std::string& sessionId) const;
    bool setApprovalPolicy(MaiApprovalPolicy policy);
    MaiApprovalPolicy approvalPolicy() const;

    // 现在有哪些工具调用在等授权。
    //
    // 界面重连之后必须能补上这一份：SSE 断开的那个窗口期里发出的 permission.asked 是看不到的，
    // 没有这个查询，那一轮会一直挂着，而界面上什么都没显示。
    std::vector<MaiPermissionRequest> listPendingPermissions() const;

    // 还在等回答的提问。界面刷新后靠它重新摆出输入框——
    // 没有这个，断线重连窗口期里发出的提问就永远看不见了，那一轮会一直挂着。
    std::vector<MaiQuestionRequest> listPendingQuestions() const;

    // ── 子 Agent（MaiSubAgentHost）──────────────────────────────
    //
    // 这几个是给 spawn_agent 那组工具用的，不是给界面用的。界面要看子 Agent
    // 的话走 listSessions()，那里已经把非根会话滤掉了。
    MaiResult<std::string> spawnSubAgent(const std::string& parentSessionId,
                                         const std::string& taskName,
                                         const std::string& prompt) override;
    MaiError sendToSubAgent(const std::string& parentSessionId, const std::string& childSessionId,
                            const std::string& prompt) override;
    bool waitForSubAgent(const std::string& parentSessionId, const std::string& childSessionId,
                         MaiMillis timeoutMs, const std::atomic<bool>& cancel) override;
    std::vector<MaiSubAgentInfo> listSubAgents(const std::string& parentSessionId) override;
    MaiError closeSubAgent(const std::string& parentSessionId,
                           const std::string& childSessionId) override;
    std::string subAgentReport(const std::string& childSessionId) override;

    // ── 变更 ────────────────────────────────────────────────────

    // 提交一个操作。**立刻返回**，见上面"submit 是异步的"。
    //
    // 成功时返回受影响的对象 id：建会话返回 ses_...，
    // 发消息返回新建的 assistant 消息 id（msg_...），裁决授权返回 per_...。
    //
    // 失败时带错误码，调用方据此分支：NotFound（会话不存在）、Busy（这个会话已经有一轮在跑）、
    // InvalidInput（空 prompt 等）。界面据此决定是弹错误、还是提示"先等这一轮跑完"。
    MaiResult<std::string> submit(const MaiOperation& operation);

    // 等所有在跑的轮次结束。给测试和优雅退出用。
    void waitIdle();

    MaiEventBus& eventBus();
    const MaiEventBus& eventBus() const;

private:
    // pimpl。实现体叫 Runtime 而不是 Implementation：后者任何一个 pimpl 类都能叫，等于没说。
    // 这个名字说的是它装什么——运行时状态：依赖、事件管线、正在跑的轮次。
    //
    // 前向声明必须跟着写 struct（实现体内部全公开），否则 MSVC 报 C4099。
    struct Runtime;
    std::unique_ptr<Runtime> mRuntime;
};
