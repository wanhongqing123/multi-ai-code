#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "MaiError.h"
#include "MaiTool.h"

struct sqlite3;

struct MaiScheduledAgentTask {
    std::string id;
    std::string sessionId;
    std::string title;
    std::string instruction;
    std::string status;
    std::string messageId;
    std::string error;
    std::int64_t dueAtMs = 0;
    std::int64_t createdAtMs = 0;
    std::int64_t startedAtMs = 0;
};

// Persistent owner-device store. All callers, including the Agent tools and
// the ordinary scheduler, share the same SQLite implementation.
class MaiSessionScheduleStore final {
public:
    static MaiResult<std::shared_ptr<MaiSessionScheduleStore>> open(
        const std::string& databasePath);
    ~MaiSessionScheduleStore();

    MaiResult<MaiScheduledAgentTask> create(const std::string& sessionId,
        const std::string& title, const std::string& instruction,
        std::int64_t dueAtMs, std::int64_t nowMs,
        const std::string& originKey = {});
    MaiResult<std::vector<MaiScheduledAgentTask>> list(const std::string& sessionId);
    MaiError cancel(const std::string& sessionId, const std::string& taskId);
    MaiError cancelSession(const std::string& sessionId);
    MaiResult<std::vector<MaiScheduledAgentTask>> claimDue(std::int64_t nowMs,
        int limit = 10);
    MaiError markSubmitted(const std::string& taskId, const std::string& messageId);
    MaiError markFinished(const std::string& taskId, bool succeeded,
        const std::string& error = {});
    MaiError releaseBusy(const std::string& taskId);
    MaiError markInterruptedUncertain();

private:
    explicit MaiSessionScheduleStore(sqlite3* database);
    sqlite3* database_ = nullptr;
    std::mutex mutex_;
};

class MaiSessionScheduleRunner final {
public:
    using SessionCheck = std::function<bool(const std::string&)>;
    using Submit = std::function<MaiResult<std::string>(
        const std::string& sessionId, const std::string& instruction)>;

    MaiSessionScheduleRunner(std::shared_ptr<MaiSessionScheduleStore> store,
        SessionCheck exists, SessionCheck busy, Submit submit);
    MaiError poll(std::int64_t nowMs);
    MaiError finish(const std::string& sessionId, const std::string& error = {});

private:
    std::shared_ptr<MaiSessionScheduleStore> store_;
    SessionCheck exists_;
    SessionCheck busy_;
    Submit submit_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::string> activeTaskBySession_;
};

// The tool operates only on context.sessionId; it never accepts an arbitrary
// Session ID or auto-approves later model/tool actions.
void registerMaiSessionScheduleTools(MaiToolRegistry& tools,
    std::shared_ptr<MaiSessionScheduleStore> store, const std::string& deviceName);
