#include "app_base.h"

#include <GLFW/glfw3.h>

#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>

namespace gpu_bench {

AppBase::AppBase(std::int32_t gpuIndex, std::string shaderDir)
    : requestedGpuIndex_(gpuIndex), shaderDir_(std::move(shaderDir)) {}

AppBase::~AppBase() = default;

void AppBase::Run() {
    InitWindow();
    GenerateInitialParticles();
    InitBackend();
    glfwShowWindow(window_);
    MainLoop();
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
    initialParticles_.resize(kParticleCount);

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> posDist(-0.8f, 0.8f);
    std::uniform_real_distribution<float> velDist(-0.2f, 0.2f);

    for (std::uint32_t i = 0; i < kParticleCount; ++i) {
        initialParticles_[i] = {
            posDist(rng), posDist(rng), 0.0f, 1.0f,
            velDist(rng), velDist(rng), 0.0f, 0.0f,
        };
    }
}

void AppBase::MainLoop() {
    lastFrameTime_ = glfwGetTime();

    while (glfwWindowShouldClose(window_) == GLFW_FALSE) {
        glfwPollEvents();

        const double currentTime = glfwGetTime();
        const auto   deltaTime   = static_cast<float>(currentTime - lastFrameTime_);
        lastFrameTime_ = currentTime;

        DrawFrame(deltaTime);
        ReportTimingIfDue(static_cast<double>(deltaTime));
    }

    WaitIdle();
}

void AppBase::AccumulateTiming(double computeMs, double renderMs, double totalGpuMs) {
    accumComputeMs_  += computeMs;
    accumRenderMs_   += renderMs;
    accumTotalGpuMs_ += totalGpuMs;
    ++timingSampleCount_;
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
                  << " ms | FPS: " << static_cast<int>(fps) << std::endl;
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
