# GPU Compute & Rendering Pipeline — Multi-Graphics API

A real-time particle simulation using GPU compute shaders with five
interchangeable graphics API backends: **Vulkan**, **DirectX 12**,
**DirectX 11**, **OpenGL 4.3**, and **Metal**. Each backend implements the
same particle physics (Euler integration in a compute shader) and point-cloud
rendering, with GPU timestamp profiling. Runs on **Windows**, **Linux**, and
**macOS**.

See [`docs/report.md`](docs/report.md) for the full analysis.

## Supported Graphics APIs

| Graphics API | API Level | Platforms | Notes |
|---------|-----------|-----------|-------|
| Vulkan  | 1.1+       | Windows, Linux, HarmonyOS | Requires Vulkan SDK + ICD driver |
| DirectX 12 | Feature Level 11_0+ | Windows 10+ | Tries FL 12_1→12_0→11_1→11_0; works on older GPUs too |
| DirectX 11 | Feature Level 11_0 | Windows 7+  | Simplest, broadest Windows support |
| OpenGL  | 4.3 Core  | Windows, Linux, macOS (legacy) | Cross-platform fallback; requires `GL_ARB_compute_shader` |
| Metal   | Metal 2+  | macOS (Apple/Intel) | Native Apple GPU API (Apple/AMD) — highest priority on macOS |

## Project Structure

```text
.
├── CMakeLists.txt
├── README.md
├── src/
│   ├── main.cpp                # Entry point — interactive menu, GPU selection, CLI
│   ├── app_base.h/cpp          # Shared base class (window, particles, timing)
│   ├── benchmark_results.h/cpp # Result persistence, comparison tables, CSV export
│   ├── gpu_common.h            # Shared types (BenchmarkConfig, BackToMenuException)
│   ├── vulkan_backend.h/cpp    # Vulkan
│   ├── dx12_backend.h/cpp      # DirectX 12
│   ├── dx11_backend.h/cpp      # DirectX 11
│   ├── opengl_backend.h/cpp    # OpenGL 4.3
│   └── metal_backend.h/mm     # Metal (Objective-C++)
├── shaders/
│   ├── compute.comp          # Vulkan GLSL compute shader
│   ├── particle.vert         # Vulkan GLSL vertex shader
│   ├── particle.frag         # Vulkan GLSL fragment shader
│   ├── compute.hlsl          # DX12/DX11 compute shader
│   ├── particle_vs.hlsl      # DX12/DX11 vertex shader
│   ├── particle_ps.hlsl      # DX12/DX11 pixel shader
│   ├── compute_gl.comp       # OpenGL 4.3 compute shader
│   ├── particle_gl.vert      # OpenGL 4.3 vertex shader
│   ├── particle_gl.frag      # OpenGL 4.3 fragment shader
│   └── particle.metal        # Metal compute + vertex + fragment
└── build/
```

## Quick Start

See [`docs/building.md`](docs/building.md) for detailed prerequisites and
platform-specific setup (Windows/Linux/macOS).

**Linux:**
```bash
sudo apt install build-essential cmake libglfw3-dev libgl-dev  # + libvulkan-dev for Vulkan
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/gpu_benchmark
```

**Windows (Developer PowerShell):**
```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
.\build\Release\gpu_benchmark.exe
```

**macOS:**
```bash
brew install glfw cmake
cmake -S . -B build && cmake --build build --config Release
./build/gpu_benchmark
```

Toggle individual backends with `-DENABLE_VULKAN=OFF`, `-DENABLE_DX12=ON`, etc.

## Run

```bash
./build/gpu_benchmark                          # interactive menu
./build/gpu_benchmark --backend vulkan         # force specific API
./build/gpu_benchmark --backend dx12 --gpu 1   # specific API + GPU
./build/gpu_benchmark --benchmark              # benchmark mode (2000 frames, V-Sync off)
./build/gpu_benchmark --benchmark --headless   # pure GPU compute, no rendering
./build/gpu_benchmark --help                   # all options
```

### Backend Auto-Selection

When no `--backend` is specified, the application probes in order:

- **macOS:** Metal → Vulkan → OpenGL
- **Linux:** Vulkan → OpenGL
- **Windows:** Vulkan → DX12 → DX11 → OpenGL

### Result Management

```bash
./build/gpu_benchmark --results                # list saved results
./build/gpu_benchmark --compare                # compare all results
./build/gpu_benchmark --compare <id1> <id2>    # detailed side-by-side
./build/gpu_benchmark --results-export out.csv # export to CSV
```

## GPU Profiling

All backends collect per-frame GPU timestamps:

| Backend | Mechanism |
|---------|-----------|
| Vulkan  | `vkCmdWriteTimestamp` query pool |
| DX12    | `ID3D12GraphicsCommandList::EndQuery` timestamp heap |
| DX11    | `ID3D11Query` with `D3D11_QUERY_TIMESTAMP` |
| OpenGL  | `glQueryCounter` with `GL_TIMESTAMP` |
| Metal   | `MTLCommandBuffer.GPUStartTime` / `GPUEndTime` |

Built-in [RenderDoc](https://renderdoc.org/) integration via In-Application
API (`--capture <seconds>` or **F12**). See
[`docs/renderdoc-capture-guide.md`](docs/renderdoc-capture-guide.md).

## Architecture

```
               ┌──────────┐
               │ AppBase  │  window, particles, timing
               └────┬─────┘
      ┌──────┬──┴──┬──────┬────────┐
      │      │     │      │        │
┌─────┴──┐ ┌┴───┐ ┌┴────┐ ┌┴──────┐ ┌┴─────┐
│ Vulkan │ │DX12│ │DX11 │ │OpenGL │ │Metal │
│Backend │ │Back│ │Back │ │Back.  │ │Back. │
└────────┘ └────┘ └─────┘ └───────┘ └──────┘
```

Each backend overrides:
- `InitBackend()` — create device, pipelines, buffers
- `DrawFrame(dt)` — dispatch compute, render, present
- `CleanupBackend()` — release GPU resources
- `GetBackendName()` / `GetDeviceName()` — for display

## HarmonyOS PC

A standalone HarmonyOS application is provided in the `ohos/` directory.
It uses `VK_OHOS_surface` + XComponent instead of GLFW. See
[ohos/README.md](ohos/README.md) for build and run instructions.

## Further Reading

| Document | Description |
|----------|-------------|
| [`docs/report.md`](docs/report.md) | Full cross-platform & cross-GPU performance analysis |
| [`docs/building.md`](docs/building.md) | Detailed build prerequisites and platform setup |
| [`docs/roadmap.md`](docs/roadmap.md) | Completed features, in-progress work, and planned enhancements |
| [`docs/renderdoc-capture-guide.md`](docs/renderdoc-capture-guide.md) | Step-by-step RenderDoc capture instructions |
| [`docs/renderdoc-analysis.md`](docs/renderdoc-analysis.md) | RenderDoc analysis template |
| [`ohos/README.md`](ohos/README.md) | HarmonyOS build and run guide |

---

## Acknowledgements
