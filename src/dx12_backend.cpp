#ifdef HAVE_DX12

#include "dx12_backend.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace gpu_bench {

using Microsoft::WRL::ComPtr;

static std::string HrToHex(HRESULT hr) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setfill('0') << std::setw(8) << static_cast<unsigned long>(hr);
    return oss.str();
}

void DX12Backend::ThrowIfFailed(HRESULT hr, const char* msg) {
    if (FAILED(hr)) {
        std::string full = std::string(msg) + " (HRESULT " + HrToHex(hr) + ")";
        std::cout << full << std::endl;
        throw std::runtime_error(full);
    }
}

Microsoft::WRL::ComPtr<ID3DBlob> DX12Backend::CompileShader(const std::string& path,
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
        std::cout << msg << std::endl;
        throw std::runtime_error(msg);
    }
    return shader;
}

// -----------------------------------------------------------------------
// Init
// -----------------------------------------------------------------------

void DX12Backend::InitBackend() {
    std::cout << "[DX12 Init] Creating device..." << std::endl;
    CreateDevice();
    std::cout << "[DX12 Init] Creating command queue..." << std::endl;
    CreateCommandQueue();
    std::cout << "[DX12 Init] Creating swap chain..." << std::endl;
    CreateSwapChain();
    std::cout << "[DX12 Init] Creating descriptor heaps..." << std::endl;
    CreateDescriptorHeaps();
    std::cout << "[DX12 Init] Creating render targets..." << std::endl;
    CreateRenderTargets();
    std::cout << "[DX12 Init] Creating command allocators..." << std::endl;
    CreateCommandAllocatorsAndList();
    std::cout << "[DX12 Init] Creating fence..." << std::endl;
    CreateFence();
    std::cout << "[DX12 Init] Creating root signatures..." << std::endl;
    CreateRootSignatures();
    std::cout << "[DX12 Init] Creating pipeline states (compiling shaders)..." << std::endl;
    CreatePipelineStates();
    std::cout << "[DX12 Init] Creating particle buffer..." << std::endl;
    CreateParticleBuffer();
    std::cout << "[DX12 Init] Creating timestamp resources..." << std::endl;
    CreateTimestampResources();
    std::cout << "[DX12 Init] Initialisation complete." << std::endl;
}

void DX12Backend::CreateDevice() {
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory_)), "CreateDXGIFactory1 failed");

    // Sentinel -2: WARP software renderer requested from main.cpp.
    if (requestedGpuIndex_ == -2) {
        std::cout << "[DX12] Using Microsoft WARP software renderer (CPU)." << std::endl;
        ComPtr<IDXGIAdapter> warpAdapter;
        ThrowIfFailed(factory_->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)),
                      "EnumWarpAdapter failed");
        ThrowIfFailed(D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&device_)),
                      "D3D12CreateDevice (WARP) failed");
        deviceName_ = "Microsoft WARP (CPU Software Renderer)";
        driverVersion_ = "WARP";
        std::cout << "Selected GPU: " << deviceName_ << std::endl;
        return;
    }

    static const D3D_FEATURE_LEVEL kFeatureLevels[] = {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    static const char* kFeatureLevelNames[] = {
        "12_1", "12_0", "11_1", "11_0",
    };
    constexpr int kNumFeatureLevels = _countof(kFeatureLevels);

    // Enumerate adapters, deduplicating by LUID so that identical GPUs
    // with distinct LUIDs are listed separately.
    struct AdapterEntry {
        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 desc;
        UINT rawIndex;
        int featureLevelIdx = -1;
    };
    std::vector<AdapterEntry> uniqueAdapters;
    {
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory_->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);

            bool duplicate = false;
            for (const auto& existing : uniqueAdapters) {
                if (existing.desc.AdapterLuid.HighPart == desc.AdapterLuid.HighPart &&
                    existing.desc.AdapterLuid.LowPart  == desc.AdapterLuid.LowPart) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                uniqueAdapters.push_back({adapter, desc, i, -1});
            }
            adapter.Reset();
        }
    }

    std::cout << "Available GPUs:\n";
    for (std::uint32_t i = 0; i < uniqueAdapters.size(); ++i) {
        auto& entry = uniqueAdapters[i];
        bool isSoftware = (entry.desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        char name[256]{};
        size_t converted = 0;
        wcstombs_s(&converted, name, sizeof(name), entry.desc.Description, _TRUNCATE);
        std::cout << "  [" << i << "] " << name
                  << (isSoftware ? " (Software)" : " (Hardware)")
                  << "  VRAM: " << (entry.desc.DedicatedVideoMemory / (1024 * 1024)) << " MB"
                  << std::endl;

        for (int fl = 0; fl < kNumFeatureLevels; ++fl) {
            HRESULT hr = D3D12CreateDevice(entry.adapter.Get(), kFeatureLevels[fl],
                                           __uuidof(ID3D12Device), nullptr);
            if (SUCCEEDED(hr)) {
                entry.featureLevelIdx = fl;
                std::cout << "    -> D3D12 supported (Feature Level " << kFeatureLevelNames[fl] << ")\n";
                break;
            }
        }
        if (entry.featureLevelIdx < 0) {
            std::cout << "    -> Skipped: D3D12 not supported at any feature level\n";
        }
    }

    // Collect D3D12-capable hardware adapters.
    std::vector<std::uint32_t> d3d12Indices;
    for (std::uint32_t i = 0; i < uniqueAdapters.size(); ++i) {
        if (uniqueAdapters[i].featureLevelIdx >= 0)
            d3d12Indices.push_back(i);
    }

    // Select adapter: --gpu flag, single-choice auto, or interactive prompt.
    std::uint32_t chosen = 0;
    bool hasChoice = false;

    // Try LUID-based selection first (reliable across factory instances).
    if (config_.adapterLuidHigh != 0 || config_.adapterLuidLow != 0) {
        for (std::uint32_t i = 0; i < uniqueAdapters.size(); ++i) {
            const auto& d = uniqueAdapters[i].desc;
            if (d.AdapterLuid.HighPart == static_cast<LONG>(config_.adapterLuidHigh) &&
                d.AdapterLuid.LowPart  == static_cast<DWORD>(config_.adapterLuidLow) &&
                uniqueAdapters[i].featureLevelIdx >= 0) {
                chosen = i;
                hasChoice = true;
                break;
            }
        }
        if (!hasChoice)
            throw std::runtime_error("Requested GPU (LUID) not found or does not support D3D12");
    } else if (requestedGpuIndex_ >= 0) {
        auto idx = static_cast<std::uint32_t>(requestedGpuIndex_);
        if (idx >= uniqueAdapters.size() || uniqueAdapters[idx].featureLevelIdx < 0)
            throw std::runtime_error("Requested GPU index " +
                std::to_string(requestedGpuIndex_) + " does not support D3D12");
        chosen = idx;
        hasChoice = true;
    } else if (d3d12Indices.size() == 1) {
        chosen = d3d12Indices[0];
        hasChoice = true;
    } else if (d3d12Indices.size() > 1) {
        SIZE_T bestMem = 0;
        for (auto i : d3d12Indices) {
            if (uniqueAdapters[i].desc.DedicatedVideoMemory > bestMem) {
                bestMem = uniqueAdapters[i].desc.DedicatedVideoMemory;
                chosen = i;
            }
        }
        std::cout << "Multiple D3D12 GPUs detected. Default: [" << chosen << "]\n"
                  << "Enter GPU index (or 'b' to go back): " << std::flush;
        std::string line;
        if (std::getline(std::cin, line) && !line.empty()) {
            if (line == "b" || line == "B")
                throw gpu_bench::BackToMenuException();
            auto idx = static_cast<std::uint32_t>(std::stoi(line));
            if (idx >= uniqueAdapters.size() || uniqueAdapters[idx].featureLevelIdx < 0)
                throw std::runtime_error("GPU index " + line + " does not support D3D12");
            chosen = idx;
        }
        hasChoice = true;
    }

    if (!hasChoice || d3d12Indices.empty()) {
        throw std::runtime_error(
            "No hardware GPU with D3D12 support was found.\n"
            "  Your GPU may only support D3D11. Try: --backend dx11");
    }

    {
        auto& entry = uniqueAdapters[chosen];
        D3D_FEATURE_LEVEL selectedFL = kFeatureLevels[entry.featureLevelIdx];
        HRESULT hr = D3D12CreateDevice(entry.adapter.Get(), selectedFL,
                                       IID_PPV_ARGS(&device_));
        if (FAILED(hr))
            throw std::runtime_error("D3D12CreateDevice failed for adapter ["
                + std::to_string(chosen) + "] at FL "
                + kFeatureLevelNames[entry.featureLevelIdx]
                + " (HRESULT " + HrToHex(hr) + ")");

        char name[256]{};
        size_t converted = 0;
        wcstombs_s(&converted, name, sizeof(name), entry.desc.Description, _TRUNCATE);
        deviceName_ = name;
        deviceName_ += " (FL ";
        deviceName_ += kFeatureLevelNames[entry.featureLevelIdx];
        deviceName_ += ")";

        // Query driver version via DXGI CheckInterfaceSupport
        LARGE_INTEGER umdVer{};
        ComPtr<IDXGIAdapter> baseAdapter;
        if (SUCCEEDED(entry.adapter.As(&baseAdapter)) &&
            SUCCEEDED(baseAdapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umdVer))) {
            auto v = static_cast<std::uint64_t>(umdVer.QuadPart);
            driverVersion_ = std::to_string((v >> 48) & 0xffff) + "."
                           + std::to_string((v >> 32) & 0xffff) + "."
                           + std::to_string((v >> 16) & 0xffff) + "."
                           + std::to_string(v & 0xffff);
        }
    }

    std::cout << "Selected GPU: " << deviceName_;
    if (!driverVersion_.empty())
        std::cout << "  |  Driver: " << driverVersion_;
    std::cout << std::endl;
}

void DX12Backend::CreateCommandQueue() {
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&commandQueue_)),
                  "CreateCommandQueue failed");
}

void DX12Backend::CreateSwapChain() {
    HWND hwnd = glfwGetWin32Window(window_);

    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory_.As(&factory5))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
            tearingSupported_ = (allow == TRUE);
    }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.BufferCount      = kFrameCount;
    sd.Width            = kWindowWidth;
    sd.Height           = kWindowHeight;
    sd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.SampleDesc.Count = 1;
    if (tearingSupported_)
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    ComPtr<IDXGISwapChain1> sc1;
    ThrowIfFailed(factory_->CreateSwapChainForHwnd(
        commandQueue_.Get(), hwnd, &sd, nullptr, nullptr, &sc1),
        "CreateSwapChainForHwnd failed");
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    ThrowIfFailed(sc1.As(&swapChain_), "QueryInterface IDXGISwapChain3 failed");
    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
}

void DX12Backend::CreateDescriptorHeaps() {
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.NumDescriptors = kFrameCount;
    rtvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(device_->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap_)),
                  "CreateDescriptorHeap RTV failed");
    rtvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC uavDesc{};
    uavDesc.NumDescriptors = 1;
    uavDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    uavDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device_->CreateDescriptorHeap(&uavDesc, IID_PPV_ARGS(&cbvSrvUavHeap_)),
                  "CreateDescriptorHeap CBV/SRV/UAV failed");
}

void DX12Backend::CreateRenderTargets() {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i) {
        ThrowIfFailed(swapChain_->GetBuffer(i, IID_PPV_ARGS(&renderTargets_[i])),
                      "GetBuffer failed");
        device_->CreateRenderTargetView(renderTargets_[i].Get(), nullptr, handle);
        handle.ptr += rtvDescriptorSize_;
    }
}

void DX12Backend::CreateCommandAllocatorsAndList() {
    for (UINT i = 0; i < kFrameCount; ++i) {
        ThrowIfFailed(device_->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocators_[i])),
            "CreateCommandAllocator failed");
    }
    ThrowIfFailed(device_->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocators_[0].Get(), nullptr,
        IID_PPV_ARGS(&commandList_)),
        "CreateCommandList failed");
    commandList_->Close();
}

// -----------------------------------------------------------------------
// Root signatures & PSOs
// -----------------------------------------------------------------------

void DX12Backend::CreateRootSignatures() {
    // Compute root signature: UAV table (u0) + 32-bit constants (b0, 2 floats)
    {
        D3D12_DESCRIPTOR_RANGE uavRange{};
        uavRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors     = 1;
        uavRange.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges   = &uavRange;
        params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 0;
        params[1].Constants.Num32BitValues = 2;
        params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = 2;
        desc.pParameters   = params;

        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                  &sig, &err),
                      "Serialize compute root signature failed");
        ThrowIfFailed(device_->CreateRootSignature(0, sig->GetBufferPointer(),
                                                   sig->GetBufferSize(),
                                                   IID_PPV_ARGS(&computeRootSig_)),
                      "CreateRootSignature (compute) failed");
    }

    // Graphics root signature: empty, allows input assembler
    {
        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                  &sig, &err),
                      "Serialize graphics root signature failed");
        ThrowIfFailed(device_->CreateRootSignature(0, sig->GetBufferPointer(),
                                                   sig->GetBufferSize(),
                                                   IID_PPV_ARGS(&graphicsRootSig_)),
                      "CreateRootSignature (graphics) failed");
    }
}

void DX12Backend::CreatePipelineStates() {
    auto csBlob = CompileShader(shaderDir_ + "compute.hlsl",    "CSMain", "cs_5_1");
    auto vsBlob = CompileShader(shaderDir_ + "particle_vs.hlsl","VSMain", "vs_5_1");
    auto psBlob = CompileShader(shaderDir_ + "particle_ps.hlsl","PSMain", "ps_5_1");

    // Compute PSO
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = computeRootSig_.Get();
        d.CS = { csBlob->GetBufferPointer(), csBlob->GetBufferSize() };
        ThrowIfFailed(device_->CreateComputePipelineState(&d, IID_PPV_ARGS(&computePSO_)),
                      "CreateComputePipelineState failed");
    }

    // Graphics PSO
    {
        D3D12_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "VELOCITY", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.InputLayout    = { layout, _countof(layout) };
        d.pRootSignature = graphicsRootSig_.Get();
        d.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
        d.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };

        d.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable       = TRUE;
        d.RasterizerState.FrontCounterClockwise = FALSE;

        d.BlendState.RenderTarget[0].BlendEnable           = TRUE;
        d.BlendState.RenderTarget[0].SrcBlend               = D3D12_BLEND_SRC_ALPHA;
        d.BlendState.RenderTarget[0].DestBlend              = D3D12_BLEND_INV_SRC_ALPHA;
        d.BlendState.RenderTarget[0].BlendOp                = D3D12_BLEND_OP_ADD;
        d.BlendState.RenderTarget[0].SrcBlendAlpha          = D3D12_BLEND_ONE;
        d.BlendState.RenderTarget[0].DestBlendAlpha         = D3D12_BLEND_ZERO;
        d.BlendState.RenderTarget[0].BlendOpAlpha           = D3D12_BLEND_OP_ADD;
        d.BlendState.RenderTarget[0].RenderTargetWriteMask  = D3D12_COLOR_WRITE_ENABLE_ALL;

        d.SampleMask            = UINT_MAX;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        d.NumRenderTargets      = 1;
        d.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count      = 1;

        ThrowIfFailed(device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&graphicsPSO_)),
                      "CreateGraphicsPipelineState failed");
    }
}

// -----------------------------------------------------------------------
// Resources
// -----------------------------------------------------------------------

void DX12Backend::CreateParticleBuffer() {
    const UINT bufferSize = sizeof(Particle) * config_.particleCount;

    // Default heap buffer (GPU-only, UAV-capable)
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width              = bufferSize;
    rd.Height             = 1;
    rd.DepthOrArraySize   = 1;
    rd.MipLevels          = 1;
    rd.SampleDesc.Count   = 1;
    rd.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags              = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ThrowIfFailed(device_->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&particleBuffer_)),
        "CreateCommittedResource (particle) failed");

    // Upload heap for initial data
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    rd.Flags = D3D12_RESOURCE_FLAG_NONE;

    ThrowIfFailed(device_->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&particleUpload_)),
        "CreateCommittedResource (upload) failed");

    // Copy initial particle data
    void* mapped = nullptr;
    particleUpload_->Map(0, nullptr, &mapped);
    std::memcpy(mapped, initialParticles_.data(), bufferSize);
    particleUpload_->Unmap(0, nullptr);

    // Execute copy on the GPU
    commandAllocators_[0]->Reset();
    commandList_->Reset(commandAllocators_[0].Get(), nullptr);

    commandList_->CopyBufferRegion(particleBuffer_.Get(), 0,
                                   particleUpload_.Get(), 0, bufferSize);

    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = particleBuffer_.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList_->ResourceBarrier(1, &b);

    commandList_->Close();
    ID3D12CommandList* lists[] = { commandList_.Get() };
    commandQueue_->ExecuteCommandLists(1, lists);
    WaitForGpu();

    // Create UAV descriptor for compute
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements         = config_.particleCount;
    uavDesc.Buffer.StructureByteStride = sizeof(Particle);
    device_->CreateUnorderedAccessView(particleBuffer_.Get(), nullptr, &uavDesc,
                                       cbvSrvUavHeap_->GetCPUDescriptorHandleForHeapStart());

    // Vertex buffer view
    vbView_.BufferLocation = particleBuffer_->GetGPUVirtualAddress();
    vbView_.SizeInBytes    = bufferSize;
    vbView_.StrideInBytes  = sizeof(Particle);

    std::cout << "Created particle buffer: " << config_.particleCount << " particles\n";
}

void DX12Backend::CreateTimestampResources() {
    if (FAILED(commandQueue_->GetTimestampFrequency(&gpuFrequency_)) || gpuFrequency_ == 0) {
        std::cout << "[Profiling] Timestamps not supported -- disabled.\n";
        return;
    }

    D3D12_QUERY_HEAP_DESC qhd{};
    qhd.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qhd.Count = kTimestampsPerFrame * kFrameCount;
    if (FAILED(device_->CreateQueryHeap(&qhd, IID_PPV_ARGS(&timestampHeap_)))) {
        std::cout << "[Profiling] Failed to create query heap -- disabled.\n";
        return;
    }

    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = sizeof(UINT64) * kTimestampsPerFrame * kFrameCount;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device_->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE,
            &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&timestampReadback_)))) {
        std::cout << "[Profiling] Failed to create readback buffer -- disabled.\n";
        timestampHeap_.Reset();
        return;
    }

    timestampsSupported_ = true;
    std::cout << "[Profiling] DX12 timestamp queries enabled (freq = "
              << gpuFrequency_ << " Hz)\n";
}

void DX12Backend::CreateFence() {
    ThrowIfFailed(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)),
                  "CreateFence failed");
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_) throw std::runtime_error("CreateEvent failed");
}

void DX12Backend::WaitForGpu() {
    commandQueue_->Signal(fence_.Get(), nextFenceValue_);
    fence_->SetEventOnCompletion(nextFenceValue_, fenceEvent_);
    WaitForSingleObjectEx(fenceEvent_, INFINITE, FALSE);
    ++nextFenceValue_;
}

void DX12Backend::WaitIdle() {
    WaitForGpu();
}

void DX12Backend::CollectTimestampResults() {
    if (!timestampsSupported_) return;

    const UINT base = frameIndex_ * kTimestampsPerFrame;
    const UINT64 byteOffset = base * sizeof(UINT64);

    D3D12_RANGE readRange{ byteOffset, byteOffset + kTimestampsPerFrame * sizeof(UINT64) };
    void* mapped = nullptr;
    if (FAILED(timestampReadback_->Map(0, &readRange, &mapped))) return;

    auto* ts = reinterpret_cast<UINT64*>(static_cast<char*>(mapped) + byteOffset);

    if (ts[0] != 0 && ts[3] != 0) {
        double toMs = 1000.0 / static_cast<double>(gpuFrequency_);
        AccumulateTiming(
            static_cast<double>(ts[1] - ts[0]) * toMs,
            static_cast<double>(ts[3] - ts[2]) * toMs,
            static_cast<double>(ts[3] - ts[0]) * toMs);
    }

    D3D12_RANGE writeRange{ 0, 0 };
    timestampReadback_->Unmap(0, &writeRange);
}

// -----------------------------------------------------------------------
// Frame
// -----------------------------------------------------------------------

void DX12Backend::DrawFrame(float deltaTime) {
    // Wait for previous use of this frame slot
    if (fence_->GetCompletedValue() < frameFenceValues_[frameIndex_]) {
        fence_->SetEventOnCompletion(frameFenceValues_[frameIndex_], fenceEvent_);
        WaitForSingleObjectEx(fenceEvent_, INFINITE, FALSE);
    }

    CollectTimestampResults();

    auto& alloc = commandAllocators_[frameIndex_];
    alloc->Reset();
    commandList_->Reset(alloc.Get(), nullptr);

    const UINT tsBase = frameIndex_ * kTimestampsPerFrame;

    // --- Transition particle buffer VBV -> UAV ---
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource   = particleBuffer_.Get();
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList_->ResourceBarrier(1, &barriers[0]);

    // --- Compute pass ---
    if (timestampsSupported_)
        commandList_->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, tsBase + 0);

    commandList_->SetComputeRootSignature(computeRootSig_.Get());
    commandList_->SetPipelineState(computePSO_.Get());

    ID3D12DescriptorHeap* heaps[] = { cbvSrvUavHeap_.Get() };
    commandList_->SetDescriptorHeaps(1, heaps);
    commandList_->SetComputeRootDescriptorTable(0,
        cbvSrvUavHeap_->GetGPUDescriptorHandleForHeapStart());

    ComputeParams params{ deltaTime, 0.9f };
    commandList_->SetComputeRoot32BitConstants(1, 2, &params, 0);
    commandList_->Dispatch(config_.particleCount / kComputeWorkGroupSize, 1, 1);

    if (timestampsSupported_)
        commandList_->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, tsBase + 1);

    // UAV barrier then transition to VBV
    D3D12_RESOURCE_BARRIER uavBarrier{};
    uavBarrier.Type           = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource  = particleBuffer_.Get();
    commandList_->ResourceBarrier(1, &uavBarrier);

    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    commandList_->ResourceBarrier(1, &barriers[0]);

    // --- Transition render target PRESENT -> RT ---
    barriers[0].Transition.pResource   = renderTargets_[frameIndex_].Get();
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList_->ResourceBarrier(1, &barriers[0]);

    // --- Graphics pass ---
    if (timestampsSupported_)
        commandList_->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, tsBase + 2);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(frameIndex_) * rtvDescriptorSize_;

    const float clearColor[] = { 0.04f, 0.08f, 0.14f, 1.0f };
    commandList_->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    commandList_->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    D3D12_VIEWPORT vp{ 0, 0, static_cast<float>(kWindowWidth),
                       static_cast<float>(kWindowHeight), 0.0f, 1.0f };
    D3D12_RECT sc{ 0, 0, static_cast<LONG>(kWindowWidth),
                   static_cast<LONG>(kWindowHeight) };
    commandList_->RSSetViewports(1, &vp);
    commandList_->RSSetScissorRects(1, &sc);

    commandList_->SetGraphicsRootSignature(graphicsRootSig_.Get());
    commandList_->SetPipelineState(graphicsPSO_.Get());
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    commandList_->IASetVertexBuffers(0, 1, &vbView_);
    commandList_->DrawInstanced(config_.particleCount, 1, 0, 0);

    if (timestampsSupported_)
        commandList_->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, tsBase + 3);

    // Resolve timestamps
    if (timestampsSupported_)
        commandList_->ResolveQueryData(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                       tsBase, kTimestampsPerFrame,
                                       timestampReadback_.Get(),
                                       tsBase * sizeof(UINT64));

    // --- Transition render target RT -> PRESENT ---
    barriers[0].Transition.pResource   = renderTargets_[frameIndex_].Get();
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    commandList_->ResourceBarrier(1, &barriers[0]);

    ThrowIfFailed(commandList_->Close(), "CommandList Close failed");

    ID3D12CommandList* lists[] = { commandList_.Get() };
    commandQueue_->ExecuteCommandLists(1, lists);

    UINT presentFlags = 0;
    if (!config_.vsync && tearingSupported_)
        presentFlags = DXGI_PRESENT_ALLOW_TEARING;
    swapChain_->Present(config_.vsync ? 1 : 0, presentFlags);

    // Signal fence for this frame
    commandQueue_->Signal(fence_.Get(), nextFenceValue_);
    frameFenceValues_[frameIndex_] = nextFenceValue_;
    ++nextFenceValue_;

    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
}

// -----------------------------------------------------------------------
// Cleanup
// -----------------------------------------------------------------------

void DX12Backend::CleanupBackend() {
    WaitForGpu();

    if (fenceEvent_) {
        CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }
}

}  // namespace gpu_bench

#endif  // HAVE_DX12
