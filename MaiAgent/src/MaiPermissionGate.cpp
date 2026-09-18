#include "MaiPermission.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <unordered_map>

const char* maiPermissionDecisionToString(MaiPermissionDecision decision) {
    switch (decision) {
        case MaiPermissionDecision::Once: return "once";
        case MaiPermissionDecision::AlwaysInSession: return "always";
        case MaiPermissionDecision::Reject: return "reject";
    }
    return "reject";
}

bool maiParsePermissionDecision(const std::string& text, MaiPermissionDecision& out) {
    if (text == "once" || text == "allow") {
        out = MaiPermissionDecision::Once;
        return true;
    }
    if (text == "always") {
        out = MaiPermissionDecision::AlwaysInSession;
        return true;
    }
    if (text == "reject" || text == "deny") {
        out = MaiPermissionDecision::Reject;
        return true;
    }
    // 认不出来就说认不出来。这里绝不能兜底成 Once——
    // 界面写错一个拼写，就等于把闸门整个拆了，而且没有任何报错。
    return false;
}

namespace {

// 等待被中断时的兜底轮询间隔。
//
// 正常路径上 cancelSession() 会显式唤醒，不靠这个。留着是因为取消的
// 触发点以后一定会变多（会话删除、进程退出、权限超时……），只要有一条
// 路径忘了调 cancelSession，这一轮就永远醒不过来——一个挂死的会话，
// 用户看到的是"一直在转"，排查起来极其难受。
//
// 每秒 4 次空唤醒，对着"等人点按钮"这个量级完全无所谓。
constexpr auto kCancelPollInterval = std::chrono::milliseconds(250);

struct Pending {
    MaiPermissionRequest request;
    MaiPermissionDecision decision = MaiPermissionDecision::Reject;
    bool settled = false;
};

}  // namespace

struct MaiPermissionGate::Implementation {
    Options options;

    mutable std::mutex mutex;
    std::condition_variable decided;
    // 用 shared_ptr 是因为等待的那个线程要在解锁之后仍然能看着自己那条
    // 记录，而 reply() 可能已经把它从表里摘掉了。
    std::unordered_map<std::string, std::shared_ptr<Pending>> pending;
    // sessionId -> 用户说了"本会话都允许"的工具名
    std::unordered_map<std::string, std::set<std::string>> sessionAllowlist;

    explicit Implementation(Options options) : options(options) {}
};

MaiPermissionGate::MaiPermissionGate(Options options)
    : mImplementation(std::make_unique<Implementation>(options)) {}

MaiPermissionGate::~MaiPermissionGate() {
    // 析构时把还在等的全部叫醒，否则那些线程会一直挂在已经销毁的
    // condition_variable 上。
    {
        std::lock_guard<std::mutex> lock(mImplementation->mutex);
        for (auto& [_, entry] : mImplementation->pending) {
            entry->decision = MaiPermissionDecision::Reject;
            entry->settled = true;
        }
        mImplementation->pending.clear();
    }
    mImplementation->decided.notify_all();
}

MaiPermissionDecision MaiPermissionGate::ask(const MaiPermissionRequest& request,
                                             const Announce& announce,
                                             const std::atomic<bool>& cancel) {
    auto entry = std::make_shared<Pending>();
    entry->request = request;

    {
        std::lock_guard<std::mutex> lock(mImplementation->mutex);
        // 先登记。广播必须在登记之后，否则界面可能抢在登记前就回了裁决，
        // reply() 找不到这个 id，这一轮就永远醒不过来。
        mImplementation->pending[request.id] = entry;
    }

    // 在锁外广播：处理函数是订阅方的代码（HTTP 适配器会在里面序列化并
    // 写 socket），拿着锁调用它等于把闸门的锁交给了外部代码。
    if (announce) announce(request);

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(mImplementation->options.timeoutMs);

    std::unique_lock<std::mutex> lock(mImplementation->mutex);
    while (!entry->settled) {
        if (cancel.load(std::memory_order_relaxed)) break;
        if (mImplementation->options.timeoutMs > 0 && std::chrono::steady_clock::now() >= deadline)
            break;
        mImplementation->decided.wait_for(lock, kCancelPollInterval);
    }

    mImplementation->pending.erase(request.id);
    // 没裁决就醒了（中断或超时）一律按拒绝走。这是唯一安全的兜底方向：
    // 兜底成允许，就意味着"没人点头"也能改用户的文件。
    const MaiPermissionDecision decision =
        entry->settled ? entry->decision : MaiPermissionDecision::Reject;

    if (decision == MaiPermissionDecision::AlwaysInSession) {
        mImplementation->sessionAllowlist[request.sessionId].insert(request.toolName);
    }
    return decision;
}

bool MaiPermissionGate::reply(const std::string& permissionId, MaiPermissionDecision decision) {
    {
        std::lock_guard<std::mutex> lock(mImplementation->mutex);
        auto it = mImplementation->pending.find(permissionId);
        if (it == mImplementation->pending.end()) return false;
        if (it->second->settled) return false;  // 界面重复点
        it->second->decision = decision;
        it->second->settled = true;
    }
    mImplementation->decided.notify_all();
    return true;
}

bool MaiPermissionGate::isAllowedInSession(const std::string& sessionId,
                                           const std::string& toolName) const {
    std::lock_guard<std::mutex> lock(mImplementation->mutex);
    auto it = mImplementation->sessionAllowlist.find(sessionId);
    return it != mImplementation->sessionAllowlist.end() && it->second.count(toolName) > 0;
}

std::vector<MaiPermissionRequest> MaiPermissionGate::listPending() const {
    std::lock_guard<std::mutex> lock(mImplementation->mutex);
    std::vector<MaiPermissionRequest> out;
    out.reserve(mImplementation->pending.size());
    for (const auto& [_, entry] : mImplementation->pending) {
        if (!entry->settled) out.push_back(entry->request);
    }
    return out;
}

void MaiPermissionGate::cancelSession(const std::string& sessionId) {
    {
        std::lock_guard<std::mutex> lock(mImplementation->mutex);
        for (auto& [_, entry] : mImplementation->pending) {
            if (entry->request.sessionId != sessionId || entry->settled) continue;
            entry->decision = MaiPermissionDecision::Reject;
            entry->settled = true;
        }
    }
    mImplementation->decided.notify_all();
}

void MaiPermissionGate::forgetSession(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(mImplementation->mutex);
    mImplementation->sessionAllowlist.erase(sessionId);
}
