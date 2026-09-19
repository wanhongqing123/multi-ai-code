#include "MaiEventBus.h"

#include "MaiBlockingCheck.h"

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

const char* maiEventTypeToString(MaiEventType type) {
    switch (type) {
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

struct MaiEventBus::SubscriberTable {
    // 读多写极少：publish 每秒几十次并发读订阅表，subscribe/unsubscribe 只在连接建立和断开时发生。
    // 用 shared_mutex 让 publish 之间不互相阻塞。
    mutable std::shared_mutex mutex;
    std::unordered_map<Token, Handler> handlers;
    std::atomic<Token> next{1};
};

MaiEventBus::MaiEventBus() : mSubscribers(std::make_unique<SubscriberTable>()) {}
MaiEventBus::~MaiEventBus() = default;

MaiEventBus::Token MaiEventBus::subscribe(Handler handler) {
    const Token token = mSubscribers->next.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock lock(mSubscribers->mutex);
    mSubscribers->handlers.emplace(token, std::move(handler));
    return token;
}

void MaiEventBus::unsubscribe(Token token) {
    std::unique_lock lock(mSubscribers->mutex);
    mSubscribers->handlers.erase(token);
}

void MaiEventBus::publish(const MaiEvent& event) {
    // 先在读锁内把 handler 拷出来再调用，
    // 避免 handler 里反过来 subscribe/unsubscribe 造成自死锁——SSE 连接断开时正是在 handler 里触发 u
    // nsubscribe 的。
    std::vector<Handler> snapshot;
    {
        std::shared_lock lock(mSubscribers->mutex);
        snapshot.reserve(mSubscribers->handlers.size());
        for (const auto& [_, handler] : mSubscribers->handlers) snapshot.push_back(handler);
    }
    // 处理函数在**这个线程上同步跑**，流式期间那就是网络读线程。
    // 在里面读文件或写库会直接拖慢模型吐字，而症状（"吐字变卡了"）没人会联想到事件总线。
    // 以前这只是句注释，现在有人守着了。
    MaiScopedDisallowBlocking noBlocking;
    for (const auto& handler : snapshot) handler(event);
}

std::size_t MaiEventBus::subscriberCount() const {
    std::shared_lock lock(mSubscribers->mutex);
    return mSubscribers->handlers.size();
}
