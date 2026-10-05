#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "MaiTool.h"

// GPU counters are supplied by a platform host. On iOS, Metal reports memory allocated by this
// App and a recommended working-set budget, but does not expose a public GPU utilization meter or
// dedicated VRAM amount on unified-memory devices. Missing fields must remain absent in results.
struct MaiGpuResources {
    std::string name;
    bool unifiedMemory = false;
    std::optional<std::uint64_t> appAllocatedBytes;
    std::optional<std::uint64_t> recommendedWorkingSetBytes;
    std::optional<std::uint64_t> dedicatedMemoryBytes;
};

struct MaiSystemResources {
    unsigned logicalCpuCount = 0;
    std::uint64_t totalMemoryBytes = 0;
    std::uint64_t processResidentBytes = 0;
    std::optional<std::uint64_t> processFootprintBytes;
    double processCpuUserSeconds = 0;
    double processCpuSystemSeconds = 0;
};

// Synchronous OS snapshot. Caller must run it off the UI thread. Returns unavailable counters as
// zero/empty, never a guessed value. CPU utilization requires two observations and is calculated
// by the tool wrapper from process CPU time over monotonic wall time.
MaiSystemResources maiReadSystemResources();

std::unique_ptr<MaiTool> makeMaiSystemResourcesTool(
    std::function<std::optional<MaiGpuResources>()> gpuReader = {});
