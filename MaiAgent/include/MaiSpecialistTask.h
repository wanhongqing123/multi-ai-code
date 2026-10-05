#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "MaiError.h"
#include "MaiTime.h"

// One task session in a specialist revision chain. Identity, original intent, provider task ID,
// and parentTaskId are immutable; execution status and bounded result snapshots may change.
// Credentials, uploaded bytes, and full parent conversation histories must not be stored here.
// Values are persisted as integers in SQLite; append new values, never reorder existing ones.
enum class MaiSpecialistTaskStatus {
    Submitted,
    Running,
    NeedsInput,
    Succeeded,
    Failed,
    Canceled,
};

struct MaiSpecialistTask {
    std::string id;
    std::string ownerSessionId;
    std::string specialistName;
    std::string parentTaskId;
    std::string providerTaskId;
    std::string intent;
    std::string contextSummary;
    std::string inputReference;
    std::string outputPath;
    MaiMillis created = 0;
    MaiSpecialistTaskStatus status = MaiSpecialistTaskStatus::Submitted;
    std::string partialText;
    std::string finalText;
    std::string errorText;
    MaiMillis lastCheckedAt = 0;
    std::string notificationMessageId;
    MaiMillis notificationAttemptAt = 0;
    MaiMillis notifiedAt = 0;
};

// Narrow store interface exposed to model-backed tools. Insertion is atomic and rejects missing
// owners, cross-session parents, and duplicate IDs. A lookup must match both task ID and owner
// session. Streaming text stays in the task row; only one final reply reaches the parent.
class MaiSpecialistTaskStore {
public:
    virtual ~MaiSpecialistTaskStore() = default;
    virtual MaiError insertSpecialistTask(const MaiSpecialistTask& task) = 0;
    virtual bool getSpecialistTask(const std::string& id, const std::string& ownerSessionId,
                                   MaiSpecialistTask& out) const = 0;

    virtual MaiError appendSpecialistText(const std::string& taskId,
                                          const std::string& ownerSessionId,
                                          const std::string& chunk, MaiMillis checkedAt) = 0;
    virtual MaiError updateSpecialistProgress(const std::string& taskId,
                                              const std::string& ownerSessionId,
                                              MaiMillis checkedAt) = 0;
    // A text-only specialist may succeed without a path. Media generation tools must verify
    // their output exists before calling this with Succeeded.
    virtual MaiError finishSpecialistTask(const std::string& taskId,
                                          const std::string& ownerSessionId,
                                          MaiSpecialistTaskStatus status,
                                          const std::string& finalText,
                                          const std::string& outputPath, MaiMillis finishedAt) = 0;
    virtual std::vector<MaiSpecialistTask> listActiveSpecialistTasks(std::size_t limit) const = 0;
    virtual std::vector<MaiSpecialistTask> listUnnotifiedSpecialistTasks(
        std::size_t limit) const = 0;
    // Reserve one stable assistant ID before notifying. Retry returns the same ID; mark done
    // only after the parent completes its answer. Both operations reject a different owner.
    virtual MaiResult<std::string> reserveSpecialistNotification(
        const std::string& taskId, const std::string& ownerSessionId,
        const std::string& proposedMessageId, MaiMillis attemptedAt) = 0;
    virtual MaiError markSpecialistNotified(const std::string& taskId,
                                            const std::string& ownerSessionId,
                                            const std::string& messageId, MaiMillis notifiedAt) = 0;
};
