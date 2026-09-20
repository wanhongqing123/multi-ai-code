#include "MaiPermission.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <unordered_map>

const char* maiPermissionDecisionToString(MaiPermissionDecision decision) {
    // 线上取值用 codex 的 snake_case 写法，和它的 ReviewDecision 对得上。
    switch (decision) {
        case MaiPermissionDecision::Approved: return "approved";
        case MaiPermissionDecision::ApprovedForSession: return "approved_for_session";
        case MaiPermissionDecision::Denied: return "denied";
        case MaiPermissionDecision::TimedOut: return "timed_out";
    }
    return "denied";
}

bool maiParsePermissionDecision(const std::string& text, MaiPermissionDecision& out) {
    if (text == "approved") {
        out = MaiPermissionDecision::Approved;
        return true;
    }
    if (text == "approved_for_session") {
        out = MaiPermissionDecision::ApprovedForSession;
        return true;
    }
    if (text == "denied") {
        out = MaiPermissionDecision::Denied;
        return true;
    }
    // timed_out 是闸门自己的结论，不接受从线上传进来——允许调用方声称"超时了"，
    // 等于给了它一个绕过用户的说法。认不出来就说认不出来。这里绝不能兜底成 Once——界面写错一个拼写，
    // 就等于把闸门整个拆了，而且没有任何报错。
    return false;
}

namespace {

// 等待被中断时的兜底轮询间隔。
//
// 正常路径上 cancelSession() 会显式唤醒，不靠这个。留着是因为取消的触发点以后一定会变多（会话删除、
// 进程退出、权限超时……），只要有一条路径忘了调 cancelSession，
// 这一轮就永远醒不过来——一个挂死的会话，用户看到的是"一直在转"，排查起来极其难受。
//
// 每秒 4 次空唤醒，对着"等人点按钮"这个量级完全无所谓。
constexpr auto kCancelPollInterval = std::chrono::milliseconds(250);

struct Pending {
    MaiPermissionRequest request;
    MaiPermissionDecision decision = MaiPermissionDecision::Denied;
    bool settled = false;
};

}  // namespace

struct MaiPermissionGate::ApprovalTable {
    Options options;

    mutable std::mutex mutex;
    std::condition_variable decided;
    // 用 shared_ptr 是因为等待的那个线程要在解锁之后仍然能看着自己那条记录，
    // 而 reply() 可能已经把它从表里摘掉了。
    std::unordered_map<std::string, std::shared_ptr<Pending>> pending;
    // sessionId -> 用户说了"本会话都允许"的工具名
    std::unordered_map<std::string, std::set<std::string>> sessionAllowlist;

    explicit ApprovalTable(Options options) : options(options) {}
};

MaiPermissionGate::MaiPermissionGate(Options options)
    : mApprovals(std::make_unique<ApprovalTable>(options)) {}

MaiPermissionGate::~MaiPermissionGate() {
    // 析构时把还在等的全部叫醒，否则那些线程会一直挂在已经销毁的 condition_variable 上。
    {
        std::lock_guard<std::mutex> lock(mApprovals->mutex);
        for (auto& [_, entry] : mApprovals->pending) {
            entry->decision = MaiPermissionDecision::Denied;
            entry->settled = true;
        }
        mApprovals->pending.clear();
    }
    mApprovals->decided.notify_all();
}

MaiPermissionDecision MaiPermissionGate::ask(const MaiPermissionRequest& request,
                                             const Announce& announce,
                                             const std::atomic<bool>& cancel) {
    auto entry = std::make_shared<Pending>();
    entry->request = request;

    {
        std::lock_guard<std::mutex> lock(mApprovals->mutex);
        // 先登记。广播必须在登记之后，否则界面可能抢在登记前就回了裁决，reply() 找不到这个 id，
        // 这一轮就永远醒不过来。
        mApprovals->pending[request.id] = entry;
    }

    // 在锁外广播：处理函数是订阅方的代码（HTTP 适配器会在里面序列化并写 socket），
    // 拿着锁调用它等于把闸门的锁交给了外部代码。
    if (announce) announce(request);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(mApprovals->options.timeoutMs);

    std::unique_lock<std::mutex> lock(mApprovals->mutex);
    while (!entry->settled) {
        if (cancel.load(std::memory_order_relaxed)) break;
        if (mApprovals->options.timeoutMs > 0 && std::chrono::steady_clock::now() >= deadline)
            break;
        mApprovals->decided.wait_for(lock, kCancelPollInterval);
    }

    mApprovals->pending.erase(request.id);
    // 没裁决就醒了（中断或超时）一律按拒绝走。这是唯一安全的兜底方向：兜底成允许，
    // 就意味着"没人点头"也能改用户的文件。没裁决就醒了：超时和被中断要分开。兜底方向都是"不放行"，
    // 但对模型说的话不一样（见 MaiPermissionDecision::TimedOut 的注释）。
    MaiPermissionDecision decision = entry->decision;
    if (!entry->settled) {
        const bool timedOut = mApprovals->options.timeoutMs > 0 &&
                              std::chrono::steady_clock::now() >= deadline &&
                              !cancel.load(std::memory_order_relaxed);
        decision = timedOut ? MaiPermissionDecision::TimedOut : MaiPermissionDecision::Denied;
    }

    if (decision == MaiPermissionDecision::ApprovedForSession) {
        mApprovals->sessionAllowlist[request.sessionId].insert(
            request.approvalKey.empty() ? request.toolName : request.approvalKey);
    }
    return decision;
}

bool MaiPermissionGate::reply(const std::string& permissionId, MaiPermissionDecision decision) {
    {
        std::lock_guard<std::mutex> lock(mApprovals->mutex);
        auto it = mApprovals->pending.find(permissionId);
        if (it == mApprovals->pending.end()) return false;
        if (it->second->settled) return false;  // 界面重复点
        it->second->decision = decision;
        it->second->settled = true;
    }
    mApprovals->decided.notify_all();
    return true;
}

bool MaiPermissionGate::isAllowedInSession(const std::string& sessionId,
                                           const std::string& approvalKey) const {
    std::lock_guard<std::mutex> lock(mApprovals->mutex);
    auto it = mApprovals->sessionAllowlist.find(sessionId);
    return it != mApprovals->sessionAllowlist.end() && it->second.count(approvalKey) > 0;
}

std::vector<MaiPermissionRequest> MaiPermissionGate::listPending() const {
    std::lock_guard<std::mutex> lock(mApprovals->mutex);
    std::vector<MaiPermissionRequest> out;
    out.reserve(mApprovals->pending.size());
    for (const auto& [_, entry] : mApprovals->pending) {
        if (!entry->settled) out.push_back(entry->request);
    }
    return out;
}

void MaiPermissionGate::cancelSession(const std::string& sessionId) {
    {
        std::lock_guard<std::mutex> lock(mApprovals->mutex);
        for (auto& [_, entry] : mApprovals->pending) {
            if (entry->request.sessionId != sessionId || entry->settled) continue;
            entry->decision = MaiPermissionDecision::Denied;
            entry->settled = true;
        }
    }
    mApprovals->decided.notify_all();
}

void MaiPermissionGate::forgetSession(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(mApprovals->mutex);
    mApprovals->sessionAllowlist.erase(sessionId);
}
