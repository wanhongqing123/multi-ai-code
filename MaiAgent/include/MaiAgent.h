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
};

struct MaiDeleteSession {
    std::string sessionId;
};

// 发一轮消息。**立刻返回**，真正的输出全部走事件流。
struct MaiSendPrompt {
    std::string sessionId;
    std::string text;
};

// 中断正在跑的那一轮。
struct MaiInterrupt {
    std::string sessionId;
};

// 对一次工具调用的授权请求作出裁决。
//
// 做成操作而不是直接调闸门，是为了让它和别的变更走同一条路：
// 一样的错误码、一样的返回形状，适配器不用为它单开一套处理。
struct MaiReplyPermission {
    std::string permissionId;
    MaiPermissionDecision decision = MaiPermissionDecision::Denied;
};

using MaiOperation = std::variant<MaiCreateSession, MaiUpdateSession, MaiDeleteSession,
                                  MaiSendPrompt, MaiInterrupt, MaiReplyPermission>;

// ── 核心的门面 ──────────────────────────────────────────────────
//
// 它只做四件事：接收操作、转发查询、持有依赖、暴露事件流。
// **跑一轮对话的逻辑不在这里**——那在 MaiTurnRunner，上下文组装在
// MaiContextBuilder，给会话起名在 MaiSessionTitler。
//
// Qt 桌面、iOS / Android、命令行、HTTP 适配器都只认这一个类。
// 这里不出现任何 HTTP 或 JSON 的概念——这是"库是边界"的落点。
class MaiAgent {
public:
    struct Options {
        std::string defaultModel = "glm-5.3";
        // 模型可以连着调工具，一轮对话因此会有多次请求。设上限是因为
        // 模型会绕圈——拿同样的参数反复调同一个工具，没上限就一直烧钱。
        int maxToolIterations = 12;
        // 每个会话同时只跑一轮。第二条来了直接拒绝而不是排队——
        // 排队会让用户以为消息丢了，界面上看不出区别。
        bool rejectWhenBusy = true;
        // 等用户授权的超时。0 = 无限等，理由见 MaiPermissionGate::Options。
        MaiMillis permissionTimeoutMs = 0;
    };

    // model 可以为空：那样发消息会以 NotConfigured 收场，但其余功能照常。
    // tools 可以为空：那是纯对话模式，模型收不到任何工具。
    MaiAgent(std::unique_ptr<MaiSessionStore> store, std::unique_ptr<MaiModelClient> model,
             std::unique_ptr<MaiToolRegistry> tools = nullptr, Options options = {});
    ~MaiAgent();
    MaiAgent(const MaiAgent&) = delete;
    MaiAgent& operator=(const MaiAgent&) = delete;

    // ── 查询（纯读，不经过操作队列）──────────────────────────────
    std::vector<MaiSession> listSessions() const;
    bool getSession(const std::string& id, MaiSession& out) const;
    std::vector<MaiMessage> listMessages(const std::string& sessionId) const;
    bool isBusy(const std::string& sessionId) const;

    // 现在有哪些工具调用在等授权。
    //
    // 界面重连之后必须能补上这一份：SSE 断开的那个窗口期里发出的
    // permission.asked 是看不到的，没有这个查询，那一轮会一直挂着，
    // 而界面上什么都没显示。
    std::vector<MaiPermissionRequest> listPendingPermissions() const;

    // ── 变更 ────────────────────────────────────────────────────
    // 返回受影响的对象 id；发消息返回新建的 assistant 消息 id。
    // 失败时带错误码，调用方能区分"会话不存在"和"正忙"——
    // 之前只返回空字符串，上层只能猜。
    MaiResult<std::string> submit(const MaiOperation& operation);

    // 等所有在跑的轮次结束。给测试和优雅退出用。
    void waitIdle();

    MaiEventBus& eventBus();
    const MaiEventBus& eventBus() const;

private:
    // pimpl。实现体叫 Runtime 而不是 Implementation：后者任何一个
    // pimpl 类都能叫，等于没说。这个名字说的是它装什么——运行时状态：依赖、事件管线、正在跑的轮次。
    //
    // 前向声明必须跟着写 struct（实现体内部全公开），否则 MSVC 报 C4099。
    struct Runtime;
    std::unique_ptr<Runtime> mRuntime;
};
