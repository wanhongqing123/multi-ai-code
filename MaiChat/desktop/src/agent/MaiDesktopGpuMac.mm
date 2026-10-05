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
