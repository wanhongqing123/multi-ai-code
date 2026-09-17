#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "MaiTime.h"

// ── 事件类型 ────────────────────────────────────────────────────
// 只做第一阶段要的 13 种。openapi.json 里有 89 种，其余 76 种属于
// pty / tui / lsp / mcp / vcs / workspace 这些第一阶段不做的子系统。
//
// 另外 spec 里有两套并行的流式协议：session.next.* 和 message.part.*。
// 翻过界面的消费端，它吃的是后者（session.next.* 全仓只有 2 处引用），
// 所以这里只产出 message.part.* 那一套。
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

// 事件载荷。刻意做成扁平结构而不是一个 JSON 对象：
// 流式期间每秒几十条，走 JSON DOM 等于每条一堆堆分配。
struct MaiEvent {
    std::string id;  // evt_...
    MaiEventType type = MaiEventType::SessionStatus;
    std::string sessionId;
    std::string messageId;
    std::string partId;

    // MessagePartDelta 用：field 说明增量打在哪个字段上（"text" 等），
    // delta 是这次追加的内容。**只带增量，不带全量**——全量重推是 CPU 杀手。
    std::string field;
    std::string delta;

    // 其它事件的附带信息（权限 id、错误文本、标题等）。
    std::string detail;
    MaiMillis time = 0;
};

// ── 事件总线 ────────────────────────────────────────────────────
// 核心里它是观察者接口，不是 SSE。HTTP 适配器订阅之后翻译成 SSE；
// 移动端 / 嵌入式直接订阅，完全不经过 HTTP。
//
// 线程契约：处理函数**在 publish 的那个线程上同步调用**，流式期间
// 那就是网络读线程。处理函数里不要做慢活，否则会拖慢模型读取；
// 需要慢处理自己塞队列（HTTP 适配器的 SSE 就是这么做的）。
class MaiEventBus {
public:
    using Handler = std::function<void(const MaiEvent&)>;
    using Token = std::uint64_t;

    MaiEventBus();
    ~MaiEventBus();
    MaiEventBus(const MaiEventBus&) = delete;
    MaiEventBus& operator=(const MaiEventBus&) = delete;

    Token subscribe(Handler handler);
    void unsubscribe(Token token);
    void publish(const MaiEvent& event);

    std::size_t subscriberCount() const;

private:
    // pimpl：实现体是 struct（内部全公开），前向声明必须跟着写 struct，
    // 否则 MSVC 会报 C4099。
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
