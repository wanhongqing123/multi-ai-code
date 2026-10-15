// 文件作用：桌面端创作中心和 Agent 会话；MaiDesktopGpuMac 负责macOS GPU 设备和绘制资源接入，本文件给出其实现。
// 直接依赖：agent/MaiDesktopGpu.h；实现与这些接口协作。
#include "agent/MaiDesktopGpu.h"

#import <Metal/Metal.h>

std::optional<MaiGpuResources> maiDesktopGpuResources() {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return std::nullopt;
    MaiGpuResources result;
    result.name = [[device name] UTF8String] ?: "";
    result.unifiedMemory = device.hasUnifiedMemory;
    result.appAllocatedBytes = device.currentAllocatedSize;
    result.recommendedWorkingSetBytes = device.recommendedMaxWorkingSetSize;
#if !__has_feature(objc_arc)
    [device release];
#endif
    return result;
}
