#include "MaiEventBus.h"

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

const char* maiEventTypeToString(MaiEventType t) {
    switch (t) {
        case MaiEventType::SessionCreated: return "session.created";
        case MaiEventType::SessionUpdated: return "session.updated";
        case MaiEventType::SessionDeleted: return "session.deleted";
        case MaiEventType::SessionIdle: return "session.idle";
        case MaiEventType::SessionError: return "session.error";
        case MaiEventType::SessionStatus: return "session.status";
        case MaiEventType::MessageUpdated: return "message.updated";
        case MaiEventType::MessageRemoved: return "message.removed";
        case MaiEventType::MessagePartUpdated: return "message.part.updated";
        case MaiEventType::MessagePartDelta: return "message.part.delta";
        case MaiEventType::MessagePartRemoved: return "message.part.removed";
        case MaiEventType::PermissionAsked: return "permission.asked";
        case MaiEventType::PermissionReplied: return "permission.replied";
    }
    return "session.status";
}

struct MaiEventBus::Impl {
    // 读多写极少：publish 每秒几十次并发读订阅表，subscribe/unsubscribe 只在
    // 连接建立和断开时发生。用 shared_mutex 让 publish 之间不互相阻塞。
    mutable std::shared_mutex mu;
    std::unordered_map<Token, Handler> handlers;
    std::atomic<Token> next{1};
};

MaiEventBus::MaiEventBus() : impl_(std::make_unique<Impl>()) {}
MaiEventBus::~MaiEventBus() = default;

MaiEventBus::Token MaiEventBus::subscribe(Handler h) {
    const Token t = impl_->next.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock lock(impl_->mu);
    impl_->handlers.emplace(t, std::move(h));
    return t;
}

void MaiEventBus::unsubscribe(Token t) {
    std::unique_lock lock(impl_->mu);
    impl_->handlers.erase(t);
}

void MaiEventBus::publish(const MaiEvent& e) {
    // 先在读锁内把 handler 拷出来再调用，避免 handler 里反过来 subscribe/unsubscribe
    // 造成自死锁——SSE 连接断开时正是在 handler 里触发 unsubscribe 的。
    std::vector<Handler> snapshot;
    {
        std::shared_lock lock(impl_->mu);
        snapshot.reserve(impl_->handlers.size());
        for (const auto& [_, h] : impl_->handlers) snapshot.push_back(h);
    }
    for (const auto& h : snapshot) h(e);
}

std::size_t MaiEventBus::subscriberCount() const {
    std::shared_lock lock(impl_->mu);
    return impl_->handlers.size();
}
