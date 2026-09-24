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
// 各个 wire 实现（Chat Completions / Responses / Anthropic）负责把它翻译成自家的请求、
// 把自家的响应翻译回这里的回调。
//
// 之前这一层直接长成了 OpenAI 的形状（tool_calls 原样照搬），那样加第二个供应商时要么污染这个头，
// 要么在上层写一堆 if。

enum class MaiWireApi {
    ChatCompletions,  // 第一版只实现这个
    Responses,        // 枚举先立着，加实现时只动工厂函数
};

const char* maiWireApiToString(MaiWireApi wire);

// 模型发起的一次工具调用。
//
// **攒完整才交付**：流式过程中 arguments 可能是一个个 JSON 碎片到达的
// （见 MaiStreamSink::onToolCall 的说明），中途给出去会是半截 JSON。
//
// 注意"可能"两个字——**分不分片是供应商的自由，不是协议的规定**。
// 实测过 GLM-5.3（zhipuai coding plan）：tool_calls 永远只有一帧，arguments 整块给，
// 连 1.2 KB 的长参数也不切。所以拿 GLM 跑通**不等于**这段聚合验过了，它根本没被执行到。
//
// 真正证明这段逻辑的是 MaiModelClientTests：
// 那里喂的是对抗性分片——切点故意落在 JSON 的引号和冒号中间，而且从 chunk=1 开始每个块大小都跑一遍，
// 比任何真实供应商都狠。换供应商时别把这组用例删了。
struct MaiToolInvocation {
    // 服务端给的调用 id。回灌结果时要**原样带回**，对不上的话模型认不出这是哪次调用的结果，
    // 下一轮会把同样的工具再调一遍。
    std::string id;
    std::string name;  // 工具名，要能在 MaiToolRegistry 里查到
    // 参数的 JSON 原文。核心**不解析**它，交给工具实现去解——这样加新工具不用动这一层的任何代码。
    //
    // 模型有时会给出不合法的 JSON（尤其是被截断时），所以工具实现解析失败要当成"参数不对"返回错误，
    // 不能崩。
    std::string arguments;
};

// 发给模型的一条消息里，说话的是谁。
//
// 比 MaiRole（只有 User / Assistant）多两个，因为这是**模型看到的视角**：
// 临时的系统指令和工具结果在协议里都是独立消息，而在我们的领域模型里前者根本不存历史、
// 后者是 assistant 消息里的一个片段。稳定的基础指令单独放在 MaiModelRequest::baseInstructions。
enum class MaiModelRole {
    System,  // 某一圈临时追加的系统指令，不属于对话历史
    User,
    Assistant,
    ToolResult,  // 一次工具调用的结果。线格式里是 role="tool"
};

// 发给模型的一条消息。
//
// **刻意和领域模型的 MaiMessage 分开**：MaiMessage 是"我们怎么存"，这个是"模型怎么看"，
// 两者演化节奏不一样。比如 reasoning 片段存着但不回灌、
// 工具结果存成片段但发出去是独立消息——如果共用一个类型，
// 这些差异就得靠"某些字段在某些场景下不填"来表达，很快就没人说得清了。
//
// 谁来填：MaiContextBuilder 把历史 MaiMessage 翻成这个。谁来读：
// 各个 wire 实现（现在只有 MaiOpenAiClient）翻成自家的线格式。
struct MaiModelImage {
    // 相对路径必须在工作目录内；绝对路径只允许宿主在用户明确选择附件后传入。
    // 线格式实现负责读取并编码，模型与工具都不能创建这个字段。
    std::string path;
    std::string mimeType;
};

struct MaiModelMessage {
    MaiModelRole role = MaiModelRole::User;
    // 正文。assistant 发起工具调用的那条可以是空的（那时候 invocations 非空），
    // 线格式里对应 content: null。
    std::string content;
    std::vector<MaiModelImage> images;
    // ToolResult 时**必填**：这条结果对应哪次调用。线上字段名是 tool_call_id（snake_case），
    // 别写成驼峰——那个 bug 犯过一次，见 MaiModelClientTests 里的线格式用例。
    std::string toolCallId;
    // Assistant 发起调用时填。一条消息可以同时发起多个调用。
    std::vector<MaiToolInvocation> invocations;
};

// 一个工具对模型的完整声明。模型靠它决定什么时候调、怎么调。
//
// description 和 parametersJson 是**给模型看的提示词**，不是给人看的文档：写得含糊模型就会用错，
// 写得啰嗦就白烧 token。每次请求都会原样发过去。
struct MaiToolSpec {
    std::string name;
    std::string description;
    // JSON Schema 原文。
    // 这一层不校验它合不合法——序列化时解析失败会退化成空对象（见 MaiOpenAiClient 的 buildRequestBod
    // y）。
    std::string parametersJson;
};

// 一次请求的全部内容。每一轮对话会发好几次——模型每调一次工具就要重新问一遍，
// 所以 messages 会越滚越长（见 MaiContextBuilder 对增量缓存的说明，那是一号性能风险）。
struct MaiModelRequest {
    // 模型名。会话上配了就用会话的，否则用 MaiAgent::Options::defaultModel。
    std::string model;
    // 模型的基础指令。它独立于历史：Chat Completions 在线协议边界转成首条 system 消息，
    // Responses 实现应直接映射到顶层 instructions 字段。
    std::string baseInstructions;
    // 相对图片路径从这个目录解析，且不能越过它。桌面端由用户明确选择的图片可以传绝对路径，
    // 因而允许位于工作区之外；移动端应先复制进自己的稳定沙盒目录，再传相对路径。
    std::string workingDirectory;
    // 完整历史，按时间顺序。**每次都要带全**——协议是无状态的，少带了模型就没有上下文。
    std::vector<MaiModelMessage> messages;
    // 这一轮允许模型用的工具。空表示纯对话模式，
    // 线上会**整个省掉 tools 字段**而不是发一个空数组（有的服务端见到空数组会报错）。
    std::vector<MaiToolSpec> tools;
    // < 0 表示不传这个字段，用服务端默认。不用 0 当哨兵：
    // 0 是一个合法且有意义的取值（完全确定性输出）。
    double temperature = -1.0;
};

// ── 流式回调 ────────────────────────────────────────────────────
//
// 线程契约（重要）：这些回调**在网络读线程上同步调用**——具体说就是 libcurl 的写回调里面。
// 实现方不要在里面做慢活（写 socket、落盘、等锁），否则会直接拖慢模型吐字。需要慢处理就自己塞队列，
// 控制台的渲染线程就是这么做的（见 cli/MaiConsoleMain.cpp）。
//
// 回调可以为空，实现方调用前要判。比如只关心最终文本的调用方可以不设 onReasoning。
struct MaiStreamSink {
    // 正文增量。**只带这次新增的部分**，不是累计值——调用方自己往后拼。每秒会被调几十次。
    //
    // 参数是 string_view，指向的缓冲区**在回调返回后就不保证有效**了。要留着就自己拷一份。
    std::function<void(std::string_view)> onText;

    // 思考过程的增量（OpenAI 协议的 reasoning_content）。语义和 onText 一样，
    // 但要落成**另一个片段**——界面要能单独折叠它。
    std::function<void(std::string_view)> onReasoning;

    // 工具调用是**攒完整**才给的，不是增量。
    //
    // 流式过程中 arguments 是 JSON 字符串的碎片，按 index 分批到达：
    //     {index:0, id:"call_x", function:{name:"read", arguments:""}}
    //     {index:0,             function:{arguments:"{\"pa"}}
    //     {index:0,             function:{arguments:"th\":\"a.txt\"}"}}
    // 不同服务端分片时机不一样，有的整块给，有的一个字符一个字符给。中途交付会是半截 JSON，
    // 所以攒到流结束再一次性给。
    std::function<void(const MaiToolInvocation&)> onToolCall;
};

struct MaiModelConfig {
    // 不带末尾斜杠，也不带具体路径。客户端自己拼 "/chat/completions"。例如：
    //   https://open.bigmodel.cn/api/paas/v4   GLM
    //   http://127.0.0.1:11434/v1              Ollama
    std::string baseUrl;
    // 放进 Authorization: Bearer 头。
    std::string apiKey;
    MaiWireApi wire = MaiWireApi::ChatCompletions;
    long connectTimeoutSeconds = 20;
    // 0 = 不限。流式对话可能跑很久（模型想很长时间，或者输出很长），设了上限就会在半截把人掐掉。
    // 真要停用 cancel，那个干净得多。
    long totalTimeoutSeconds = 0;
    // 只在还没有向调用方交付任何正文/思考/工具调用时重试，避免流式内容重复。
    int maxRetries = 2;
    long retryInitialDelayMs = 250;
    // 空时使用平台默认根证书；Android 宿主可传系统信任库导出的 PEM 文件。
    // 只更换信任根来源，不能关闭证书或主机名校验。
    std::string caBundlePath;
};

// 模型客户端。一个接口，多个 wire 实现。
class MaiModelClient {
public:
    virtual ~MaiModelClient() = default;

    // 阻塞直到这一次应答结束。调用方负责把它放到自己的线程上。
    //
    // 回调在**这个线程**上同步触发（见 MaiStreamSink 的线程契约）。
    //
    // cancel 置 true 会让传输尽快中断——实现方要在收数据的循环里查它，这是中断功能的落点，
    // 比等超时干净得多。主动取消返回的是
    // MaiErrorCode::Canceled，**不是故障**，上层不该当错误报给用户。
    //
    // 返回错误时，**已经通过回调交付出去的内容仍然有效**——半截回答要保留下来，
    // 不要因为最后失败了就整段丢掉。
    virtual MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                            const std::atomic<bool>& cancel) = 0;

    virtual MaiWireApi wireApi() const = 0;
};

// 工厂在 MaiOpenAiClient.h——那是**实现**，这里是接口。
// 自己接别的供应商的人只需要这个文件，不该被迫看见我们碰巧提供了哪一种实现。
