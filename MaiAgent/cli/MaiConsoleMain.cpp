// maiagent-console：直接链库的交互式控制台。
//
// ── 它是这个仓库里唯一的宿主进程 ────────────────────────────────
//
// 以前还有一个 maiagent-bridge：核心 + REST/SSE 适配器，637 行，存在的唯一理由是
// 当时的界面是 Electron、JS 写的、调不了 C++。那个界面撤了，那层壳跟着删了，
// **核心一行都没改**——那就是当初要有这条边界的全部意义。
//
// 所以现在它同时是证明和工具：
//
//   证明  CMake 里这个目标只链 `maiagent`，而 httplib 只在
//         mai_thirdparty_for_tests 里。哪天有人把 HTTP 服务端漏进核心，
//         它当场链不过。tests/e2e/console_e2e.py 还会翻产出的 exe 复核一遍。
//   工具  Qt 界面就位之前，调 agent 跑这一个进程就够。移动端和嵌入式将来的
//         调用方式和这里一模一样：构造 MaiAgent，订阅事件总线，submit 操作。
//
// ── 为什么不做成 TUI ────────────────────────────────────────────
//
// 逐行读写，不接管终端：不进 raw mode，不画光标，不重排屏幕。做 TUI 要在三个平台上
// 各写一套终端控制，那是另一件事，和这里要证明的东西无关。
//
// 代价是模型流式输出的时候用户在"盲打"——敲进去的命令照样收得到（主线程一直挂在读输入上），
// 只是屏幕上会和模型的输出混在一起。所以中断、授权这些命令都做得很短（/n、/y）。
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#if defined(_WIN32)
// windows.h 必须排在标准库**后面**：它会 #define max / min 这类很短的名字，
// 排在前面会污染标准库头。
#include <windows.h>
#endif

#include "MaiAgent.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiThread.h"

namespace {

// ── 控制台的编码 ────────────────────────────────────────────────
//
// 这一段是纯粹的平台适配，和 agent 无关，但少了它中文全是乱码。
//
// 核心内部一路到模型都是 UTF-8。Windows 控制台不是：它按**当前代码页**收发字节，
// 中文机器上是 GBK。直接把 UTF-8 字节 fwrite 给 stdout，屏幕上出来的是一堆问号；
// 直接把控制台给的字节当 UTF-8 送进核心，模型收到的是乱码。两个方向都得转。
//
// 这个坑在这个项目里已经付过两次代价了——端到端脚本刻意不走 shell 传参就是为了躲开它
// （见 tests/e2e/console_e2e.py 开头）。

#if defined(_WIN32)

// 只有**真的控制台**才要转宽字符。输出被重定向到文件或管道时（`maiagent-console > out.txt`、
// 或者端到端脚本用管道喂它）WriteConsoleW 会失败，那种情况下原样写 UTF-8 字节才是对的——
// 文件里存 UTF-8，拿到的东西是能用的。GetConsoleMode 成功就是"这是个控制台"。
bool isRealConsole(HANDLE handle) {
    DWORD mode = 0;
    return handle != INVALID_HANDLE_VALUE && ::GetConsoleMode(handle, &mode) != 0;
}

std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int length =
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                          length);
    return wide;
}

std::string wideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string utf8(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(),
                          length, nullptr, nullptr);
    return utf8;
}

#endif

// 写一段 UTF-8 到标准输出。
void writeUtf8(const std::string& text) {
    if (text.empty()) return;
#if defined(_WIN32)
    const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (isRealConsole(out)) {
        const std::wstring wide = utf8ToWide(text);
        DWORD written = 0;
        ::WriteConsoleW(out, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr);
        return;
    }
#endif
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

// 从标准输入读一行，不含行尾。返回 false 表示到头了（EOF / Ctrl+Z / Ctrl+D）。
//
// 空字符串 + true 是合法结果：用户直接敲了回车。
bool readLineFromStdio(std::string& out) {
    out.clear();
    char chunk[512];
    bool sawInput = false;
    while (std::fgets(chunk, sizeof(chunk), stdin) != nullptr) {
        sawInput = true;
        out += chunk;
        if (!out.empty() && out.back() == '\n') break;
    }
    if (!sawInput) return false;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return true;
}

bool readLineUtf8(std::string& out) {
#if defined(_WIN32)
    const HANDLE in = ::GetStdHandle(STD_INPUT_HANDLE);
    if (isRealConsole(in)) {
        std::wstring line;
        wchar_t chunk[512];
        bool sawInput = false;
        for (;;) {
            DWORD read = 0;
            if (::ReadConsoleW(in, chunk, 512, &read, nullptr) == 0 || read == 0) break;
            sawInput = true;
            line.append(chunk, read);
            if (line.find(L'\n') != std::wstring::npos) break;
        }
        if (!sawInput) return false;
        // Ctrl+Z 是作为一个**字符**送上来的，不是"读到 0 字节"。
        if (line.find(L'\x1a') != std::wstring::npos) return false;
        while (!line.empty() && (line.back() == L'\n' || line.back() == L'\r')) line.pop_back();
        out = wideToUtf8(line);
        return true;
    }
#endif
    return readLineFromStdio(out);
}

// ── 输出线程 ────────────────────────────────────────────────────

// 事件处理函数不能直接往控制台写。三个理由，后两个都是实打实会出事的：
//
//   1. 处理函数跑在**发布事件的那个线程**上，流式期间那是网络读线程
//      （见 MaiEventBus.h 的线程契约）。往控制台写慢了就是拖慢模型读取。
//
//   2. Windows 控制台开着"快速编辑模式"（默认就是开的）时，用户一旦用鼠标选中一段文字，
//      所有写操作就**一直阻塞到他取消选中**。那一下会把网络读线程整个冻住，
//      表现是"模型突然不动了"——而没人会把它和鼠标选中联系起来。
//
//   3. 排版一条事件需要知道片段的内容，而事件只带 id，内容要去问存储（可能是 SQLite）。
//      那是慢活，在处理函数里做会被守卫当场打死（见 MaiBlockingCheck.h）。
//
// 所以处理函数只把事件塞进队列（一把锁 + 一次拷贝），排版和真正的写都由这个线程做。
// 顺带还保证了输出是**单一写者**：主线程打印的提示符也走这里，不会和模型的输出交错成一团。
//
// 形状参考 codex 的 EventProcessor（codex-rs/exec/src/event_processor.rs）：
// 那边也是把"事件怎么变成人看的文字"单独拎出来，和跑 agent 的代码分开。
class MaiConsoleRenderer {
public:
    MaiConsoleRenderer() = default;
    ~MaiConsoleRenderer();
    MaiConsoleRenderer(const MaiConsoleRenderer&) = delete;
    MaiConsoleRenderer& operator=(const MaiConsoleRenderer&) = delete;

    // 起渲染线程。**必须在 eventBus().subscribe() 之前调用。**
    //
    // agent 只用来查片段详情，不会往里提交操作。它必须活得比这个渲染器**短**——
    // MaiAgent 析构时还会发最后几条事件，渲染器先死的话那几条就打到悬空引用上了
    // （见 MaiAgent.h 的析构一节）。main 里靠声明顺序保证：渲染器先声明，agent 后声明。
    void start(MaiAgent& agent, bool showReasoning);

    // 事件处理函数调这个。只入队，立刻返回。
    void onEvent(const MaiEvent& event);

    // 主线程打印自己的东西也走这里，保证输出是单一写者。
    void say(std::string text);

    // 停掉渲染线程，**把队列里剩下的都写完**再返回。
    void stop();

private:
    // 队列里的一项：要么是一条待排版的事件，要么是已经排好的一段文字。
    struct Entry {
        bool isEvent = false;
        MaiEvent event;
        std::string text;
    };

    // 记住一个片段是什么形态。用来做两件事：
    //
    //   1. 分清正文和思考过程。两种增量的 field 都是 "text"，区别只在 partId 指向的片段
    //      是 MaiTextPart 还是 MaiReasoningPart（见 MaiTurnRunner.cpp 里那两个 sink）。
    //      首个增量之前一定先有一条 message.part.updated，那时查一次记下来就够。
    //
    //   2. 工具卡只在状态**真的变了**的时候打一行。同一个工具会连着来三条
    //      message.part.updated（建卡、放行、跑完），不记状态就会打三遍一样的话。
    struct PartMemo {
        enum class Kind { Text, Reasoning, Tool } kind = Kind::Text;
        MaiToolState toolState = MaiToolState::Pending;
        bool toolStateKnown = false;
    };

    void drain();
    std::string format(const MaiEvent& event);
    std::string formatPartUpdated(const MaiEvent& event);
    bool findPart(const MaiEvent& event, MaiMessagePart& out) const;

    MaiAgent* mAgent = nullptr;
    bool mShowReasoning = false;
    std::thread mThread;
    mutable std::mutex mMutex;
    std::condition_variable mReady;
    std::deque<Entry> mQueue;
    bool mStopping = false;

    // 只有渲染线程碰这两个，不用加锁。
    std::map<std::string, PartMemo> mParts;
    // 上一次写出去的东西是不是以换行结尾。模型的增量不带换行，
    // 而工具卡、提示这些必须从行首开始——不补这一个换行的话它们会接在半句话后面。
    bool mAtLineStart = true;
};

MaiConsoleRenderer::~MaiConsoleRenderer() {
    stop();
}

void MaiConsoleRenderer::start(MaiAgent& agent, bool showReasoning) {
    mAgent = &agent;
    mShowReasoning = showReasoning;
    mThread = std::thread([this] { drain(); });
}

void MaiConsoleRenderer::stop() {
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mStopping) return;
        mStopping = true;
    }
    mReady.notify_all();
    if (mThread.joinable()) mThread.join();
}

void MaiConsoleRenderer::onEvent(const MaiEvent& event) {
    Entry entry;
    entry.isEvent = true;
    entry.event = event;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mQueue.push_back(std::move(entry));
    }
    mReady.notify_one();
}

void MaiConsoleRenderer::say(std::string text) {
    Entry entry;
    entry.text = std::move(text);
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mQueue.push_back(std::move(entry));
    }
    mReady.notify_one();
}

void MaiConsoleRenderer::drain() {
    MaiThread::setCurrentName("mai-console");
    for (;;) {
        std::deque<Entry> batch;
        {
            std::unique_lock<std::mutex> lock(mMutex);
            mReady.wait(lock, [this] { return mStopping || !mQueue.empty(); });
            // 停了也要把剩下的写完：最后那几条事件（SessionIdle、错误）正是用户最想看的。
            if (mQueue.empty()) return;
            batch.swap(mQueue);
        }
        // 一批攒成一次写。流式期间每秒几十条增量，一条一次 WriteConsoleW 是纯浪费，
        // 而控制台的写本来就可能很慢（见这个类开头的第 2 条）。
        //
        // mAtLineStart 要**在循环里逐条更新**，不能等整批拼完再更新一次：
        // 同一批里前一条的输出决定了后一条在不在行首。等到最后才更新的话，一批里每一条
        // 看到的都是上一批留下的旧状态，补出来的换行就全错了——
        // 表现是提示符前面凭空多一个空行，而模型的回答又被接在提示符后面。
        std::string out;
        for (const Entry& entry : batch) {
            const std::string piece = entry.isEvent ? format(entry.event) : entry.text;
            if (piece.empty()) continue;
            out += piece;
            mAtLineStart = piece.back() == '\n';
        }
        if (!out.empty()) writeUtf8(out);
    }
}

// 从行首开始打一行。不在行首就先补一个换行——模型的增量是不带换行的。
std::string atLineStart(bool alreadyThere, const std::string& line) {
    return alreadyThere ? line : "\n" + line;
}

std::string MaiConsoleRenderer::format(const MaiEvent& event) {
    switch (event.type) {
        case MaiEventType::MessagePartDelta: {
            auto found = mParts.find(event.partId);
            const bool isReasoning =
                found != mParts.end() && found->second.kind == PartMemo::Kind::Reasoning;
            if (isReasoning && !mShowReasoning) return {};
            return event.delta;
        }
        case MaiEventType::MessagePartUpdated: return formatPartUpdated(event);
        case MaiEventType::SessionIdle:
            // 一轮结束。打个提示符，用户知道可以说下一句了。
            mParts.clear();
            return atLineStart(mAtLineStart, "\n> ");
        case MaiEventType::SessionError:
            return atLineStart(mAtLineStart, "[error] " + event.detail + "\n");
        case MaiEventType::SessionUpdated:
            if (event.detail.empty()) return {};
            return atLineStart(mAtLineStart, "[session] titled \"" + event.detail + "\"\n");
        case MaiEventType::PermissionAsked:
            // 工具名和参数已经在上一条 message.part.updated 里打出来了（那个片段就是 Pending 状态），
            // 这里只说怎么答，不重复一遍参数。
            return atLineStart(mAtLineStart,
                               "       needs approval -> /y approve   /n deny   "
                               "/a approve for this session\n");
        case MaiEventType::PermissionReplied:
            return atLineStart(mAtLineStart, "       " + event.detail + "\n");
        default:
            // SessionCreated / SessionDeleted / MessageUpdated / MessageRemoved /
            // MessagePartRemoved / SessionStatus：控制台上没什么好说的。
            // 建会话和删会话由发起那条命令的地方自己回话，说得更清楚。
            return {};
    }
}

std::string MaiConsoleRenderer::formatPartUpdated(const MaiEvent& event) {
    auto found = mParts.find(event.partId);

    // 文字和思考片段只在第一次出现时查一次：它们的形态不会变，之后来的全是增量。
    if (found != mParts.end() && found->second.kind != PartMemo::Kind::Tool) return {};

    MaiMessagePart part;
    if (!findPart(event, part)) return {};

    if (const auto* reasoning = std::get_if<MaiReasoningPart>(&part.body)) {
        (void)reasoning;
        mParts[event.partId].kind = PartMemo::Kind::Reasoning;
        return mShowReasoning ? atLineStart(mAtLineStart, "[thinking] ") : std::string{};
    }
    if (std::get_if<MaiTextPart>(&part.body) != nullptr) {
        mParts[event.partId].kind = PartMemo::Kind::Text;
        return atLineStart(mAtLineStart, "");
    }

    const auto& tool = std::get<MaiToolPart>(part.body);
    PartMemo& memo = mParts[event.partId];
    memo.kind = PartMemo::Kind::Tool;
    // 状态没变就不重复打。同一个工具会连着来三条 message.part.updated。
    if (memo.toolStateKnown && memo.toolState == tool.state) return {};
    memo.toolState = tool.state;
    memo.toolStateKnown = true;

    std::string line = "[tool] " + tool.tool + "  " + maiToolStateToString(tool.state);
    switch (tool.state) {
        case MaiToolState::Pending:
            // 参数只在建卡这一下打出来：用户要看清授权的是什么。
            line += "  " + tool.input;
            break;
        case MaiToolState::Completed:
            line += "  (" + std::to_string(tool.output.size()) + " bytes)";
            break;
        case MaiToolState::Error: line += "  " + tool.error; break;
        case MaiToolState::Running: break;
    }
    return atLineStart(mAtLineStart, line + "\n");
}

bool MaiConsoleRenderer::findPart(const MaiEvent& event, MaiMessagePart& out) const {
    if (mAgent == nullptr) return false;
    // 事件只带 id，片段的内容要去问存储——这是事件总线的设计：事件是通知，存储是真相
    // （见 MaiEventBus.h "新订阅者看不到之前的事件"那一段）。
    //
    // 这个查询**只能在渲染线程做**：存储可能是 SQLite，读它是慢活，
    // 在事件处理函数里做会被守卫当场打死（见 MaiBlockingCheck.h）。
    for (const MaiMessage& message : mAgent->listMessages(event.sessionId)) {
        if (message.id != event.messageId) continue;
        for (const MaiMessagePart& part : message.parts) {
            if (part.id != event.partId) continue;
            out = part;
            return true;
        }
        return false;
    }
    return false;
}

// ── 命令 ────────────────────────────────────────────────────────

void usage() {
    std::printf(
        "Usage: maiagent-console [options]\n"
        "\n"
        "An interactive console that links the maiagent library directly.\n"
        "No HTTP, no JSON, no sockets: this is how Qt and mobile will call it.\n"
        "\n"
        "  --dir <path>         Working directory for the first session.\n"
        "                       Default: the current directory. File tools cannot\n"
        "                       reach outside it.\n"
        "  --model-url <url>    Model base url, for example\n"
        "                         https://open.bigmodel.cn/api/paas/v4   (GLM)\n"
        "                         http://127.0.0.1:11434/v1              (Ollama)\n"
        "  --model-key <key>    API key; MAIAGENT_API_KEY works too\n"
        "  --model <name>       Default model name, default glm-5.3\n"
        "  --db <path>          SQLite file for sessions and messages.\n"
        "                       Omitted means in-memory: everything is gone on exit.\n"
        "  --reasoning          Also print the model's thinking. Off by default: it\n"
        "                       interleaves with the answer and this is a line-based\n"
        "                       console, not a TUI.\n"
        "  --permission-timeout <ms>\n"
        "                       Milliseconds to wait for approval. 0 (default) waits\n"
        "                       forever.\n"
        "\n"
        "Without --model-url the console idles: sessions work, prompts get no reply.\n");
}

void help() {
    writeUtf8(
        "\n"
        "  /help                  show this\n"
        "  /new [directory]       start a new session, default the current one's directory\n"
        "  /sessions              list sessions, newest first\n"
        "  /use <id>              switch session; a unique id prefix is enough\n"
        "  /history               print the current session's messages\n"
        "  /interrupt             stop the running turn\n"
        "  /y  /n  /a             approve / deny / approve-for-session the oldest request\n"
        "  /quit                  leave\n"
        "\n"
        "  anything else is sent to the model as a prompt\n"
        "\n");
}

const char* envOrEmpty(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? value : "";
}

// 裁决最早那一个还在等的授权请求。
//
// "最早那一个"是刻意的：控制台一次只显示一个请求，用户答的一定是他刚看到的那个。
// 待裁决列表直接问核心（listPendingPermissions），不在这边存一份——存一份就有两个真相，
// 而核心那份才是闸门真正在等的。
bool replyOldestPermission(MaiAgent& agent, MaiConsoleRenderer& renderer,
                           MaiPermissionDecision decision) {
    std::vector<MaiPermissionRequest> pending = agent.listPendingPermissions();
    if (pending.empty()) {
        renderer.say("[console] nothing is waiting for approval\n");
        return false;
    }
    const MaiPermissionRequest* oldest = &pending.front();
    for (const MaiPermissionRequest& request : pending) {
        if (request.asked < oldest->asked) oldest = &request;
    }
    MaiResult<std::string> replied = agent.submit(MaiReplyPermission{oldest->id, decision});
    if (!replied) {
        renderer.say("[console] " + replied.error().message() + "\n");
        return false;
    }
    return true;
}

void printSessions(MaiAgent& agent, MaiConsoleRenderer& renderer,
                   const std::string& currentSessionId) {
    std::string out = "\n";
    for (const MaiSession& session : agent.listSessions()) {
        out += (session.id == currentSessionId ? "  * " : "    ");
        out += session.id;
        out += "  ";
        out += session.isUntitled() ? "(untitled)" : session.title;
        out += "\n";
    }
    renderer.say(out + "\n");
}

void printHistory(MaiAgent& agent, MaiConsoleRenderer& renderer, const std::string& sessionId) {
    std::string out = "\n";
    for (const MaiMessage& message : agent.listMessages(sessionId)) {
        out += std::string("  ") + maiRoleToString(message.role) + "\n";
        for (const MaiMessagePart& part : message.parts) {
            if (const auto* text = std::get_if<MaiTextPart>(&part.body)) {
                out += "    " + text->text + "\n";
            } else if (const auto* tool = std::get_if<MaiToolPart>(&part.body)) {
                out += "    [tool] " + tool->tool + "  " + maiToolStateToString(tool->state) +
                       "  " + tool->input + "\n";
            }
            // 思考片段不进历史：它是草稿，核心也不回灌给模型（见 MaiMessage.h）。
        }
    }
    renderer.say(out + "\n");
}

// 按 id 前缀切换会话。前缀而不是全 id，是因为 ses_01M2WDWM8A001Y7117JYM 这种东西没人愿意抄全。
// 撞上多个就不猜——猜错会让用户对着另一个会话说话，而屏幕上看不出区别。
bool switchSession(MaiAgent& agent, MaiConsoleRenderer& renderer, const std::string& prefix,
                   std::string& currentSessionId) {
    std::vector<std::string> matches;
    for (const MaiSession& session : agent.listSessions()) {
        if (session.id.rfind(prefix, 0) == 0) matches.push_back(session.id);
    }
    if (matches.empty()) {
        renderer.say("[console] no session id starts with " + prefix + "\n");
        return false;
    }
    if (matches.size() > 1) {
        renderer.say("[console] " + std::to_string(matches.size()) + " sessions start with " +
                     prefix + ", be more specific\n");
        return false;
    }
    currentSessionId = matches.front();
    renderer.say("[console] now talking to " + currentSessionId + "\n");
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string directory;
    std::string modelUrl;
    std::string modelKey = envOrEmpty("MAIAGENT_API_KEY");
    std::string modelName = "glm-5.3";
    std::string databasePath;
    MaiMillis permissionTimeoutMs = 0;
    bool showReasoning = false;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const bool hasNext = (i + 1) < argc;
        if (argument == "--help" || argument == "-h") {
            usage();
            return 0;
        } else if (argument == "--dir" && hasNext) {
            directory = argv[++i];
        } else if (argument == "--model-url" && hasNext) {
            modelUrl = argv[++i];
        } else if (argument == "--model-key" && hasNext) {
            modelKey = argv[++i];
        } else if (argument == "--model" && hasNext) {
            modelName = argv[++i];
        } else if (argument == "--db" && hasNext) {
            databasePath = argv[++i];
        } else if (argument == "--permission-timeout" && hasNext) {
            permissionTimeoutMs = std::atoll(argv[++i]);
        } else if (argument == "--reasoning") {
            showReasoning = true;
        } else {
            std::fprintf(stderr, "maiagent-console: unrecognized argument %s\n", argument.c_str());
            usage();
            return 2;
        }
    }

    // 没给 --dir 就用当前目录。走 MaiFileSystem::resolve 而不是 getcwd/GetCurrentDirectoryW：
    // 库里已经有这一层平台适配了，壳里不该再写一份（规范 12.5）。
    // resolve(".") 会把当前目录解析成绝对路径，符号链接也一起解掉。
    if (directory.empty()) {
        directory = MaiFileSystem::resolve(MaiFilePath::fromUtf8(".")).toUtf8();
    }

    std::unique_ptr<MaiModelClient> model;
    if (!modelUrl.empty()) {
        MaiModelConfig config;
        config.baseUrl = modelUrl;
        config.apiKey = modelKey;
        model = makeMaiModelClient(config);
    }

    auto tools = std::make_unique<MaiToolRegistry>();
    registerMaiBuiltinTools(*tools);

    std::unique_ptr<MaiSessionStore> store;
    if (databasePath.empty()) {
        store = makeMaiMemoryStore();
    } else {
        auto opened = makeMaiSqliteStore(databasePath);
        if (!opened) {
            std::fprintf(stderr, "maiagent-console: %s\n", opened.error().message().c_str());
            return 1;
        }
        store = std::move(opened.value());
    }

    MaiAgent::Options agentOptions;
    agentOptions.defaultModel = modelName;
    agentOptions.permissionTimeoutMs = permissionTimeoutMs;

    // 声明顺序有讲究，别调。渲染器在前、agent 在后，所以析构时 **agent 先走**——
    // MaiAgent 析构会叫停所有轮次并发最后几条事件，那时渲染器必须还活着
    // （见 MaiAgent.h 的析构一节：订阅者必须活得比 MaiAgent 久）。
    MaiConsoleRenderer renderer;
    MaiAgent agent(std::move(store), std::move(model), std::move(tools), agentOptions);
    renderer.start(agent, showReasoning);

    // 先订阅再建会话，否则会漏掉最前面的事件。
    agent.eventBus().subscribe([&renderer](const MaiEvent& event) { renderer.onEvent(event); });

    MaiResult<std::string> created = agent.submit(MaiCreateSession{directory, "", ""});
    if (!created) {
        std::fprintf(stderr, "maiagent-console: %s\n", created.error().message().c_str());
        return 1;
    }
    std::string sessionId = created.value();

    std::string banner =
        "\nmaiagent-console  (links maiagent only: no HTTP, no JSON, no sockets)\n";
    banner += "  session   " + sessionId + "\n";
    banner += "  directory " + directory + "\n";
    banner += "  storage   " +
              (databasePath.empty() ? std::string("in-memory (nothing is kept after exit)")
                                    : databasePath) +
              "\n";
    banner += "  model     " +
              (modelUrl.empty() ? std::string("not configured (prompts get no "
                                              "reply)")
                                : modelName + " @ " + modelUrl) +
              "\n";
    banner += "  /help for commands, /quit to leave\n\n> ";
    renderer.say(banner);

    std::string line;
    while (readLineUtf8(line)) {
        if (line.empty()) {
            renderer.say("> ");
            continue;
        }
        if (line == "/quit" || line == "/exit") break;

        if (line[0] == '/') {
            // 命令和参数之间就一个空格，不做引号解析——路径带空格的场合用 --dir 起。
            const std::size_t space = line.find(' ');
            const std::string command = line.substr(0, space);
            const std::string rest =
                space == std::string::npos ? std::string{} : line.substr(space + 1);

            if (command == "/help") {
                help();
            } else if (command == "/y") {
                replyOldestPermission(agent, renderer, MaiPermissionDecision::Approved);
                continue;  // 放行之后这一轮会接着跑，提示符等 SessionIdle 再打
            } else if (command == "/n") {
                replyOldestPermission(agent, renderer, MaiPermissionDecision::Denied);
                continue;
            } else if (command == "/a") {
                replyOldestPermission(agent, renderer, MaiPermissionDecision::ApprovedForSession);
                continue;
            } else if (command == "/sessions") {
                printSessions(agent, renderer, sessionId);
            } else if (command == "/history") {
                printHistory(agent, renderer, sessionId);
            } else if (command == "/use" && !rest.empty()) {
                switchSession(agent, renderer, rest, sessionId);
            } else if (command == "/new") {
                std::string where = rest.empty() ? directory : rest;
                MaiResult<std::string> fresh = agent.submit(MaiCreateSession{where, "", ""});
                if (!fresh) {
                    renderer.say("[console] " + fresh.error().message() + "\n");
                } else {
                    sessionId = fresh.value();
                    directory = where;
                    renderer.say("[console] new session " + sessionId + " in " + where + "\n");
                }
            } else if (command == "/interrupt") {
                MaiResult<std::string> stopped = agent.submit(MaiInterrupt{sessionId});
                if (stopped) {
                    continue;  // 打断成功之后还会来一条 SessionIdle，提示符交给它
                }
                // 失败的路上没有 SessionIdle，提示符得自己打。不打的话用户会停在
                // 一个没有提示符的屏幕上，以为控制台卡死了——最常见的情况就是
                // 手快按了两次 /interrupt。
                renderer.say("[console] " + stopped.error().message() + "\n> ");
                continue;
            } else {
                renderer.say("[console] unknown command " + command + ", try /help\n");
            }
            renderer.say("> ");
            continue;
        }

        MaiResult<std::string> sent = agent.submit(MaiSendPrompt{sessionId, line});
        if (!sent) {
            // Busy 是最常见的一个：上一轮还在跑。说清楚能怎么办，别只报错。
            renderer.say("[console] " + sent.error().message() +
                         (sent.error().code() == MaiErrorCode::Busy ? "  (try /interrupt)" : "") +
                         "\n> ");
        }
        // 成功的话什么都不打：模型的输出马上就从事件流里过来了，提示符等 SessionIdle 再打。
    }

    // 收尾的顺序：先让在跑的轮次结束，再把队列里剩下的写完。
    // 反过来的话最后那几条事件（SessionIdle、错误）就丢了。
    renderer.say("\n");
    agent.waitIdle();
    renderer.stop();
    return 0;
}
