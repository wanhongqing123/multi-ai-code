#include "MaiSystemResourcesTool.h"

#include <json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <utility>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#endif

#include "MaiFilePath.h"
#include "MaiFileSystem.h"

namespace {

using Json = nlohmann::json;

#if !defined(_WIN32)
double seconds(const timeval& time) {
    return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_usec) / 1000000.0;
}
#endif

class SystemResourcesTool final : public MaiTool {
public:
    explicit SystemResourcesTool(std::function<std::optional<MaiGpuResources>()> gpuReader)
        : mGpuReader(std::move(gpuReader)) {}

    std::string name() const override {
        return "system_resources";
    }
    std::string description() const override {
        // 读取 App 自身资源状态，不是系统全局性能诊断；CPU 占比需两次采样才有意义。
        return "Read this App's CPU time and memory footprint, device total memory and CPU count, "
               "and available GPU allocation/budget counters. CPU percent needs two samples. "
               "On unified-memory iPhones dedicated VRAM and GPU utilization are unavailable.";
    }
    std::string parametersSchema() const override {
        // 无参数；工具读取当前 App 的资源快照，不接受任意进程 ID。
        return R"({"type":"object","properties":{},"required":[],"additionalProperties":false})";
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext&) override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.empty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "no arguments expected");
        const MaiSystemResources sample = maiReadSystemResources();
        const double cpuSeconds = sample.processCpuUserSeconds + sample.processCpuSystemSeconds;
        const auto now = std::chrono::steady_clock::now();
        Json response{{"logical_cpu_count", sample.logicalCpuCount},
                      {"total_memory_bytes", sample.totalMemoryBytes},
                      {"process_resident_bytes", sample.processResidentBytes},
                      {"process_cpu_user_seconds", sample.processCpuUserSeconds},
                      {"process_cpu_system_seconds", sample.processCpuSystemSeconds},
                      {"process_cpu_percent_one_core", nullptr}};
        if (sample.processFootprintBytes)
            response["process_physical_footprint_bytes"] = *sample.processFootprintBytes;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            const double elapsed = std::chrono::duration<double>(now - mPreviousAt).count();
            if (mHasPrevious && elapsed > 0.05 && cpuSeconds >= mPreviousCpuSeconds)
                response["process_cpu_percent_one_core"] =
                    (cpuSeconds - mPreviousCpuSeconds) / elapsed * 100.0;
            mPreviousCpuSeconds = cpuSeconds;
            mPreviousAt = now;
            mHasPrevious = true;
        }
        Json gpu{{"available", false},
                 {"gpu_utilization_percent", nullptr},
                 {"dedicated_vram_bytes", nullptr}};
        if (mGpuReader) {
            const auto reading = mGpuReader();
            if (reading) {
                gpu["available"] = true;
                gpu["name"] = reading->name;
                gpu["unified_memory"] = reading->unifiedMemory;
                if (reading->appAllocatedBytes)
                    gpu["app_allocated_bytes"] = *reading->appAllocatedBytes;
                if (reading->recommendedWorkingSetBytes)
                    gpu["recommended_working_set_bytes"] = *reading->recommendedWorkingSetBytes;
                if (reading->dedicatedMemoryBytes)
                    gpu["dedicated_vram_bytes"] = *reading->dedicatedMemoryBytes;
            }
        }
        response["gpu"] = std::move(gpu);
        return MaiToolResult::success(response.dump());
    }

private:
    std::function<std::optional<MaiGpuResources>()> mGpuReader;
    std::mutex mMutex;
    std::chrono::steady_clock::time_point mPreviousAt{};
    double mPreviousCpuSeconds = 0;
    bool mHasPrevious = false;
};

}  // namespace

MaiSystemResources maiReadSystemResources() {
    MaiSystemResources result;
#if defined(__APPLE__)
    const long cores = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (cores > 0) result.logicalCpuCount = static_cast<unsigned>(cores);
    std::size_t size = sizeof(result.totalMemoryBytes);
    if (::sysctlbyname("hw.memsize", &result.totalMemoryBytes, &size, nullptr, 0) != 0)
        result.totalMemoryBytes = 0;
    mach_task_basic_info_data_t basic{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (::task_info(::mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&basic),
                    &count) == KERN_SUCCESS)
        result.processResidentBytes = basic.resident_size;
    task_vm_info_data_t vm{};
    count = TASK_VM_INFO_COUNT;
    if (::task_info(::mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count) ==
        KERN_SUCCESS)
        result.processFootprintBytes = vm.phys_footprint;
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) == 0) {
        result.processCpuUserSeconds = seconds(usage.ru_utime);
        result.processCpuSystemSeconds = seconds(usage.ru_stime);
    }
#elif defined(_WIN32)
    SYSTEM_INFO system{};
    ::GetSystemInfo(&system);
    result.logicalCpuCount = system.dwNumberOfProcessors;
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (::GlobalMemoryStatusEx(&memory)) result.totalMemoryBytes = memory.ullTotalPhys;
    PROCESS_MEMORY_COUNTERS_EX process{};
    if (::K32GetProcessMemoryInfo(::GetCurrentProcess(),
                                  reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process),
                                  sizeof(process))) {
        result.processResidentBytes = process.WorkingSetSize;
        result.processFootprintBytes = process.PrivateUsage;
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    if (::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        ULARGE_INTEGER time{};
        time.LowPart = user.dwLowDateTime;
        time.HighPart = user.dwHighDateTime;
        result.processCpuUserSeconds = static_cast<double>(time.QuadPart) / 10000000.0;
        time.LowPart = kernel.dwLowDateTime;
        time.HighPart = kernel.dwHighDateTime;
        result.processCpuSystemSeconds = static_cast<double>(time.QuadPart) / 10000000.0;
    }
#else
    const long cores = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (cores > 0) result.logicalCpuCount = static_cast<unsigned>(cores);
    struct sysinfo info{};
    if (::sysinfo(&info) == 0)
        result.totalMemoryBytes = static_cast<std::uint64_t>(info.totalram) * info.mem_unit;
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) == 0) {
        result.processCpuUserSeconds = seconds(usage.ru_utime);
        result.processCpuSystemSeconds = seconds(usage.ru_stime);
    }
    std::string status;
    if (!MaiFileSystem::readFile(MaiFilePath::fromUtf8("/proc/self/statm"), status, 128)
             .hasError()) {
        unsigned long long totalPages = 0;
        unsigned long long residentPages = 0;
        if (std::sscanf(status.c_str(), "%llu %llu", &totalPages, &residentPages) == 2) {
            const long pageSize = ::sysconf(_SC_PAGESIZE);
            if (pageSize > 0)
                result.processResidentBytes = static_cast<std::uint64_t>(residentPages) *
                                              static_cast<std::uint64_t>(pageSize);
        }
    }
#endif
    return result;
}

std::unique_ptr<MaiTool> makeMaiSystemResourcesTool(
    std::function<std::optional<MaiGpuResources>()> gpuReader) {
    return std::make_unique<SystemResourcesTool>(std::move(gpuReader));
}
