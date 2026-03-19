#pragma once

#include "gpu_common.h"
#include "benchmark_results.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

struct GLFWwindow;

namespace gpu_bench {

class AppBase {
public:
    AppBase(std::int32_t gpuIndex, std::string shaderDir,
            BenchmarkConfig config = {});
    virtual ~AppBase();

    AppBase(const AppBase&) = delete;
    AppBase& operator=(const AppBase&) = delete;

    void Run();

    virtual std::string GetBackendName() const = 0;
    virtual std::string GetDeviceName() const  = 0;
    virtual std::string GetDriverVersion() const { return ""; }
    virtual bool NeedsOpenGLContext() const { return false; }
    const std::string& GetLastCapturePath() const { return lastCapturePath_; }

protected:
    virtual void InitBackend()            = 0;
    virtual void DrawFrame(float deltaTime) = 0;
    virtual void CleanupBackend()         = 0;
    virtual void WaitIdle()               = 0;

    void AccumulateTiming(double computeMs, double renderMs, double totalGpuMs);

    static std::vector<char> ReadFileBytes(const std::string& filename);
    static std::string GetCpuName();
    static std::string GetOsVersion();

    std::int32_t    requestedGpuIndex_;
    std::string     shaderDir_;
    BenchmarkConfig config_;
    GLFWwindow*     window_ = nullptr;
    std::vector<Particle> initialParticles_;

    bool IsRenderDocAttached() const { return rdocApi_ != nullptr; }
    void TriggerRenderDocCapture();
    std::uint32_t GetRenderDocCaptureCount() const;

private:
    void InitRenderDoc();
    void InitWindow();
    void GenerateInitialParticles();
    void MainLoop();
    void ReportTimingIfDue(double deltaTime);
    void PrintSummary() const;
    BenchmarkResult CollectResult() const;
    void CleanupWindow();

    void* rdocApi_ = nullptr;
    bool     rdocCaptureRequested_ = false;
    uint32_t rdocCaptureCount_     = 0;
    std::string lastCapturePath_;

    double        lastFrameTime_      = 0.0;
    double        runStartTime_       = 0.0;
    double        accumComputeMs_     = 0.0;
    double        accumRenderMs_      = 0.0;
    double        accumTotalGpuMs_    = 0.0;
    std::uint32_t timingSampleCount_  = 0;
    double        timingReportTimer_  = 0.0;
    std::uint32_t frameCount_         = 0;
    bool          warmupDone_         = false;

    std::uint32_t totalFrameCount_    = 0;

    double benchMinComputeMs_  = std::numeric_limits<double>::max();
    double benchMaxComputeMs_  = 0.0;
    double benchMinRenderMs_   = std::numeric_limits<double>::max();
    double benchMaxRenderMs_   = 0.0;
    double benchMinTotalGpuMs_ = std::numeric_limits<double>::max();
    double benchMaxTotalGpuMs_ = 0.0;
    double benchSumComputeMs_  = 0.0;
    double benchSumRenderMs_   = 0.0;
    double benchSumTotalGpuMs_ = 0.0;
    std::uint32_t benchSampleCount_ = 0;

    double benchStartTime_     = 0.0;
    double benchEndTime_       = 0.0;
    std::uint32_t benchMeasuredFrames_ = 0;
    double benchMinFrameTime_  = std::numeric_limits<double>::max();
};

}  // namespace gpu_bench
