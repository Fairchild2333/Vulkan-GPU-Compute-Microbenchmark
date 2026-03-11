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

| Metric | Vulkan | DirectX 12 | DirectX 11 | OpenGL 4.3 |
|--------|--------|------------|------------|------------|
| **Avg FPS** | 3,611 | 6,547 | 8,955 | 2,442 |
| **Avg GPU Time** | 0.094 ms | 0.065 ms | 0.104 ms | 0.087 ms |
| **Avg Frame Time** | 0.277 ms | 0.153 ms | 0.112 ms | 0.409 ms |
| **CPU Overhead / Frame** | 0.183 ms | 0.088 ms | 0.008 ms | 0.322 ms |
| **GPU Utilisation** | 33.9% | 42.4% | 93.2% | 21.2% |
| **Bottleneck** | CPU-bound | CPU-bound | GPU-bound | CPU-bound |

### Key Finding: DX11 Achieves Highest FPS for Simple Workloads

All four APIs deliver nearly identical GPU execution times (0.065–0.104 ms), confirming the GPU-side workload is equivalent. The FPS difference is entirely driven by **per-frame CPU overhead**.

**Ranking: DX11 > DX12 > Vulkan > OpenGL**

**Why DX11 is fastest here:**

- DX11 is an implicit API — the driver handles command batching, resource state tracking, and barrier insertion internally. NVIDIA's DX11 driver path has been optimised for over a decade, making it extremely efficient for simple, single-threaded workloads.
- Per-frame CPU overhead is only **0.008 ms**, leaving the GPU as the actual bottleneck (93.2% utilisation).

**Why DX12/Vulkan are slower here:**

- Both are explicit APIs requiring the application to manually manage command allocators, fences, resource barriers, and descriptor heaps/sets.
- This shifts work from the driver to application code, adding **10–20× more CPU overhead per frame** compared to DX11.
- With only 1 compute dispatch + 1 draw call per frame, there is no opportunity for multi-threaded command recording — the very feature that justifies explicit APIs in complex scenes.

**Why OpenGL is slowest here:**

- OpenGL has the highest per-frame CPU overhead at **0.322 ms** — roughly 40× more than DX11.
- `glfwSwapBuffers` on Windows goes through WGL, which has less efficient frame queue management than DXGI's `Present` path.
- OpenGL's global state machine model means every `glUseProgram`, `glBindBuffer`, and `glBindVertexArray` call triggers internal driver state validation, accumulating significant overhead even with minimal draw calls.
- Despite being an implicit API like DX11, OpenGL's Windows driver path has received far less optimisation from NVIDIA in recent years, as industry focus has shifted to Vulkan and DirectX.
- However, OpenGL still produces the **second-fastest GPU execution time** (0.087 ms), confirming the bottleneck is purely in the CPU-side driver, not in the shader or buffer management.

**When DX12/Vulkan win:**

Explicit APIs excel when a scene contains hundreds or thousands of draw calls. In that scenario, DX11's single-threaded driver becomes the bottleneck, while DX12/Vulkan can parallelise command recording across multiple CPU threads, reducing total CPU time proportionally.

**When OpenGL makes sense:**

OpenGL 4.3 remains the most portable option — it runs on Windows, Linux, and macOS (legacy profile) without requiring Vulkan drivers or platform-specific APIs. For workloads that are GPU-bound (high particle counts, complex shaders), OpenGL's higher CPU overhead becomes negligible relative to total frame time.

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

**WARP** (Windows Advanced Rasterisation Platform) is Microsoft's CPU-based software rasteriser bundled with every modern Windows installation. It runs the entire graphics pipeline on the CPU using SIMD (SSE/AVX) and multi-threading, serving as both a correctness reference and a fallback when no hardware GPU driver is available.

**Native API support:** WARP natively implements **Direct3D 11** and **Direct3D 12** only. It does **not** implement Vulkan or OpenGL. If Vulkan is reported as available on a WARP-only system, this is provided by **Mesa Dozen** — a Vulkan-on-D3D12 translation layer distributed via the Microsoft Store's *OpenCL, OpenGL & Vulkan Compatibility Pack*. Similarly, OpenGL support on WARP comes from **OpenGLOn12** in the same compatibility pack. Both layers translate their respective API calls to D3D12, which WARP then executes on the CPU.

### 4a. Hardware GPU vs WARP

| Metric | RTX 5090 / DX12 (Hardware) | WARP / DX12 (Software) |
|--------|--------------------|-----------------------------|
| **Avg FPS** | 6,547 | 83 |
| **Compute** | 0.014 ms | 1.1 ms |
| **Render** | 0.050 ms | 10.6 ms |
| **Total** | 0.065 ms | 11.7 ms |

WARP demonstrates a ~80× performance gap compared to hardware GPU execution, which is expected for CPU-based software rasterisation.

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

On systems without a native Vulkan ICD (e.g. Windows on ARM VMs, virtual GPUs), the Vulkan loader may enumerate **"Microsoft Basic Render Driver"** as a Vulkan physical device. The full call chain is:

```
Vulkan application  →  Dozen (Vulkan → D3D12)  →  WARP (D3D12 → CPU)
```

Dozen is distributed as part of the **OpenCL, OpenGL & Vulkan Compatibility Pack** from the Microsoft Store (`D3DMappingLayers` app package). Windows 11 may install this pack automatically on devices that lack native Vulkan/OpenGL drivers, particularly Windows on ARM devices and virtual machines.

On systems with a hardware Vulkan ICD (e.g. NVIDIA, AMD), the Dozen/WARP device is typically not enumerated or is deprioritised by the Vulkan loader. Selecting Vulkan on a WARP-only system is **functionally identical to selecting DX12 on WARP** — both end up as CPU-based software rendering, with Dozen adding a thin additional translation layer.

---

## 5. OpenGL GPU Selection — Platform Limitations

Unlike Vulkan, DirectX 11, and DirectX 12, **OpenGL has no standard API for enumerating or selecting a specific GPU** on a multi-GPU system. Each of the other backends provides an adapter/device enumeration mechanism:

| API | GPU Enumeration | Per-GPU Selection |
|-----|----------------|-------------------|
| Vulkan | `vkEnumeratePhysicalDevices` | Create device on any enumerated physical device |
| DirectX 12 | `IDXGIFactory::EnumAdapters` | Pass chosen adapter to `D3D12CreateDevice` |
| DirectX 11 | `IDXGIFactory::EnumAdapters` | Pass chosen adapter to `D3D11CreateDevice` |
| **OpenGL** | **None** | **OS/driver decides** |

### Windows

On Windows, the OpenGL context is created by the OS display driver model (WDDM), which assigns the GPU based on system-level configuration. The application has no standard API to override this at runtime.

**Available workarounds (limited):**

| Method | Scope | Limitation |
|--------|-------|------------|
| `NvOptimusEnablement` export symbol | Forces discrete NVIDIA GPU on Optimus laptops | Only works on NVIDIA + Intel hybrid laptops; no effect on desktop multi-GPU |
| `AmdPowerXpressRequestHighPerformance` export symbol | Forces discrete AMD GPU on switchable graphics laptops | Same — laptop-only, binary choice (discrete vs integrated) |
| `WGL_NV_gpu_affinity` extension | Per-GPU context creation | **Quadro professional cards only** — not available on GeForce/consumer GPUs |
| Windows Graphics Settings panel | Per-executable GPU assignment | Requires manual user configuration outside the application |

On the test system (RTX 5090 + AMD Radeon iGPU desktop), **none of the programmatic methods are effective** — the only way to force OpenGL onto the integrated GPU is through the Windows Graphics Settings panel.

### Linux

Linux provides significantly better OpenGL GPU selection:

| Method | Scope | How |
|--------|-------|-----|
| `DRI_PRIME=N` environment variable | Per-process GPU selection (Mesa drivers) | `DRI_PRIME=1 ./gpu_benchmark` |
| `__NV_PRIME_RENDER_OFFLOAD=1` | Per-process offload to NVIDIA GPU | `__NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia ./gpu_benchmark` |
| `EGL_EXT_platform_device` | Programmatic per-GPU EGLDisplay creation | Requires EGL instead of GLX; Mesa 23.3+ |
| `EGL_EXT_explicit_device` | Same, with native windowing support | Mesa 23.3+ |

The application detects Linux at runtime and uses `DRI_PRIME` to route OpenGL to the user's requested GPU index.

### Impact on Benchmarking

This limitation means OpenGL cross-GPU comparisons on Windows require manual configuration, whereas all other backends support interactive GPU selection within the application. On Linux, `DRI_PRIME` provides equivalent functionality to other backends' built-in GPU selection.

---

## 6. DX11 Timestamp Query Failures — Three Distinct Causes

DX11 is the only API in the benchmark where GPU timestamp queries can silently fail to produce results. Vulkan and DX12 always return timestamp values regardless of GPU clock state. DX11, by contrast, uses a `D3D11_QUERY_TIMESTAMP_DISJOINT` wrapper that can actively refuse to return data.

Three distinct failure modes were observed during testing:

### 6a. Driver Never Resolves Queries (`GetData` → `S_FALSE` indefinitely)

**Affected:** Windows on ARM virtual machines (SVGA virtual GPU driver).

`ID3D11Device::CreateQuery` succeeds for both `D3D11_QUERY_TIMESTAMP` and `D3D11_QUERY_TIMESTAMP_DISJOINT`, and the application reports timestamps as "enabled". However, `ID3D11DeviceContext::GetData` for the disjoint query perpetually returns `S_FALSE` — the result is never ready.

This is a **driver limitation**: the virtual GPU driver accepts query creation but does not implement the hardware counters needed to resolve them. No application-level workaround exists.

See [`docs/woa-dx11-timestamp-issue.md`](woa-dx11-timestamp-issue.md) for a detailed write-up.

### 6b. GPU Clock Frequency Instability (`Disjoint = TRUE`)

**Affected:** Integrated GPUs under fluctuating load, discrete GPUs during power-state transitions.

The `D3D11_QUERY_DATA_TIMESTAMP_DISJOINT` structure contains a `Disjoint` boolean. When `TRUE`, it signals that the GPU's clock frequency changed during the frame (P-state transition, thermal throttling, power-saving downclock), making the timestamp-to-millisecond conversion unreliable.

The D3D11 specification recommends discarding the entire frame's timing data when `Disjoint = TRUE`. If the GPU is frequently switching power states — common on integrated GPUs under variable load, or during the first few seconds of a benchmark run while the GPU ramps up — this can result in many consecutive frames with no timing data.

**This is not a driver bug.** It is a deliberate DX11 design choice to prioritise timestamp accuracy over availability.

**Key difference from Vulkan/DX12:** Neither Vulkan nor DX12 has a `Disjoint` concept. Their timestamp queries always return values based on a fixed `timestampPeriod` / `Frequency`, even if the GPU clock changes mid-frame. The precision may degrade slightly, but data is never withheld entirely. This is why Vulkan and DX12 report timestamps reliably in scenarios where DX11 reports none.

**Mitigation implemented:** The application now caches the last known stable frequency (`lastGoodFrequency`). When `Disjoint = TRUE`, timestamps are still read and converted using the cached frequency rather than being discarded. This mirrors the behaviour of Vulkan/DX12 — accepting marginally less precise data in exchange for continuous availability.

### 6c. Query Pipeline Too Shallow (Ring Buffer Depth)

**Affected:** Slow GPUs (integrated, software renderer) under high particle counts.

If the GPU takes significantly longer than one frame to process submitted work, the application may attempt to read a query result before the GPU has finished writing it. `GetData` returns `S_FALSE` because the query genuinely hasn't resolved yet — not because the driver doesn't support it.

**Fix:** The ring buffer was increased from 4 to 8 slots, and `GetData` retries were increased to 128 with periodic `Sleep(1)` yields, giving slow GPUs more time to resolve queries.

### Summary Table

| Scenario | Root Cause | Driver Bug? | Fix |
|----------|-----------|-------------|-----|
| WoA virtual GPU — never returns data | Driver doesn't implement timestamp counters | Yes | None (graceful fallback to CPU-only timing) |
| iGPU / dGPU ramp-up — intermittent gaps | `Disjoint = TRUE` during clock transitions | No (spec behaviour) | Use cached frequency instead of discarding |
| Slow GPU — first N frames missing | Query not resolved before read | No (pipeline depth) | Deeper ring buffer (8 slots) + retry with Sleep |
| WARP DX11 — works after warm-up | Combination of 6b and 6c | No | Same mitigations as above |

### Cross-API Timestamp Mechanism Comparison

Each backend uses its own API's timestamp mechanism — there is no cross-API data sharing. The same GPU executing the same workload produces nearly identical execution times (0.065–0.104 ms on RTX 5090), but the **measurement infrastructure** differs significantly:

| | Vulkan | DX12 | DX11 | OpenGL |
|--|--------|------|------|--------|
| **Write** | `vkCmdWriteTimestamp` | `EndQuery` → `ID3D12QueryHeap` | `context->End(query)` | `glQueryCounter(GL_TIMESTAMP)` |
| **Read** | `vkGetQueryPoolResults` with `WAIT_BIT` | `ResolveQueryData` → readback buffer | `GetData` (CPU polling) | `glGetQueryObjectui64v` |
| **Synchronisation** | GPU-side wait (guaranteed ready) | GPU-side resolve (ordered in command list) | CPU polls until `S_OK` (may never arrive) | CPU polls (typically resolves quickly) |
| **Disjoint / clock check** | None | None | Required (`D3D11_QUERY_TIMESTAMP_DISJOINT`) | None |
| **Counter frequency** | Fixed (`timestampPeriod`), independent of core clock | Fixed (`GetTimestampFrequency`), independent of core clock | May vary with GPU core clock | Fixed, monotonic counter |
| **Clock-change handling** | Returns data; timer may reset across submissions† | Returns data; stable-clock design, no resets | Refuses data if `Disjoint = TRUE` | Returns data, counter is monotonic |
| **First-frame data** | Yes | Yes | No (ring buffer warm-up required) | Yes (after 1–2 frame delay) |

**† Vulkan caveat:** The Vulkan spec notes that power management events (e.g. GPU idle → active transitions) *can reset the timestamp counter* on some implementations. This affects **cross-submission comparisons** only — timestamps within the same command buffer are always reliably comparable. The `VK_EXT_calibrated_timestamps` extension provides monotonic timestamps immune to power events, but is not required for within-frame profiling. In this benchmark, all four timestamps (compute begin/end, render begin/end) are recorded within a single command buffer, so power-state resets do not affect the results.

**DX12's stable-clock design:** DX12 goes further than Vulkan by explicitly stabilising the GPU clock for timestamp purposes. Two timestamps within the same command list are always comparable, and two timestamps from *different* command lists are also reliable as long as the GPU did not idle between them. There is no Disjoint equivalent — the API guarantees clock stability by design.

DX11 is the only API that can actively **withhold** timestamp data based on GPU clock stability. DX12 and Vulkan both use a fixed counter frequency independent of the GPU core clock, so frequency scaling and P-state transitions do not invalidate their results. This makes DX11 the most fragile timestamp implementation from an application developer's perspective, despite the underlying GPU hardware being identical across all backends.

---

## Summary

| Observation | Explanation |
|-------------|-------------|
| DX11 > DX12 > Vulkan > OpenGL in FPS (hardware GPU, simple workload) | Per-frame CPU overhead: DX11 (0.008 ms) < DX12 (0.088 ms) < Vulkan (0.183 ms) < OpenGL (0.322 ms) |
| All four APIs have similar GPU execution times (0.065–0.104 ms) | The GPU-side workload is identical; only CPU-side driver overhead differs |
| OpenGL has fastest GPU time but lowest FPS | WGL swap path and state-machine validation overhead dominate CPU time |
| DX12 > DX11 in FPS (WARP software renderer) | DX11's implicit driver layer is pure overhead when there is no GPU hardware to optimise for |
| DX12 has lowest GPU time on hardware | Slightly more efficient GPU command scheduling, but CPU overhead negates the advantage at low complexity |
| dGPU ~37× faster than iGPU | Bandwidth-bound workload; ratio matches memory bandwidth difference |
| Device-local 35× faster than host-visible on dGPU | PCIe round-trips dominate compute shader memory access patterns |
| Host-visible = Device-local on iGPU | Unified memory architecture — no PCIe hop |
| WARP ~80× slower than hardware GPU | Expected for CPU-based software rasterisation |
| WARP can appear as a Vulkan device via Dozen | Mesa Dozen (Vulkan→D3D12) wraps WARP; only visible when no hardware Vulkan ICD is present |
| OpenGL cannot select GPU on Windows | No standard API; OS assigns GPU. Linux provides `DRI_PRIME` as a workaround |
| DX11 timestamps fail where Vulkan/DX12 succeed | DX11's `Disjoint` flag discards data on GPU clock changes; Vulkan/DX12 have no such mechanism |

These results demonstrate that **API overhead, memory placement, and hardware architecture** all significantly affect GPU compute performance — and that the optimal configuration depends on workload complexity and hardware topology.
