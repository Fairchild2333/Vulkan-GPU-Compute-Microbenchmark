# GPU Compute & Rendering Pipeline — Multi-Backend

A real-time particle simulation using GPU compute shaders with four
interchangeable rendering backends: **Vulkan**, **DirectX 12**,
**DirectX 11**, and **Metal**. Each backend implements the same particle
physics (Euler integration in a compute shader) and point-cloud rendering,
with GPU timestamp profiling.

## Supported Backends

| Backend | API Level | Platforms | Notes |
|---------|-----------|-----------|-------|
| Vulkan  | 1.2       | Windows, Linux, **HarmonyOS** | Requires Vulkan SDK + ICD driver |
| DX12    | Feature Level 11_0 | Windows 10+ | Best compatibility on Windows on ARM |
| DX11    | Feature Level 11_0 | Windows 7+  | Simplest, broadest Windows support |
| Metal   | Metal 2+  | **macOS** (Apple Silicon / Intel) | Native Apple GPU API |

### HarmonyOS PC

A standalone HarmonyOS application is provided in the `ohos/` directory.
It uses `VK_OHOS_surface` + XComponent instead of GLFW. See
[ohos/README.md](ohos/README.md) for build and run instructions.

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
│   ├── dx11_backend.h/cpp    # DirectX 11 backend
│   └── metal_backend.h/mm   # Metal backend (Objective-C++)
├── shaders/
│   ├── compute.comp          # Vulkan GLSL compute shader
│   ├── particle.vert         # Vulkan GLSL vertex shader
│   ├── particle.frag         # Vulkan GLSL fragment shader
│   ├── compute.hlsl          # DX12/DX11 compute shader
│   ├── particle_vs.hlsl      # DX12/DX11 vertex shader
│   ├── particle_ps.hlsl      # DX12/DX11 pixel shader
│   └── particle.metal        # Metal compute + vertex + fragment
└── build/
```

## Prerequisites

| Dependency | Install |
|---|---|
| **CMake 3.20+** | https://cmake.org/download/ |
| **C++17 compiler** | MSVC (Visual Studio 2019+), Clang, or Apple Clang |
| **GLFW** | `vcpkg install glfw3` or `brew install glfw` |
| **Vulkan SDK** (optional) | [LunarG](https://vulkan.lunarg.com/sdk/home) |
| **Windows SDK** (for DX) | Included with Visual Studio |
| **Xcode CLT** (for Metal) | `xcode-select --install` (macOS) |

### Windows on ARM

```powershell
vcpkg install glfw3:arm64-windows
```

The DX12 and DX11 backends only need the Windows SDK (bundled with Visual
Studio). No additional driver installation is needed — D3D12/D3D11 work
through the built-in Windows graphics stack.

### macOS (Apple Silicon / Intel)

```bash
brew install glfw cmake
```

The Metal backend uses the system Metal framework — no additional SDK or
driver installation is needed.

## Build

### Verify Environment (Windows)

Before building on Windows, ensure that `cmake`, `cl` (MSVC compiler), and
`glslc` (Vulkan shader compiler — optional) are available in your PATH.
Verify in PowerShell:

```powershell
cmake --version   # Should be 3.20+
cl                # Should print MSVC version information
glslc --version   # Optional — only required for the Vulkan backend
```

Typical default paths (Visual Studio 2026 Community on ARM64 as an example):

| Tool | Default Path |
|------|-------------|
| cmake | `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin` |
| cl | `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\<version>\bin\Hostarm64\arm64` |
| glslc | `C:\VulkanSDK\<version>\Bin` |

Add the relevant directories to **User environment variables → Path**, then
reopen your terminal for the changes to take effect.

### Verify Environment (macOS)

```bash
cmake --version   # Should be 3.20+
clang --version   # Apple Clang (comes with Xcode Command Line Tools)
```

If `cmake` is not found, install it via Homebrew: `brew install cmake`.

### Build Steps (Windows)

```powershell
# Configure (vcpkg toolchain, all backends auto-detected)
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake

# For ARM64 native builds:
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=arm64-windows -A ARM64

# Build
cmake --build build --config Release
```

### Build Steps (macOS)

```bash
# Configure (Metal backend auto-detected on macOS)
cmake -S . -B build

# Build
cmake --build build --config Release
```

### Backend Toggles

```bash
cmake -S . -B build -DENABLE_VULKAN=OFF -DENABLE_DX12=ON -DENABLE_DX11=ON -DENABLE_METAL=OFF ...
```

## Run

```bash
# Windows
.\build\Release\gpu_benchmark.exe                    # auto-select backend
.\build\Release\gpu_benchmark.exe --backend vulkan    # force Vulkan
.\build\Release\gpu_benchmark.exe --backend dx12      # force DX12
.\build\Release\gpu_benchmark.exe --backend dx11      # force DX11
.\build\Release\gpu_benchmark.exe --backend dx12 --gpu 1   # DX12 + specific GPU

# macOS
./build/gpu_benchmark                                # auto-selects Metal
./build/gpu_benchmark --backend metal                # force Metal
./build/gpu_benchmark --backend vulkan               # force Vulkan (needs MoltenVK)

# Help
./build/gpu_benchmark --help
```

The auto-selection prefers Metal on macOS, DX11 on Windows, then DX12,
then Vulkan.

### Benchmark Mode

Run a fixed number of frames with V-Sync off, then output a standardised
performance report for cross-GPU comparison:

```bash
# Default: 2000 measured frames (+ 100 warmup), V-Sync off
./build/gpu_benchmark --benchmark

# Custom frame count
./build/gpu_benchmark --benchmark 5000

# Normal mode with V-Sync enabled (capped to display refresh rate)
./build/gpu_benchmark --vsync
```

Example output on Apple M4 Pro:

```
==========================================================
                   GPU Benchmark Report
==========================================================
Backend:      Metal
GPU:          Apple M4 Pro
Resolution:   1280x720
Particles:    65536
V-Sync:       OFF
Frames:       1000 (warmup: 100, measured: 1000)
Duration:     3.348 s

--- GPU Timing (ms) ---
                     Avg       Min       Max
Compute:         0.044     0.011     0.366
Render:          0.760     0.251     2.777
Total GPU:       1.711     0.265    10.975

--- Throughput ---
Avg FPS:    298
Peak FPS:   43636
==========================================================
```

## GPU Profiling

All four backends collect per-frame GPU timestamps:

| Backend | Mechanism |
|---------|-----------|
| Vulkan  | `vkCmdWriteTimestamp` query pool (4 timestamps per frame) |
| DX12    | `ID3D12GraphicsCommandList::EndQuery` timestamp heap |
| DX11    | `ID3D11Query` with `D3D11_QUERY_TIMESTAMP` |
| Metal   | `MTLCommandBuffer.GPUStartTime` / `GPUEndTime` on separate compute & render command buffers |

Every second the application prints averaged timing to stdout:

```
[GPU Timing] Compute: 0.031 ms | Render: 0.054 ms | Total GPU: 0.112 ms | FPS: 3200
```

### Metal Performance HUD (macOS)

Enable Apple's built-in Metal performance overlay for real-time GPU metrics:

```bash
MTL_HUD_ENABLED=1 ./build/gpu_benchmark --backend metal
```

## Architecture

```
            ┌──────────┐
            │ AppBase  │  window, particles, timing
            └────┬─────┘
      ┌──────┬───┴───┬───────┐
      │      │       │       │
┌─────┴──┐ ┌┴────┐ ┌┴─────┐ ┌┴─────┐
│ Vulkan │ │ DX12│ │ DX11 │ │Metal │
│Backend │ │Back.│ │Back. │ │Back. │
└────────┘ └─────┘ └──────┘ └──────┘
```

Each backend overrides:
- `InitBackend()` — create device, pipelines, buffers
- `DrawFrame(dt)` — dispatch compute, render, present
- `CleanupBackend()` — release GPU resources
- `GetBackendName()` / `GetDeviceName()` — for display
