#pragma once

#ifdef HAVE_METAL

#include <cstdint>
#include <string>
#include <vector>

namespace gpu_bench {

struct MetalGpuInfo {
    std::string name;
    std::uint64_t vramBytes   = 0;
    bool isHeadless           = false;
    bool isLowPower           = false;
    bool isRemovable          = false;
    std::uint32_t registryID  = 0;
};

std::vector<MetalGpuInfo> ProbeMetalDevices();

}  // namespace gpu_bench

#endif  // HAVE_METAL
