# Benchmark Results Comparison

> Generated: 2026-03-20 01:08  
> Results: 9 entries

## Ranked by Average FPS

| # | GPU | API | Particles | Avg FPS | Avg GPU (ms) | Compute (ms) | Render (ms) | Frame (ms) | CPU OH (ms) | Bottleneck |
|---|-----|-----|-----------|---------|-------------|-------------|-------------|------------|-------------|------------|
| 1 | RTX 5090 | Vulkan | 1.0M | 3,832 | 0.090 | 0.025 | 0.064 | 0.261 | 0.171 | CPU-bound |
| 2 | RTX 5090 | DX11 | 1.0M | 3,601 (-6%) | 0.086 | 0.020 | 0.049 | 0.278 | 0.192 | CPU-bound |
| 3 | RTX 5090 (FL 12_1) | DX12 | 1.0M | 2,967 (-23%) | 0.065 | 0.014 | 0.050 | 0.337 | 0.272 | CPU-bound |
| 4 | RTX 5090/PCIe/SSE2 | OpenGL | 1.0M | 2,745 (-28%) | 0.091 | 0.020 | 0.069 | 0.364 | 0.273 | CPU-bound |
| 5 | AMD Radeon iGPU (FL 12_1) | DX12 | 1.0M | 307 (-92%) | 3.138 | 1.616 | 1.507 | 3.254 | 0.116 | GPU-bound |
| 6 | AMD Radeon iGPU | DX11 | 1.0M | 245 (-94%) | 3.964 | 1.477 | 1.440 | 4.076 | 0.112 | GPU-bound |
| 7 | AMD Radeon iGPU | Vulkan | 1.0M | 221 (-94%) | 3.426 | 1.374 | 2.052 | 4.516 | 1.090 | Balanced |
| 8 | Microsoft WARP (CPU Software Renderer) | DX12 | 1.0M | 83 (-98%) | 11.446 | 1.068 | 10.377 | 11.997 | 0.551 | Software |
| 9 | Microsoft WARP (CPU Software Renderer) | DX11 | 1.0M | 51 (-99%) | 19.362 | 0.468 | 10.879 | 19.576 | 0.214 | Software |

## GPU Time Breakdown

| GPU | API | Particles | Compute (ms) | Render (ms) | Total GPU (ms) | GPU Util (%) |
|-----|-----|-----------|-------------|-------------|---------------|-------------|
| RTX 5090 | Vulkan | 1.0M | 0.025 | 0.064 | 0.090 | 0.3 |
| RTX 5090 | DX11 | 1.0M | 0.020 | 0.049 | 0.086 | 0.3 |
| RTX 5090 (FL 12_1) | DX12 | 1.0M | 0.014 | 0.050 | 0.065 | 0.2 |
| RTX 5090/PCIe/SSE2 | OpenGL | 1.0M | 0.020 | 0.069 | 0.091 | 0.2 |
| AMD Radeon iGPU (FL 12_1) | DX12 | 1.0M | 1.616 | 1.507 | 3.138 | 1.0 |
| AMD Radeon iGPU | DX11 | 1.0M | 1.477 | 1.440 | 3.964 | 1.0 |
| AMD Radeon iGPU | Vulkan | 1.0M | 1.374 | 2.052 | 3.426 | 0.8 |
| Microsoft WARP (CPU Software Renderer) | DX12 | 1.0M | 1.068 | 10.377 | 11.446 | 1.0 |
| Microsoft WARP (CPU Software Renderer) | DX11 | 1.0M | 0.468 | 10.879 | 19.362 | 1.0 |
