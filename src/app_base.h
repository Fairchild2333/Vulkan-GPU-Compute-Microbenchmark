#pragma once

#include "gpu_common.h"

#include <fstream>
#include <string>
#include <vector>

struct GLFWwindow;

namespace gpu_bench {

class AppBase {
public:
    AppBase(std::int32_t gpuIndex, std::string shaderDir);
    virtual ~AppBase();

    AppBase(const AppBase&) = delete;
    AppBase& operator=(const AppBase&) = delete;

    void Run();

    virtual std::string GetBackendName() const = 0;
    virtual std::string GetDeviceName() const  = 0;

protected:
    virtual void InitBackend()            = 0;
    virtual void DrawFrame(float deltaTime) = 0;
    virtual void CleanupBackend()         = 0;
    virtual void WaitIdle()               = 0;

    void AccumulateTiming(double computeMs, double renderMs, double totalGpuMs);

    static std::vector<char> ReadFileBytes(const std::string& filename);

    std::int32_t requestedGpuIndex_;
    std::string  shaderDir_;
    GLFWwindow*  window_ = nullptr;
    std::vector<Particle> initialParticles_;

private:
    void InitWindow();
    void GenerateInitialParticles();
    void MainLoop();
    void ReportTimingIfDue(double deltaTime);
    void CleanupWindow();

    double        lastFrameTime_      = 0.0;
    double        accumComputeMs_     = 0.0;
    double        accumRenderMs_      = 0.0;
    double        accumTotalGpuMs_    = 0.0;
    std::uint32_t timingSampleCount_  = 0;
    double        timingReportTimer_  = 0.0;
    std::uint32_t frameCount_         = 0;
};

}  // namespace gpu_bench
