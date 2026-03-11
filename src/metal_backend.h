#pragma once

#ifdef HAVE_METAL

#include "app_base.h"

#include <memory>

namespace gpu_bench {

class MetalBackend : public AppBase {
public:
    MetalBackend(std::int32_t gpuIndex, std::string shaderDir,
                 BenchmarkConfig config = {});
    ~MetalBackend() override;

    MetalBackend(const MetalBackend&) = delete;
    MetalBackend& operator=(const MetalBackend&) = delete;

    std::string GetBackendName()    const override { return "Metal"; }
    std::string GetDeviceName()     const override;
    std::string GetDriverVersion()  const override;

protected:
    void InitBackend()              override;
    void DrawFrame(float deltaTime) override;
    void CleanupBackend()           override;
    void WaitIdle()                 override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gpu_bench

#endif  // HAVE_METAL
