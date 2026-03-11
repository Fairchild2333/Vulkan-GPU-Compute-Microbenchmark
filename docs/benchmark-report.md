# Multi-Backend GPU Benchmark Report

**System Configuration**

| Component | Specification |
|-----------|--------------|
| CPU | AMD Ryzen 7 9800X3D 8-Core Processor |
| Discrete GPU | NVIDIA GeForce RTX 5090 (32 GB GDDR7) |
| Integrated GPU | AMD Radeon Graphics (Zen 4, 2 CU, 2 GB shared DDR5) |
| OS | Windows 11 25H2 |
| Resolution | 1280 × 720 |
| V-Sync | OFF |
| Memory Mode | Device-local (staging buffer → VRAM on dGPU) |

---

## 1. Cross-API Comparison — RTX 5090, 1M Particles (Medium)

| Metric | Vulkan | DirectX 12 | DirectX 11 |
|--------|--------|------------|------------|
| **Avg FPS** | 3,611 | 6,547 | 9,077 |
| **Avg GPU Time** | 0.094 ms | 0.065 ms | 0.092 ms |
| **Avg Frame Time** | 0.277 ms | 0.153 ms | 0.110 ms |
| **CPU Overhead / Frame** | 0.183 ms | 0.088 ms | 0.018 ms |
| **GPU Utilisation** | 33.9% | 42.4% | 83.2% |
| **Bottleneck** | CPU-bound | CPU-bound | GPU-bound |

### Key Finding: DX11 Achieves Highest FPS for Simple Workloads

All three APIs deliver nearly identical GPU execution times (0.065–0.094 ms), confirming the GPU-side workload is equivalent. The FPS difference is entirely driven by **per-frame CPU overhead**.

**Why DX11 is fastest here:**

- DX11 is an implicit API — the driver handles command batching, resource state tracking, and barrier insertion internally. NVIDIA's DX11 driver path has been optimised for over a decade, making it extremely efficient for simple, single-threaded workloads.
- Per-frame CPU overhead is only **0.018 ms**, leaving the GPU as the actual bottleneck (83.2% utilisation).

**Why DX12/Vulkan are slower here:**

- Both are explicit APIs requiring the application to manually manage command allocators, fences, resource barriers, and descriptor heaps/sets.
- This shifts work from the driver to application code, adding **5–10× more CPU overhead per frame** compared to DX11.
- With only 1 compute dispatch + 1 draw call per frame, there is no opportunity for multi-threaded command recording — the very feature that justifies explicit APIs in complex scenes.

**When DX12/Vulkan win:**

Explicit APIs excel when a scene contains hundreds or thousands of draw calls. In that scenario, DX11's single-threaded driver becomes the bottleneck, while DX12/Vulkan can parallelise command recording across multiple CPU threads, reducing total CPU time proportionally.

---

## 2. Cross-GPU Comparison — Vulkan, 1M Particles (Medium), Device-local

| Metric | RTX 5090 (Discrete) | AMD Radeon iGPU (Integrated) |
|--------|--------------------|-----------------------------|
| **Avg FPS** | 2,700+ | ~320 |
| **Compute** | 0.035 ms | 1.47 ms |
| **Render** | 0.045 ms | 1.5 ms |
| **Total GPU** | 0.08 ms | ~3.0 ms |
| **Ratio** | 1× | ~37× slower |

The RTX 5090 (21,760 CUDA cores, ~3,000 GB/s bandwidth) outperforms the Zen 4 iGPU (128 shaders, ~50 GB/s shared DDR5) by approximately **37×** in GPU execution time. This aligns with the memory bandwidth ratio (~60×), confirming the particle simulation is **bandwidth-bound** rather than compute-bound at this scale.

---

## 3. Memory Allocation Impact — Vulkan, RTX 5090

| Memory Mode | Compute | Render | Total GPU | FPS |
|-------------|---------|--------|-----------|-----|
| **Device-local** (default) | 0.035 ms | 0.045 ms | 0.08 ms | 2,700+ |
| **Host-visible** (`--host-memory`) | 1.25 ms | 0.15 ms | 1.4 ms | ~600 |

Using host-visible memory (system RAM accessed over PCIe) instead of device-local VRAM causes a **35× increase in compute time** on a discrete GPU. The compute shader reads/writes particle data every frame — over PCIe, this becomes the dominant bottleneck.

On an integrated GPU, this penalty disappears because host-visible and device-local memory both reside in the same physical DDR5, making the distinction meaningless.

---

## 4. Software Renderer Baseline — WARP, 1M Particles

### 4a. Hardware GPU vs WARP

| Metric | RTX 5090 / DX12 (Hardware) | WARP / DX12 (Software) |
|--------|--------------------|-----------------------------|
| **Avg FPS** | 6,547 | 83 |
| **Compute** | 0.014 ms | 1.1 ms |
| **Render** | 0.050 ms | 10.6 ms |
| **Total** | 0.065 ms | 11.7 ms |

Microsoft WARP (Windows Advanced Rasterisation Platform) runs the entire graphics pipeline on the CPU. It serves as a correctness baseline and demonstrates the ~80× performance gap between hardware GPU execution and CPU-based software rasterisation.

### 4b. WARP: DX11 vs DX12

| Metric | WARP + DX11 | WARP + DX12 |
|--------|-------------|-------------|
| **Avg FPS** | 52 | 83 |
| **Timestamp queries** | Not available | 11.7 ms total |

On hardware GPUs, DX11 outperforms DX12/Vulkan because the driver's implicit state management is highly optimised and adds negligible overhead. **On WARP, the result reverses: DX12 is 60% faster than DX11.**

**Why the reversal:**

- **DX11's driver layer becomes pure overhead.** On a hardware GPU, the DX11 runtime performs implicit resource state tracking, dependency analysis, and barrier insertion to optimise GPU command submission. When the "GPU" is WARP (a CPU-based software renderer), there is no hardware to optimise for — this entire layer is wasted CPU work.
- **DX12's thin runtime is a better fit.** DX12's explicit model has minimal runtime between the application and the execution engine. The application specifies exactly what to do, and WARP executes it directly with less translation overhead.
- **WARP's DX12 implementation is more modern.** DX12 (introduced 2015) benefits from a newer WARP backend that may leverage more efficient internal scheduling compared to the legacy DX11 WARP path.

This observation reinforces that the DX11 driver's "free optimisation" is specifically valuable for **hardware GPU command submission** — when that hardware is absent, the optimisation layer becomes a liability.

### 4c. WARP as a Vulkan Device — Dozen Translation Layer

On some systems (notably Windows on ARM devices where the hardware GPU lacks a native Vulkan ICD), the Vulkan physical device enumeration reports **"Microsoft Basic Render Driver"** with Vulkan support. This is not WARP natively implementing Vulkan — it is **Mesa Dozen**, a Vulkan-on-D3D12 translation layer shipped with Windows 11, wrapping WARP's D3D12 interface as a Vulkan device:

```
Vulkan application  →  Dozen (Vulkan → D3D12)  →  WARP (D3D12, CPU)
```

On systems with a hardware Vulkan ICD (e.g. NVIDIA, AMD), the Dozen/WARP device is typically not enumerated or is deprioritised by the Vulkan loader. This explains why WARP appears as a Vulkan-capable device only on certain configurations.

The practical consequence: selecting Vulkan on such a system is **functionally identical to selecting DX12 on WARP** — both end up as CPU-based software rendering with an additional translation overhead from Dozen.

---

## Summary

| Observation | Explanation |
|-------------|-------------|
| DX11 > DX12 > Vulkan in FPS (hardware GPU, simple workload) | Implicit API has lower per-frame CPU overhead; explicit APIs add manual management cost |
| DX12 > DX11 in FPS (WARP software renderer) | DX11's implicit driver layer is pure overhead when there is no GPU hardware to optimise for |
| DX12 has lowest GPU time on hardware | Slightly more efficient GPU command scheduling, but CPU overhead negates the advantage at low complexity |
| dGPU ~37× faster than iGPU | Bandwidth-bound workload; ratio matches memory bandwidth difference |
| Device-local 35× faster than host-visible on dGPU | PCIe round-trips dominate compute shader memory access patterns |
| Host-visible = Device-local on iGPU | Unified memory architecture — no PCIe hop |
| WARP ~80× slower than hardware GPU | Expected for CPU-based software rasterisation |
| WARP can appear as a Vulkan device via Dozen | Mesa Dozen (Vulkan→D3D12) wraps WARP; only visible when no hardware Vulkan ICD is present |

These results demonstrate that **API overhead, memory placement, and hardware architecture** all significantly affect GPU compute performance — and that the optimal configuration depends on workload complexity and hardware topology.
