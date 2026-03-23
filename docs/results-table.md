# Benchmark Results Comparison

> Generated: 2026-03-24 01:14  
> Results: 6 entries

## Ranked by Average FPS

| # | GPU | API | Particles | Avg FPS | Avg GPU (ms) | Compute (ms) | Render (ms) | Frame (ms) | CPU OH (ms) | Bottleneck |
|---|-----|-----|-----------|---------|-------------|-------------|-------------|------------|-------------|------------|
| 1 | AMD Radeon iGPU (FL 12_1) | DX12 | 1.0M | 308 | 2.968 | 1.384 | 1.571 | 3.246 | 0.278 | GPU-bound |
| 2 | AMD Radeon iGPU | OpenGL | 1.0M | 274 (-11%) | 3.385 | 1.395 | 1.924 | 3.646 | 0.261 | GPU-bound |
| 3 | AMD Radeon iGPU | Vulkan | 1.0M | 274 (-11%) | 3.428 | 1.367 | 2.061 | 3.646 | 0.218 | GPU-bound |
| 4 | AMD Radeon iGPU | DX11 | 1.0M | 192 (-38%) | 5.027 | 1.189 | 2.985 | 5.195 | 0.168 | GPU-bound |
| 5 | Microsoft WARP (CPU Software Renderer) | DX12 | 1.0M | 65 (-79%) | 15.140 | 1.807 | 13.331 | 15.425 | 0.285 | Software |
| 6 | Microsoft WARP (CPU Software Renderer) | DX11 | 1.0M | 45 (-85%) | 22.118 | 0.906 | 14.266 | 22.032 | 0.000 | Software |

## GPU Time Breakdown

| GPU | API | Particles | Compute (ms) | Render (ms) | Total GPU (ms) | GPU Util (%) |
|-----|-----|-----------|-------------|-------------|---------------|-------------|
| AMD Radeon iGPU (FL 12_1) | DX12 | 1.0M | 1.384 | 1.571 | 2.968 | 0.9 |
| AMD Radeon iGPU | OpenGL | 1.0M | 1.395 | 1.924 | 3.385 | 0.9 |
| AMD Radeon iGPU | Vulkan | 1.0M | 1.367 | 2.061 | 3.428 | 0.9 |
| AMD Radeon iGPU | DX11 | 1.0M | 1.189 | 2.985 | 5.027 | 1.0 |
| Microsoft WARP (CPU Software Renderer) | DX12 | 1.0M | 1.807 | 13.331 | 15.140 | 1.0 |
| Microsoft WARP (CPU Software Renderer) | DX11 | 1.0M | 0.906 | 14.266 | 22.118 | 1.0 |
