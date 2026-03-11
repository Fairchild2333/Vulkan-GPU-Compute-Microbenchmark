#pragma once

#ifdef HAVE_OPENGL

#include "app_base.h"

#include <cstdint>

namespace gpu_bench {

class OpenGLBackend : public AppBase {
public:
    using AppBase::AppBase;

    std::string GetBackendName() const override { return "OpenGL"; }
    std::string GetDeviceName()  const override { return deviceName_; }
    bool NeedsOpenGLContext() const override { return true; }

protected:
    void InitBackend()              override;
    void DrawFrame(float deltaTime) override;
    void CleanupBackend()           override;
    void WaitIdle()                 override;

private:
    void CreateShaders();
    void CreateParticleBuffers();
    void CreateTimestampQueries();
    void CollectTimestampResults();

    std::uint32_t CompileShaderGL(const std::string& path, std::uint32_t type);
    std::uint32_t LinkProgramGL(std::uint32_t s1, std::uint32_t s2);

    std::string deviceName_;

    std::uint32_t computeProgram_ = 0;
    std::uint32_t renderProgram_  = 0;

    std::uint32_t ssbo_ = 0;
    std::uint32_t vao_  = 0;
    std::uint32_t ubo_  = 0;

    static constexpr int kTimestampsPerFrame = 4;
    static constexpr int kTimestampSlotCount = 4;
    std::uint32_t timestampQueries_[kTimestampSlotCount][kTimestampsPerFrame]{};
    bool   timestampsSupported_ = false;
    int    timestampFrameCount_ = 0;
    int    currentFrame_        = 0;
};

}  // namespace gpu_bench

#endif  // HAVE_OPENGL
