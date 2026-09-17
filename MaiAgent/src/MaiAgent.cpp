#include "MaiAgent.h"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "MaiIdGenerator.h"

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

struct MaiAgent::Impl {
    std::unique_ptr<MaiSessionStore> store;
    std::unique_ptr<MaiModelClient> model;
    std::unique_ptr<MaiToolRegistry> tools;
    Options options;

    MaiEventBus bus;
    MaiEventEmitter emitter{bus};
    MaiContextBuilder context;
    MaiSessionTitler titler;

    mutable std::mutex mu;
    std::condition_variable cv;
    std::unordered_map<std::string, std::shared_ptr<ActiveTurn>> active;

    ~Impl() {
        // 析构时把所有在跑的轮次叫停并等它们退出，
        // 否则工作线程会访问已经销毁的 store/emitter。
        std::vector<std::shared_ptr<ActiveTurn>> pending;
        {
            std::lock_guard<std::mutex> lock(mu);
            for (auto& [_, t] : active) {
                t->cancel.store(true, std::memory_order_relaxed);
                pending.push_back(t);
            }
        }
        for (auto& t : pending) {
            if (t->worker.joinable()) t->worker.join();
        }
    }

    MaiTurnRunner::Deps deps() {
        MaiTurnRunner::Deps d;
        d.store = store.get();
        d.model = model.get();
        d.emitter = &emitter;
        d.context = &context;
        d.tools = tools.get();
        d.titler = &titler;
        d.defaultModel = options.defaultModel;
        d.maxIterations = options.maxToolIterations;
        return d;
    }

    void retire(const std::string& sessionId, const std::shared_ptr<ActiveTurn>& turn) {
        emitter.emitSession(MaiEventType::SessionIdle, sessionId);
        {
            std::lock_guard<std::mutex> lock(mu);
            auto it = active.find(sessionId);
            // 只有还是自己这一轮时才移除：可能已经有新的一轮顶上来了。
            if (it != active.end() && it->second == turn) {
                // 正在执行的就是这个线程，不能 join 自己。
                it->second->worker.detach();
                active.erase(it);
            }
        }
        cv.notify_all();
    }
};

MaiAgent::MaiAgent(std::unique_ptr<MaiSessionStore> store, std::unique_ptr<MaiModelClient> model,
                   std::unique_ptr<MaiToolRegistry> tools, Options options)
    : impl_(std::make_unique<Impl>()) {
    impl_->store = std::move(store);
    impl_->model = std::move(model);
    impl_->tools = std::move(tools);
    impl_->options = std::move(options);
}

MaiAgent::~MaiAgent() = default;

std::vector<MaiSession> MaiAgent::listSessions() const {
    return impl_->store->listSessions();
}

bool MaiAgent::getSession(const std::string& id, MaiSession& out) const {
    return impl_->store->getSession(id, out);
}

std::vector<MaiMessage> MaiAgent::listMessages(const std::string& sessionId) const {
    return impl_->store->listMessages(sessionId);
}

bool MaiAgent::isBusy(const std::string& sessionId) const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->active.count(sessionId) > 0;
}

void MaiAgent::waitIdle() {
    std::unique_lock<std::mutex> lock(impl_->mu);
    impl_->cv.wait(lock, [this] { return impl_->active.empty(); });
}

MaiEventBus& MaiAgent::eventBus() {
    return impl_->bus;
}
const MaiEventBus& MaiAgent::eventBus() const {
    return impl_->bus;
}

MaiResult<std::string> MaiAgent::submit(const MaiOperation& op) {
    return std::visit(
        [this](const auto& o) -> MaiResult<std::string> {
            using T = std::decay_t<decltype(o)>;

            if constexpr (std::is_same_v<T, MaiCreateSession>) {
                MaiSession s;
                s.id = MaiIdGenerator::newSessionId();
                s.directory = o.directory;
                s.title = o.title.empty() ? "新会话" : o.title;
                s.model = o.model;
                s.created = MaiTime::getCurrentTime();
                s.updated = s.created;
                impl_->store->putSession(s);
                impl_->emitter.emitSession(MaiEventType::SessionCreated, s.id, s.title);
                return s.id;

            } else if constexpr (std::is_same_v<T, MaiUpdateSession>) {
                std::string title;
                const bool found = impl_->store->mutateSession(o.sessionId, [&](MaiSession& s) {
                    if (!o.title.empty()) s.title = o.title;
                    if (!o.model.empty()) s.model = o.model;
                    if (!o.agent.empty()) s.agent = o.agent;
                    s.updated = MaiTime::getCurrentTime();
                    title = s.title;
                });
                if (!found) return {MaiErrorCode::NotFound, "会话不存在"};
                impl_->emitter.emitSession(MaiEventType::SessionUpdated, o.sessionId, title);
                return o.sessionId;

            } else if constexpr (std::is_same_v<T, MaiDeleteSession>) {
                if (!impl_->store->removeSession(o.sessionId))
                    return {MaiErrorCode::NotFound, "会话不存在"};
                impl_->emitter.emitSession(MaiEventType::SessionDeleted, o.sessionId);
                return o.sessionId;

            } else if constexpr (std::is_same_v<T, MaiSendPrompt>) {
                if (o.text.empty()) return {MaiErrorCode::InvalidInput, "消息不能为空"};

                MaiSession s;
                if (!impl_->store->getSession(o.sessionId, s))
                    return {MaiErrorCode::NotFound, "会话不存在"};

                {
                    std::lock_guard<std::mutex> lock(impl_->mu);
                    if (impl_->options.rejectWhenBusy && impl_->active.count(o.sessionId))
                        return {MaiErrorCode::Busy, "这个会话已经有一轮在跑"};
                }

                // 用户消息先落库并广播，界面立刻看到自己发的话。
                MaiMessage user;
                user.id = MaiIdGenerator::newMessageId();
                user.role = MaiRole::User;
                user.created = MaiTime::getCurrentTime();
                user.completed = user.created;
                MaiMessagePart up;
                up.id = MaiIdGenerator::newPartId();
                up.body = MaiTextPart{o.text};
                up.created = user.created;
                user.parts.push_back(std::move(up));
                impl_->store->putMessage(o.sessionId, user);
                impl_->emitter.emitMessage(MaiEventType::MessageUpdated, o.sessionId, user.id);

                // assistant 消息此刻就建好，后续 delta 都挂在它下面。
                MaiMessage assistant;
                assistant.id = MaiIdGenerator::newMessageId();
                assistant.role = MaiRole::Assistant;
                assistant.created = MaiTime::getCurrentTime();
                impl_->store->putMessage(o.sessionId, assistant);
                impl_->emitter.emitMessage(MaiEventType::MessageUpdated, o.sessionId, assistant.id);

                impl_->store->mutateSession(o.sessionId, [](MaiSession& sess) {
                    sess.updated = MaiTime::getCurrentTime();
                });

                auto turn = std::make_shared<ActiveTurn>();
                {
                    std::lock_guard<std::mutex> lock(impl_->mu);
                    impl_->active[o.sessionId] = turn;
                }

                // 单独线程跑，submit 立刻返回——同步等会让 HTTP 请求挂几十秒。
                const std::string sid = o.sessionId;
                auto deps = impl_->deps();
                turn->worker = std::thread([this, sid, turn, deps, assistant] {
                    MaiTurnRunner runner(deps, sid, assistant);
                    runner.run(turn->cancel);
                    impl_->retire(sid, turn);
                });
                return assistant.id;

            } else if constexpr (std::is_same_v<T, MaiInterrupt>) {
                std::lock_guard<std::mutex> lock(impl_->mu);
                auto it = impl_->active.find(o.sessionId);
                if (it == impl_->active.end()) return {MaiErrorCode::NotFound, "没有正在跑的轮次"};
                it->second->cancel.store(true, std::memory_order_relaxed);
                return o.sessionId;

            } else {
                return {MaiErrorCode::Internal, "未处理的操作"};
            }
        },
        op);
}
