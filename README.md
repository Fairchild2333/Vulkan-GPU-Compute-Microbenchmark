# GPU Compute & Rendering Pipeline — Multi-Graphics API

A real-time particle simulation using GPU compute shaders with five
interchangeable graphics API backends: **Vulkan**, **DirectX 12**,
**DirectX 11**, **OpenGL 4.3**, and **Metal**. Each backend implements the
same particle physics (Euler integration in a compute shader) and point-cloud
rendering, with GPU timestamp profiling. Runs on **Windows**, **Linux**, and
**macOS**.

## Supported Graphics APIs

| Graphics API | API Level | Platforms | Notes |
|---------|-----------|-----------|-------|
| Vulkan  | 1.2       | Windows, Linux, HarmonyOS | Requires Vulkan SDK + ICD driver |
| DirectX 12 | Feature Level 11_0+ | Windows 10+ | Tries FL 12_1→12_0→11_1→11_0; works on older GPUs too |
| DirectX 11 | Feature Level 11_0 | Windows 7+  | Simplest, broadest Windows support |
| OpenGL  | 4.3 Core  | Windows, Linux, macOS (legacy) | Cross-platform fallback; requires `GL_ARB_compute_shader` |
| Metal   | Metal 2+  | macOS (Apple/Intel) | Native Apple GPU API (Apple/AMD) — highest priority on macOS |

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

## Prerequisites

| Dependency | Install |
|---|---|
| **CMake 3.20+** | https://cmake.org/download/ or system package manager |
| **C++17 compiler** | MSVC (Visual Studio 2019+), GCC 8+, Clang, or Apple Clang |
| **GLFW** | `vcpkg install glfw3` / `brew install glfw` / `sudo apt install libglfw3-dev` |
| **Vulkan SDK** (optional) | [LunarG](https://vulkan.lunarg.com/sdk/home) or `sudo apt install libvulkan-dev` |
| **Windows SDK** (for DX) | Included with Visual Studio |
| **Xcode CLT** (for Metal) | `xcode-select --install` (macOS) |

### Linux

> **Tested on Ubuntu.** Fedora and Arch commands are provided for
> convenience but have not been verified by the author.

**Ubuntu / Debian** (`apt`):

```bash
sudo apt install build-essential cmake libglfw3-dev libgl-dev
sudo apt install libvulkan-dev vulkan-tools glslc   # optional, for Vulkan backend
```

**Fedora / RHEL** (`dnf`):

```bash
sudo dnf install gcc-c++ cmake glfw-devel mesa-libGL-devel
sudo dnf install vulkan-loader-devel vulkan-tools glslc   # optional, for Vulkan backend
```

**Arch / Manjaro** (`pacman`):

```bash
sudo pacman -S base-devel cmake glfw-x11 mesa
sudo pacman -S vulkan-icd-loader vulkan-tools shaderc   # optional, for Vulkan backend
```

At least one of the OpenGL (`libgl-dev` / `mesa-libGL-devel` / `mesa`) or
Vulkan development packages must be installed — otherwise no backend will be
available. DirectX and Metal backends are automatically disabled on Linux.

| Backend | Available on Linux | Driver Requirement |
|---------|-------------------|--------------------|
| Vulkan  | Yes (with `libvulkan-dev`) | Mesa or NVIDIA proprietary driver |
| OpenGL 4.3 | Yes (with `libgl-dev`) | Mesa or NVIDIA proprietary driver |
| DirectX 11/12 | No | Windows only |
| Metal | No | macOS only |

**GPU selection for OpenGL:** On Linux, the application uses the `DRI_PRIME`
environment variable to route OpenGL to the user's chosen GPU. This is set
automatically when a GPU is selected via the interactive menu or `--gpu`.
You can also set it manually:

```bash
DRI_PRIME=1 ./build/gpu_benchmark --backend opengl   # use secondary GPU
```

For NVIDIA proprietary drivers, use:

```bash
__NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia ./build/gpu_benchmark --backend opengl
```

### Windows (x64 / ARM64)

```powershell
# x64
vcpkg install glfw3

# ARM64
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

### Verify Environment (Linux)

```bash
cmake --version    # Should be 3.20+
g++ --version      # GCC 8+ or clang++ 7+
pkg-config --modversion glfw3   # Should print 3.x
glslc --version    # Optional — only required for the Vulkan backend
```

If `glslc` is not found and you need the Vulkan backend, install the LunarG
Vulkan SDK or `sudo apt install glslc`.

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

### Build Steps (Linux)

```bash
# Configure (backends auto-detected based on installed packages)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build

# Run
./build/gpu_benchmark
```

CMake will print which backends are enabled during configuration:

```
-- Vulkan backend: ENABLED
-- DX12 backend:   DISABLED (not Windows)
-- DX11 backend:   DISABLED (not Windows)
-- Metal backend:  DISABLED (not macOS)
-- OpenGL backend: ENABLED
```

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

```powershell
# Windows (PowerShell)
.\build\Release\gpu_benchmark.exe                         # interactive menu
.\build\Release\gpu_benchmark.exe --backend vulkan         # force Vulkan (skip menu)
.\build\Release\gpu_benchmark.exe --backend dx12           # force DX12
.\build\Release\gpu_benchmark.exe --backend dx11           # force DX11
.\build\Release\gpu_benchmark.exe --backend dx12 --gpu 1   # DX12 + specific GPU

# Result management
.\build\Release\gpu_benchmark.exe --results                # list saved results
.\build\Release\gpu_benchmark.exe --compare                # compare all results
.\build\Release\gpu_benchmark.exe --compare <id1> <id2>    # detailed side-by-side
.\build\Release\gpu_benchmark.exe --results-export out.csv  # export to CSV
```

```bash
# Linux
./build/gpu_benchmark                                # interactive menu
./build/gpu_benchmark --backend vulkan               # force Vulkan
./build/gpu_benchmark --backend opengl               # force OpenGL
./build/gpu_benchmark --backend opengl --gpu 1       # OpenGL on second GPU (sets DRI_PRIME)

# macOS
./build/gpu_benchmark                                # interactive menu
./build/gpu_benchmark --backend metal                # force Metal (skip menu)
./build/gpu_benchmark --backend vulkan               # force Vulkan (needs MoltenVK)

# Help
./build/gpu_benchmark --help
```

### Backend Auto-Selection

When no `--backend` is specified, the application probes backends in order
and falls back to the next if the current one fails to initialise:

**macOS:** Metal → Vulkan → OpenGL
**Linux:** Vulkan → OpenGL
**Windows:** Vulkan → DX12 → DX11 → OpenGL

Each fallback is logged to the terminal:

```
[Backend] Trying vulkan...
[Backend] vulkan not available: No Vulkan physical device found
[Backend] Trying dx12...
[Backend] Using dx12
```

### Multi-GPU Selection

On systems with multiple GPUs, the application lists all detected hardware
adapters (deduplicated by LUID on DirectX backends) and prompts for
selection if `--gpu` is not specified:

```
Available GPUs:
  [0] NVIDIA GeForce RTX 5090 (Hardware)  VRAM: 32480 MB
  [1] AMD Radeon(TM) Graphics (Hardware)  VRAM: 512 MB
Multiple hardware GPUs detected. Default: [0]
Enter GPU index to use (or press Enter for default):
```

### Benchmark Mode

Run a fixed number of frames with V-Sync off, then output a standardised
performance report for cross-GPU comparison:

```powershell
# Windows — default: 2000 measured frames (+ 100 warmup), V-Sync off
.\build\Release\gpu_benchmark.exe --benchmark

# Custom frame count
.\build\Release\gpu_benchmark.exe --benchmark 5000

# Normal mode with V-Sync enabled (capped to display refresh rate)
.\build\Release\gpu_benchmark.exe --vsync
```

```bash
# macOS / Linux
./build/gpu_benchmark --benchmark
./build/gpu_benchmark --benchmark 5000
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
| OpenGL  | `glQueryCounter` with `GL_TIMESTAMP` (4 queries per frame, ring buffer) |
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

## Roadmap

### Completed

- [x] Vulkan compute + graphics pipeline with particle simulation
- [x] DirectX 12 / DirectX 11 / Metal backend ports
- [x] GPU timestamp profiling (all backends)
- [x] Benchmark mode with standardised report output
- [x] Multi-GPU selection (`--gpu` flag)
- [x] HarmonyOS (OHOS) Vulkan port via XComponent
- [x] Benchmark result history — auto-save, list, compare, delete, CSV export
- [x] Interactive main menu with quick run / custom run / comparison / delete
- [x] OpenGL 4.3 compute shader backend with GLAD2 loader

#### Benchmark Result History

Results are automatically saved to `~/.gpu_bench/results.json` after each run.
Full metrics are persisted: graphics API, GPU, CPU, particle count, difficulty,
timing breakdown (compute / render / total GPU), FPS, and bottleneck analysis.

| Feature | Interactive | CLI |
|---------|-----------|-----|
| List all results | Main menu → Delete results | `--results` |
| Compare (ranked table) | Main menu → Compare results | `--compare` |
| Detailed side-by-side | Compare → enter two rank #s | `--compare <id1> <id2>` |
| Delete one result | Delete → enter ID | `--results-delete <id>` |
| Clear all results | Delete → type `all` | `--results-clear` |
| Export to CSV | — | `--results-export <file.csv>` |

#### Interactive Main Menu

On startup (when no CLI flags are given), the application presents:

```
========== GPU Benchmark ==========
  [0] Quick run (Vulkan 1.2 / RTX 5090 / Medium)  <- default
  [1] Custom run (choose API / GPU / difficulty)
  [2] Run again (same settings)
  [3] Compare results (N saved)
  [4] Delete results
  [5] Exit
====================================
```

- **Quick run** auto-selects the best GPU (discrete > integrated > software)
  and the best API that GPU supports (Vulkan > DX12 > DX11 > Metal).
- **Run again** appears after the first run and reuses the previous settings.
- After each run the menu reappears — no need to restart the application.

### In Progress / Planned

#### Web Backend — WebGL / WebGPU

Browser-based port of the particle benchmark, inspired by projects such as
[Volume Shader BM](https://volumeshader-bm.com/). Goals:

- **WebGPU** compute shader path (WGSL) for browsers with WebGPU support.
- **WebGL 2.0** fallback using transform feedback for particle updates.
- Hosted as a static site so anyone can run the benchmark without installing
  drivers or SDKs.
- Cross-platform, cross-system league table comparing results from native
  backends (Vulkan / DX / Metal) against web backends on the same hardware.

#### Cross-Platform & Cross-GPU Performance Comparison

Publish a written analysis document comparing frame rates and GPU timings
across a range of AMD hardware:

| GPU | Architecture | CUs / SPs | VRAM | Notes |
|-----|-------------|-----------|------|-------|
| HD 5770 | Evergreen (TeraScale 2, before GCN) | 800 SPs | 1 GB | Legacy DX11-era GPU |
| RX 580 | Polaris (GCN 4) | 36 CUs | 8 GB | Mid-range GCN |
| Vega Frontier Edition | Vega (GCN 5) | 64 CUs | 16 GB HBM2 | Prosumer / compute |
| RX 6600 XT | RDNA 2 | 32 CUs | 8 GB | Mid-range RDNA 2 |
| RX 6900 XT | RDNA 2 | 80 CUs | 16 GB | Flagship RDNA 2 |
| Ryzen 7 9800X3D iGPU | RDNA 3 | 2 CUs | Shared | Integrated graphics |
| Ryzen 7 9800X3D (WARP) | Software | — | System RAM | Microsoft WARP software rasteriser on AMD CPU |

> **HD 5770 note:** Evergreen does not support Vulkan. Testing will use the
> DX11 backend only (Feature Level 11_0).
>
> **WARP note:** The Windows Advanced Rasterization Platform (WARP) is a
> high-performance software renderer included in DirectX. Running the DX11 /
> DX12 backends on WARP with an AMD CPU provides a pure-software baseline,
> isolating CPU compute throughput from GPU hardware.

The document will cover:

- Per-backend (Vulkan / DX11 / DX12 / WARP) frame-rate comparison at
  65 536 particles.
- Scaling behaviour when increasing particle count (64 K → 1 M → 4 M).
- Compute vs render timing breakdown per GPU (and CPU via WARP).
- Hardware vs software rendering comparison (discrete GPU vs WARP baseline).
- Thermal throttling observations during sustained benchmark runs.
- Generational progression from TeraScale 2 → GCN → RDNA 2 → RDNA 3.

#### Workgroup Size Tuning Experiments

Sweep `local_size_x` across powers of two (32, 64, 128, 256, 512, 1024) and
measure the impact on compute dispatch time for each GPU above. Publish
findings as an analysis document covering:

- Optimal workgroup size per architecture (GCN vs RDNA 2 vs RDNA 3).
- Occupancy and wavefront utilisation implications.
- Correlation with CU count and cache hierarchy.

#### Memory Allocation Strategy Comparison

Benchmark and document the performance difference between:

| Strategy | Vulkan Flags | Use Case |
|----------|-------------|----------|
| Host-visible / host-coherent | `HOST_VISIBLE \| HOST_COHERENT` | Current approach — simple, CPU-mappable |
| Device-local + staging buffer | `DEVICE_LOCAL` + staging copy | Optimal for discrete GPUs |
| Persistent mapping | `HOST_VISIBLE \| HOST_COHERENT` + persistent `vkMapMemory` | Avoids repeated map/unmap |
| Device-local host-visible (ReBAR) | `DEVICE_LOCAL \| HOST_VISIBLE` | AMD SAM / ReBAR on supported GPUs |

Measure particle-buffer throughput and compute dispatch latency for each
strategy across integrated and discrete GPUs.

#### RenderDoc Frame-Capture Walkthrough

A step-by-step analysis document demonstrating proficiency with industry-
standard GPU profiling tools:

- Attaching RenderDoc to the Vulkan backend.
- Capturing a single frame and inspecting the event list.
- Viewing compute dispatch timings, SSBO contents, and barrier placement.
- Identifying pipeline bubbles or redundant barriers.
- Annotated screenshots with explanations.

#### Multi-Draw-Call Stress Test

The current benchmark issues a single compute dispatch and a single draw call
per frame — a workload profile that favours DX11's highly optimised implicit
driver path over DX12/Vulkan's explicit model (see
[`docs/benchmark-report.md`](docs/benchmark-report.md) for measured data).

To demonstrate the scalability advantage of explicit APIs, add an optional
**multi-draw-call mode**:

- Render particles in batches (e.g. 1 draw call per 1 024 particles), producing
  1 000+ draw calls per frame at default particle counts.
- Record draw commands across **4–8 threads** on DX12 (secondary command lists)
  and Vulkan (secondary command buffers), then submit in a single primary.
- Compare single-threaded vs multi-threaded CPU submission time per API.
- Expected outcome: DX12/Vulkan overtake DX11 when draw-call count is high
  enough for the driver's single-threaded path to become the bottleneck.

This will complete the cross-API analysis by showing both sides of the
implicit-vs-explicit trade-off.

#### Advanced Particle Interactions

Extend the compute shader to support richer physics:

- **Gravitational attraction** — N-body style pairwise forces (shared-memory
  tiling for O(N log N) or O(N²) with optimisation notes).
- **Simple collision response** — spatial hashing or grid-based broad phase.
- **Attractors / repulsors** — mouse-driven interactive forces.

This demonstrates more complex compute shader design, including shared-memory
optimisation and synchronisation within workgroups.

#### HIP / ROCm Headless Compute Backend

Add a headless (no rendering) compute benchmark using AMD's
[HIP](https://github.com/ROCm/HIP) runtime:

- Port the particle-update kernel from GLSL/HLSL to a HIP kernel.
- Time kernel dispatch with `hipEvent` and output the same standardised
  benchmark report as the graphics backends.
- Compare HIP kernel throughput against Vulkan/DX compute shader dispatch on
  identical AMD hardware.
- HIP compiles for both AMD (ROCm) and NVIDIA (CUDA back-end) GPUs, so the
  same source covers both vendors.

#### CUDA Headless Compute Backend

Equivalent headless compute benchmark targeting NVIDIA GPUs natively:

- Port the particle-update kernel to a CUDA kernel (`.cu`).
- Time with `cudaEvent` and produce the same report format.
- Compare CUDA kernel throughput against Vulkan compute and the HIP path on
  NVIDIA hardware.
·
