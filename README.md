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
| **Python 3** | Required by [GLAD](https://github.com/Dav1dde/glad) (OpenGL loader generator) at build time. [python.org](https://www.python.org/downloads/) / `sudo apt install python3` / `brew install python` |
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

#### 1. Install Visual Studio C++ Build Tools

Install [**Visual Studio 2026**](https://visualstudio.microsoft.com/)
(Community edition is free) with the following workloads selected in the
Visual Studio Installer:

- **Desktop development with C++** — provides MSVC compiler (`cl`), Windows
  SDK, CMake, and the linker.
- **C++ CMake tools for Windows** — bundled CMake integration.

If you only need command-line builds (no IDE), install
[Build Tools for Visual Studio](https://visualstudio.microsoft.com/visual-cpp-build-tools/)
instead — select the same workloads above.

> **Note:** Visual Studio does **not** add `cmake` or `cl` to the system
> PATH by default. They are only available inside the **Developer PowerShell
> for VS** (or **Native Tools Command Prompt**). If you run `cmake` or `cl`
> in a regular PowerShell window, you will get:
>
> ```
> cmake : 无法将"cmake"项识别为 cmdlet、函数、脚本文件或可运行程序的名称。
> ```
>
> To make them available globally, add their directories to your User PATH
> (adjust the VS year/edition and MSVC version to match your installation):

```powershell
# cmake
$cmakeDir = "C:\Program Files\Microsoft Visual Studio\<year>\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"

# cl (x64 — for ARM64, replace Hostx64\x64 with Hostarm64\arm64)
$clDir = "C:\Program Files\Microsoft Visual Studio\<year>\Community\VC\Tools\MSVC\<version>\bin\Hostx64\x64"

$currentPath = [Environment]::GetEnvironmentVariable("Path", "User")
foreach ($dir in @($cmakeDir, $clDir)) {
    if ($currentPath -notlike "*$dir*") {
        $currentPath = "$currentPath;$dir"
    }
}
[Environment]::SetEnvironmentVariable("Path", $currentPath, "User")
```

To find the exact MSVC version installed on your system:

```powershell
ls "C:\Program Files\Microsoft Visual Studio\<year>\Community\VC\Tools\MSVC"
# Example output: 14.50.35717
```

Reopen your terminal, then verify:

```powershell
cmake --version   # Should be 3.20+
cl                # Should print MSVC version information
```

#### 2. Install Standalone vcpkg

> **Why not the Visual Studio bundled vcpkg?**
> Visual Studio 2022 17.6+ ships with a bundled vcpkg, but it only supports
> **manifest mode** (requires a `vcpkg.json` in the project). Running
> `vcpkg install <package>` with the bundled version will fail with:
>
> ```
> error: Could not locate a manifest (vcpkg.json) above the current working directory.
> This vcpkg distribution does not have a classic mode instance.
> ```
>
> To use **classic mode** (`vcpkg install glfw3`), you need a standalone
> vcpkg installation.

Clone and bootstrap the standalone vcpkg:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
```

Then add `C:\vcpkg` to your User PATH so it is available globally:

```powershell
$vcpkgDir = "C:\vcpkg"

$currentPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($currentPath -notlike "*$vcpkgDir*") {
    [Environment]::SetEnvironmentVariable("Path", "$currentPath;$vcpkgDir", "User")
    Write-Host "vcpkg added to User PATH. Reopen your terminal for the change to take effect."
} else {
    Write-Host "vcpkg is already in User PATH."
}
```

Reopen your terminal, then verify:

```powershell
vcpkg --version
# Expected: vcpkg package management program version ...
```

#### 3. Install Vulkan SDK (optional — required for Vulkan API)

Download and install the [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home)
for Windows. The installer will set the `VULKAN_SDK` environment variable and
add the SDK `Bin` directory (containing `glslc`, `vulkaninfo`, etc.) to PATH.

After installation, verify that your GPU supports Vulkan:

```powershell
vulkaninfo --summary
```

Expected output (example):

```
==========
VULKANINFO
==========
Vulkan Instance Version: 1.x.xxx

Devices:
========
GPU0:
    apiVersion         = 1.3.xxx
    driverVersion      = xxx.xx
    vendorID           = 0x10de
    deviceID           = 0x2684
    deviceType         = PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
    deviceName         = NVIDIA GeForce RTX 5090
    driverName         = NVIDIA
    driverInfo         = xxx.xx
```

If `vulkaninfo` reports no physical devices, your GPU driver may not support
Vulkan — the application will still work with DirectX or OpenGL backends.

#### 4. Install Python 3 (required for OpenGL backend)

The OpenGL backend uses [GLAD](https://github.com/Dav1dde/glad) as its
loader generator. GLAD's CMake build invokes Python at configure time to
generate the OpenGL function loader source code. Without Python, CMake will
fail with:

```
Could NOT find Python (missing: Python_EXECUTABLE Interpreter)
```

Download and install Python from [python.org](https://www.python.org/downloads/).
During installation, make sure to check **"Add python.exe to PATH"**.

GLAD also depends on the **jinja2** Python package. Install it after Python
is set up:

```powershell
pip install jinja2
```

Without `jinja2`, the build will fail with:

```
ModuleNotFoundError: No module named 'jinja2'
```

After installation, reopen your terminal and verify:

```powershell
python --version
# Expected: Python 3.x.x
```

> **Note:** If you do not need the OpenGL backend, you can skip this step
> and disable it with `-DENABLE_OPENGL=OFF` during CMake configuration.

#### 5. Install GLFW via vcpkg

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
If they are not found, see [Prerequisites → Windows → Step 1](#1-install-visual-studio-c-build-tools)
for how to add them. Verify in PowerShell:

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

### RenderDoc Frame Capture

The application has built-in [RenderDoc](https://renderdoc.org/) integration
via the In-Application API (`--capture <seconds>` for auto-capture, or **F12**
when launched through RenderDoc). The Vulkan backend emits
`VK_EXT_debug_utils` labels and named objects for readable event inspection.

See [`docs/renderdoc-capture-guide.md`](docs/renderdoc-capture-guide.md) for
step-by-step capture instructions and
[`docs/renderdoc-analysis.md`](docs/renderdoc-analysis.md) for the analysis
template.

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
- [x] `VK_EXT_debug_utils` integration — debug labels and object names for RenderDoc
- [x] RenderDoc In-Application API — auto-detect, F12 capture, `--capture <seconds>` CLI
- [x] Python benchmark tooling — chart generation, batch runner, markdown/HTML report export

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
  [5] Full analysis — one GPU (all APIs + RenderDoc + charts)
  [6] Full analysis — all GPUs x APIs (+ RenderDoc + charts)
  [7] Exit
====================================
```

- **Quick run** auto-selects the best GPU (discrete > integrated > software)
  and the best API that GPU supports (Vulkan > DX12 > DX11 > Metal).
- **Run again** appears after the first run and reuses the previous settings.
- **Full analysis** [5]/[6] — one-click workflow that:
  1. **[5]** selects one GPU (default: best dGPU); **[6]** runs every GPU.
  2. Benchmarks every supported API on the selected GPU(s) (1M particles, 15s).
  3. Triggers a RenderDoc frame capture at the 5-second mark on each run (if
     RenderDoc is attached or the in-app API detects the DLL).
  4. After all runs, automatically calls Python scripts to generate:
     - FPS comparison charts → `docs/images/`
     - Markdown results table → `docs/results-table.md`
     - Standalone HTML report → `docs/report.html`
  5. Prints a final comparison table to the console.
  
  Requires `pip install -r scripts/requirements.txt` for the Python step.
- After each run the menu reappears — no need to restart the application.

### In Progress / Planned

#### RX 6900 XT RenderDoc Capture & Cross-GPU Analysis (P0 — next up)

End-to-end RenderDoc profiling on the **RX 6900 XT** (RDNA 2, 80 CU) with
the AMD iGPU (2 CU) as baseline.
Full step-by-step guide: [`docs/renderdoc-capture-guide.md`](docs/renderdoc-capture-guide.md).

- [ ] Run baseline benchmarks (6900 XT + iGPU, Vulkan + DX11) without RenderDoc.
- [ ] Capture one Vulkan frame on each GPU via `--capture 5` (at 5s mark).
- [ ] Take 7 annotated screenshots (event list, compute pipeline, SSBO data,
      graphics pipeline, barrier, render output, per-event timing).
- [ ] Cross-validate app timestamp queries against RenderDoc GPU timing (< 5 %
      deviation target).
- [ ] Write cross-GPU comparison (CU scaling, memory bandwidth, barrier cost).
- [ ] Propose optimisations (Vulkan 1.3 barriers, ping-pong buffer, indirect
      dispatch, dynamic point size, host-visible on iGPU).
- [ ] Fill in [`docs/renderdoc-analysis.md`](docs/renderdoc-analysis.md) and
      update [`docs/benchmark-report.md`](docs/benchmark-report.md) Section 0.

Code integration already complete:

- `VK_EXT_debug_utils` labels and object names in the Vulkan backend.
- RenderDoc In-Application API: auto-detect on launch, **F12** manual capture,
  `--capture <seconds>` for time-based unattended capture.

#### Python Benchmark Tooling (P0 — done)

A `scripts/` directory with Python utilities for automated benchmark data
analysis, satisfying the JD's *"Scripting languages — Python, Perl, shell"*
requirement.

| Script | Purpose |
|--------|---------|
| [`scripts/plot_results.py`](scripts/plot_results.py) | Read `~/.gpu_bench/results.json` and generate 4 charts: FPS by GPU × API, GPU time breakdown (compute/render), CPU overhead, particle-count scaling |
| [`scripts/batch_benchmark.py`](scripts/batch_benchmark.py) | Automate batch runs across all GPU × API × particle-count combinations, with `--dry-run` preview |
| [`scripts/export_report.py`](scripts/export_report.py) | Export results as markdown tables (for docs) or a standalone sortable HTML report |
| [`scripts/compare_3dmark.py`](scripts/compare_3dmark.py) | Cross-validate project FPS against 3DMark Time Spy / Fire Strike scores — normalised bar chart, R² correlation scatter plot, deviation table |
| [`scripts/rdoc_export_timing.py`](scripts/rdoc_export_timing.py) | Export per-event GPU timing from a RenderDoc `.rdc` capture to JSON — works in RenderDoc GUI Python Shell or standalone via `renderdoc` module |
| [`scripts/compare_rdoc_timing.py`](scripts/compare_rdoc_timing.py) | Cross-validate app timestamp queries vs RenderDoc GPU timing — side-by-side comparison, deviation analysis, per-event breakdown |
| [`scripts/3dmark_scores.json`](scripts/3dmark_scores.json) | 3DMark reference scores (edit with your own or public data) |
| [`scripts/requirements.txt`](scripts/requirements.txt) | Python dependencies (`matplotlib`, `numpy`) |

```bash
# Install dependencies
pip install -r scripts/requirements.txt

# Generate charts as PNGs
python scripts/plot_results.py --save docs/images

# Batch-run all GPU × API combos (dry-run first)
python scripts/batch_benchmark.py --gpus 0 1 --dry-run
python scripts/batch_benchmark.py --gpus 0 1 --frames 500

# Export markdown table
python scripts/export_report.py --md docs/results-table.md

# Export standalone HTML report
python scripts/export_report.py --html docs/report.html

# Cross-validate against 3DMark
# Option A: auto-import from .3dmark-result files (saved by 3DMark GUI)
python scripts/compare_3dmark.py --import-3dmark "C:/Users/*/Documents/3DMark/*.3dmark-result"

# Option B: auto-import from exported XML (3DMark Professional --export)
python scripts/compare_3dmark.py --import-xml path/to/timespy.xml path/to/firestrike.xml

# Option C: manually edit scripts/3dmark_scores.json, then:
python scripts/compare_3dmark.py --save docs/images   # generate charts
python scripts/compare_3dmark.py --md                  # markdown table to stdout

# RenderDoc timing export & cross-validation
# Step 1: Export timing from .rdc capture (inside RenderDoc Python Shell)
#   exec(open('scripts/rdoc_export_timing.py').read())
# Step 1 (alt): Standalone (requires renderdoc on PYTHONPATH)
python scripts/rdoc_export_timing.py capture_6900xt.rdc -o rdoc_6900xt.json
python scripts/rdoc_export_timing.py capture_igpu.rdc   -o rdoc_igpu.json

# Step 2: Compare app timestamps vs RenderDoc timing
python scripts/compare_rdoc_timing.py rdoc_6900xt.json rdoc_igpu.json
```

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

| GPU | Architecture | CUs / SPs | VRAM | Platform | Notes |
|-----|-------------|-----------|------|----------|-------|
| HD 5770 | Evergreen (TeraScale 2, before GCN) | 800 SPs | 1 GB | Windows (DX11) | Legacy DX11-era GPU |
| **FirePro D700 ×2** | **Tahiti (GCN 1.0)** | **2048 SPs** | **6 GB** | **macOS (Metal)** | **Mac Pro 2013 dual-GPU — each card benchmarked independently** |
| RX 580 | Polaris (GCN 4) | 36 CUs | 8 GB | Windows | Mid-range GCN |
| Vega Frontier Edition | Vega (GCN 5) | 64 CUs | 16 GB HBM2 | Windows | Prosumer / compute |
| RX 6600 XT | RDNA 2 | 32 CUs | 8 GB | Windows | Mid-range RDNA 2 |
| RX 6900 XT | RDNA 2 | 80 CUs | 16 GB | Windows | Flagship RDNA 2 |
| Ryzen 7 9800X3D iGPU | RDNA 3 | 2 CUs | Shared | Windows | Integrated graphics |
| Ryzen 7 9800X3D (WARP) | Software | — | System RAM | Windows | Microsoft WARP software rasteriser on AMD CPU |

> **HD 5770 note:** Evergreen does not support Vulkan. Testing will use the
> DX11 backend only (Feature Level 11_0).
>
> **FirePro D700 note:** The Mac Pro (Late 2013) has two identical D700
> GPUs (GCN 1.0, Tahiti XT). macOS does **not** support CrossFire; each GPU
> is an independent `MTLDevice`. One GPU handles display output while the
> other is dedicated to compute. Both cards will be benchmarked individually
> via Metal, and optionally via MoltenVK (Vulkan→Metal) or Boot Camp DX11.
> This provides the only GCN 1.0 data point in the comparison.
>
> **WARP note:** The Windows Advanced Rasterization Platform (WARP) is a
> high-performance software renderer included in DirectX. Running the DX11 /
> DX12 backends on WARP with an AMD CPU provides a pure-software baseline,
> isolating CPU compute throughput from GPU hardware.

The document will cover:

- Per-backend (Vulkan / DX11 / DX12 / Metal / WARP) frame-rate comparison at
  65 536 particles.
- Scaling behaviour when increasing particle count (64 K → 1 M → 4 M).
- Compute vs render timing breakdown per GPU (and CPU via WARP).
- Hardware vs software rendering comparison (discrete GPU vs WARP baseline).
- Thermal throttling observations during sustained benchmark runs.
- Generational progression from TeraScale 2 → GCN 1.0 → GCN 4 → GCN 5 → RDNA 2 → RDNA 3.
- Mac Pro 2013 dual-GPU analysis: display GPU vs headless GPU performance
  isolation, and macOS Metal vs Boot Camp DX11 cross-platform comparison.

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

#### Explicit Multi-GPU — Split Compute Across Dual GPUs

Implement explicit multi-GPU support, splitting the particle compute workload
across two physical GPUs and merging results for rendering. Target hardware:
**Mac Pro 2013 dual FirePro D700** (GCN 1.0, 6 GB each).

| API | Mechanism | Status |
|-----|-----------|--------|
| **Metal** (primary) | `MTLCopyAllDevices()` → two `MTLDevice` / `MTLCommandQueue`, split particle buffer, `MTLSharedEvent` cross-GPU sync | Planned — most feasible path; macOS natively exposes both D700s |
| **DX12** | `IDXGIFactory6::EnumAdapters` → Linked or Unlinked Explicit Multi-Adapter, `ID3D12Fence` cross-GPU sync | Long-term — requires Boot Camp + working DX12 driver for D700 |
| **Vulkan** | `VK_KHR_device_group` / `VK_KHR_device_group_creation`, sub-allocate per-device memory, semaphore sync | Long-term — needs dual Vulkan ICDs on the same machine |

Tasks:

- [ ] Metal: enumerate both D700s, create per-device command queues and
      particle buffers (each device owns half the particles).
- [ ] Metal: dispatch compute on both devices in parallel, synchronise with
      `MTLSharedEvent`, blit results to the display-GPU buffer.
- [ ] Metal: render merged particle buffer on the display GPU.
- [ ] Benchmark single-GPU vs dual-GPU throughput (ideal ≈ 2× compute, less
      for render due to data transfer overhead).
- [ ] Write analysis document: scaling efficiency, PCIe transfer cost,
      synchronisation overhead, comparison with implicit CrossFire AFR.
- [ ] (Optional) DX12 Explicit Multi-Adapter implementation on Boot Camp
      Windows, if D700 drivers support DX12.
- [ ] (Optional) Vulkan `VK_KHR_device_group` implementation on a system with
      two discrete Vulkan-capable GPUs.

---

## Acknowledgements
