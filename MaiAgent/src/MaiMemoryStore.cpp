#include "MaiSessionStore.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace {

class MemoryStore final : public MaiSessionStore {
public:
    void putSession(const MaiSession& session) override {
        std::lock_guard<std::mutex> lock(mMutex);
        mSessions[session.id] = session;
    }

    // 纯内存，写不会失败——除非内存耗尽，那时候进程已经没了。
    MaiError lastWriteError() const override {
        return {};
    }

    bool getSession(const std::string& id, MaiSession& out) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mSessions.find(id);
        if (it == mSessions.end()) return false;
        out = it->second;
        return true;
    }

    std::vector<MaiSession> listSessions() const override {
        std::lock_guard<std::mutex> lock(mMutex);
        std::vector<MaiSession> out;
        out.reserve(mSessions.size());
        for (const auto& [_, session] : mSessions) out.push_back(session);
        // 界面左侧列表按最近活跃排序，这里就排好，省得每个调用方各排一遍。
        std::sort(out.begin(), out.end(), [](const MaiSession& left, const MaiSession& right) {
            return left.updated > right.updated;
        });
        return out;
    }

    bool removeSession(const std::string& id) override {
        std::lock_guard<std::mutex> lock(mMutex);
        mMessages.erase(id);
        return mSessions.erase(id) > 0;
    }

    bool clearMessages(const std::string& sessionId) override {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mSessions.find(sessionId) == mSessions.end()) return false;
        mMessages.erase(sessionId);
        return true;
    }

    bool mutateSession(const std::string& id,
                       const std::function<void(MaiSession&)>& mutator) override {
        // 读-改-写整个在锁内完成。调用方自己做这三步的话，
        // 中间那段窗口里别人的改动会被盖掉（lost update）——之前跑一轮的收尾就有这个洞。
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mSessions.find(id);
        if (it == mSessions.end()) return false;
        mutator(it->second);
        return true;
    }

    void putMessage(const std::string& sessionId, const MaiMessage& message) override {
        std::lock_guard<std::mutex> lock(mMutex);
        auto& list = mMessages[sessionId];
        // 同 id 视为更新：流式期间同一条消息会被反复写回。
        auto it = std::find_if(list.begin(), list.end(), [&](const MaiMessage& existing) {
            return existing.id == message.id;
        });
        if (it != list.end()) {
            *it = message;
        } else {
            list.push_back(message);
        }
    }

    std::vector<MaiMessage> listMessages(const std::string& sessionId) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mMessages.find(sessionId);
        if (it == mMessages.end()) return {};
        return it->second;
    }

private:
    mutable std::mutex mMutex;
    std::unordered_map<std::string, MaiSession> mSessions;
    std::unordered_map<std::string, std::vector<MaiMessage>> mMessages;
};

}  // namespace

std::unique_ptr<MaiSessionStore> makeMaiMemoryStore() {
    return std::make_unique<MemoryStore>();
}
