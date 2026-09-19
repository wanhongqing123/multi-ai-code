#include "MaiAgent.h"

#include <condition_variable>
#include <mutex>
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

    mutable std::mutex mutex;
    std::condition_variable turnFinished;
    std::unordered_map<std::string, std::shared_ptr<ActiveTurn>> active;

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
    mRuntime->store = std::move(store);
    mRuntime->model = std::move(model);
    mRuntime->tools = std::move(tools);
    mRuntime->options = std::move(options);

    MaiPermissionGate::Options gateOptions;
    gateOptions.timeoutMs = mRuntime->options.permissionTimeoutMs;
    mRuntime->permissions = std::make_unique<MaiPermissionGate>(gateOptions);
}

MaiAgent::~MaiAgent() = default;

std::vector<MaiSession> MaiAgent::listSessions() const {
    return mRuntime->store->listSessions();
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
                return operation.sessionId;

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
