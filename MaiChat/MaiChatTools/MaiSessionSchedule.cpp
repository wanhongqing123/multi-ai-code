#include "MaiSessionSchedule.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include <sqlite3.h>

#include "MaiFilePath.h"
#include "MaiIdGenerator.h"

namespace {

constexpr std::int64_t kCatchUpWindowMs = 24LL * 60 * 60 * 1000;
constexpr std::int64_t kMaxAheadMs = 366LL * 24 * 60 * 60 * 1000;
constexpr const char* kColumns =
    "id,session_id,title,instruction,status,message_id,error,due_at_ms,"
    "created_at_ms,started_at_ms";

using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

MaiError sqlError(sqlite3* database) {
    return {MaiErrorCode::Internal,
            std::string("scheduled task storage failed: ") + sqlite3_errmsg(database)};
}

Statement prepare(sqlite3* database, const char* sql) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
        return {nullptr, sqlite3_finalize};
    return {raw, sqlite3_finalize};
}

bool execute(sqlite3* database, const char* sql) {
    return sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

void bindText(sqlite3_stmt* statement, int index, const std::string& value) {
    sqlite3_bind_text(statement, index, value.data(), int(value.size()), SQLITE_TRANSIENT);
}

std::string text(sqlite3_stmt* statement, int index) {
    const auto* bytes = sqlite3_column_text(statement, index);
    return bytes ? reinterpret_cast<const char*>(bytes) : std::string();
}

MaiScheduledAgentTask readTask(sqlite3_stmt* statement) {
    MaiScheduledAgentTask task;
    task.id = text(statement, 0);
    task.sessionId = text(statement, 1);
    task.title = text(statement, 2);
    task.instruction = text(statement, 3);
    task.status = text(statement, 4);
    task.messageId = text(statement, 5);
    task.error = text(statement, 6);
    task.dueAtMs = sqlite3_column_int64(statement, 7);
    task.createdAtMs = sqlite3_column_int64(statement, 8);
    task.startedAtMs = sqlite3_column_int64(statement, 9);
    return task;
}

bool validSessionId(const std::string& id) {
    if (id.empty() || id.size() > 128) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

}  // namespace

MaiSessionScheduleStore::MaiSessionScheduleStore(sqlite3* database)
    : database_(database) {}

MaiSessionScheduleStore::~MaiSessionScheduleStore() {
    if (database_) sqlite3_close(database_);
}

MaiResult<std::shared_ptr<MaiSessionScheduleStore>>
MaiSessionScheduleStore::open(const std::string& databasePath) {
    if (databasePath.empty() || (databasePath != ":memory:" &&
        !MaiFilePath::fromUtf8(databasePath).isAbsolute()))
        return {MaiErrorCode::InvalidInput, "schedule database needs an absolute path"};
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(databasePath.c_str(), &database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        const MaiError error = database ? sqlError(database) :
            MaiError{MaiErrorCode::Internal, "schedule database could not be opened"};
        if (database) sqlite3_close(database);
        return error;
    }
    std::shared_ptr<MaiSessionScheduleStore> store(
        new MaiSessionScheduleStore(database));
    sqlite3_busy_timeout(database, 3000);
    if (!execute(database, "PRAGMA journal_mode=WAL") ||
        !execute(database, "CREATE TABLE IF NOT EXISTS maichat_scheduled_tasks("
            "id TEXT PRIMARY KEY,session_id TEXT NOT NULL,title TEXT NOT NULL,"
            "instruction TEXT NOT NULL,due_at_ms INTEGER NOT NULL,"
            "created_at_ms INTEGER NOT NULL,started_at_ms INTEGER NOT NULL DEFAULT 0,"
            "status TEXT NOT NULL CHECK(status IN ('scheduled','started','completed',"
            "'failed','uncertain','missed','canceled')),"
            "message_id TEXT NOT NULL DEFAULT '',error TEXT NOT NULL DEFAULT '',"
            "origin_key TEXT)") ||
        !execute(database, "CREATE INDEX IF NOT EXISTS maichat_scheduled_due "
            "ON maichat_scheduled_tasks(status,due_at_ms)"))
        return sqlError(database);
    auto columns = prepare(database, "PRAGMA table_info(maichat_scheduled_tasks)");
    if (!columns) return sqlError(database);
    bool hasOriginKey = false;
    int code;
    while ((code = sqlite3_step(columns.get())) == SQLITE_ROW)
        hasOriginKey |= text(columns.get(), 1) == "origin_key";
    if (code != SQLITE_DONE) return sqlError(database);
    columns.reset();
    if ((!hasOriginKey && !execute(database,
            "ALTER TABLE maichat_scheduled_tasks ADD COLUMN origin_key TEXT")) ||
        !execute(database, "CREATE UNIQUE INDEX IF NOT EXISTS maichat_scheduled_origin "
            "ON maichat_scheduled_tasks(origin_key) WHERE origin_key IS NOT NULL"))
        return sqlError(database);
    return store;
}

MaiResult<MaiScheduledAgentTask> MaiSessionScheduleStore::create(
    const std::string& sessionId, const std::string& title,
    const std::string& instruction, std::int64_t dueAtMs, std::int64_t nowMs,
    const std::string& originKey) {
    if (!validSessionId(sessionId) || title.empty() || title.size() > 240 ||
        title.find('\0') != std::string::npos || instruction.empty() ||
        instruction.size() > 16 * 1024 || instruction.find('\0') != std::string::npos ||
        nowMs <= 0 ||
        originKey.size() > 256 || originKey.find('\0') != std::string::npos)
        return {MaiErrorCode::InvalidInput, "invalid Session task, title, instruction or time"};
    std::lock_guard<std::mutex> guard(mutex_);
    if (!originKey.empty()) {
        const std::string query = std::string("SELECT ") + kColumns +
            " FROM maichat_scheduled_tasks WHERE origin_key=?";
        auto previous = prepare(database_, query.c_str());
        if (!previous) return sqlError(database_);
        bindText(previous.get(), 1, originKey);
        const int found = sqlite3_step(previous.get());
        if (found == SQLITE_ROW) {
            MaiScheduledAgentTask task = readTask(previous.get());
            if (task.sessionId != sessionId || task.title != title ||
                task.instruction != instruction || task.dueAtMs != dueAtMs)
                return {MaiErrorCode::InvalidInput,
                        "scheduled task request ID was reused for different content"};
            return task;
        }
        if (found != SQLITE_DONE) return sqlError(database_);
    }
    if (dueAtMs <= nowMs + 1000 || dueAtMs - nowMs > kMaxAheadMs)
        return {MaiErrorCode::InvalidInput, "scheduled task must be in the future"};
    auto count = prepare(database_, "SELECT COUNT(*) FROM maichat_scheduled_tasks "
        "WHERE session_id=? AND status IN ('scheduled','started')");
    if (!count) return sqlError(database_);
    bindText(count.get(), 1, sessionId);
    if (sqlite3_step(count.get()) != SQLITE_ROW) return sqlError(database_);
    if (sqlite3_column_int(count.get(), 0) >= 200)
        return {MaiErrorCode::InvalidInput, "this Session already has 200 active tasks"};
    MaiScheduledAgentTask task;
    task.id = MaiIdGenerator::generate("schtask_");
    task.sessionId = sessionId;
    task.title = title;
    task.instruction = instruction;
    task.status = "scheduled";
    task.dueAtMs = dueAtMs;
    task.createdAtMs = nowMs;
    auto insert = prepare(database_, "INSERT INTO maichat_scheduled_tasks("
        "id,session_id,title,instruction,due_at_ms,created_at_ms,status,origin_key) "
        "VALUES(?,?,?,?,?,?,'scheduled',?)");
    if (!insert) return sqlError(database_);
    bindText(insert.get(), 1, task.id);
    bindText(insert.get(), 2, sessionId);
    bindText(insert.get(), 3, title);
    bindText(insert.get(), 4, instruction);
    sqlite3_bind_int64(insert.get(), 5, dueAtMs);
    sqlite3_bind_int64(insert.get(), 6, nowMs);
    if (originKey.empty()) sqlite3_bind_null(insert.get(), 7);
    else bindText(insert.get(), 7, originKey);
    if (sqlite3_step(insert.get()) != SQLITE_DONE) return sqlError(database_);
    return task;
}

MaiResult<std::vector<MaiScheduledAgentTask>>
MaiSessionScheduleStore::list(const std::string& sessionId) {
    if (!validSessionId(sessionId))
        return {MaiErrorCode::InvalidInput, "invalid Session ID"};
    std::lock_guard<std::mutex> guard(mutex_);
    const std::string query = std::string("SELECT ") + kColumns +
        " FROM maichat_scheduled_tasks WHERE session_id=? "
        "ORDER BY due_at_ms DESC,id DESC LIMIT 200";
    auto statement = prepare(database_, query.c_str());
    if (!statement) return sqlError(database_);
    bindText(statement.get(), 1, sessionId);
    std::vector<MaiScheduledAgentTask> tasks;
    int code;
    while ((code = sqlite3_step(statement.get())) == SQLITE_ROW)
        tasks.push_back(readTask(statement.get()));
    if (code != SQLITE_DONE) return sqlError(database_);
    return tasks;
}

MaiError MaiSessionScheduleStore::cancel(const std::string& sessionId,
                                         const std::string& taskId) {
    if (!validSessionId(sessionId) || !validSessionId(taskId))
        return {MaiErrorCode::InvalidInput, "invalid Session or task ID"};
    std::lock_guard<std::mutex> guard(mutex_);
    auto statement = prepare(database_, "UPDATE maichat_scheduled_tasks "
        "SET status='canceled' WHERE id=? AND session_id=? AND status='scheduled'");
    if (!statement) return sqlError(database_);
    bindText(statement.get(), 1, taskId);
    bindText(statement.get(), 2, sessionId);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) return sqlError(database_);
    return sqlite3_changes(database_) == 1 ? MaiError{} :
        MaiError{MaiErrorCode::NotFound, "scheduled task was not found or has started"};
}

MaiError MaiSessionScheduleStore::cancelSession(const std::string& sessionId) {
    if (!validSessionId(sessionId)) return {MaiErrorCode::InvalidInput, "invalid Session ID"};
    std::lock_guard<std::mutex> guard(mutex_);
    auto statement = prepare(database_, "UPDATE maichat_scheduled_tasks "
        "SET status='canceled' WHERE session_id=? AND status='scheduled'");
    if (!statement) return sqlError(database_);
    bindText(statement.get(), 1, sessionId);
    return sqlite3_step(statement.get()) == SQLITE_DONE ? MaiError{} : sqlError(database_);
}

MaiResult<std::vector<MaiScheduledAgentTask>>
MaiSessionScheduleStore::claimDue(std::int64_t nowMs, int limit) {
    if (nowMs <= 0 || limit < 1 || limit > 20)
        return {MaiErrorCode::InvalidInput, "invalid scheduler clock or batch size"};
    std::lock_guard<std::mutex> guard(mutex_);
    if (!execute(database_, "BEGIN IMMEDIATE")) return sqlError(database_);
    auto rollback = [&] { execute(database_, "ROLLBACK"); return sqlError(database_); };
    auto missed = prepare(database_, "UPDATE maichat_scheduled_tasks "
        "SET status='missed' WHERE status='scheduled' AND due_at_ms<?");
    if (!missed) return rollback();
    sqlite3_bind_int64(missed.get(), 1, nowMs - kCatchUpWindowMs);
    if (sqlite3_step(missed.get()) != SQLITE_DONE) return rollback();
    const std::string query = std::string("SELECT ") + kColumns +
        " FROM maichat_scheduled_tasks WHERE status='scheduled' AND due_at_ms<=? "
        "ORDER BY due_at_ms,id LIMIT ?";
    auto select = prepare(database_, query.c_str());
    if (!select) return rollback();
    sqlite3_bind_int64(select.get(), 1, nowMs);
    sqlite3_bind_int(select.get(), 2, limit);
    std::vector<MaiScheduledAgentTask> tasks;
    int code;
    while ((code = sqlite3_step(select.get())) == SQLITE_ROW)
        tasks.push_back(readTask(select.get()));
    if (code != SQLITE_DONE) return rollback();
    for (MaiScheduledAgentTask& task : tasks) {
        auto claim = prepare(database_, "UPDATE maichat_scheduled_tasks "
            "SET status='started',started_at_ms=? WHERE id=? AND status='scheduled'");
        if (!claim) return rollback();
        sqlite3_bind_int64(claim.get(), 1, nowMs);
        bindText(claim.get(), 2, task.id);
        if (sqlite3_step(claim.get()) != SQLITE_DONE ||
            sqlite3_changes(database_) != 1) return rollback();
        task.status = "started";
        task.startedAtMs = nowMs;
    }
    if (!execute(database_, "COMMIT")) return rollback();
    return tasks;
}

MaiError MaiSessionScheduleStore::markSubmitted(const std::string& taskId,
                                                const std::string& messageId) {
    if (!validSessionId(taskId) || !validSessionId(messageId))
        return {MaiErrorCode::InvalidInput, "invalid task or message ID"};
    std::lock_guard<std::mutex> guard(mutex_);
    auto statement = prepare(database_, "UPDATE maichat_scheduled_tasks SET message_id=? "
        "WHERE id=? AND status='started' AND message_id=''");
    if (!statement) return sqlError(database_);
    bindText(statement.get(), 1, messageId);
    bindText(statement.get(), 2, taskId);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) return sqlError(database_);
    return sqlite3_changes(database_) == 1 ? MaiError{} :
        MaiError{MaiErrorCode::NotFound, "started task was not found"};
}

MaiError MaiSessionScheduleStore::markFinished(const std::string& taskId,
                                               bool succeeded,
                                               const std::string& error) {
    if (!validSessionId(taskId) || error.size() > 1000)
        return {MaiErrorCode::InvalidInput, "invalid task ID or error"};
    std::lock_guard<std::mutex> guard(mutex_);
    auto statement = prepare(database_, "UPDATE maichat_scheduled_tasks SET status=?,error=? "
        "WHERE id=? AND status='started'");
    if (!statement) return sqlError(database_);
    bindText(statement.get(), 1, succeeded ? "completed" : "failed");
    bindText(statement.get(), 2, error);
    bindText(statement.get(), 3, taskId);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) return sqlError(database_);
    return sqlite3_changes(database_) == 1 ? MaiError{} :
        MaiError{MaiErrorCode::NotFound, "started task was not found"};
}

MaiError MaiSessionScheduleStore::releaseBusy(const std::string& taskId) {
    if (!validSessionId(taskId)) return {MaiErrorCode::InvalidInput, "invalid task ID"};
    std::lock_guard<std::mutex> guard(mutex_);
    auto statement = prepare(database_, "UPDATE maichat_scheduled_tasks "
        "SET status='scheduled',started_at_ms=0 WHERE id=? AND status='started' "
        "AND message_id=''");
    if (!statement) return sqlError(database_);
    bindText(statement.get(), 1, taskId);
    return sqlite3_step(statement.get()) == SQLITE_DONE ? MaiError{} : sqlError(database_);
}

MaiError MaiSessionScheduleStore::markInterruptedUncertain() {
    std::lock_guard<std::mutex> guard(mutex_);
    return execute(database_, "UPDATE maichat_scheduled_tasks "
        "SET status='uncertain',error='App stopped before execution was confirmed' "
        "WHERE status='started'") ? MaiError{} : sqlError(database_);
}

MaiSessionScheduleRunner::MaiSessionScheduleRunner(
    std::shared_ptr<MaiSessionScheduleStore> store, SessionCheck exists,
    SessionCheck busy, Submit submit)
    : store_(std::move(store)), exists_(std::move(exists)),
      busy_(std::move(busy)), submit_(std::move(submit)) {}

MaiError MaiSessionScheduleRunner::poll(std::int64_t nowMs) {
    if (!store_ || !exists_ || !busy_ || !submit_)
        return {MaiErrorCode::NotConfigured, "scheduled task runner is unavailable"};
    const auto due = store_->claimDue(nowMs);
    if (!due) return due.error();
    for (const MaiScheduledAgentTask& task : due.value()) {
        if (!exists_(task.sessionId)) {
            const MaiError failed = store_->markFinished(task.id, false,
                "Session was deleted before the scheduled task ran");
            if (failed.hasError()) return failed;
            continue;
        }
        bool alreadyScheduled = false;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            alreadyScheduled = activeTaskBySession_.count(task.sessionId) != 0;
        }
        if (alreadyScheduled || busy_(task.sessionId)) {
            const MaiError released = store_->releaseBusy(task.id);
            if (released.hasError()) return released;
            continue;
        }
        {
            std::lock_guard<std::mutex> guard(mutex_);
            activeTaskBySession_[task.sessionId] = task.id;
        }
        const MaiResult<std::string> sent = submit_(task.sessionId, task.instruction);
        if (!sent) {
            {
                std::lock_guard<std::mutex> guard(mutex_);
                activeTaskBySession_.erase(task.sessionId);
            }
            const MaiError handled = sent.error().code() == MaiErrorCode::Busy
                ? store_->releaseBusy(task.id)
                : store_->markFinished(task.id, false, sent.error().message());
            if (handled.hasError()) return handled;
            continue;
        }
        const MaiError saved = store_->markSubmitted(task.id, sent.value());
        if (saved.hasError()) return saved;
    }
    return {};
}

MaiError MaiSessionScheduleRunner::finish(const std::string& sessionId,
                                         const std::string& error) {
    std::string taskId;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = activeTaskBySession_.find(sessionId);
        if (found == activeTaskBySession_.end()) return {};
        taskId = found->second;
        activeTaskBySession_.erase(found);
    }
    return store_->markFinished(taskId, error.empty(), error.substr(0, 1000));
}
