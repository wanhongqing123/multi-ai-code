#include "MaiAgent.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

#include "MaiIdGenerator.h"
#include "MaiThread.h"

#include "MaiContextBuilder.h"
#include "MaiEventEmitter.h"
#include "MaiSessionTitler.h"
#include "MaiTurnRunner.h"

namespace {

// 一轮对话的在跑状态。MaiTurnRunner 负责跑，这个只负责"还在不在跑"和"叫停"。
struct ActiveTurn {
    std::thread worker;
    std::atomic<bool> cancel{false};
};

}  // namespace

struct MaiAgent::Runtime {
    std::unique_ptr<MaiSessionStore> store;
    std::unique_ptr<MaiModelClient> model;
    std::unique_ptr<MaiToolRegistry> tools;
    Options options;

    MaiEventBus bus;
    MaiEventEmitter emitter{bus};
    MaiContextBuilder context;
    MaiSessionTitler titler;
    // 闸门归门面持有而不是归某一轮：用户的"本会话都允许"要跨轮活着，
    // 而且界面查"还有什么在等授权"时可能一轮都没在跑。
    std::unique_ptr<MaiPermissionGate> permissions;
    std::unique_ptr<MaiQuestionGate> questions;
    // 指回门面。子 Agent 那组工具要通过它起会话、派活、等结果，
    // 而那些动作只有门面做得了（要走 submit，才有 Busy 判断和事件）。
    MaiSubAgentHost* owner = nullptr;

    mutable std::mutex mutex;
    std::condition_variable turnFinished;
    std::unordered_map<std::string, std::shared_ptr<ActiveTurn>> active;
    // 被 close_agent 收掉的子 Agent。只是不再占名额，消息一条不删。
    std::set<std::string> closedSubAgents;

    ~Runtime() {
        // 析构时把所有在跑的轮次叫停并等它们退出，否则工作线程会访问已经销毁的 store/emitter。
        std::vector<std::shared_ptr<ActiveTurn>> pending;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto& [_, turn] : active) {
                turn->cancel.store(true, std::memory_order_relaxed);
                pending.push_back(turn);
            }
        }
        // 卡在等授权的线程看不见 cancel 标志（它睡在闸门的 condition_variable 上），必须显式叫醒，
        // 否则下面的 join 要等满闸门那 250ms 的兜底轮询。靠兜底能过，
        // 但让退出路径依赖一个安全网是不对的。
        if (permissions) {
            for (const auto& request : permissions->listPending())
                permissions->cancelSession(request.sessionId);
        }
        for (auto& turn : pending) {
            if (turn->worker.joinable()) turn->worker.join();
        }
    }

    MaiTurnRunner::Dependencies dependencies() {
        MaiTurnRunner::Dependencies dependencies;
        dependencies.store = store.get();
        dependencies.model = model.get();
        dependencies.emitter = &emitter;
        dependencies.context = &context;
        dependencies.tools = tools.get();
        dependencies.titler = &titler;
        dependencies.permissions = permissions.get();
        dependencies.questions = questions.get();
        dependencies.subAgents = owner;
        dependencies.defaultModel = options.defaultModel;
        dependencies.maxIterations = options.maxToolIterations;
        return dependencies;
    }

    void retire(const std::string& sessionId, const std::shared_ptr<ActiveTurn>& turn) {
        emitter.emitSession(MaiEventType::SessionIdle, sessionId);
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = active.find(sessionId);
            // 只有还是自己这一轮时才移除：可能已经有新的一轮顶上来了。
            if (it != active.end() && it->second == turn) {
                // 正在执行的就是这个线程，不能 join 自己。
                it->second->worker.detach();
                active.erase(it);
            }
        }
        turnFinished.notify_all();
    }
};

MaiAgent::MaiAgent(std::unique_ptr<MaiSessionStore> store, std::unique_ptr<MaiModelClient> model,
                   std::unique_ptr<MaiToolRegistry> tools, Options options)
    : mRuntime(std::make_unique<Runtime>()) {
    // 这个指针在构造函数里就设好：turn runner 每次都从 Runtime 现取依赖，
    // 设晚了会有一轮拿到空的。
    mRuntime->owner = this;
    mRuntime->store = std::move(store);
    mRuntime->model = std::move(model);
    mRuntime->tools = std::move(tools);
    mRuntime->options = std::move(options);

    MaiPermissionGate::Options gateOptions;
    gateOptions.timeoutMs = mRuntime->options.permissionTimeoutMs;
    mRuntime->permissions = std::make_unique<MaiPermissionGate>(gateOptions);
    // 问答不设超时：超时等于替用户做了决定，而用户可能只是走开了。
    mRuntime->questions = std::make_unique<MaiQuestionGate>();
}

MaiAgent::~MaiAgent() = default;

std::vector<MaiSession> MaiAgent::listSessions() const {
    // **只给根会话。** 子 Agent 也是会话，但用户没开过它们，
    // 列出来只会让人以为自己漏了什么。要看子 Agent 走 listSubAgents()。
    std::vector<MaiSession> all = mRuntime->store->listSessions();
    std::vector<MaiSession> roots;
    roots.reserve(all.size());
    for (MaiSession& session : all) {
        if (session.isRoot()) roots.push_back(std::move(session));
    }
    return roots;
}

// ── 子 Agent ────────────────────────────────────────────────────

namespace {

// 这个会话直接起过的孩子。
std::vector<MaiSession> childrenOf(const MaiSessionStore& store, const std::string& parentId) {
    std::vector<MaiSession> children;
    for (MaiSession& session : store.listSessions()) {
        if (session.parentId == parentId) children.push_back(std::move(session));
    }
    return children;
}

}  // namespace

MaiResult<std::string> MaiAgent::spawnSubAgent(const std::string& parentSessionId,
                                               const std::string& taskName,
                                               const std::string& prompt) {
    if (prompt.empty()) return {MaiErrorCode::InvalidInput, "the sub-agent needs something to do"};

    MaiSession parent;
    if (!mRuntime->store->getSession(parentSessionId, parent))
        return {MaiErrorCode::NotFound, "session not found"};

    // 深度和数量两条上限，**报错要分得开**：一个是「别再往下分了」，
    // 一个是「先收掉几个」，模型的应对完全不一样。
    if (parent.depth + 1 > mRuntime->options.maxSubAgentDepth) {
        return {MaiErrorCode::InvalidInput,
                "sub-agents are already nested " + std::to_string(parent.depth) +
                    " deep, which is the limit. Do this part of the work yourself."};
    }
    int open = 0;
    for (const MaiSession& child : childrenOf(*mRuntime->store, parentSessionId)) {
        if (mRuntime->closedSubAgents.count(child.id) == 0) ++open;
    }
    if (open >= mRuntime->options.maxOpenSubAgents) {
        return {MaiErrorCode::InvalidInput,
                "there are already " + std::to_string(open) +
                    " sub-agents open, which is the limit. Close the ones you are done with."};
    }

    MaiSession child;
    child.id = MaiIdGenerator::newSessionId();
    child.title = taskName.empty() ? std::string(kMaiDefaultSessionTitle) : taskName;
    // **工作目录继承父的，没有参数能改。** 子能看见的文件因此是父的子集——
    // 这是「子只能比父弱」落到实处的第一条。
    child.directory = parent.directory;
    child.model = parent.model;
    child.agent = parent.agent;
    child.parentId = parentSessionId;
    child.depth = parent.depth + 1;
    child.created = MaiTime::getCurrentTime();
    child.updated = child.created;
    mRuntime->store->putSession(child);

    // 注意这里**没有**把父的会话级授权带过去。会话豁免是按 sessionId 记的，
    // 子是新 id，天然不继承——父点过「以后都允许跑 rm」，子照样要重新问。
    // 这条不是刻意写的代码，是数据结构自带的，所以写在这儿免得以后被"顺手"改掉。

    MaiResult<std::string> started = submit(MaiSendPrompt{child.id, prompt});
    if (!started) return started.error();
    return child.id;
}

MaiError MaiAgent::sendToSubAgent(const std::string& parentSessionId,
                                  const std::string& childSessionId, const std::string& prompt) {
    MaiSession child;
    if (!mRuntime->store->getSession(childSessionId, child))
        return MaiError(MaiErrorCode::NotFound, "no such sub-agent");
    // **只能戳自己的孩子。** 不查这一条的话，一个子 Agent 就能拿别人的会话 id
    // 去给别人派活，那是横向越权。
    if (child.parentId != parentSessionId)
        return MaiError(MaiErrorCode::InvalidInput, "that sub-agent belongs to someone else");

    MaiResult<std::string> sent = submit(MaiSendPrompt{childSessionId, prompt});
    return sent ? MaiError() : sent.error();
}

bool MaiAgent::waitForSubAgent(const std::string& parentSessionId,
                               const std::string& childSessionId, MaiMillis timeoutMs,
                               const std::atomic<bool>& cancel) {
    MaiSession child;
    if (!mRuntime->store->getSession(childSessionId, child)) return false;
    if (child.parentId != parentSessionId) return false;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::unique_lock<std::mutex> lock(mRuntime->mutex);
    while (mRuntime->active.count(childSessionId) > 0) {
        // 中断时没人会 notify 这个 condvar，所以不能一睡到底，
        // 得醒过来自己查一眼 cancel。和两个闸门一个路子。
        if (cancel.load(std::memory_order_relaxed)) return false;
        if (timeoutMs > 0 && std::chrono::steady_clock::now() >= deadline) return false;
        mRuntime->turnFinished.wait_for(lock, std::chrono::milliseconds(100));
    }
    return true;
}

std::vector<MaiSubAgentInfo> MaiAgent::listSubAgents(const std::string& parentSessionId) {
    std::vector<MaiSubAgentInfo> out;
    for (const MaiSession& child : childrenOf(*mRuntime->store, parentSessionId)) {
        MaiSubAgentInfo info;
        info.sessionId = child.id;
        info.taskName = child.title;
        info.depth = child.depth;
        if (mRuntime->closedSubAgents.count(child.id) > 0) {
            info.status = "closed";
        } else {
            info.status = isBusy(child.id) ? "running" : "idle";
        }
        out.push_back(std::move(info));
    }
    return out;
}

MaiError MaiAgent::closeSubAgent(const std::string& parentSessionId,
                                 const std::string& childSessionId) {
    MaiSession child;
    if (!mRuntime->store->getSession(childSessionId, child))
        return MaiError(MaiErrorCode::NotFound, "no such sub-agent");
    if (child.parentId != parentSessionId)
        return MaiError(MaiErrorCode::InvalidInput, "that sub-agent belongs to someone else");

    submit(MaiInterrupt{childSessionId});
    {
        std::lock_guard<std::mutex> lock(mRuntime->mutex);
        mRuntime->closedSubAgents.insert(childSessionId);
    }
    // **消息不删。** 父之后还要能翻它说过什么，而且用户排障时那段历史是唯一的线索。
    // 收掉只是把名额腾出来。
    return MaiError();
}

std::string MaiAgent::subAgentReport(const std::string& childSessionId) {
    const std::vector<MaiMessage> messages = mRuntime->store->listMessages(childSessionId);
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role != MaiRole::Assistant) continue;
        const std::string text = it->text();
        if (!text.empty()) return text;
    }
    return std::string();
}

bool MaiAgent::getSession(const std::string& id, MaiSession& out) const {
    return mRuntime->store->getSession(id, out);
}

std::vector<MaiMessage> MaiAgent::listMessages(const std::string& sessionId) const {
    return mRuntime->store->listMessages(sessionId);
}

std::vector<MaiPermissionRequest> MaiAgent::listPendingPermissions() const {
    return mRuntime->permissions->listPending();
}

std::vector<MaiQuestionRequest> MaiAgent::listPendingQuestions() const {
    return mRuntime->questions->listPending();
}

bool MaiAgent::isBusy(const std::string& sessionId) const {
    std::lock_guard<std::mutex> lock(mRuntime->mutex);
    return mRuntime->active.count(sessionId) > 0;
}

void MaiAgent::waitIdle() {
    std::unique_lock<std::mutex> lock(mRuntime->mutex);
    mRuntime->turnFinished.wait(lock, [this] { return mRuntime->active.empty(); });
}

MaiEventBus& MaiAgent::eventBus() {
    return mRuntime->bus;
}
const MaiEventBus& MaiAgent::eventBus() const {
    return mRuntime->bus;
}

MaiResult<std::string> MaiAgent::submit(const MaiOperation& operation) {
    return std::visit(
        [this](const auto& operation) -> MaiResult<std::string> {
            using T = std::decay_t<decltype(operation)>;

            if constexpr (std::is_same_v<T, MaiCreateSession>) {
                MaiSession session;
                session.id = MaiIdGenerator::newSessionId();
                session.directory = operation.directory;
                session.title = operation.title.empty() ? kMaiDefaultSessionTitle : operation.title;
                session.model = operation.model;
                session.created = MaiTime::getCurrentTime();
                session.updated = session.created;
                mRuntime->store->putSession(session);
                mRuntime->emitter.emitSession(MaiEventType::SessionCreated, session.id,
                                              session.title);
                return session.id;

            } else if constexpr (std::is_same_v<T, MaiUpdateSession>) {
                std::string title;
                const bool found =
                    mRuntime->store->mutateSession(operation.sessionId, [&](MaiSession& session) {
                        if (!operation.title.empty()) session.title = operation.title;
                        if (!operation.model.empty()) session.model = operation.model;
                        if (!operation.agent.empty()) session.agent = operation.agent;
                        session.updated = MaiTime::getCurrentTime();
                        title = session.title;
                    });
                if (!found) return {MaiErrorCode::NotFound, "session not found"};
                mRuntime->emitter.emitSession(MaiEventType::SessionUpdated, operation.sessionId,
                                              title);
                return operation.sessionId;

            } else if constexpr (std::is_same_v<T, MaiDeleteSession>) {
                if (!mRuntime->store->removeSession(operation.sessionId))
                    return {MaiErrorCode::NotFound, "session not found"};
                // 会话没了，它还在等的授权就没意义了；"本会话都允许"也要一起清掉，
                // 否则以后建一个同 id 的会话会白捡上一个的授权。
                mRuntime->permissions->cancelSession(operation.sessionId);
                mRuntime->permissions->forgetSession(operation.sessionId);
                mRuntime->emitter.emitSession(MaiEventType::SessionDeleted, operation.sessionId);
                return operation.sessionId;

            } else if constexpr (std::is_same_v<T, MaiClearMessages>) {
                {
                    std::lock_guard<std::mutex> lock(mRuntime->mutex);
                    if (mRuntime->active.count(operation.sessionId))
                        return {MaiErrorCode::Busy,
                                "a turn is still running; interrupt it before clearing"};
                }
                if (!mRuntime->store->clearMessages(operation.sessionId))
                    return {MaiErrorCode::NotFound, "session not found"};

                // 标题是拿第一句话起的。记录清了标题还留着的话，头部会一直挂着一句
                // 已经不存在的对话——所以一起清掉，让下一轮重新起名。
                std::string title;
                mRuntime->store->mutateSession(operation.sessionId, [](MaiSession& session) {
                    session.title.clear();
                    session.updated = MaiTime::getCurrentTime();
                });

                // **不逐条发 MessageRemoved。** 历史可能有几百条，那是几百条事件，
                // 而界面要做的只有一件事：把列表清空。发一条会话级的更新，
                // 界面照常去 listMessages 拉全量（这次拉到的是空的）。
                mRuntime->emitter.emitSession(MaiEventType::SessionUpdated, operation.sessionId,
                                              title);
                return operation.sessionId;

            } else if constexpr (std::is_same_v<T, MaiSendPrompt>) {
                if (operation.text.empty())
                    return {MaiErrorCode::InvalidInput, "prompt text must not be empty"};

                MaiSession session;
                if (!mRuntime->store->getSession(operation.sessionId, session))
                    return {MaiErrorCode::NotFound, "session not found"};

                {
                    std::lock_guard<std::mutex> lock(mRuntime->mutex);
                    if (mRuntime->options.rejectWhenBusy &&
                        mRuntime->active.count(operation.sessionId))
                        return {MaiErrorCode::Busy, "a turn is already running for this session"};
                }

                // 用户消息先落库并广播，界面立刻看到自己发的话。
                MaiMessage user;
                user.id = MaiIdGenerator::newMessageId();
                user.role = MaiRole::User;
                user.created = MaiTime::getCurrentTime();
                user.completed = user.created;
                MaiMessagePart up;
                up.id = MaiIdGenerator::newPartId();
                up.body = MaiTextPart{operation.text};
                up.created = user.created;
                user.parts.push_back(std::move(up));
                mRuntime->store->putMessage(operation.sessionId, user);
                mRuntime->emitter.emitMessage(MaiEventType::MessageUpdated, operation.sessionId,
                                              user.id);

                // assistant 消息此刻就建好，后续 delta 都挂在它下面。
                MaiMessage assistant;
                assistant.id = MaiIdGenerator::newMessageId();
                assistant.role = MaiRole::Assistant;
                assistant.created = MaiTime::getCurrentTime();
                mRuntime->store->putMessage(operation.sessionId, assistant);
                mRuntime->emitter.emitMessage(MaiEventType::MessageUpdated, operation.sessionId,
                                              assistant.id);

                mRuntime->store->mutateSession(operation.sessionId, [](MaiSession& sess) {
                    sess.updated = MaiTime::getCurrentTime();
                });

                auto turn = std::make_shared<ActiveTurn>();
                {
                    std::lock_guard<std::mutex> lock(mRuntime->mutex);
                    mRuntime->active[operation.sessionId] = turn;
                }

                // 单独线程跑，submit 立刻返回——同步等会让 HTTP 请求挂几十秒。
                const std::string sessionId = operation.sessionId;
                auto dependencies = mRuntime->dependencies();
                turn->worker = std::thread([this, sessionId, turn, dependencies, assistant] {
                    // 给线程起名字。抓 dump 或者挂调试器时，
                    // 一堆并发的轮次才分得清谁是谁——否则只有一串线程 ID。Linux 上限 15 字节，
                    // 所以名字要短。
                    MaiThread::setCurrentName("mai-turn");
                    MaiTurnRunner runner(dependencies, sessionId, assistant);
                    runner.run(turn->cancel);
                    mRuntime->retire(sessionId, turn);
                });
                return assistant.id;

            } else if constexpr (std::is_same_v<T, MaiInterrupt>) {
                {
                    std::lock_guard<std::mutex> lock(mRuntime->mutex);
                    auto it = mRuntime->active.find(operation.sessionId);
                    if (it == mRuntime->active.end())
                        return {MaiErrorCode::NotFound, "no turn is running for this session"};
                    it->second->cancel.store(true, std::memory_order_relaxed);
                }
                // 光置 cancel 叫不醒卡在等授权的那个线程——它睡在闸门的 condition_variable 上，
                // 看不见这个标志，必须显式敲一下。（闸门那边还有个 250ms 的兜底轮询，但那是安全网，
                // 不是主路径。）
                mRuntime->permissions->cancelSession(operation.sessionId);
                // 等回答的那一轮也睡在自己的 condition_variable 上，同样要敲一下。
                mRuntime->questions->cancelSession(operation.sessionId);
                return operation.sessionId;

            } else if constexpr (std::is_same_v<T, MaiReplyQuestion>) {
                if (operation.questionId.empty())
                    return {MaiErrorCode::InvalidInput, "questionId is required"};
                // 找不到就是找不到：界面重复回答、或者对着已经被中断的提问回答，
                // 都会走到这里。不是故障，但也不能假装成功。
                if (!mRuntime->questions->reply(operation.questionId, operation.answer))
                    return {MaiErrorCode::NotFound, "question is no longer pending"};
                return operation.questionId;

            } else if constexpr (std::is_same_v<T, MaiReplyPermission>) {
                if (operation.permissionId.empty())
                    return {MaiErrorCode::InvalidInput, "permissionId is required"};
                // 找不到就是找不到：界面重复点、或者对着已经被中断的请求点，都会走到这里。
                // 不是故障，但也不能假装成功——界面要据此把那个已经过期的对话框收掉。
                if (!mRuntime->permissions->reply(operation.permissionId, operation.decision))
                    return {MaiErrorCode::NotFound, "permission request is no longer pending"};
                // permission.replied 由等在闸门上的那一轮发出（只有它知道请求的全貌）。
                // 这里只负责放行，不重复广播。
                return operation.permissionId;

            } else {
                return {MaiErrorCode::Internal, "unhandled operation"};
            }
        },
        operation);
}
