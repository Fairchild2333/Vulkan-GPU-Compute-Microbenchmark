#ifdef HAVE_METAL

#include "metal_probe.h"

#import <Metal/Metal.h>

namespace gpu_bench {

std::vector<MetalGpuInfo> ProbeMetalDevices() {
    std::vector<MetalGpuInfo> result;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
#pragma clang diagnostic pop

    if (!devices) return result;

    for (NSUInteger i = 0; i < devices.count; ++i) {
        id<MTLDevice> dev = devices[i];
        MetalGpuInfo info;
        info.name         = [dev.name UTF8String];
        info.vramBytes    = dev.recommendedMaxWorkingSetSize;
        info.isHeadless   = dev.isHeadless;
        info.isLowPower   = dev.isLowPower;
        info.isRemovable  = dev.isRemovable;
        info.registryID   = static_cast<std::uint32_t>(dev.registryID);
        result.push_back(std::move(info));
    }

    return result;
}

}  // namespace gpu_bench

#endif  // HAVE_METAL
