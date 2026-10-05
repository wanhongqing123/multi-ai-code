#include "MaiMemoryStore.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <variant>

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
        for (auto it = mSpecialistTasks.begin(); it != mSpecialistTasks.end();) {
            if (it->second.ownerSessionId == id) {
                it = mSpecialistTasks.erase(it);
            } else
                ++it;
        }
        return mSessions.erase(id) > 0;
    }

    bool clearMessages(const std::string& sessionId) override {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mSessions.find(sessionId) == mSessions.end()) return false;
        mMessages.erase(sessionId);
        for (auto it = mSpecialistTasks.begin(); it != mSpecialistTasks.end();) {
            if (it->second.ownerSessionId == sessionId) {
                it = mSpecialistTasks.erase(it);
            } else
                ++it;
        }
        return true;
    }

    MaiError insertSpecialistTask(const MaiSpecialistTask& task) override {
        std::lock_guard<std::mutex> lock(mMutex);
        if (task.id.empty() || task.ownerSessionId.empty() || task.specialistName.empty() ||
            mSessions.find(task.ownerSessionId) == mSessions.end() ||
            mSpecialistTasks.find(task.id) != mSpecialistTasks.end())
            return MaiError::make(MaiErrorCode::InvalidInput, "invalid specialist task identity");
        if (!task.parentTaskId.empty()) {
            const auto parent = mSpecialistTasks.find(task.parentTaskId);
            if (parent == mSpecialistTasks.end() ||
                parent->second.ownerSessionId != task.ownerSessionId ||
                parent->second.specialistName != task.specialistName)
                return MaiError::make(MaiErrorCode::InvalidInput,
                                      "specialist parent belongs to another task chain");
        }
        MaiSpecialistTask stored = task;
        if (!stored.outputPath.empty()) {
            stored.status = MaiSpecialistTaskStatus::Succeeded;
            stored.notifiedAt = stored.created;
        }
        mSpecialistTasks.emplace(task.id, std::move(stored));
        return {};
    }

    bool getSpecialistTask(const std::string& id, const std::string& ownerSessionId,
                           MaiSpecialistTask& out) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        const auto it = mSpecialistTasks.find(id);
        if (it == mSpecialistTasks.end() || it->second.ownerSessionId != ownerSessionId)
            return false;
        out = it->second;
        return true;
    }

    MaiError appendSpecialistText(const std::string& taskId, const std::string& ownerSessionId,
                                  const std::string& chunk, MaiMillis checkedAt) override {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mSpecialistTasks.find(taskId);
        if (it == mSpecialistTasks.end() || it->second.ownerSessionId != ownerSessionId)
            return MaiError::make(MaiErrorCode::NotFound, "specialist task was not found");
        if (it->second.status != MaiSpecialistTaskStatus::Submitted &&
            it->second.status != MaiSpecialistTaskStatus::Running)
            return MaiError::make(MaiErrorCode::InvalidInput, "specialist task is not running");
        if (chunk.size() > 16 * 1024 || it->second.partialText.size() + chunk.size() > 64 * 1024)
            return MaiError::make(MaiErrorCode::InvalidInput, "specialist text is too large");
        it->second.partialText += chunk;
        it->second.lastCheckedAt = checkedAt;
        it->second.status = MaiSpecialistTaskStatus::Running;
        return {};
    }

    MaiError updateSpecialistProgress(const std::string& taskId, const std::string& ownerSessionId,
                                      MaiMillis checkedAt) override {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mSpecialistTasks.find(taskId);
        if (it == mSpecialistTasks.end() || it->second.ownerSessionId != ownerSessionId)
            return MaiError::make(MaiErrorCode::NotFound, "specialist task was not found");
        if (it->second.status != MaiSpecialistTaskStatus::Submitted &&
            it->second.status != MaiSpecialistTaskStatus::Running)
            return MaiError::make(MaiErrorCode::InvalidInput, "specialist task is not running");
        it->second.status = MaiSpecialistTaskStatus::Running;
        it->second.lastCheckedAt = checkedAt;
        return {};
    }

    MaiError finishSpecialistTask(const std::string& taskId, const std::string& ownerSessionId,
                                  MaiSpecialistTaskStatus status, const std::string& finalText,
                                  const std::string& outputPath, MaiMillis finishedAt) override {
        if (status != MaiSpecialistTaskStatus::Succeeded &&
            status != MaiSpecialistTaskStatus::NeedsInput &&
            status != MaiSpecialistTaskStatus::Failed &&
            status != MaiSpecialistTaskStatus::Canceled)
            return MaiError::make(MaiErrorCode::InvalidInput, "invalid final specialist status");
        if (finalText.size() > 64 * 1024 || outputPath.size() > 4096 || finishedAt == 0)
            return MaiError::make(MaiErrorCode::InvalidInput, "invalid specialist reply");
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mSpecialistTasks.find(taskId);
        if (it == mSpecialistTasks.end() || it->second.ownerSessionId != ownerSessionId)
            return MaiError::make(MaiErrorCode::NotFound, "specialist task was not found");
        if (it->second.status != MaiSpecialistTaskStatus::Submitted &&
            it->second.status != MaiSpecialistTaskStatus::Running)
            return MaiError::make(MaiErrorCode::InvalidInput,
                                  "specialist task is already finished");
        if (status == MaiSpecialistTaskStatus::Succeeded && finalText.empty() &&
            it->second.partialText.empty() && outputPath.empty())
            return MaiError::make(MaiErrorCode::InvalidInput,
                                  "a successful specialist needs text or an output file");
        it->second.status = status;
        it->second.finalText = finalText.empty() ? it->second.partialText : finalText;
        it->second.outputPath = outputPath;
        if (status == MaiSpecialistTaskStatus::Failed ||
            status == MaiSpecialistTaskStatus::Canceled)
            it->second.errorText = it->second.finalText;
        it->second.lastCheckedAt = finishedAt;
        return {};
    }

    std::vector<MaiSpecialistTask> listActiveSpecialistTasks(std::size_t limit) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        std::vector<MaiSpecialistTask> tasks;
        for (const auto& [_, task] : mSpecialistTasks) {
            if (!task.providerTaskId.empty() &&
                (task.status == MaiSpecialistTaskStatus::Submitted ||
                 task.status == MaiSpecialistTaskStatus::Running))
                tasks.push_back(task);
        }
        std::sort(tasks.begin(), tasks.end(), [](const auto& left, const auto& right) {
            if (left.lastCheckedAt != right.lastCheckedAt)
                return left.lastCheckedAt < right.lastCheckedAt;
            return left.created < right.created;
        });
        if (tasks.size() > limit) tasks.resize(limit);
        return tasks;
    }

    std::vector<MaiSpecialistTask> listUnnotifiedSpecialistTasks(std::size_t limit) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        std::vector<MaiSpecialistTask> tasks;
        for (const auto& [_, task] : mSpecialistTasks) {
            if (task.notifiedAt == 0 && task.status != MaiSpecialistTaskStatus::Submitted &&
                task.status != MaiSpecialistTaskStatus::Running)
                tasks.push_back(task);
        }
        std::sort(tasks.begin(), tasks.end(), [](const auto& left, const auto& right) {
            return left.lastCheckedAt < right.lastCheckedAt;
        });
        if (tasks.size() > limit) tasks.resize(limit);
        return tasks;
    }

    MaiResult<std::string> reserveSpecialistNotification(const std::string& taskId,
                                                         const std::string& ownerSessionId,
                                                         const std::string& proposedMessageId,
                                                         MaiMillis attemptedAt) override {
        if (proposedMessageId.empty() || attemptedAt == 0)
            return {MaiErrorCode::InvalidInput, "invalid specialist notification"};
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mSpecialistTasks.find(taskId);
        if (it == mSpecialistTasks.end() || it->second.ownerSessionId != ownerSessionId ||
            it->second.notifiedAt != 0 || it->second.status == MaiSpecialistTaskStatus::Submitted ||
            it->second.status == MaiSpecialistTaskStatus::Running)
            return {MaiErrorCode::NotFound, "specialist reply is unavailable"};
        if (it->second.notificationMessageId.empty())
            it->second.notificationMessageId = proposedMessageId;
        it->second.notificationAttemptAt = attemptedAt;
        return it->second.notificationMessageId;
    }

    MaiError markSpecialistNotified(const std::string& taskId, const std::string& ownerSessionId,
                                    const std::string& messageId, MaiMillis notifiedAt) override {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mSpecialistTasks.find(taskId);
        if (it == mSpecialistTasks.end() || it->second.ownerSessionId != ownerSessionId ||
            it->second.notificationMessageId != messageId || messageId.empty() || notifiedAt == 0)
            return MaiError::make(MaiErrorCode::InvalidInput,
                                  "specialist notification identity does not match");
        it->second.notifiedAt = notifiedAt;
        return {};
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

    std::vector<MaiMessage> listMessagesPage(const std::string& sessionId,
                                             const std::string& beforeId,
                                             std::size_t limit) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        std::vector<MaiMessage> page;
        if (limit == 0) return page;
        auto it = mMessages.find(sessionId);
        if (it == mMessages.end()) return page;
        page.reserve(std::min(limit, it->second.size()));
        for (auto message = it->second.rbegin(); message != it->second.rend(); ++message) {
            if (!beforeId.empty() && message->id >= beforeId) continue;
            page.push_back(*message);
            if (page.size() == limit) break;
        }
        return page;
    }

    void recoverInterruptedTools(const std::string& error, std::int64_t completedAt) override {
        std::lock_guard<std::mutex> lock(mMutex);
        for (auto& entry : mMessages) {
            for (MaiMessage& message : entry.second) {
                bool changed = false;
                for (MaiMessagePart& part : message.parts) {
                    auto* tool = std::get_if<MaiToolPart>(&part.body);
                    if (!tool || (tool->state != MaiToolState::Pending &&
                                  tool->state != MaiToolState::Running))
                        continue;
                    tool->state = MaiToolState::Error;
                    tool->error = error;
                    tool->output = error;
                    changed = true;
                }
                if (changed && message.completed == 0) message.completed = completedAt;
            }
        }
    }

private:
    mutable std::mutex mMutex;
    std::unordered_map<std::string, MaiSession> mSessions;
    std::unordered_map<std::string, std::vector<MaiMessage>> mMessages;
    std::unordered_map<std::string, MaiSpecialistTask> mSpecialistTasks;
};

}  // namespace

std::unique_ptr<MaiSessionStore> makeMaiMemoryStore() {
    return std::make_unique<MemoryStore>();
}
