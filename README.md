# GPU Compute & Rendering Pipeline — Multi-Backend

A real-time particle simulation using GPU compute shaders with three
interchangeable rendering backends: **Vulkan**, **DirectX 12**, and
**DirectX 11**. Each backend implements the same particle physics (Euler
integration in a compute shader) and point-cloud rendering, with GPU
timestamp profiling.

## Supported Backends

| Backend | API Level | Platforms | Notes |
|---------|-----------|-----------|-------|
| Vulkan  | 1.2       | Windows, Linux | Requires Vulkan SDK + ICD driver |
| DX12    | Feature Level 11_0 | Windows 10+ | Best compatibility on Windows on ARM |
| DX11    | Feature Level 11_0 | Windows 7+  | Simplest, broadest Windows support |

## Project Structure

```text
.
├── CMakeLists.txt
├── README.md
├── src/
│   ├── main.cpp              # Entry point — backend selection via --backend flag
│   ├── app_base.h            # Shared base class (window, particles, timing)
│   ├── app_base.cpp
│   ├── vulkan_backend.h/cpp  # Vulkan backend
│   ├── dx12_backend.h/cpp    # DirectX 12 backend
│   └── dx11_backend.h/cpp    # DirectX 11 backend
├── shaders/
│   ├── compute.comp          # Vulkan GLSL compute shader
│   ├── particle.vert         # Vulkan GLSL vertex shader
│   ├── particle.frag         # Vulkan GLSL fragment shader
│   ├── compute.hlsl          # DX12/DX11 compute shader
│   ├── particle_vs.hlsl      # DX12/DX11 vertex shader
│   └── particle_ps.hlsl      # DX12/DX11 pixel shader
└── build/
```

## Prerequisites

| Dependency | Install |
|---|---|
| **CMake 3.20+** | https://cmake.org/download/ |
| **C++17 compiler** | MSVC (Visual Studio 2019+) or Clang |
| **GLFW** | `vcpkg install glfw3` (with appropriate triplet) |
| **Vulkan SDK** (optional) | [LunarG](https://vulkan.lunarg.com/sdk/home) |
| **Windows SDK** (for DX) | Included with Visual Studio |

### Windows on ARM

```powershell
vcpkg install glfw3:arm64-windows
```

The DX12 and DX11 backends only need the Windows SDK (bundled with Visual
Studio). No additional driver installation is needed — D3D12/D3D11 work
through the built-in Windows graphics stack.

## Build

### Verify Environment Variables

Before building, ensure that `cmake`, `cl` (MSVC compiler), and `glslc`
(Vulkan shader compiler — optional) are available in your system or user
**PATH** environment variable. You can quickly verify in PowerShell:

```powershell
cmake --version   # Should be 3.20+
cl                # Should print MSVC version information
glslc --version   # Optional — only required for the Vulkan backend
```

If any of these commands are not recognised, the corresponding tool is not
in your PATH. Typical default paths are listed below (using Visual Studio
2026 Community on ARM64 as an example — adjust to match your installation):

| Tool | Default Path |
|------|-------------|
| cmake | `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin` |
| cl | `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\<version>\bin\Hostarm64\arm64` |
| glslc | `C:\VulkanSDK\<version>\Bin` |

Add the relevant directories to **User environment variables → Path**, then
reopen your terminal for the changes to take effect.

### Build Steps

```powershell
# Configure (vcpkg toolchain, all backends auto-detected)
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake

# For ARM64 native builds:
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=arm64-windows -A ARM64

# Build
cmake --build build --config Release
```

### Backend Toggles

```powershell
cmake -S . -B build -DENABLE_VULKAN=OFF -DENABLE_DX12=ON -DENABLE_DX11=ON ...
```

## Run

```powershell
.\build\Release\gpu_benchmark.exe                    # auto-select backend
.\build\Release\gpu_benchmark.exe --backend vulkan    # force Vulkan
.\build\Release\gpu_benchmark.exe --backend dx12      # force DX12
.\build\Release\gpu_benchmark.exe --backend dx11      # force DX11
.\build\Release\gpu_benchmark.exe --backend dx12 --gpu 1   # DX12 + specific GPU
.\build\Release\gpu_benchmark.exe --help
```

The auto-selection prefers DX12 on Windows (best WoA compatibility),
falls back to DX11, then Vulkan.

## GPU Profiling

All three backends write 4 timestamps per frame:

| Index | Where | Stage |
|-------|-------|-------|
| 0 | Before compute dispatch | Pipeline start |
| 1 | After compute dispatch | Compute complete |
| 2 | Before render pass | Pipeline start |
| 3 | After render pass | Render complete |

Every second the application prints averaged timing to stdout:

```
[GPU Timing] Compute: 0.031 ms | Render: 0.054 ms | Total GPU: 0.112 ms | FPS: 3200
```

## Architecture

```
         ┌──────────┐
         │ AppBase  │  window, particles, timing
         └────┬─────┘
      ┌───────┼───────┐
      │       │       │
┌─────┴──┐ ┌──┴───┐ ┌─┴──────┐
│ Vulkan │ │ DX12 │ │  DX11  │
│Backend │ │Backend│ │Backend │
└────────┘ └──────┘ └────────┘
```

Each backend overrides:
- `InitBackend()` — create device, pipelines, buffers
- `DrawFrame(dt)` — dispatch compute, render, present
- `CleanupBackend()` — release GPU resources
- `GetBackendName()` / `GetDeviceName()` — for display
