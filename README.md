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
|- CMakeLists.txt
|- src/
|  |- main.cpp
|- shaders/
|  |- compute.comp
|  |- particle.vert
|  |- particle.frag
|- scripts/
|  |- compile_shaders.bat
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
# Configure (use vcpkg toolchain so CMake can find GLFW)
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

## Next Milestones

1. Add swapchain, render pass, and graphics pipeline creation.
2. Add a minimal graphics pipeline and draw call (triangle or point cloud).
3. Add storage buffers and compute pipeline for particle simulation.
4. Add compute-to-graphics synchronisation (barriers + semaphores).
5. Add timestamp queries and RenderDoc frame analysis notes.

## Resume-Oriented Outcome Targets

After finishing the milestones, this project can support resume points such as:

- Built a real-time particle simulation using Vulkan compute shaders with
	GPU-side storage buffers.
- Implemented a Vulkan graphics pipeline (descriptors, uniform buffers,
	command buffers, synchronisation) to render compute outputs.
- Profiled on AMD RDNA2 and NVIDIA GPUs using RenderDoc and Vulkan timestamps,
	then tuned workgroup sizing and memory-access patterns.