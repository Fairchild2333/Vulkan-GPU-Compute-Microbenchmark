#pragma once

#include <cstdint>

namespace gpu_bench {

constexpr std::uint32_t kWindowWidth  = 1280;
constexpr std::uint32_t kWindowHeight = 720;
constexpr std::uint32_t kMaxFramesInFlight    = 2;
constexpr std::uint32_t kParticleCount        = 65536;
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
    bool          vsync         = false;
    bool          benchmarkMode = false;
    std::uint32_t benchFrames   = 2000;
    std::uint32_t warmupFrames  = 100;
    std::uint32_t particleCount = kParticleCount;
};

}  // namespace gpu_bench
