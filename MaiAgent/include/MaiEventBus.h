#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "MaiTime.h"

// ── 事件类型 ────────────────────────────────────────────────────
// 只做第一阶段要的 13 种。openapi.json 里有 89 种，
// 其余 76 种属于 pty / tui / lsp / mcp / vcs / workspace 这些第一阶段不做的子系统。
//
// 另外 spec 里有两套并行的流式协议：session.next.* 和 message.part.*。翻过界面的消费端，
// 它吃的是后者（session.next.* 全仓只有 2 处引用），所以这里只产出 message.part.* 那一套。
enum class MaiEventType {
    SessionCreated,
    SessionUpdated,
    SessionDeleted,
    SessionIdle,
    SessionError,
    SessionStatus,
    MessageUpdated,
    MessageRemoved,
    MessagePartUpdated,
    MessagePartDelta,
    MessagePartRemoved,
    PermissionAsked,
    PermissionReplied,
};

// 转成 "message.part.delta" 这类线上字符串。
const char* maiEventTypeToString(MaiEventType type);

// 事件载荷。刻意做成扁平结构而不是一个 JSON 对象：流式期间每秒几十条，
// 走 JSON DOM 等于每条一堆堆分配。
struct MaiEvent {
    std::string id;  // evt_...
    MaiEventType type = MaiEventType::SessionStatus;
    std::string sessionId;
    std::string messageId;
    std::string partId;

    // MessagePartDelta 用：field 说明增量打在哪个字段上（"text" 等），delta 是这次追加的内容。
    // **只带增量，不带全量**——全量重推是 CPU 杀手。
    std::string field;
    std::string delta;

    // PermissionAsked / PermissionReplied 用。
    //
    // 单独一个字段而不是塞进 detail：权限事件同时要带"是哪一次请求"和"裁决是什么"两样东西，
    // 两样都往 detail 里挤，解析方就得约定分隔符——那种约定没人会去读，
    // 只会在某天出现带分隔符的内容时静默出错。
    //
    // 请求的工具名和参数不放在这里：它们已经在 partId 指向的那个 MaiToolPart 上了，
    // 重复一份就会有两个真相。
    std::string permissionId;

    // 其它事件的附带信息（错误文本、标题、权限裁决等）。
    std::string detail;
    MaiMillis time = 0;
};

// ── 事件总线 ────────────────────────────────────────────────────
// 这是个观察者接口，**不是**任何一种网络协议。界面直接订阅就行：控制台、Qt、移动端、
// 嵌入式都是进程内订阅，不经过 HTTP。
//
// （曾经有过一个适配器把它翻译成 SSE 喂 Electron 界面，那层已经删了。
// 这个接口当时也一个字没改——它本来就不该知道下游是什么。）
//
// 线程契约：处理函数**在 publish 的那个线程上同步调用**，流式期间那就是网络读线程。
// 处理函数里不要做慢活，否则会拖慢模型读取；需要慢处理自己塞队列（控制台的渲染线程就是这么做的，
// 见 cli/MaiConsoleMain.cpp）。
class MaiEventBus {
public:
    using Handler = std::function<void(const MaiEvent&)>;
    using Token = std::uint64_t;

    MaiEventBus();
    ~MaiEventBus();
    MaiEventBus(const MaiEventBus&) = delete;
    MaiEventBus& operator=(const MaiEventBus&) = delete;

    // 订阅。返回的 token 拿来退订，**别丢掉**——丢了就退不掉了。
    //
    // 可以从任意线程调用，也可以在处理函数里面调（发布时用的是订阅表的快照，不会边遍历边改）。
    //
    // 新订阅者**看不到之前的事件**，这里没有回放。
    // 界面重连之后要自己去拉一次当前状态（listMessages / listPendingPermissions），
    // 否则断线那段时间发生的事就永远看不见了。
    Token subscribe(Handler handler);

    // 退订。token 不存在就什么也不做，不算错——重复退订、或者对着已经销毁的总线退订，都会走到这里。
    //
    // 注意：**退订返回之后，处理函数仍可能正在别的线程上执行**。处理函数捕获的东西要么活得够久，
    // 要么用 shared_ptr 保住。
    void unsubscribe(Token token);

    // 发布。**同步**调用所有处理函数，全部返回后这个函数才返回。
    //
    // 处理函数跑在**调用 publish 的这个线程**上，流式期间那是网络读线程。
    // 里面不许做慢活——有守卫盯着，违反了当场终止并打印原因（见 MaiBlockingCheck.h）。
    //
    // 处理函数里再 publish 是可以的（不持锁调用），但要自己小心别写成无限递归。
    //
    // 某个处理函数抛异常的话会一路往上抛，后面的处理函数收不到这条事件。所以处理函数不要抛。
    void publish(const MaiEvent& event);

    // 当前订阅者个数。给测试和诊断用，不要拿它做逻辑判断——读到的那一刻别的线程可能正在订阅或退订。
    std::size_t subscriberCount() const;

private:
    // pimpl。实现体叫 SubscriberTable 而不是 Implementation：后者任何一个 pimpl 类都能叫，
    // 等于没说。这个名字说的是它装什么——订阅表：处理函数、读写锁、下一个 token。
    //
    // 前向声明必须跟着写 struct（实现体内部全公开），否则 MSVC 报 C4099。
    struct SubscriberTable;
    std::unique_ptr<SubscriberTable> mSubscribers;
};
