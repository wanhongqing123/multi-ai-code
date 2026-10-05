#include "agent/MaiDesktopGpu.h"

#include <dxgi1_4.h>
#include <windows.h>

#include <string>
#include <utility>

namespace {

std::string toUtf8(const wchar_t* text) {
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
    result.pop_back();
    return result;
}

}  // namespace

std::optional<MaiGpuResources> maiDesktopGpuResources() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(::CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                     reinterpret_cast<void**>(&factory))))
        return std::nullopt;
    std::optional<MaiGpuResources> result;
    for (UINT index = 0; !result; ++index) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(index, &adapter) != S_OK) break;
        DXGI_ADAPTER_DESC1 description{};
        const bool hardware = SUCCEEDED(adapter->GetDesc1(&description)) &&
                              !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
        if (hardware) {
            MaiGpuResources gpu;
            gpu.name = toUtf8(description.Description);
            gpu.unifiedMemory = description.DedicatedVideoMemory == 0;
            if (description.DedicatedVideoMemory > 0)
                gpu.dedicatedMemoryBytes = description.DedicatedVideoMemory;
            IDXGIAdapter3* adapter3 = nullptr;
            if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3),
                                                  reinterpret_cast<void**>(&adapter3)))) {
                DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
                if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                                                             &memory))) {
                    gpu.appAllocatedBytes = memory.CurrentUsage;
                    gpu.recommendedWorkingSetBytes = memory.Budget;
                }
                adapter3->Release();
            }
            result = std::move(gpu);
        }
        adapter->Release();
    }
    factory->Release();
    return result;
}
