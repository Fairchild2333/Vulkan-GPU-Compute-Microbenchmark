#pragma once

#ifdef HAVE_DX12

#include "app_base.h"

#include <d3d12.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace gpu_bench {

class DX12Backend : public AppBase {
public:
    using AppBase::AppBase;

    std::string GetBackendName()    const override { return "DX12"; }
    std::string GetDeviceName()     const override { return deviceName_; }
    std::string GetDriverVersion()  const override { return driverVersion_; }

protected:
    void InitBackend()              override;
    void DrawFrame(float deltaTime) override;
    void CleanupBackend()           override;
    void WaitIdle()                 override;

private:
    template<typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    static void ThrowIfFailed(HRESULT hr, const char* msg);
    ComPtr<ID3DBlob> CompileShader(const std::string& path,
                                   const char* entry, const char* target);

    void CreateDevice();
    void CreateCommandQueue();
    void CreateSwapChain();
    void CreateDescriptorHeaps();
    void CreateRenderTargets();
    void CreateCommandAllocatorsAndList();
    void CreateRootSignatures();
    void CreatePipelineStates();
    void CreateParticleBuffer();
    void CreateTimestampResources();
    void CreateFence();
    void WaitForGpu();
    void CollectTimestampResults();

    std::string deviceName_;
    std::string driverVersion_;

    static constexpr UINT kTimestampsPerFrame = 4;
    UINT frameCount_ = kMaxFramesInFlight;

    ComPtr<IDXGIFactory4>           factory_;
    ComPtr<ID3D12Device>            device_;
    ComPtr<ID3D12CommandQueue>      commandQueue_;
    ComPtr<IDXGISwapChain3>         swapChain_;

    ComPtr<ID3D12DescriptorHeap>    rtvHeap_;
    UINT                            rtvDescriptorSize_ = 0;
    std::vector<ComPtr<ID3D12Resource>> renderTargets_;

    ComPtr<ID3D12DescriptorHeap>    cbvSrvUavHeap_;

    std::vector<ComPtr<ID3D12CommandAllocator>> commandAllocators_;
    ComPtr<ID3D12GraphicsCommandList> commandList_;

    ComPtr<ID3D12RootSignature>     computeRootSig_;
    ComPtr<ID3D12PipelineState>     computePSO_;
    ComPtr<ID3D12RootSignature>     graphicsRootSig_;
    ComPtr<ID3D12PipelineState>     graphicsPSO_;

    ComPtr<ID3D12Resource>          particleBuffer_;
    ComPtr<ID3D12Resource>          particleUpload_;
    D3D12_VERTEX_BUFFER_VIEW        vbView_{};

    ComPtr<ID3D12QueryHeap>         timestampHeap_;
    ComPtr<ID3D12Resource>          timestampReadback_;
    UINT64                          gpuFrequency_ = 0;
    bool                            timestampsSupported_ = false;

    ComPtr<ID3D12Fence>             fence_;
    HANDLE                          fenceEvent_ = nullptr;
    UINT64                          nextFenceValue_ = 1;
    std::vector<UINT64>             frameFenceValues_;
    UINT                            frameIndex_ = 0;
    bool                            tearingSupported_ = false;
};

}  // namespace gpu_bench

#endif  // HAVE_DX12
