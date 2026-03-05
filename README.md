# Vulkan GPU Compute & Rendering Pipeline

This repository now contains a baseline Vulkan project scaffold for a combined
compute + graphics pipeline portfolio project.

Current status:
- Window creation and Vulkan instance/device initialisation are implemented.
- Queue family setup covers graphics, present, and compute queues.
- Swapchain, image views, render pass, framebuffers, and command buffers are implemented.
- Per-frame submit/present synchronisation is implemented with semaphores and fences.
- The app now renders a visible minimal frame (clear colour pass).
- GLSL shader placeholders exist for compute, vertex, and fragment stages.
- Build system compiles the C++ app and optionally compiles shaders to SPIR-V.

## Tech Stack

- C++17
- Vulkan 1.2
- GLSL
- GLFW
- CMake
- RenderDoc (planned profiling workflow)

## Project Structure

```text
.
├── CMakeLists.txt          # Build config: C++ compilation, GLSL→SPIR-V, post-build copy
├── README.md
├── .gitignore
├── src/
│   └── main.cpp            # Vulkan app entry point – instance, device, swapchain,
│                            #   graphics pipeline, compute pipeline, timestamp profiling
├── shaders/
│   ├── compute.comp        # Compute shader – per-particle Euler integration (SSBO + push constants)
│   ├── particle.vert       # Vertex shader – positions particles as GL_POINT, velocity → colour
│   └── particle.frag       # Fragment shader – outputs interpolated colour to framebuffer
└── build/                  # (generated) CMake output
    └── Release/
        ├── vulkan_gpu_pipeline.exe
        ├── compute.comp.spv    # Compiled SPIR-V (auto-copied by CMake post-build)
        ├── particle.vert.spv
        └── particle.frag.spv
```

## Prerequisites

| Dependency | Install |
|---|---|
| **Vulkan SDK** | Download from [LunarG](https://vulkan.lunarg.com/sdk/home) and run the installer. This provides the Vulkan headers, libraries, `glslc` shader compiler, and validation layers. After installation, the `VULKAN_SDK` environment variable should be set automatically. |
| **GLFW** | Install via [vcpkg](https://github.com/microsoft/vcpkg): `vcpkg install glfw3:x64-windows` |
| **CMake 3.20+** | https://cmake.org/download/ |
| **C++17 compiler** | MSVC (Visual Studio Build Tools) or Clang |

Verify your environment:

```powershell
vulkaninfo --summary   # should list your GPU(s)
glslc --version        # should print shaderc version
```

> **Note:** If `glslc` is not found after installing the Vulkan SDK, restart your
> IDE / terminal so the updated `PATH` takes effect.

## Build (Windows)

```powershell
# Configure (use vcpkg toolchain so CMake can find GLFW), only need run in first time
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake

# Compile (shaders are automatically compiled to SPIR-V)
cmake --build build --config Release
```

## Run

```powershell
.\build\Release\vulkan_gpu_pipeline.exe
```

On startup the app lists every Vulkan-capable GPU and selects one:

```
Available GPUs:
  [0] NVIDIA GeForce RTX 5090 (Discrete GPU)
  [1] AMD Radeon(TM) Graphics (Integrated GPU)
Selected GPU [0]: NVIDIA GeForce RTX 5090
```

### GPU Selection

By default the app prefers a discrete GPU. Use `--gpu <index>` to override:

```powershell
# Use the NVIDIA discrete GPU (index 0)
.\build\Release\vulkan_gpu_pipeline.exe --gpu 0

# Use the AMD integrated GPU (index 1)
.\build\Release\vulkan_gpu_pipeline.exe --gpu 1
```

The index corresponds to the `[N]` shown in the GPU list at startup.

## GPU Profiling

The app uses Vulkan **timestamp queries** to measure GPU-side timings
(separate from CPU frame time). Four timestamps are written per frame:

| Index | Where | Pipeline stage |
|-------|-------|----------------|
| 0 | Before compute dispatch | `TOP_OF_PIPE` |
| 1 | After compute dispatch | `COMPUTE_SHADER` |
| 2 | Before render pass | `TOP_OF_PIPE` |
| 3 | After render pass | `COLOR_ATTACHMENT_OUTPUT` |

Every second the app prints an averaged summary to **stdout** and updates
the window title:

```
[GPU Timing] Compute: 0.031 ms | Render: 0.054 ms | Total GPU: 0.112 ms | FPS: 3200
```

> **Note:** If the selected GPU queue family does not support timestamps
> (timestampValidBits == 0), profiling is silently disabled and only FPS is
> reported.

### RenderDoc

For deeper analysis, attach [RenderDoc](https://renderdoc.org/) to the
executable and capture a frame. The tool will display per-draw-call timings,
resource state, and shader debugging for both the compute and graphics passes.

## Next Milestones

1. ~~Add swapchain, render pass, and graphics pipeline creation.~~
2. ~~Add a minimal graphics pipeline and draw call (particle point cloud).~~
3. ~~Add storage buffers and compute pipeline for particle simulation.~~
4. ~~Add compute-to-graphics synchronisation (barriers + semaphores).~~
5. ~~Add timestamp queries and GPU profiling.~~
6. Add workgroup-size tuning experiments and analysis.
7. Add RenderDoc frame-capture walkthrough documentation.

## Resume-Oriented Outcome Targets

After finishing the milestones, this project can support resume points such as:

- Built a real-time particle simulation using Vulkan compute shaders with
	GPU-side storage buffers.
- Implemented a Vulkan graphics pipeline (descriptors, uniform buffers,
	command buffers, synchronisation) to render compute outputs.
- Profiled on AMD RDNA2 and NVIDIA GPUs using RenderDoc and Vulkan timestamps,
	then tuned workgroup sizing and memory-access patterns.