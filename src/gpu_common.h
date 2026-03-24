#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace gpu_bench {

class BackToMenuException : public std::exception {
public:
    const char* what() const noexcept override { return "User requested return to backend menu"; }
};

constexpr std::uint32_t kWindowWidth  = 1280;
constexpr std::uint32_t kWindowHeight = 720;
constexpr std::uint32_t kMaxFramesInFlight    = 2;
constexpr std::uint32_t kParticleCount        = 1048576;
constexpr std::uint32_t kComputeWorkGroupSize = 256;
constexpr double kTimingReportIntervalSec = 1.0;

struct Particle {
    float px, py, pz, pw;
    float vx, vy, vz, vw;
};

struct ComputeParams {
    float deltaTime;
    float bounds;
};

struct BenchmarkConfig {
    bool          vsync              = false;
    bool          benchmarkMode      = false;
    bool          hostMemory         = false;
    bool          particlesOverridden = false;
    bool          headless           = false;    // pure compute, no window/swapchain/present
    double        maxRunTimeSec      = 15.0;
    double        warmupTimeSec      = 2.0;
    std::uint32_t benchFrames        = 2000;
    std::uint32_t warmupFrames       = 100;
    std::uint32_t particleCount      = kParticleCount;
    std::uint32_t framesInFlight     = kMaxFramesInFlight;  // runtime override
    const char*   difficultyLabel    = "Medium";
    double        captureAtSec       = -1.0;
    std::string   gpuDisplayName;           // if set, overrides deviceName_ for results/RenderDoc
    // DXGI adapter LUID for precise GPU selection across factory instances.
    // When both are 0, backends fall back to index-based selection.
    std::int64_t  adapterLuidHigh   = 0;
    std::int64_t  adapterLuidLow    = 0;
};

}  // namespace gpu_bench
