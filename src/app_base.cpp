#include "app_base.h"

#include <GLFW/glfw3.h>

#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>

namespace gpu_bench {

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
    if (config_.benchmarkMode)
        PrintBenchmarkSummary();
    CleanupBackend();
    CleanupWindow();
}

void AppBase::InitWindow() {
    if (glfwInit() != GLFW_TRUE) {
        throw std::runtime_error("glfwInit failed");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
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

    const std::uint32_t totalBenchFrames =
        config_.benchFrames + config_.warmupFrames;

    while (glfwWindowShouldClose(window_) == GLFW_FALSE) {
        glfwPollEvents();

        const double currentTime = glfwGetTime();
        const auto   deltaTime   = static_cast<float>(currentTime - lastFrameTime_);
        lastFrameTime_ = currentTime;

        DrawFrame(deltaTime);
        ++totalFrameCount_;

        if (config_.benchmarkMode) {
            if (totalFrameCount_ == config_.warmupFrames) {
                benchStartTime_ = glfwGetTime();
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
        }

        ReportTimingIfDue(static_cast<double>(deltaTime));
    }

    WaitIdle();
}

void AppBase::AccumulateTiming(double computeMs, double renderMs,
                               double totalGpuMs) {
    accumComputeMs_  += computeMs;
    accumRenderMs_   += renderMs;
    accumTotalGpuMs_ += totalGpuMs;
    ++timingSampleCount_;

    if (config_.benchmarkMode && totalFrameCount_ > config_.warmupFrames) {
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

        std::cout << "[GPU Timing] Compute: " << std::fixed << std::setprecision(3)
                  << avgCompute << " ms | Render: " << avgRender
                  << " ms | Total GPU: " << avgTotal
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

void AppBase::PrintBenchmarkSummary() const {
    const double duration = benchEndTime_ - benchStartTime_;
    const double avgFps   = (duration > 0.0)
        ? static_cast<double>(benchMeasuredFrames_) / duration
        : 0.0;
    const double peakFps  = (benchMinFrameTime_ > 0.0)
        ? 1.0 / benchMinFrameTime_
        : 0.0;

    std::cout << "\n"
        "==========================================================\n"
        "                   GPU Benchmark Report\n"
        "==========================================================\n";

    std::cout << std::left
        << std::setw(14) << "Backend:"    << GetBackendName() << "\n"
        << std::setw(14) << "GPU:"        << GetDeviceName()  << "\n"
        << std::setw(14) << "Resolution:" << kWindowWidth << "x" << kWindowHeight << "\n"
        << std::setw(14) << "Particles:"  << config_.particleCount << "\n"
        << std::setw(14) << "V-Sync:"     << (config_.vsync ? "ON" : "OFF") << "\n"
        << std::setw(14) << "Frames:"     << config_.benchFrames
            << " (warmup: " << config_.warmupFrames
            << ", measured: " << benchMeasuredFrames_ << ")\n"
        << std::setw(14) << "Duration:"
            << std::fixed << std::setprecision(3) << duration << " s\n";

    if (benchSampleCount_ > 0) {
        const double avgCompute  = benchSumComputeMs_  / benchSampleCount_;
        const double avgRender   = benchSumRenderMs_   / benchSampleCount_;
        const double avgTotalGpu = benchSumTotalGpuMs_ / benchSampleCount_;

        std::cout << "\n--- GPU Timing (ms) ---\n"
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
            << "Total GPU:  " << std::setw(10) << avgTotalGpu
                              << std::setw(10) << benchMinTotalGpuMs_
                              << std::setw(10) << benchMaxTotalGpuMs_ << "\n";
    }

    std::cout << "\n--- Throughput ---\n"
        << "Avg FPS:    " << static_cast<int>(avgFps)  << "\n"
        << "Peak FPS:   " << static_cast<int>(peakFps) << "\n"
        << "==========================================================\n"
        << std::endl;
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
