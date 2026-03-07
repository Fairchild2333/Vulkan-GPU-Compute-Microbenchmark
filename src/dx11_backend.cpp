#ifdef HAVE_DX11

#include "dx11_backend.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <cstring>
#include <iostream>
#include <stdexcept>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace gpu_bench {

using Microsoft::WRL::ComPtr;

void DX11Backend::ThrowIfFailed(HRESULT hr, const char* msg) {
    if (FAILED(hr)) throw std::runtime_error(msg);
}

Microsoft::WRL::ComPtr<ID3DBlob> DX11Backend::CompileShader(const std::string& path,
                                                             const char* entry,
                                                             const char* target) {
    auto src = ReadFileBytes(path);
    ComPtr<ID3DBlob> shader, errors;
    HRESULT hr = D3DCompile(src.data(), src.size(), path.c_str(),
                            nullptr, nullptr, entry, target, 0, 0,
                            &shader, &errors);
    if (FAILED(hr)) {
        std::string msg = "Shader compilation failed: " + path;
        if (errors) msg += "\n" + std::string(static_cast<char*>(errors->GetBufferPointer()),
                                              errors->GetBufferSize());
        throw std::runtime_error(msg);
    }
    return shader;
}

// -----------------------------------------------------------------------
// Init
// -----------------------------------------------------------------------

void DX11Backend::InitBackend() {
    std::cout << "[DX11 Init] Creating device and swap chain..." << std::endl;
    CreateDeviceAndSwapChain();
    std::cout << "[DX11 Init] Creating render target..." << std::endl;
    CreateRenderTarget();
    std::cout << "[DX11 Init] Compiling shaders..." << std::endl;
    CreateShaders();
    std::cout << "[DX11 Init] Creating particle buffers..." << std::endl;
    CreateParticleBuffers();
    std::cout << "[DX11 Init] Creating compute params CB..." << std::endl;
    CreateComputeParamsCB();
    std::cout << "[DX11 Init] Creating timestamp queries..." << std::endl;
    CreateTimestampQueries();
    std::cout << "[DX11 Init] Initialisation complete." << std::endl;
}

void DX11Backend::CreateDeviceAndSwapChain() {
    HWND hwnd = glfwGetWin32Window(window_);

    ComPtr<IDXGIFactory2> factory;
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1 failed");

    // Enumerate adapters
    ComPtr<IDXGIAdapter1> bestAdapter;
    int bestIdx = -1;
    SIZE_T bestMem = 0;

    ComPtr<IDXGIAdapter1> adapter;
    std::cout << "Available GPUs:\n";
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        bool isSoftware = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

        char name[256]{};
        wcstombs(name, desc.Description, sizeof(name) - 1);
        std::cout << "  [" << i << "] " << name
                  << (isSoftware ? " (Software)" : " (Hardware)") << '\n';

        if (isSoftware) { adapter.Reset(); continue; }

        if (requestedGpuIndex_ >= 0 && static_cast<int>(i) == requestedGpuIndex_) {
            bestIdx = static_cast<int>(i);
            bestAdapter = adapter;
            break;
        }
        if (desc.DedicatedVideoMemory > bestMem) {
            bestMem = desc.DedicatedVideoMemory;
            bestIdx = static_cast<int>(i);
            bestAdapter = adapter;
        }
        adapter.Reset();
    }

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL actualFeatureLevel{};
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_DRIVER_TYPE driverType = bestAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    ThrowIfFailed(D3D11CreateDevice(
        bestAdapter.Get(), driverType, nullptr, flags,
        featureLevels, _countof(featureLevels), D3D11_SDK_VERSION,
        &device_, &actualFeatureLevel, &context_),
        "D3D11CreateDevice failed");

    // Query the actual adapter the device is using via DXGI
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> actualAdapter;
    std::string adapterType = "Unknown";
    if (SUCCEEDED(device_.As(&dxgiDevice)) &&
        SUCCEEDED(dxgiDevice->GetAdapter(&actualAdapter))) {
        DXGI_ADAPTER_DESC adDesc{};
        actualAdapter->GetDesc(&adDesc);
        char name[256]{};
        wcstombs(name, adDesc.Description, sizeof(name) - 1);
        deviceName_ = name;

        bool isSoftware = (adDesc.VendorId == 0x1414 && adDesc.DeviceId == 0x8c);
        adapterType = isSoftware ? "Software (WARP/Basic Render)" : "Hardware";
    } else if (bestAdapter) {
        DXGI_ADAPTER_DESC1 desc{};
        bestAdapter->GetDesc1(&desc);
        char name[256]{};
        wcstombs(name, desc.Description, sizeof(name) - 1);
        deviceName_ = name;
        adapterType = "Hardware";
    } else {
        deviceName_ = "Default Hardware Adapter";
    }

    auto flName = [](D3D_FEATURE_LEVEL fl) -> const char* {
        switch (fl) {
        case D3D_FEATURE_LEVEL_11_1: return "11_1";
        case D3D_FEATURE_LEVEL_11_0: return "11_0";
        case D3D_FEATURE_LEVEL_10_1: return "10_1";
        case D3D_FEATURE_LEVEL_10_0: return "10_0";
        default: return "unknown";
        }
    };

    std::cout << "Selected GPU: " << deviceName_ << '\n'
              << "  Driver type:   " << adapterType << '\n'
              << "  Feature Level: " << flName(actualFeatureLevel) << std::endl;

    // Create swap chain
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width            = kWindowWidth;
    sd.Height           = kWindowHeight;
    sd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount      = 2;
    sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ThrowIfFailed(factory->CreateSwapChainForHwnd(
        device_.Get(), hwnd, &sd, nullptr, nullptr, &swapChain_),
        "CreateSwapChainForHwnd failed");
}

void DX11Backend::CreateRenderTarget() {
    ComPtr<ID3D11Texture2D> backBuffer;
    ThrowIfFailed(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)),
                  "GetBuffer failed");
    ThrowIfFailed(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv_),
                  "CreateRenderTargetView failed");
}

void DX11Backend::CreateShaders() {
    auto csBlob = CompileShader(shaderDir_ + "compute.hlsl",    "CSMain", "cs_5_0");
    auto vsBlob = CompileShader(shaderDir_ + "particle_vs.hlsl","VSMain", "vs_5_0");
    auto psBlob = CompileShader(shaderDir_ + "particle_ps.hlsl","PSMain", "ps_5_0");

    ThrowIfFailed(device_->CreateComputeShader(
        csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, &computeShader_),
        "CreateComputeShader failed");

    ThrowIfFailed(device_->CreateVertexShader(
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vertexShader_),
        "CreateVertexShader failed");

    ThrowIfFailed(device_->CreatePixelShader(
        psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pixelShader_),
        "CreatePixelShader failed");

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "VELOCITY", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ThrowIfFailed(device_->CreateInputLayout(
        layout, _countof(layout),
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &inputLayout_),
        "CreateInputLayout failed");

    // Blend state (alpha blending)
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ThrowIfFailed(device_->CreateBlendState(&bd, &blendState_), "CreateBlendState failed");

    // Rasterizer state (no culling)
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ThrowIfFailed(device_->CreateRasterizerState(&rd, &rasterizerState_),
                  "CreateRasterizerState failed");
}

void DX11Backend::CreateParticleBuffers() {
    const UINT bufSize = sizeof(Particle) * config_.particleCount;

    D3D11_SUBRESOURCE_DATA initData{};
    initData.pSysMem = initialParticles_.data();

    // Structured buffer for compute (UAV)
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth           = bufSize;
    cbd.Usage               = D3D11_USAGE_DEFAULT;
    cbd.BindFlags           = D3D11_BIND_UNORDERED_ACCESS;
    cbd.MiscFlags           = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    cbd.StructureByteStride = sizeof(Particle);
    ThrowIfFailed(device_->CreateBuffer(&cbd, &initData, &computeBuffer_),
                  "CreateBuffer (compute) failed");

    // Create UAV for the structured buffer
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension      = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = config_.particleCount;
    ThrowIfFailed(device_->CreateUnorderedAccessView(
        computeBuffer_.Get(), &uavDesc, &computeUAV_),
        "CreateUnorderedAccessView failed");

    // Vertex buffer (copy destination each frame)
    D3D11_BUFFER_DESC vbd{};
    vbd.ByteWidth = bufSize;
    vbd.Usage     = D3D11_USAGE_DEFAULT;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    ThrowIfFailed(device_->CreateBuffer(&vbd, &initData, &vertexBuffer_),
                  "CreateBuffer (vertex) failed");

    std::cout << "Created particle buffers: " << config_.particleCount << " particles\n";
}

void DX11Backend::CreateComputeParamsCB() {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = 16;  // must be 16-byte aligned
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ThrowIfFailed(device_->CreateBuffer(&bd, nullptr, &computeParamsCB_),
                  "CreateBuffer (CB) failed");
}

void DX11Backend::CreateTimestampQueries() {
    D3D11_QUERY_DESC dj{};
    dj.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;

    D3D11_QUERY_DESC ts{};
    ts.Query = D3D11_QUERY_TIMESTAMP;

    for (UINT f = 0; f < kMaxFramesInFlight; ++f) {
        if (FAILED(device_->CreateQuery(&dj, &disjointQueries_[f]))) return;
        for (UINT q = 0; q < kTimestampsPerFrame; ++q)
            if (FAILED(device_->CreateQuery(&ts, &timestampQueries_[f][q]))) return;
    }

    timestampsSupported_ = true;
    std::cout << "[Profiling] DX11 timestamp queries enabled.\n";
}

void DX11Backend::CollectTimestampResults() {
    if (!timestampsSupported_) return;

    UINT readSlot = (currentFrame_ + 1) % kMaxFramesInFlight;

    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    if (context_->GetData(disjointQueries_[readSlot].Get(),
                          &disjoint, sizeof(disjoint), 0) != S_OK)
        return;
    if (disjoint.Disjoint) return;

    UINT64 ts[kTimestampsPerFrame]{};
    for (UINT i = 0; i < kTimestampsPerFrame; ++i) {
        if (context_->GetData(timestampQueries_[readSlot][i].Get(),
                              &ts[i], sizeof(UINT64), 0) != S_OK)
            return;
    }

    double toMs = 1000.0 / static_cast<double>(disjoint.Frequency);
    AccumulateTiming(
        static_cast<double>(ts[1] - ts[0]) * toMs,
        static_cast<double>(ts[3] - ts[2]) * toMs,
        static_cast<double>(ts[3] - ts[0]) * toMs);
}

// -----------------------------------------------------------------------
// Frame
// -----------------------------------------------------------------------

void DX11Backend::DrawFrame(float deltaTime) {
    CollectTimestampResults();

    UINT slot = currentFrame_;

    // Begin timestamp disjoint
    if (timestampsSupported_)
        context_->Begin(disjointQueries_[slot].Get());

    // --- Compute pass ---
    if (timestampsSupported_)
        context_->End(timestampQueries_[slot][0].Get());

    // Update compute params
    D3D11_MAPPED_SUBRESOURCE mapped{};
    context_->Map(computeParamsCB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    ComputeParams params{ deltaTime, 0.9f };
    std::memcpy(mapped.pData, &params, sizeof(params));
    context_->Unmap(computeParamsCB_.Get(), 0);

    context_->CSSetShader(computeShader_.Get(), nullptr, 0);
    ID3D11UnorderedAccessView* uavs[] = { computeUAV_.Get() };
    context_->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ID3D11Buffer* cbs[] = { computeParamsCB_.Get() };
    context_->CSSetConstantBuffers(0, 1, cbs);
    context_->Dispatch(config_.particleCount / kComputeWorkGroupSize, 1, 1);

    // Unbind UAV
    ID3D11UnorderedAccessView* nullUav[] = { nullptr };
    context_->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);

    if (timestampsSupported_)
        context_->End(timestampQueries_[slot][1].Get());

    // Copy compute buffer to vertex buffer
    context_->CopyResource(vertexBuffer_.Get(), computeBuffer_.Get());

    // --- Graphics pass ---
    if (timestampsSupported_)
        context_->End(timestampQueries_[slot][2].Get());

    const float clearColor[] = { 0.04f, 0.08f, 0.14f, 1.0f };
    context_->ClearRenderTargetView(rtv_.Get(), clearColor);
    context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    context_->OMSetBlendState(blendState_.Get(), nullptr, 0xFFFFFFFF);
    context_->RSSetState(rasterizerState_.Get());

    D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(kWindowWidth),
                       static_cast<float>(kWindowHeight), 0.0f, 1.0f };
    context_->RSSetViewports(1, &vp);

    context_->IASetInputLayout(inputLayout_.Get());
    UINT stride = sizeof(Particle);
    UINT offset = 0;
    context_->IASetVertexBuffers(0, 1, vertexBuffer_.GetAddressOf(), &stride, &offset);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);

    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context_->Draw(config_.particleCount, 0);

    if (timestampsSupported_)
        context_->End(timestampQueries_[slot][3].Get());

    // End timestamp disjoint
    if (timestampsSupported_)
        context_->End(disjointQueries_[slot].Get());

    swapChain_->Present(config_.vsync ? 1 : 0, 0);

    currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
}

void DX11Backend::WaitIdle() {
    if (context_) context_->Flush();
}

void DX11Backend::CleanupBackend() {
    WaitIdle();
}

}  // namespace gpu_bench

#endif  // HAVE_DX11
