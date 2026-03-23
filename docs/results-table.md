# Benchmark Results Comparison

> Generated: 2026-03-23 16:23  
> Results: 27 entries

## Ranked by Average FPS

| # | GPU | API | Particles | Avg FPS | Avg GPU (ms) | Compute (ms) | Render (ms) | Frame (ms) | CPU OH (ms) | Bottleneck |
|---|-----|-----|-----------|---------|-------------|-------------|-------------|------------|-------------|------------|
| 1 | RTX 5090 | DX11 | 1.0M | 7,737 | 0.120 | 0.019 | 0.085 | 0.129 | 0.009 | GPU-bound |
| 2 | RTX 5090 | DX11 | 1.0M | 7,718 (-0%) | 0.120 | 0.019 | 0.085 | 0.130 | 0.010 | GPU-bound |
| 3 | RTX 5090 (FL 12_1) | DX12 | 1.0M | 5,646 (-27%) | 0.065 | 0.014 | 0.050 | 0.177 | 0.112 | CPU-bound |
| 4 | RTX 5090 (FL 12_1) | DX12 | 1.0M | 5,603 (-28%) | 0.065 | 0.014 | 0.050 | 0.178 | 0.113 | CPU-bound |
| 5 | RTX 5090 | Vulkan | 1.0M | 3,832 (-50%) | 0.090 | 0.025 | 0.064 | 0.261 | 0.171 | CPU-bound |
| 6 | RTX 5090 | Vulkan | 1.0M | 3,613 (-53%) | 0.088 | 0.024 | 0.064 | 0.277 | 0.189 | CPU-bound |
| 7 | RTX 5090 | DX11 | 1.0M | 3,601 (-53%) | 0.086 | 0.020 | 0.049 | 0.278 | 0.192 | CPU-bound |
| 8 | RTX 5090 | Vulkan | 1.0M | 3,526 (-54%) | 0.089 | 0.024 | 0.065 | 0.284 | 0.195 | CPU-bound |
| 9 | RTX 5090 (FL 12_1) | DX12 | 1.0M | 2,967 (-62%) | 0.065 | 0.014 | 0.050 | 0.337 | 0.272 | CPU-bound |
| 10 | RTX 5090/PCIe/SSE2 | OpenGL | 1.0M | 2,745 (-65%) | 0.091 | 0.020 | 0.069 | 0.364 | 0.273 | CPU-bound |
| 11 | RTX 5090/PCIe/SSE2 | OpenGL | 1.0M | 2,062 (-73%) | 0.088 | 0.019 | 0.068 | 0.485 | 0.397 | CPU-bound |
| 12 | RTX 5090/PCIe/SSE2 | OpenGL | 1.0M | 2,027 (-74%) | 0.088 | 0.019 | 0.068 | 0.493 | 0.405 | CPU-bound |
| 13 | AMD Radeon iGPU (FL 12_1) | DX12 | 1.0M | 313 (-96%) | 3.082 | 1.603 | 1.476 | 3.192 | 0.110 | GPU-bound |
| 14 | AMD Radeon iGPU (FL 12_1) | DX12 | 1.0M | 313 (-96%) | 3.081 | 1.604 | 1.474 | 3.193 | 0.112 | GPU-bound |
| 15 | AMD Radeon iGPU (FL 12_1) | DX12 | 1.0M | 307 (-96%) | 3.138 | 1.616 | 1.507 | 3.254 | 0.116 | GPU-bound |
| 16 | AMD Radeon iGPU | DX11 | 1.0M | 246 (-97%) | 3.950 | 1.472 | 1.434 | 4.062 | 0.112 | GPU-bound |
| 17 | AMD Radeon iGPU | DX11 | 1.0M | 246 (-97%) | 3.957 | 1.475 | 1.437 | 4.063 | 0.106 | GPU-bound |
| 18 | AMD Radeon iGPU | DX11 | 1.0M | 245 (-97%) | 3.964 | 1.477 | 1.440 | 4.076 | 0.112 | GPU-bound |
| 19 | AMD Radeon iGPU | Vulkan | 1.0M | 225 (-97%) | 3.396 | 1.366 | 2.030 | 4.450 | 1.054 | Balanced |
| 20 | AMD Radeon iGPU | Vulkan | 1.0M | 222 (-97%) | 3.432 | 1.381 | 2.051 | 4.509 | 1.077 | Balanced |
| 21 | AMD Radeon iGPU | Vulkan | 1.0M | 221 (-97%) | 3.426 | 1.374 | 2.052 | 4.516 | 1.090 | Balanced |
| 22 | Microsoft WARP (CPU Software Renderer) | DX12 | 1.0M | 87 (-99%) | 11.059 | 1.034 | 10.024 | 11.542 | 0.483 | Software |
| 23 | Microsoft WARP (CPU Software Renderer) | DX12 | 1.0M | 86 (-99%) | 11.014 | 1.027 | 9.987 | 11.565 | 0.551 | Software |
| 24 | Microsoft WARP (CPU Software Renderer) | DX12 | 1.0M | 83 (-99%) | 11.446 | 1.068 | 10.377 | 11.997 | 0.551 | Software |
| 25 | Microsoft WARP (CPU Software Renderer) | DX11 | 1.0M | 54 (-99%) | 18.449 | 0.351 | 10.519 | 18.650 | 0.201 | Software |
| 26 | Microsoft WARP (CPU Software Renderer) | DX11 | 1.0M | 53 (-99%) | 18.812 | 0.380 | 10.697 | 18.997 | 0.185 | Software |
| 27 | Microsoft WARP (CPU Software Renderer) | DX11 | 1.0M | 51 (-99%) | 19.362 | 0.468 | 10.879 | 19.576 | 0.214 | Software |

## GPU Time Breakdown

| GPU | API | Particles | Compute (ms) | Render (ms) | Total GPU (ms) | GPU Util (%) |
|-----|-----|-----------|-------------|-------------|---------------|-------------|
| RTX 5090 | DX11 | 1.0M | 0.019 | 0.085 | 0.120 | 0.9 |
| RTX 5090 | DX11 | 1.0M | 0.019 | 0.085 | 0.120 | 0.9 |
| RTX 5090 (FL 12_1) | DX12 | 1.0M | 0.014 | 0.050 | 0.065 | 0.4 |
| RTX 5090 (FL 12_1) | DX12 | 1.0M | 0.014 | 0.050 | 0.065 | 0.4 |
| RTX 5090 | Vulkan | 1.0M | 0.025 | 0.064 | 0.090 | 0.3 |
| RTX 5090 | Vulkan | 1.0M | 0.024 | 0.064 | 0.088 | 0.3 |
| RTX 5090 | DX11 | 1.0M | 0.020 | 0.049 | 0.086 | 0.3 |
| RTX 5090 | Vulkan | 1.0M | 0.024 | 0.065 | 0.089 | 0.3 |
| RTX 5090 (FL 12_1) | DX12 | 1.0M | 0.014 | 0.050 | 0.065 | 0.2 |
| RTX 5090/PCIe/SSE2 | OpenGL | 1.0M | 0.020 | 0.069 | 0.091 | 0.2 |
| RTX 5090/PCIe/SSE2 | OpenGL | 1.0M | 0.019 | 0.068 | 0.088 | 0.2 |
| RTX 5090/PCIe/SSE2 | OpenGL | 1.0M | 0.019 | 0.068 | 0.088 | 0.2 |
| AMD Radeon iGPU (FL 12_1) | DX12 | 1.0M | 1.603 | 1.476 | 3.082 | 1.0 |
| AMD Radeon iGPU (FL 12_1) | DX12 | 1.0M | 1.604 | 1.474 | 3.081 | 1.0 |
| AMD Radeon iGPU (FL 12_1) | DX12 | 1.0M | 1.616 | 1.507 | 3.138 | 1.0 |
| AMD Radeon iGPU | DX11 | 1.0M | 1.472 | 1.434 | 3.950 | 1.0 |
| AMD Radeon iGPU | DX11 | 1.0M | 1.475 | 1.437 | 3.957 | 1.0 |
| AMD Radeon iGPU | DX11 | 1.0M | 1.477 | 1.440 | 3.964 | 1.0 |
| AMD Radeon iGPU | Vulkan | 1.0M | 1.366 | 2.030 | 3.396 | 0.8 |
| AMD Radeon iGPU | Vulkan | 1.0M | 1.381 | 2.051 | 3.432 | 0.8 |
| AMD Radeon iGPU | Vulkan | 1.0M | 1.374 | 2.052 | 3.426 | 0.8 |
| Microsoft WARP (CPU Software Renderer) | DX12 | 1.0M | 1.034 | 10.024 | 11.059 | 1.0 |
| Microsoft WARP (CPU Software Renderer) | DX12 | 1.0M | 1.027 | 9.987 | 11.014 | 1.0 |
| Microsoft WARP (CPU Software Renderer) | DX12 | 1.0M | 1.068 | 10.377 | 11.446 | 1.0 |
| Microsoft WARP (CPU Software Renderer) | DX11 | 1.0M | 0.351 | 10.519 | 18.449 | 1.0 |
| Microsoft WARP (CPU Software Renderer) | DX11 | 1.0M | 0.380 | 10.697 | 18.812 | 1.0 |
| Microsoft WARP (CPU Software Renderer) | DX11 | 1.0M | 0.468 | 10.879 | 19.362 | 1.0 |
