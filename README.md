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

## Build (Windows)

Prerequisites:
- Vulkan SDK installed (`vulkaninfo` and `glslc` available in PATH)
- CMake 3.20+
- A C++17 compiler (MSVC or clang)
- GLFW development package installed

Commands:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

Run:

```powershell
.\build\Release\vulkan_gpu_pipeline.exe
```

Optional shader compile script:

```powershell
scripts\compile_shaders.bat
```

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