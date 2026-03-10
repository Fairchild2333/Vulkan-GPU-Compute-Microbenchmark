#pragma once

#include <cstdint>
#include <stdexcept>

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
    double        maxRunTimeSec      = 15.0;
    double        warmupTimeSec      = 2.0;
    std::uint32_t benchFrames        = 2000;
    std::uint32_t warmupFrames       = 100;
    std::uint32_t particleCount      = kParticleCount;
    const char*   difficultyLabel    = "Medium";
};

}  // namespace gpu_bench
