#include "MaiQuestion.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <unordered_map>

namespace {

// 不能一睡到底：被中断时没人会 notify，得醒过来自己查一眼 cancel。
// 和 MaiPermissionGate 用同一个间隔。
constexpr std::chrono::milliseconds kCancelPollInterval{100};

struct Pending {
    MaiQuestionRequest request;
    std::string answer;
    bool settled = false;
};

}  // namespace

struct MaiQuestionGate::QuestionTable {
    Options options;

    mutable std::mutex mutex;
    std::condition_variable answered;
    // 用 shared_ptr：等待的那个线程解锁之后还要看着自己那条记录，
    // 而这时 reply() 可能已经把它从表里摘掉了。
    std::unordered_map<std::string, std::shared_ptr<Pending>> pending;

    explicit QuestionTable(Options options) : options(options) {}
};

MaiQuestionGate::MaiQuestionGate() : MaiQuestionGate(Options{}) {}

MaiQuestionGate::MaiQuestionGate(Options options)
    : mQuestions(std::make_unique<QuestionTable>(options)) {}

MaiQuestionGate::~MaiQuestionGate() {
    // 析构时把还在等的全部叫醒，否则那些线程会挂在已经销毁的 condition_variable 上。
    {
        std::lock_guard<std::mutex> lock(mQuestions->mutex);
        for (auto& [_, entry] : mQuestions->pending) entry->settled = true;
        mQuestions->pending.clear();
    }
    mQuestions->answered.notify_all();
}

std::string MaiQuestionGate::ask(const MaiQuestionRequest& request, const Announce& announce,
                                 const std::atomic<bool>& cancel) {
    auto entry = std::make_shared<Pending>();
    entry->request = request;

    {
        std::lock_guard<std::mutex> lock(mQuestions->mutex);
        // 先登记再广播。反过来的话，界面可能抢在登记前就回了答案，reply() 找不到这个 id，
        // 这一轮就永远醒不过来。
        mQuestions->pending[request.id] = entry;
    }

    // 在锁外广播：处理函数是订阅方的代码，拿着锁调用它等于把闸门的锁交给外部。
    if (announce) announce(request);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(mQuestions->options.timeoutMs);

    std::unique_lock<std::mutex> lock(mQuestions->mutex);
    while (!entry->settled) {
        if (cancel.load(std::memory_order_relaxed)) break;
        if (mQuestions->options.timeoutMs > 0 && std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        mQuestions->answered.wait_for(lock, kCancelPollInterval);
    }

    mQuestions->pending.erase(request.id);
    // 没等到就返回空。**不要编一个答案**——编出来的会让模型基于一个用户从没说过的
    // 决定往下做，而它不知道那是编的。
    return entry->settled ? entry->answer : std::string();
}

bool MaiQuestionGate::reply(const std::string& questionId, const std::string& answer) {
    {
        std::lock_guard<std::mutex> lock(mQuestions->mutex);
        auto it = mQuestions->pending.find(questionId);
        if (it == mQuestions->pending.end()) return false;
        if (it->second->settled) return false;  // 界面重复点
        // 空回答也算回答过了：用户可能就是想说「你看着办」。
        // 但不能让它和「没人回答」撞上，所以这里换成一句明确的话。
        it->second->answer =
            answer.empty() ? std::string("(the user answered with nothing)") : answer;
        it->second->settled = true;
    }
    mQuestions->answered.notify_all();
    return true;
}

std::vector<MaiQuestionRequest> MaiQuestionGate::listPending() const {
    std::lock_guard<std::mutex> lock(mQuestions->mutex);
    std::vector<MaiQuestionRequest> out;
    out.reserve(mQuestions->pending.size());
    for (const auto& [_, entry] : mQuestions->pending) {
        if (!entry->settled) out.push_back(entry->request);
    }
    return out;
}

void MaiQuestionGate::cancelSession(const std::string& sessionId) {
    {
        std::lock_guard<std::mutex> lock(mQuestions->mutex);
        for (auto& [_, entry] : mQuestions->pending) {
            if (entry->request.sessionId != sessionId) continue;
            // 叫醒但**不给答案**：被中断不是「用户回答了空」，
            // 回灌给模型的话必须分得开这两件事。
            entry->settled = false;
            entry->answer.clear();
        }
    }
    // 真正叫醒靠的是 ask() 里每 100ms 查一次 cancel，这里只是让它早一点转一圈。
    mQuestions->answered.notify_all();
}
