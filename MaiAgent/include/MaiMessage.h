#pragma once

#include <string>
#include <variant>
#include <vector>

#include "MaiTime.h"

// 一次对话里的消息和它的组成片段。
//
// **纯数据，没有行为**：不知道自己怎么被存、怎么上网、怎么变成 JSON。
// 领域模型独立于一切基础设施——换存储（内存 <-> SQLite）、换模型供应商、
// 换界面（Electron -> Qt）都不该动这个文件。
//
// 这也是为什么这里没有任何 include 指向 MaiSessionStore 或 MaiModelClient：依赖方向是单向的，
// 它们认识 MaiMessage，MaiMessage 不认识它们。
//
// ── 一条 assistant 消息长什么样 ────────────────────────────────
// 模型答一轮可能夹着工具调用，所以一条消息是若干**片段**按顺序排列的：
//
//   [reasoning "先看看文件"] [tool read(a.txt) -> "..."] [text "这个文件有两行"]
//
// 界面就按这个顺序渲染。片段顺序 = 生成顺序，
// 落库靠 id 的字典序还原（见 MaiIdGenerator.h 对单调性的许诺）。

// ── 消息片段 ────────────────────────────────────────────────────
//
// 用 std::variant 而不是继承 + 虚函数，理由有三个：
//   - 片段在流式期间高频读写，variant 是值语义、无堆分配、cache 友好；
//   - 穷尽 visit 时编译器会提醒漏掉的分支，加新片段类型不会悄悄漏处理；
//   - 纯数据结构体可以直接聚合初始化，写测试时不用造工厂。

// 模型说给用户听的话。界面上显示的正文就是它。
struct MaiTextPart {
    std::string text;
};

// 模型的思考过程（OpenAI 协议里的 reasoning_content）。
//
// **不回灌给模型**——它是草稿，回灌会污染下一轮上下文。MaiContextBuilder 组装历史时会跳过这种片段，
// 那里有一条用例盯着。
//
// 界面上通常折叠显示，所以它和 MaiTextPart 必须是**两个片段**而不是拼在一起：混成一个的话，
// 界面没办法只折叠思考那一段。
struct MaiReasoningPart {
    std::string text;
};

// 用户明确附加到这一轮的图片。path 可以是工作目录相对路径，也可以是宿主文件选择器
// 授权的绝对路径；只有模型线格式层读取它，文件工具无法凭空创建这种片段。
struct MaiImagePart {
    std::string path;
    std::string mimeType;
};

// 一次工具调用走到哪一步了。
//
// 这几个值和 SQLite 里 parts.state 列的数字绑死（见 MaiSqliteStore.cpp），**不要改它们的顺序**，
// 库里已经存下的行不会跟着变。加新值往后追加。
enum class MaiToolState {
    // 等用户授权。会改东西的工具（write）在闸门那儿停下时就是这个状态，界面靠它显示"等待授权"。
    // 见 MaiPermission.h。
    Pending,
    Running,    // 正在执行
    Completed,  // 跑完了，output 有效
    Error,      // 失败了，error 和 output 都放着失败原因（要回灌给模型）
};

const char* maiToolStateToString(MaiToolState state);

// 一次工具调用。从"模型说要调"到"结果回来了"，全程是**同一个片段**在原地更新状态，
// 不是每一步新建一个——界面据此做增量刷新，新建的话工具卡会闪。
struct MaiToolPart {
    std::string tool;  // read / write / glob / grep ...
    // 模型给的 tool_call_id，回灌结果时要**原样带回**。对不上的话模型认不出这是哪次调用的结果，
    // 下一轮会把同样的工具再调一遍。
    std::string callId;
    // 参数的 JSON 原文。核心**不解析**它，原样交给工具实现去解——这样加新工具不用动核心的任何代码。
    std::string input;
    // 成功时的输出。会被塞进上下文喂给模型，
    // 所以有长度上限（见 MaiFileTools.cpp 里的 kMaxOutputBytes），截断时会明确告诉模型。
    std::string output;
    // 失败原因。**失败时 output 里放的是同一句话**——模型要知道失败了才能换个做法，
    // 而它只读 tool 消息的 content。
    std::string error;
    MaiToolState state = MaiToolState::Pending;
};

using MaiMessagePartBody = std::variant<MaiTextPart, MaiReasoningPart, MaiImagePart, MaiToolPart>;

struct MaiMessagePart {
    // prt_...，**创建后永不改变**。
    //
    // 界面靠它做增量更新：message.part.delta 事件只带 partId 和这次新增的内容，
    // 界面找到对应的片段往后追加。id 变了就等于换了个片段，界面会当成新的渲染一遍。
    std::string id;
    MaiMessagePartBody body;
    MaiMillis created = 0;
};

// ── 消息 ────────────────────────────────────────────────────────

// 只有两种角色。
//
// 没有 System：系统提示词是 MaiContextBuilder 在组装请求时**临时加**的，不属于对话历史，
// 也不该被存下来或显示给用户。没有 Tool：工具结果是 assistant 消息里的一个片段（MaiToolPart），
// 不是独立的一条消息——那是 OpenAI 线格式的事，翻译发生在 MaiOpenAiClient。
enum class MaiRole {
    User,
    Assistant,
};

const char* maiRoleToString(MaiRole role);

struct MaiMessage {
    std::string id;  // msg_...
    MaiRole role = MaiRole::User;
    // 按生成顺序。界面直接照这个顺序渲染。
    std::vector<MaiMessagePart> parts;
    MaiMillis created = 0;
    // 这一轮结束的时间。**0 表示还在进行中**——流式期间 assistant 消息
    // 一直是 0，直到 MaiTurnRunner::finish 填上。
    MaiMillis completed = 0;

    bool isInProgress() const;

    // 把所有 MaiTextPart 拼起来。reasoning 和 tool 片段**不算**。
    //
    // 用在两个地方：给会话起标题（取用户第一句话），
    // 以及把历史组装成上下文时还原 assistant 说过的话。两处都只要"说出来的话"，
    // 不要草稿也不要工具输出。
    std::string text() const;
};
