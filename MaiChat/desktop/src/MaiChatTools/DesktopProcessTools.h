#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "MaiError.h"

class MaiToolRegistry;

struct DesktopProcessSnapshot {
    std::string output;
    std::uint64_t cursor = 0;
    std::uint64_t firstAvailableCursor = 0;
    bool running = false;
    bool hasMore = false;
    bool olderOutputDiscarded = false;
    int exitCode = 0;
};

// Ordinary code callers and Agent tools share this Session-owned process manager.
// Processes live until they exit, are stopped, or the owning Agent is destroyed.
class DesktopProcessManager {
public:
    DesktopProcessManager();
    ~DesktopProcessManager();
    DesktopProcessManager(const DesktopProcessManager&) = delete;
    DesktopProcessManager& operator=(const DesktopProcessManager&) = delete;

    MaiResult<std::string> start(const std::string& sessionId,
        const std::string& workingDirectory, const std::string& command);
    MaiResult<DesktopProcessSnapshot> read(const std::string& sessionId,
        const std::string& processId, std::uint64_t cursor) const;
    MaiError write(const std::string& sessionId, const std::string& processId,
        const std::string& input);
    MaiError stop(const std::string& sessionId, const std::string& processId);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void registerDesktopProcessTools(MaiToolRegistry& registry,
    std::shared_ptr<DesktopProcessManager> manager);
