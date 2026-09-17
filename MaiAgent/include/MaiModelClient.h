#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "MaiError.h"

// ── 中立的模型抽象 ──────────────────────────────────────────────
// 这里的结构**不是** OpenAI 的线格式，是我们自己的中间表示。
// 各个 wire 实现（Chat Completions / Responses / Anthropic）负责把它
// 翻译成自家的请求、把自家的响应翻译回这里的回调。
//
// 之前这一层直接长成了 OpenAI 的形状（tool_calls 原样照搬），
// 那样加第二个供应商时要么污染这个头，要么在上层写一堆 if。

enum class MaiWireApi {
    ChatCompletions,  // 第一版只实现这个
    Responses,        // 枚举先立着，加实现时只动工厂函数
};

const char* maiWireApiToString(MaiWireApi wire);

// 模型发起的一次工具调用。arguments 保持 JSON 原文——
// 核心不解析它，交给工具实现去解，这样加新工具不用动这一层。
struct MaiToolInvocation {
    std::string id;
    std::string name;
    std::string arguments;
};

// 发给模型的一条消息里，说话的是谁。
enum class MaiModelRole {
    System,
    User,
    Assistant,
    ToolResult,  // 一次工具调用的结果，协议里是 role="tool"
};

// 发给模型的一条消息。刻意和领域模型的 MaiMessage 分开：
// MaiMessage 是"我们怎么存"，这个是"模型怎么看"，两者演化节奏不一样。
struct MaiModelMessage {
    MaiModelRole role = MaiModelRole::User;
    std::string content;
    std::string toolCallId;                      // ToolResult 时必填
    std::vector<MaiToolInvocation> invocations;  // Assistant 发起调用时
};

// 一个工具对模型的完整声明。
struct MaiToolSpec {
    std::string name;
    std::string description;
    std::string parametersJson;  // JSON Schema 原文
};

struct MaiModelRequest {
    std::string model;
    std::vector<MaiModelMessage> messages;
    std::vector<MaiToolSpec> tools;
    double temperature = -1.0;  // < 0 表示不传，用服务端默认
};

// ── 流式回调 ────────────────────────────────────────────────────
//
// 线程契约（重要）：这些回调**在网络读线程上同步调用**。
// 实现方不要在里面做慢活（写 socket、落盘、等锁），否则会拖慢模型读取。
// 需要慢处理就自己塞队列——HTTP 适配器的 SSE 就是这么做的。
struct MaiStreamSink {
    std::function<void(std::string_view)> onText;
    std::function<void(std::string_view)> onReasoning;
    // 工具调用是**攒完整**才给的。流式过程中参数是碎片，中途给会是半截 JSON。
    std::function<void(const MaiToolInvocation&)> onToolCall;
};

struct MaiModelConfig {
    std::string baseUrl;
    std::string apiKey;
    MaiWireApi wire = MaiWireApi::ChatCompletions;
    long connectTimeoutSeconds = 20;
    long totalTimeoutSeconds = 0;  // 0 = 不限；流式对话可能很久
};

// 模型客户端。一个接口，多个 wire 实现。
class MaiModelClient {
public:
    virtual ~MaiModelClient() = default;

    // 阻塞直到这一次应答结束。
    // cancel 置 true 会让传输尽快中断——这是中断功能的落点，比等超时干净。
    // 主动取消返回的是 MaiErrorCode::Canceled，**不是故障**。
    virtual MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                            const std::atomic<bool>& cancel) = 0;

    virtual MaiWireApi wireApi() const = 0;
};

std::unique_ptr<MaiModelClient> makeMaiModelClient(MaiModelConfig config);
