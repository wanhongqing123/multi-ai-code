// 文件作用：查询设备资源与系统状态供 Agent 决策。
// 对外类型：MaiGpuResources、MaiSystemResources；线程、失败和生命周期以类型附近注释为准。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "MaiTool.h"

// GPU 指标由平台宿主提供。iOS Metal 可报告本 App 已分配的内存与建议工作集预算，
// 但统一内存设备没有公开的 GPU 利用率或专用显存容量接口。
// 不可取得的字段在结果中必须缺席，不能填猜测值。
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

// 同步读取操作系统快照，调用方必须避开 UI 线程。
// 不可用的计数器返回零或空，不编造数值。
// CPU 利用率需要两次观测，由工具封装按进程 CPU 时间与单调墙上时间计算。
MaiSystemResources maiReadSystemResources();

std::unique_ptr<MaiTool> makeMaiSystemResourcesTool(
    std::function<std::optional<MaiGpuResources>()> gpuReader = {});
