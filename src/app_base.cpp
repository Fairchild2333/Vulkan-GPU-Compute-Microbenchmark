#include "app_base.h"

#include <GLFW/glfw3.h>

#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <fstream>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace gpu_bench {

std::string AppBase::GetCpuName() {
#ifdef _WIN32
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            0, KEY_READ, &key) == ERROR_SUCCESS) {
        char buf[256]{};
        DWORD size = sizeof(buf);
        if (RegQueryValueExA(key, "ProcessorNameString", nullptr, nullptr,
                reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS) {
            RegCloseKey(key);
            std::string name(buf);
            while (!name.empty() && name.front() == ' ') name.erase(name.begin());
            return name;
        }
        RegCloseKey(key);
    }
#elif defined(__linux__)
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.rfind("model name", 0) == 0) {
            auto pos = line.find(':');
            if (pos != std::string::npos) {
                std::string name = line.substr(pos + 1);
                while (!name.empty() && name.front() == ' ') name.erase(name.begin());
                return name;
            }
        }
    }
#elif defined(__APPLE__)
    char buf[256]{};
    size_t len = sizeof(buf);
    if (sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0)
        return std::string(buf);
#endif
    return "Unknown";
}

AppBase::AppBase(std::int32_t gpuIndex, std::string shaderDir,
                 BenchmarkConfig config)
    : requestedGpuIndex_(gpuIndex),
      shaderDir_(std::move(shaderDir)),
      config_(config) {}

AppBase::~AppBase() = default;

void AppBase::Run() {
    InitWindow();
    GenerateInitialParticles();
    InitBackend();
    glfwShowWindow(window_);
    MainLoop();
    PrintSummary();

    auto result = CollectResult();
    if (AppendResult(result)) {
        std::cout << "[Results] Saved as " << result.id
                  << " -> " << ResultsFilePath() << std::endl;
    }

    CleanupBackend();
    CleanupWindow();
}

void AppBase::InitWindow() {
    if (glfwInit() != GLFW_TRUE) {
        throw std::runtime_error("glfwInit failed");
    }

    if (NeedsOpenGLContext()) {
        glfwWindowHint(GLFW_CLIENT_API,            GLFW_OPENGL_API);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,  4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,  3);
        glfwWindowHint(GLFW_OPENGL_PROFILE,         GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT,  GL_TRUE);
#endif
    } else {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    }
    glfwWindowHint(GLFW_RESIZABLE,  GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE,    GLFW_FALSE);

    const std::string title = GetBackendName() + " GPU Compute & Rendering Pipeline";
    window_ = glfwCreateWindow(static_cast<int>(kWindowWidth),
                               static_cast<int>(kWindowHeight),
                               title.c_str(), nullptr, nullptr);
    if (window_ == nullptr) {
        glfwTerminate();
        throw std::runtime_error("glfwCreateWindow failed");
    }
}

void AppBase::GenerateInitialParticles() {
    initialParticles_.resize(config_.particleCount);

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> posDist(-0.8f, 0.8f);
    std::uniform_real_distribution<float> velDist(-0.2f, 0.2f);

    for (std::uint32_t i = 0; i < config_.particleCount; ++i) {
        initialParticles_[i] = {
            posDist(rng), posDist(rng), 0.0f, 1.0f,
            velDist(rng), velDist(rng), 0.0f, 0.0f,
        };
    }
}

void AppBase::MainLoop() {
    lastFrameTime_ = glfwGetTime();
    runStartTime_  = lastFrameTime_;

    const std::uint32_t totalBenchFrames =
        config_.benchFrames + config_.warmupFrames;

    while (glfwWindowShouldClose(window_) == GLFW_FALSE) {
        glfwPollEvents();

        const double currentTime = glfwGetTime();
        const auto   deltaTime   = static_cast<float>(currentTime - lastFrameTime_);
        lastFrameTime_ = currentTime;

        DrawFrame(deltaTime);
        ++totalFrameCount_;

        const double elapsed = currentTime - runStartTime_;

        if (config_.benchmarkMode) {
            if (totalFrameCount_ == config_.warmupFrames) {
                benchStartTime_ = glfwGetTime();
                warmupDone_ = true;
            }
            if (totalFrameCount_ > config_.warmupFrames) {
                ++benchMeasuredFrames_;
                benchMinFrameTime_ =
                    std::min(benchMinFrameTime_,
                             static_cast<double>(deltaTime));
            }
            if (totalFrameCount_ >= totalBenchFrames) {
                benchEndTime_ = glfwGetTime();
                break;
            }
        } else {
            if (!warmupDone_ && elapsed >= config_.warmupTimeSec) {
                warmupDone_ = true;
                benchStartTime_ = currentTime;
            }
            if (warmupDone_) {
                ++benchMeasuredFrames_;
                benchMinFrameTime_ =
                    std::min(benchMinFrameTime_,
                             static_cast<double>(deltaTime));
            }
            if (config_.maxRunTimeSec > 0.0 && elapsed >= config_.maxRunTimeSec) {
                benchEndTime_ = glfwGetTime();
                break;
            }
        }

        ReportTimingIfDue(static_cast<double>(deltaTime));
    }

    if (benchEndTime_ == 0.0)
        benchEndTime_ = glfwGetTime();

    WaitIdle();
}

void AppBase::AccumulateTiming(double computeMs, double renderMs,
                               double totalGpuMs) {
    accumComputeMs_  += computeMs;
    accumRenderMs_   += renderMs;
    accumTotalGpuMs_ += totalGpuMs;
    ++timingSampleCount_;

    if (warmupDone_) {
        benchMinComputeMs_  = std::min(benchMinComputeMs_,  computeMs);
        benchMaxComputeMs_  = std::max(benchMaxComputeMs_,  computeMs);
        benchMinRenderMs_   = std::min(benchMinRenderMs_,   renderMs);
        benchMaxRenderMs_   = std::max(benchMaxRenderMs_,   renderMs);
        benchMinTotalGpuMs_ = std::min(benchMinTotalGpuMs_, totalGpuMs);
        benchMaxTotalGpuMs_ = std::max(benchMaxTotalGpuMs_, totalGpuMs);
        benchSumComputeMs_  += computeMs;
        benchSumRenderMs_   += renderMs;
        benchSumTotalGpuMs_ += totalGpuMs;
        ++benchSampleCount_;
    }
}

void AppBase::ReportTimingIfDue(double deltaTime) {
    timingReportTimer_ += deltaTime;
    ++frameCount_;

    if (timingReportTimer_ < kTimingReportIntervalSec)
        return;

    const double fps = static_cast<double>(frameCount_) / timingReportTimer_;

    if (timingSampleCount_ > 0) {
        const double avgCompute = accumComputeMs_  / timingSampleCount_;
        const double avgRender  = accumRenderMs_   / timingSampleCount_;
        const double avgTotal   = accumTotalGpuMs_ / timingSampleCount_;

        const std::string devName = GetDeviceName();
        const bool sw = (devName.find("Basic Render") != std::string::npos ||
                         devName.find("WARP") != std::string::npos ||
                         devName.find("Software") != std::string::npos);

        std::cout << "[" << (sw ? "Timing" : "GPU Timing") << "] Compute: "
                  << std::fixed << std::setprecision(3)
                  << avgCompute << " ms | Render: " << avgRender
                  << " ms | Total: " << avgTotal
                  << " ms | FPS: " << static_cast<int>(fps);

        if (config_.benchmarkMode) {
            std::cout << " | Frame " << totalFrameCount_ << "/"
                      << (config_.benchFrames + config_.warmupFrames);
        }
        std::cout << std::endl;
    } else {
        std::cout << "[FPS] " << static_cast<int>(fps) << std::endl;
    }

    std::string title = GetBackendName() + " Particle Sim  |  FPS: "
                      + std::to_string(static_cast<int>(fps));
    if (timingSampleCount_ > 0) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2)
            << "  |  GPU: " << (accumTotalGpuMs_ / timingSampleCount_) << " ms";
        title += oss.str();
    }
    glfwSetWindowTitle(window_, title.c_str());

    accumComputeMs_    = 0.0;
    accumRenderMs_     = 0.0;
    accumTotalGpuMs_   = 0.0;
    timingSampleCount_ = 0;
    frameCount_        = 0;
    timingReportTimer_ = 0.0;
}

void AppBase::PrintSummary() const {
    const double duration = benchEndTime_ - benchStartTime_;
    const double avgFps   = (duration > 0.0)
        ? static_cast<double>(benchMeasuredFrames_) / duration
        : 0.0;

    const std::string devName = GetDeviceName();
    const bool isSoftware = (devName.find("Basic Render") != std::string::npos ||
                             devName.find("WARP") != std::string::npos ||
                             devName.find("Software") != std::string::npos);
    const char* devLabel   = isSoftware ? "CPU Renderer:" : "GPU:";
    const char* timerLabel = isSoftware ? "Device Timing (ms)" : "GPU Timing (ms)";
    const char* totalLabel = isSoftware ? "Total:      " : "Total GPU:  ";

    std::cout << "\n"
        "==========================================================\n"
        "                   Benchmark Summary\n"
        "==========================================================\n";

    std::cout << std::left << std::fixed
        << std::setw(14) << "Graphics API:" << GetBackendName() << "\n"
        << std::setw(14) << devLabel      << devName  << "\n"
        << std::setw(14) << "CPU:"        << GetCpuName() << "\n"
        << std::setw(14) << "Memory:"     << (config_.hostMemory ? "Host-visible (System RAM)" : "Device-local") << "\n"
        << std::setw(14) << "Resolution:" << kWindowWidth << "x" << kWindowHeight << "\n"
        << std::setw(14) << "Particles:"  << config_.particleCount
            << " (" << config_.difficultyLabel << ")\n"
        << std::setw(14) << "V-Sync:"     << (config_.vsync ? "ON" : "OFF") << "\n"
        << std::setw(14) << "Duration:"
            << std::setprecision(1) << duration << " s"
            << " (warmup: " << std::setprecision(1) << config_.warmupTimeSec << " s"
            << ", measured: " << benchMeasuredFrames_ << " frames)\n";

    if (benchSampleCount_ > 0) {
        const double avgCompute  = benchSumComputeMs_  / benchSampleCount_;
        const double avgRender   = benchSumRenderMs_   / benchSampleCount_;
        const double avgTotal    = benchSumTotalGpuMs_ / benchSampleCount_;

        std::cout << "\n--- " << timerLabel << " ---\n"
            << std::right
            << "              " << std::setw(10) << "Avg"
                                << std::setw(10) << "Min"
                                << std::setw(10) << "Max" << "\n"
            << "Compute:    " << std::setw(10) << std::setprecision(3) << avgCompute
                              << std::setw(10) << benchMinComputeMs_
                              << std::setw(10) << benchMaxComputeMs_ << "\n"
            << "Render:     " << std::setw(10) << avgRender
                              << std::setw(10) << benchMinRenderMs_
                              << std::setw(10) << benchMaxRenderMs_ << "\n"
            << totalLabel     << std::setw(10) << avgTotal
                              << std::setw(10) << benchMinTotalGpuMs_
                              << std::setw(10) << benchMaxTotalGpuMs_ << "\n";

        std::cout << "\n--- Throughput ---\n"
            << "Avg FPS:      " << static_cast<int>(avgFps) << "\n";

        const double avgFrameMs = (avgFps > 0.0) ? 1000.0 / avgFps : 0.0;
        const double devUtil = (avgFrameMs > 0.0) ? avgTotal / avgFrameMs : 0.0;

        std::cout << "\n--- Analysis ---\n"
            << "Avg frame time:  " << std::setprecision(3) << avgFrameMs << " ms\n"
            << "Avg " << (isSoftware ? "device" : "GPU") << " time: "
            << std::string(isSoftware ? 1 : 3, ' ')
            << avgTotal << " ms\n"
            << (isSoftware ? "Device" : "GPU") << " utilisation: "
            << std::setprecision(1) << (devUtil * 100.0) << "%\n";

        if (isSoftware) {
            std::cout << ">> Software renderer -- all work runs on CPU.\n";
        } else if (devUtil < 0.5) {
            std::cout << ">> CPU-bound: GPU is idle "
                      << static_cast<int>((1.0 - devUtil) * 100.0)
                      << "% of the time.\n"
                      << "   -> Try a higher difficulty for more accurate GPU benchmarking.\n";
        } else if (devUtil > 0.8) {
            std::cout << ">> GPU-bound: GPU is the bottleneck.\n";
        } else {
            std::cout << ">> Balanced: CPU and GPU workloads are roughly matched.\n";
        }
    } else {
        std::cout << "\n--- Throughput ---\n"
            << "Avg FPS:      " << static_cast<int>(avgFps)  << "\n"
            << "\n(No timestamp data available for analysis.)\n";
    }

    std::cout << "==========================================================\n"
        << std::endl;
}

BenchmarkResult AppBase::CollectResult() const {
    BenchmarkResult r;
    r.id          = GenerateResultId();
    r.timestamp   = GenerateTimestamp();
    r.graphicsApi = GetBackendName();
    r.deviceName  = GetDeviceName();
    r.cpuName     = GetCpuName();
    r.memory      = config_.hostMemory ? "Host-visible" : "Device-local";
    r.resWidth    = kWindowWidth;
    r.resHeight   = kWindowHeight;
    r.particleCount = config_.particleCount;
    r.difficulty  = config_.difficultyLabel;
    r.vsync       = config_.vsync;

    const std::string devName = GetDeviceName();
    r.isSoftware = (devName.find("Basic Render") != std::string::npos ||
                    devName.find("WARP") != std::string::npos ||
                    devName.find("Software") != std::string::npos);

    const double duration = benchEndTime_ - benchStartTime_;
    r.durationSec    = duration;
    r.warmupSec      = config_.warmupTimeSec;
    r.measuredFrames = benchMeasuredFrames_;
    r.timingSamples  = benchSampleCount_;

    if (benchSampleCount_ > 0) {
        r.avgComputeMs  = benchSumComputeMs_  / benchSampleCount_;
        r.minComputeMs  = benchMinComputeMs_;
        r.maxComputeMs  = benchMaxComputeMs_;
        r.avgRenderMs   = benchSumRenderMs_   / benchSampleCount_;
        r.minRenderMs   = benchMinRenderMs_;
        r.maxRenderMs   = benchMaxRenderMs_;
        r.avgTotalGpuMs = benchSumTotalGpuMs_ / benchSampleCount_;
        r.minTotalGpuMs = benchMinTotalGpuMs_;
        r.maxTotalGpuMs = benchMaxTotalGpuMs_;
    }

    r.avgFps = (duration > 0.0)
        ? static_cast<double>(benchMeasuredFrames_) / duration : 0.0;
    r.avgFrameTimeMs = (r.avgFps > 0.0) ? 1000.0 / r.avgFps : 0.0;
    r.gpuUtilisation = (r.avgFrameTimeMs > 0.0 && benchSampleCount_ > 0)
        ? r.avgTotalGpuMs / r.avgFrameTimeMs : 0.0;

    if (r.isSoftware)
        r.bottleneck = "Software";
    else if (benchSampleCount_ == 0)
        r.bottleneck = "Unknown";
    else if (r.gpuUtilisation < 0.5)
        r.bottleneck = "CPU-bound";
    else if (r.gpuUtilisation > 0.8)
        r.bottleneck = "GPU-bound";
    else
        r.bottleneck = "Balanced";

    return r;
}

std::vector<char> AppBase::ReadFileBytes(const std::string& filename) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + filename);
    }
    const auto fileSize = static_cast<std::size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(fileSize));
    return buffer;
}

void AppBase::CleanupWindow() {
    if (window_ != nullptr) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
    glfwTerminate();
}

}  // namespace gpu_bench
