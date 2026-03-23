# Multi-Backend GPU Benchmark Report

---

## 1. Project Overview

### What This Project Does

A cross-platform GPU compute and rendering microbenchmark written in **C++17**.
It simulates millions of particles on the GPU using a **compute shader**, then
renders them as point-sprites using a **graphics pipeline** — all within a
single command buffer per frame.

The project implements **five interchangeable graphics API backends**:

| Backend | API | Shader Language | Platforms |
|---------|-----|----------------|-----------|
| Vulkan 1.2 | Explicit, low-level | GLSL → SPIR-V | Windows, Linux, macOS (MoltenVK) |
| DirectX 12 | Explicit, low-level | HLSL (SM 5.1) | Windows 10+ |
| DirectX 11 | Implicit, driver-managed | HLSL (SM 5.0) | Windows 7+ |
| OpenGL 4.3 | Implicit, driver-managed | GLSL 430 | Windows, Linux |
| Metal | Explicit (Apple) | MSL | macOS |

All five backends share the same `AppBase` class for windowing (GLFW), particle
initialisation, timing, and benchmark result management. Each backend overrides
`InitBackend()`, `DrawFrame()`, `CleanupBackend()`, and `WaitIdle()`.

### Key Features

- **Multi-GPU support**: enumerate and select from all available GPUs
  (discrete → integrated → software), with `--gpu` CLI override.
- **GPU timestamp profiling**: per-frame compute / render / total GPU timing
  via `VkQueryPool` (Vulkan), `ID3D12QueryHeap` (DX12),
  `ID3D11Query` (DX11), `glQueryCounter` (OpenGL).
- **Benchmark mode**: fixed frame count or timed run, warmup period, min/max/avg
  statistics, CPU-bound vs GPU-bound analysis.
- **Result persistence**: auto-save to `~/.gpu_bench/results.json`, compare,
  delete, CSV export.
- **RenderDoc integration**: `VK_EXT_debug_utils` labels + In-Application API
  for programmatic frame capture (`--capture <frame>`).
- **Python tooling**: chart generation, batch benchmark automation, markdown/HTML
  report export, 3DMark cross-validation.

---

## 2. Pipeline Architecture

### Per-Frame Execution Model

Every frame executes the following sequence in a single command buffer
(shown for the Vulkan backend; other backends follow the same logical
structure):

```
┌──────────────────────────────────────────────────────┐
│                  Command Buffer                       │
│                                                       │
│  ┌─ Timestamp T0 (TOP_OF_PIPE) ───────────────────┐  │
│  │                                                 │  │
│  │  ┌─ Particle Compute ────────────────────────┐  │  │
│  │  │  Bind compute pipeline                    │  │  │
│  │  │  Bind descriptor set (SSBO)               │  │  │
│  │  │  Push constants (deltaTime, damping)      │  │  │
│  │  │  vkCmdDispatch(N/256, 1, 1)               │  │  │
│  │  └───────────────────────────────────────────┘  │  │
│  │                                                 │  │
│  ├─ Timestamp T1 (COMPUTE_SHADER) ────────────────┤  │
│  │                                                 │  │
│  │  ┌─ SSBO Barrier ───────────────────────────┐  │  │
│  │  │  srcStage: COMPUTE_SHADER_BIT            │  │  │
│  │  │  dstStage: VERTEX_INPUT_BIT              │  │  │
│  │  │  SHADER_WRITE → VERTEX_ATTRIBUTE_READ    │  │  │
│  │  └──────────────────────────────────────────┘  │  │
│  │                                                 │  │
│  ├─ Timestamp T2 (TOP_OF_PIPE) ───────────────────┤  │
│  │                                                 │  │
│  │  ┌─ Particle Render ────────────────────────┐  │  │
│  │  │  Begin render pass (clear to dark blue)  │  │  │
│  │  │  Bind graphics pipeline                  │  │  │
│  │  │  Bind vertex buffer (same SSBO)          │  │  │
│  │  │  vkCmdDraw(particleCount, 1, 0, 0)       │  │  │
│  │  │  End render pass                         │  │  │
│  │  └──────────────────────────────────────────┘  │  │
│  │                                                 │  │
│  └─ Timestamp T3 (COLOR_ATTACHMENT_OUTPUT) ────────┘  │
│                                                       │
└──────────────────────────────────────────────────────┘
```

### Compute Shader

The compute shader (`shaders/compute.comp`) performs Euler integration on each
particle:

```glsl
layout(local_size_x = 256) in;

layout(set = 0, binding = 0, std430) buffer ParticleBuffer {
    Particle particles[];   // vec4 position + vec4 velocity = 32 bytes
};

layout(push_constant) uniform ComputeParams {
    float deltaTime;
    float bounds;
};

void main() {
    uint i = gl_GlobalInvocationID.x;
    particles[i].position.xyz += particles[i].velocity.xyz * deltaTime;
    if (particles[i].position.x > bounds)
        particles[i].position.x = -bounds;
}
```

- **Workgroup size**: 256 threads — balances occupancy across AMD (wavefront
  64) and NVIDIA (warp 32) architectures.
- **SSBO layout**: `std430` guarantees C++-compatible packing (no padding).
- **Push constants**: avoid descriptor set updates every frame; only 8 bytes
  pushed per dispatch.

For 16M particles: `16,777,216 / 256 = 65,536` workgroups dispatched.

### Graphics Pipeline

The render pipeline draws all particles as `POINT_LIST` using the same SSBO
as a vertex buffer:

| Stage | Configuration |
|-------|--------------|
| **Vertex Input** | Binding 0, stride 32 bytes: `position` (R32G32_SFLOAT, offset 0), `colour` (R32G32B32A32_SFLOAT, offset 16) |
| **Vertex Shader** | Maps particle speed to a blue → red colour gradient, sets `gl_PointSize = 2.0` |
| **Rasteriser** | Point topology, no culling |
| **Fragment Shader** | Passes interpolated colour to output |
| **Colour Blend** | Additive blending (`SRC_ALPHA + ONE`) — overlapping particles create bright clusters |
| **Render Pass** | Single subpass, one colour attachment (swapchain image, B8G8R8A8_SRGB) |

### Memory Architecture

| Mode | Vulkan Flags | When Used |
|------|-------------|-----------|
| **Device-local** (default) | `DEVICE_LOCAL` + staging buffer copy | Discrete GPU — particle data lives in VRAM |
| **Host-visible** (`--host-memory`) | `HOST_VISIBLE \| HOST_COHERENT` | Integrated GPU or debugging — CPU-mappable, no staging copy |

On discrete GPUs, the staging buffer is created, filled with initial particle
data, copied via `vkCmdCopyBuffer`, then destroyed. All subsequent compute
and render operations access only the device-local buffer.

### Synchronisation

The single `VkBufferMemoryBarrier` between compute and render ensures:

- All compute shader writes to particle positions are visible before the
  vertex shader reads them.
- No additional barriers are needed because the entire frame is recorded
  into one command buffer on one queue.
- On integrated GPUs with unified memory, the barrier is essentially a
  no-op (no cache flush between separate memory domains).

### Timestamp Query Pipeline

Each backend implements GPU timestamp queries using a **ring buffer** of
`kTimestampSlotCount` (8) frame slots to avoid blocking on in-flight frames:

| API | Write | Read | Sync | Clock Frequency |
|-----|-------|------|------|----------------|
| Vulkan | `vkCmdWriteTimestamp` | `vkGetQueryPoolResults` | Fence wait from previous frame | `timestampPeriod` from device properties |
| DX12 | `ID3D12GraphicsCommandList::EndQuery` | `ResolveQueryData` + readback buffer | Fence signal/wait | `ID3D12CommandQueue::GetTimestampFrequency` |
| DX11 | `ID3D11DeviceContext::End(query)` | `GetData` with retry loop | Disjoint query (`S_FALSE` → retry) | `D3D11_QUERY_DATA_TIMESTAMP_DISJOINT.Frequency` |
| OpenGL | `glQueryCounter(GL_TIMESTAMP)` | `glGetQueryObjectui64v` | `GL_QUERY_RESULT_AVAILABLE` poll | Fixed 1 ns resolution |

Four timestamps per frame yield three intervals: **compute time** (T1−T0),
**render time** (T3−T2), and **total GPU time** (T3−T0).

---

## 3. RenderDoc Frame-Capture Analysis

> Industry-standard GPU profiling of the Vulkan particle compute + render pipeline
> using [RenderDoc](https://renderdoc.org/) — the most widely used cross-vendor,
> cross-API GPU frame debugger.

### What is RenderDoc?

RenderDoc is a free, open-source (MIT licence) GPU frame debugger created by
Baldur Karlsson. It intercepts a single frame's worth of graphics API calls,
allowing post-mortem inspection of every resource, pipeline state, and GPU
operation. It is one of the tools referenced by AMD's JD under *"Use of
industry-standard profiling and debug tools"*.

| Tool | Vendor | Platform |
|------|--------|----------|
| **RenderDoc** | Open-source | Vulkan, DX11, DX12, OpenGL — all GPUs |
| PIX | Microsoft | DX12 (Windows only) |
| Radeon GPU Profiler (RGP) | AMD | Vulkan, DX12 (AMD GPUs only) |
| Nsight Graphics | NVIDIA | Vulkan, DX, OpenGL (NVIDIA GPUs only) |
| Xcode GPU Debugger | Apple | Metal (macOS / iOS only) |

### Integration in This Project

The Vulkan backend integrates two layers of RenderDoc support:

1. **`VK_EXT_debug_utils`** — debug labels and object names baked into the
   command buffer, providing readable annotations inside RenderDoc's event
   browser.
2. **RenderDoc In-Application API** (`renderdoc_app.h`) — runtime detection of
   RenderDoc, enabling **F12** manual capture and `--capture <frame>` CLI
   auto-capture from within the application.

### Per-Frame Command Buffer Structure

A single frame records the following sequence into one `VkCommandBuffer`:

| # | Event | Debug Label | Description |
|---|-------|-------------|-------------|
| 1 | `vkCmdResetQueryPool` | — | Reset timestamp query slots for this frame |
| 2 | `vkCmdWriteTimestamp` (TOP_OF_PIPE) | — | T0: frame start |
| 3 | `vkCmdBindPipeline` (COMPUTE) | **Particle Compute** (green) | Bind compute pipeline |
| 4 | `vkCmdBindDescriptorSets` | | Bind SSBO descriptor (set 0, binding 0) |
| 5 | `vkCmdPushConstants` | | Push `deltaTime` (float) + `damping` (float) |
| 6 | `vkCmdDispatch(N/256, 1, 1)` | | Dispatch compute workgroups |
| 7 | `vkCmdWriteTimestamp` (COMPUTE_SHADER) | — | T1: compute end |
| 8 | `vkCmdPipelineBarrier` | **SSBO Barrier** (yellow) | `SHADER_WRITE → VERTEX_ATTRIBUTE_READ` |
| 9 | `vkCmdWriteTimestamp` (TOP_OF_PIPE) | — | T2: render start |
| 10 | `vkCmdBeginRenderPass` | **Particle Render** (blue) | Clear to (0.04, 0.08, 0.14, 1.0) |
| 11 | `vkCmdBindPipeline` (GRAPHICS) | | Bind graphics pipeline |
| 12 | `vkCmdBindVertexBuffers` | | Bind particle SSBO as VBO |
| 13 | `vkCmdDraw(particleCount, 1, 0, 0)` | | Draw all particles as `POINT_LIST` |
| 14 | `vkCmdEndRenderPass` | | Finish render pass |
| 15 | `vkCmdWriteTimestamp` (COLOR_OUTPUT) | — | T3: render end |

### Named Vulkan Objects

| Object | `VK_EXT_debug_utils` Name | Purpose |
|--------|--------------------------|---------|
| `particleBuffer_` | `Particle SSBO` | Interleaved position + velocity + colour buffer, used as both compute SSBO and vertex VBO |
| `computePipeline_` | `Compute Pipeline` | Particle physics update (gravity, damping, boundary reflection) |
| `graphicsPipeline_` | `Graphics Pipeline` | Point-sprite rendering with additive blending |
| `renderPass_` | `Main Render Pass` | Single subpass, colour-only attachment |

### What to Inspect in RenderDoc

**1. SSBO Data (Particle Buffer)**

- Select the `vkCmdDispatch` event → Pipeline State → Compute Shader →
  Descriptor Set 0 → Binding 0.
- View buffer contents with custom struct format:
  `float2 pos; float2 vel; float4 col;`
- Compare particle positions **before** and **after** the dispatch using
  RenderDoc's timeline — values should change by `velocity × deltaTime`.
- Verify no NaN or out-of-bounds values.

**2. Pipeline State**

| Stage | Key Settings |
|-------|-------------|
| Compute | `local_size = (256, 1, 1)`, 1 SSBO, 2 push constants |
| Vertex Input | Binding 0 stride = 32 bytes; attr 0 = position (`R32G32_SFLOAT`, offset 0), attr 1 = colour (`R32G32B32A32_SFLOAT`, offset 16) |
| Rasteriser | `POINT_LIST` topology, point size = 2.0 (set in vertex shader) |
| Blend | Additive: `srcColourBlendFactor = SRC_ALPHA`, `dstColourBlendFactor = ONE` |

**3. Barrier Correctness**

The single `vkCmdPipelineBarrier` between compute and render ensures:

- **Source**: `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT` / `VK_ACCESS_SHADER_WRITE_BIT`
- **Destination**: `VK_PIPELINE_STAGE_VERTEX_INPUT_BIT` / `VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT`
- Scope: single buffer (`Particle SSBO`), `VK_WHOLE_SIZE`.

No additional implicit barriers should be inserted by the driver. If any appear
in RenderDoc's event list, they indicate suboptimal synchronisation.

**4. GPU Timing Cross-Validation**

Compare RenderDoc's built-in per-event timing against the application's own
`vkCmdWriteTimestamp` results:

| Metric | App Timestamps (ms) | Notes |
|--------|-------------------:|-------|
| Compute dispatch | 0.025 | `vkCmdWriteTimestamp` T0 → T1 |
| Render pass | 0.064 | `vkCmdWriteTimestamp` T2 → T3 |
| Total GPU time | 0.090 | T0 → T3 (RTX 5090, 1M particles) |

The Chrome JSON from `renderdoccmd convert` records CPU-side API call
durations (nanosecond resolution). For GPU-side per-event timing, open the
`.rdc` capture in RenderDoc GUI → Window → Performance Counter Viewer.
Deviation between app timestamps and RenderDoc GPU counters should be < 5 %.
Larger discrepancies may indicate RenderDoc interception overhead or
single-frame vs multi-frame averaging.

**5. Potential Optimisations (identified via RenderDoc)**

- **Vulkan 1.3 barrier upgrade**: Replace `VERTEX_INPUT_BIT` with the more
  precise `VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT` to reduce stall
  scope.
- **Indirect dispatch**: Replace hardcoded `vkCmdDispatch(N/256, 1, 1)` with
  `vkCmdDispatchIndirect` to allow GPU-driven workload sizing.
- **Dynamic point size**: Move the hardcoded `gl_PointSize = 2.0` to a push
  constant for runtime adjustment without pipeline recreation.

### How to Capture

```powershell
# Option A — GUI: launch from RenderDoc, press F12 during rendering

# Option B — CLI (Windows)
& "C:\Program Files\RenderDoc\renderdoccmd.exe" capture `
    .\build\Release\gpu_benchmark.exe --backend vulkan --benchmark 200

# Option C — Auto-capture frame 50 (must be launched via RenderDoc)
.\build\Release\gpu_benchmark.exe --backend vulkan --benchmark 200 --capture 50
```

```bash
# Linux
renderdoccmd capture ./build/gpu_benchmark --backend vulkan --benchmark 200
```

### Automated Cross-API Capture Analysis

The Full Analysis workflow (menu option 5/6) automatically captures one frame per
API backend via the RenderDoc In-Application API, converts each `.rdc` to Chrome
JSON using `renderdoccmd convert`, and runs `rdoc_analyse.py` to produce a
structural comparison across all four Windows backends.

**Per-Frame Event Count Comparison** (RTX 5090, 1M particles, Medium):

| API | Total Events | Frame Events | Dispatches | Draw Calls | Barriers |
|-----|------------:|-------------:|-----------:|-----------:|---------:|
| Vulkan 1.2 | 133 | 28 | 1 | 1 | 1 |
| DirectX 12 | 126 | 30 | 1 | 1 | **5** |
| DirectX 11 | 162 | 26 | 1 | 1 | **0** |
| OpenGL 4.3 | 148 | 17 | 1 | 1 | 1 |

#### Key Observations

1. **DX12 requires 5 resource barriers** to accomplish what Vulkan and OpenGL
   each handle with a single barrier. This reflects DX12's finer-grained resource
   state tracking — each buffer/texture transition (e.g. `UNORDERED_ACCESS →
   VERTEX_AND_CONSTANT_BUFFER`, `RENDER_TARGET → PRESENT`) is an explicit
   barrier. Vulkan batches the same transitions into one
   `vkCmdPipelineBarrier` call with multiple memory barriers.

2. **DX11 has zero explicit barriers**. The DX11 driver silently inserts all
   necessary synchronisation on behalf of the application. This is the key
   trade-off of implicit APIs: simpler code at the cost of opaque scheduling
   decisions that profiling tools like RenderDoc cannot surface.

3. **OpenGL's frame has the fewest events** (17 frame events) because the
   OpenGL driver consolidates many state changes into fewer internal calls.
   The single `glMemoryBarrier(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT)` maps
   directly to the same compute → vertex synchronisation as Vulkan's
   `vkCmdPipelineBarrier`.

4. **Vulkan debug labels are visible** in captures — 6 `vkCmdBeginDebugUtilsLabelEXT` /
   `vkCmdEndDebugUtilsLabelEXT` calls (3 pairs: "Particle Compute", "SSBO Barrier",
   "Particle Render") and 4 `vkCmdWriteTimestamp` calls confirm the profiling
   instrumentation is correctly captured.

5. **DX12 has the most frame events** (30) despite having fewer total events.
   This is because explicit APIs expose more per-frame work: command allocator
   reset, command list recording, root signature binding, and explicit resource
   transitions that implicit APIs hide from the application.

#### Synchronisation Model Comparison

| | Vulkan | DX12 | DX11 | OpenGL |
|--|--------|------|------|--------|
| **Barrier model** | `vkCmdPipelineBarrier` — stage + access masks | `ResourceBarrier` — state transitions | Implicit (driver-managed) | `glMemoryBarrier` — bit flags |
| **Barriers per frame** | 1 | 5 | 0 | 1 |
| **Who manages sync?** | Application | Application | Driver | Application (coarse) |
| **Profiler visibility** | Full | Full | Hidden | Partial |

> The full per-event command sequences for all APIs are in
> [`docs/rdoc_comparison.md`](rdoc_comparison.md), auto-generated by
> `scripts/rdoc_analyse.py`.

> See [`docs/renderdoc-analysis.md`](renderdoc-analysis.md) for the detailed
> Vulkan analysis template and [`docs/renderdoc-capture-guide.md`](renderdoc-capture-guide.md)
> for step-by-step capture instructions.

---

## 4. Cross-Validation Against 3DMark

To confirm that this benchmark accurately reflects real-world GPU performance
differences, results are cross-validated against
[3DMark](https://www.3dmark.com/) — the industry-standard graphics benchmark
by UL (formerly Futuremark).

### Methodology

1. Run this project's benchmark on each GPU (best FPS across all APIs).
2. Run 3DMark **Time Spy** (DX12) and **Fire Strike** (DX11) on the same GPUs.
3. Normalise all scores to a common baseline GPU (e.g. RX 580 = 1.00×).
4. Compare the relative performance ratios.

If both benchmarks rank GPUs in the same order with similar ratios, it
validates that this project's compute-heavy workload is a meaningful GPU
performance indicator.

### Normalised Performance Comparison

Baseline: **RX 580** = 1.00×

| GPU | Architecture | This Benchmark | 3DMark Time Spy | 3DMark Fire Strike | Deviation (TS) |
|-----|-------------|---------------|----------------|-------------------|----------------|
| RX 6900 XT | RDNA 2 (80 CU) | *fill* | 4.25× | 3.52× | *fill* |
| RX 6600 XT | RDNA 2 (32 CU) | *fill* | 2.09× | 1.87× | *fill* |
| Vega FE | GCN 5 (64 CU) | *fill* | 1.26× | 1.37× | *fill* |
| **RX 580** | **GCN 4 (36 CU)** | **1.00×** | **1.00×** | **1.00×** | **—** |
| iGPU (2 CU) | RDNA 2 | *fill* | 0.13× | 0.13× | *fill* |
| HD 5770 | TeraScale 2 | *fill* | N/A (no DX12) | 0.21× | *fill* |
| RTX 5090 | Blackwell (170 SM) | *fill* | 8.26× | 6.30× | *fill* |

> **Deviation (TS)** = how much this project's relative performance differs
> from 3DMark Time Spy's relative ranking. Positive means our benchmark
> favours that GPU more than 3DMark; negative means less.

### Expected Deviations and Why

This project runs a **single compute dispatch + single draw call** per frame.
3DMark runs complex multi-pass rasterisation with thousands of draw calls,
tessellation, post-processing, and full-screen effects. Expected differences:

| GPU | Expected Deviation | Reason |
|-----|-------------------|--------|
| RX 6900 XT | Negative (−20–30%) | 80 CU + 256-bit bus benefits complex scenes more than a single dispatch |
| iGPU (2 CU) | Near zero | Both benchmarks are GPU-limited at 2 CU regardless of workload type |
| RTX 5090 | Negative | NVIDIA's DX12 driver path is highly optimised for 3DMark-style workloads |
| HD 5770 | N/A for Time Spy | TeraScale 2 has no DX12 support; Fire Strike only |

Deviations within **±15%** indicate strong correlation. Larger deviations are
expected and explainable by workload characteristics.

### Correlation Analysis

A linear regression of project FPS vs 3DMark Time Spy scores across all GPUs
yields an R² value of *fill after running data*. An R² > 0.90 confirms a
strong linear relationship, validating the benchmark.

> **Charts**: Run `python scripts/compare_3dmark.py --save docs/images` to
> generate the normalised bar chart and correlation scatter plot
> (`docs/images/3dmark_comparison.png`, `docs/images/3dmark_correlation.png`).

### Data Source

3DMark scores are stored in [`scripts/3dmark_scores.json`](../scripts/3dmark_scores.json).
To auto-import from 3DMark result files:

```powershell
# Import from .3dmark-result files (3DMark Advanced/Professional)
python scripts/compare_3dmark.py --import-3dmark "C:\Users\*\Documents\3DMark\*.3dmark-result"
```

The `.3dmark-result` format is a ZIP archive containing `arielle.xml`
(benchmark scores, per-loop FPS) and `si.xml` (GPU name, VRAM, driver
version). The import script parses both and merges into the JSON scores file.

---

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

## 5. Python Benchmark Tooling

Automated data analysis scripts in the `scripts/` directory, demonstrating
Python scripting proficiency (JD: *"Scripting languages — Python, Perl,
shell"*).

| Script | Purpose |
|--------|---------|
| `plot_results.py` | Read `results.json` and generate 4 charts: FPS by GPU × API, GPU time breakdown, CPU overhead, particle-count scaling |
| `batch_benchmark.py` | Iterate over all GPU × API × particle-count combinations, invoke the benchmark executable, and collect results automatically |
| `export_report.py` | Export results as markdown tables or a standalone sortable HTML report with dark theme |
| `compare_3dmark.py` | Cross-validate against 3DMark: normalised bar chart, R² correlation scatter plot, auto-import from `.3dmark-result` files |

All scripts read from `~/.gpu_bench/results.json` (the application's auto-saved
benchmark results). Charts use a dark colour scheme with API-specific colours
(Vulkan = red, DX12 = blue, DX11 = green, OpenGL = orange, Metal = purple).

---

## 6. Cross-API Comparison — RTX 5090, 1M Particles (Medium)

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

## 7. Cross-GPU Comparison — Vulkan, 1M Particles (Medium), Device-local

| Metric | RTX 5090 (Discrete) | AMD Radeon iGPU (Integrated) |
|--------|--------------------|-----------------------------|
| **Avg FPS** | 2,700+ | ~320 |
| **Compute** | 0.035 ms | 1.47 ms |
| **Render** | 0.045 ms | 1.5 ms |
| **Total GPU** | 0.08 ms | ~3.0 ms |
| **Ratio** | 1× | ~37× slower |

The RTX 5090 (21,760 CUDA cores, ~3,000 GB/s bandwidth) outperforms the Zen 4 iGPU (128 shaders, ~50 GB/s shared DDR5) by approximately **37×** in GPU execution time. This aligns with the memory bandwidth ratio (~60×), confirming the particle simulation is **bandwidth-bound** rather than compute-bound at this scale.

---

## 8. Memory Allocation Impact — Vulkan, RTX 5090

| Memory Mode | Compute | Render | Total GPU | FPS |
|-------------|---------|--------|-----------|-----|
| **Device-local** (default) | 0.035 ms | 0.045 ms | 0.08 ms | 2,700+ |
| **Host-visible** (`--host-memory`) | 1.25 ms | 0.15 ms | 1.4 ms | ~600 |

Using host-visible memory (system RAM accessed over PCIe) instead of device-local VRAM causes a **35× increase in compute time** on a discrete GPU. The compute shader reads/writes particle data every frame — over PCIe, this becomes the dominant bottleneck.

On an integrated GPU, this penalty disappears because host-visible and device-local memory both reside in the same physical DDR5, making the distinction meaningless.

---

## 9. Software Renderer Baseline — WARP, 1M Particles

**WARP** (Windows Advanced Rasterisation Platform) is Microsoft's CPU-based software rasteriser bundled with every modern Windows installation. It runs the entire graphics pipeline on the CPU using SIMD (SSE/AVX) and multi-threading, serving as both a correctness reference and a fallback when no hardware GPU driver is available.

**Native API support:** WARP natively implements **Direct3D 11** and **Direct3D 12** only. It does **not** implement Vulkan or OpenGL. If Vulkan is reported as available on a WARP-only system, this is provided by **Mesa Dozen** — a Vulkan-on-D3D12 translation layer distributed via the Microsoft Store's *OpenCL, OpenGL & Vulkan Compatibility Pack*. Similarly, OpenGL support on WARP comes from **OpenGLOn12** in the same compatibility pack. Both layers translate their respective API calls to D3D12, which WARP then executes on the CPU.

### 9a. Hardware GPU vs WARP

| Metric | RTX 5090 / DX12 (Hardware) | WARP / DX12 (Software) |
|--------|--------------------|-----------------------------|
| **Avg FPS** | 6,547 | 83 |
| **Compute** | 0.014 ms | 1.1 ms |
| **Render** | 0.050 ms | 10.6 ms |
| **Total** | 0.065 ms | 11.7 ms |

WARP demonstrates a ~80× performance gap compared to hardware GPU execution, which is expected for CPU-based software rasterisation.

### 9b. WARP: DX11 vs DX12

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

### 9c. WARP as a Vulkan Device — Dozen Translation Layer

On systems without a native Vulkan ICD (e.g. Windows on ARM VMs, virtual GPUs), the Vulkan loader may enumerate **"Microsoft Basic Render Driver"** as a Vulkan physical device. The full call chain is:

```
Vulkan application  →  Dozen (Vulkan → D3D12)  →  WARP (D3D12 → CPU)
```

Dozen is distributed as part of the **OpenCL, OpenGL & Vulkan Compatibility Pack** from the Microsoft Store (`D3DMappingLayers` app package). Windows 11 may install this pack automatically on devices that lack native Vulkan/OpenGL drivers, particularly Windows on ARM devices and virtual machines.

On systems with a hardware Vulkan ICD (e.g. NVIDIA, AMD), the Dozen/WARP device is typically not enumerated or is deprioritised by the Vulkan loader. Selecting Vulkan on a WARP-only system is **functionally identical to selecting DX12 on WARP** — both end up as CPU-based software rendering, with Dozen adding a thin additional translation layer.

---

## 10. OpenGL GPU Selection — Platform Limitations

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

## 11. DX11 Timestamp Query Failures — Three Distinct Causes

DX11 is the only API in the benchmark where GPU timestamp queries can silently fail to produce results. Vulkan and DX12 always return timestamp values regardless of GPU clock state. DX11, by contrast, uses a `D3D11_QUERY_TIMESTAMP_DISJOINT` wrapper that can actively refuse to return data.

Three distinct failure modes were observed during testing:

### 11a. Driver Never Resolves Queries (`GetData` → `S_FALSE` indefinitely)

**Affected:** Windows on ARM virtual machines (SVGA virtual GPU driver).

`ID3D11Device::CreateQuery` succeeds for both `D3D11_QUERY_TIMESTAMP` and `D3D11_QUERY_TIMESTAMP_DISJOINT`, and the application reports timestamps as "enabled". However, `ID3D11DeviceContext::GetData` for the disjoint query perpetually returns `S_FALSE` — the result is never ready.

This is a **driver limitation**: the virtual GPU driver accepts query creation but does not implement the hardware counters needed to resolve them. No application-level workaround exists.

See [`docs/woa-dx11-timestamp-issue.md`](woa-dx11-timestamp-issue.md) for a detailed write-up.

### 11b. GPU Clock Frequency Instability (`Disjoint = TRUE`)

**Affected:** Integrated GPUs under fluctuating load, discrete GPUs during power-state transitions.

The `D3D11_QUERY_DATA_TIMESTAMP_DISJOINT` structure contains a `Disjoint` boolean. When `TRUE`, it signals that the GPU's clock frequency changed during the frame (P-state transition, thermal throttling, power-saving downclock), making the timestamp-to-millisecond conversion unreliable.

The D3D11 specification recommends discarding the entire frame's timing data when `Disjoint = TRUE`. If the GPU is frequently switching power states — common on integrated GPUs under variable load, or during the first few seconds of a benchmark run while the GPU ramps up — this can result in many consecutive frames with no timing data.

**This is not a driver bug.** It is a deliberate DX11 design choice to prioritise timestamp accuracy over availability.

**Key difference from Vulkan/DX12:** Neither Vulkan nor DX12 has a `Disjoint` concept. Their timestamp queries always return values based on a fixed `timestampPeriod` / `Frequency`, even if the GPU clock changes mid-frame. The precision may degrade slightly, but data is never withheld entirely. This is why Vulkan and DX12 report timestamps reliably in scenarios where DX11 reports none.

**Mitigation implemented:** The application now caches the last known stable frequency (`lastGoodFrequency`). When `Disjoint = TRUE`, timestamps are still read and converted using the cached frequency rather than being discarded. This mirrors the behaviour of Vulkan/DX12 — accepting marginally less precise data in exchange for continuous availability.

### 11c. Query Pipeline Too Shallow (Ring Buffer Depth)

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

## 12. Legacy Discrete GPU vs Modern iGPU — Compute Efficiency Beyond TFLOPS

### Test System B

| Component | Specification |
|-----------|--------------|
| CPU | AMD Ryzen 5 7600 6-Core Processor |
| Discrete GPU | AMD Radeon HD 5770 (757 MB GDDR5, TeraScale 2, 2009) |
| Integrated GPU | AMD Radeon Graphics (Zen 4 / RDNA 2, 2 CU, shared DDR5) |
| OS | Windows 11 (NT 10.0.26200) |
| Resolution | 1280 × 720 |
| V-Sync | OFF |
| Memory Mode | Device-local |

The HD 5770 only supports DX11 and OpenGL 4.3 — no Vulkan or DX12 drivers exist for TeraScale 2 hardware. The Ryzen 7600 iGPU supports all four APIs.

### 12a. Raw Results — 1M Particles (Medium)

| # | API | GPU | Avg FPS | Compute (ms) | Render (ms) | Total GPU (ms) | Utilisation |
|---|-----|-----|---------|-------------|------------|---------------|-------------|
| 1 | DX12 | Radeon iGPU (RDNA 2) | 313 | — | — | 2.956 | — |
| 2 | Vulkan | Radeon iGPU (RDNA 2) | 275 | — | — | 3.426 | — |
| 3 | OpenGL | Radeon iGPU (RDNA 2) | 275 | — | — | 3.391 | — |
| 4 | OpenGL | HD 5770 (TeraScale 2) | 193 | 1.789 | 3.025 | 4.820 | 93.1% |
| 5 | DX11 | Radeon iGPU (RDNA 2) | 190 | — | — | 5.017 | — |
| 6 | DX11 | HD 5770 (TeraScale 2) | 111 | 1.078 | 2.715 | 8.973 | 99.6% |
| 7 | DX12 | WARP (CPU) | 64 | — | — | 15.234 | — |
| 8 | DX11 | WARP (CPU) | 45 | — | — | 21.954 | — |

### 12b. Same-API Head-to-Head

| API | HD 5770 FPS | iGPU FPS | iGPU Advantage |
|-----|-------------|----------|----------------|
| DX11 | 111 | 190 | +71% |
| OpenGL | 193 | 275 | +42% |

The RDNA 2 integrated GPU outperforms the HD 5770 discrete GPU in **every** comparable API, despite having far fewer hardware resources on paper.

### 12c. Theoretical TFLOPS Comparison

| | HD 5770 | Ryzen 5 7600 iGPU |
|--|---------|----------------|
| Architecture | TeraScale 2 (2009) | RDNA 2 (2022) |
| Stream Processors | 800 (160 × VLIW5) | 128 (2 CU × 64) |
| Core Clock | 850 MHz | 2,200 MHz |
| FP32 TFLOPS | **~1.36** | **~0.56** |
| Memory | 1 GB GDDR5, ~76.8 GB/s | Shared DDR5, ~83 GB/s |

The HD 5770 has **2.4× more raw FP32 TFLOPS** than the iGPU, yet it is **42–71% slower** in this compute benchmark. This inversion demonstrates that TFLOPS alone is a poor predictor of real-world compute shader performance.

**This is not anomalous — TFLOPS comparisons across different architectures are unreliable as an industry rule of thumb.** Well-documented examples include AMD Vega 64 (13.7 TFLOPS) losing to NVIDIA GTX 1080 (8.9 TFLOPS) in many gaming and compute workloads, and Intel Arc A770 (19.7 TFLOPS) underperforming against the RTX 3060 (12.7 TFLOPS) at launch despite a 55% TFLOPS advantage. TFLOPS measures only the theoretical rate of fused multiply-add operations — it says nothing about whether the ALUs can actually be kept fed with data and useful instructions. Cache hit rates, memory bandwidth, scheduling efficiency, VLIW slot utilisation, and driver code generation quality all determine how much of the theoretical peak is realised in practice.

**The discrepancy between TFLOPS rankings and benchmark results is itself a validation of the test.** If results tracked TFLOPS perfectly, it would suggest the benchmark is merely saturating ALU throughput with a trivially parallel workload — essentially an artificial peak-FLOPS test. The fact that a 0.56 TFLOPS GPU outperforms a 1.36 TFLOPS GPU confirms that this benchmark exercises real-world bottlenecks — memory access patterns, compute scheduler overhead, wave occupancy, and driver-side code generation — rather than measuring a synthetic upper bound.

### 12d. Why the iGPU Wins Despite Lower TFLOPS

**Architecture efficiency matters more than shader count.** TeraScale 2 uses a VLIW5 (Very Long Instruction Word) design where each "stream processor" is actually five tightly coupled ALUs that must execute in lockstep. If the compiler cannot fill all five slots (a common occurrence for compute shaders with irregular control flow), the vacant slots are wasted. Real-world VLIW5 utilisation in compute workloads is estimated at 50–70%, reducing the HD 5770's effective throughput to roughly 0.7–0.95 TFLOPS.

RDNA 2, by contrast, uses a scalar + SIMD32 design where each compute unit contains two independent SIMD32 units. Every lane executes useful work on every clock — there is no VLIW packing problem. At 2,200 MHz, the 128 shaders deliver nearly their full 0.56 TFLOPS.

**The real gap is far smaller than the spec sheet suggests.** After accounting for VLIW5 utilisation losses, the effective compute advantage shrinks from the theoretical 2.4× (1.36 vs 0.56 TFLOPS) down to roughly **1.3–1.7×** (0.7–0.95 vs 0.56 TFLOPS). The remaining factors below — memory bandwidth parity, driver quality, and compute scheduler maturity — are more than sufficient to close this residual gap and tip the balance in the iGPU's favour.

**Compute shader support maturity.** The HD 5770 was designed primarily for DirectX 11-era pixel and vertex shading. Its compute shader support (DirectCompute 5.0) was a first-generation implementation with limited occupancy, no asynchronous compute queues, and restricted shared memory bandwidth. RDNA 2 treats compute as a first-class workload with dedicated hardware schedulers, LDS (Local Data Share) bandwidth matched to ALU throughput, and fine-grained wave management.

**Driver optimisation.** AMD's current Radeon Software **Adrenalin** Edition drivers for RDNA 2 are actively maintained and optimised. The HD 5770's legacy **Crimson** Edition drivers (version 16.2.1, Mar 2016) have not received performance updates in over a decade (actually, it‘s real 10 years, now it's Mar 2026). Compute shader code generation for TeraScale 2 was never a priority — these drivers were written when GPU compute was still in its infancy.

**Memory bandwidth parity.** The HD 5770's theoretical advantage in dedicated GDDR5 is largely neutralised here. Its 76.8 GB/s bandwidth is slightly below the iGPU's ~83 GB/s from dual-channel DDR5-6000 C28. For a bandwidth-sensitive particle simulation, this effectively levels the playing field — or tilts it slightly in the iGPU's favour.

### 12e. Would the HD 5770 Win in a Gaming Benchmark?

Very likely **yes**, for traditional 3D rendering workloads. The HD 5770 has 6.25× more shader units, 5× more texture mapping units, and 4× more render output units than the 2-CU iGPU. In a conventional rasterisation pipeline — vertex processing, texture sampling, pixel shading, and blending — these fixed-function resources matter far more than per-CU compute efficiency.

Online gaming benchmarks broadly confirm this: the HD 5770 can run older titles (pre-2015) at low-medium settings, whereas the Ryzen 7600 iGPU struggles to maintain playable frame rates in the same scenarios.

**The key insight:** This benchmark is a **compute-first** workload — a particle simulation driven by a compute shader, with a simple instanced rendering pass for visualisation. It exercises the GPU's general-purpose compute pipeline, not its fixed-function rasterisation hardware. The result is a measure of **compute shader throughput and scheduling efficiency**, where architectural modernity dominates raw shader count.

This makes the benchmark a useful complement to traditional GPU tests. A gaming benchmark tells you how fast a GPU can rasterise triangles; this benchmark tells you how efficiently it can execute general-purpose parallel computation — a workload increasingly relevant to physics simulation, machine learning inference, post-processing, and scientific computing.

---

## 13. AMD GPU Generational Analysis — TeraScale 2 → GCN → RDNA 2

> Cross-generational compute shader performance comparison across seven AMD
> GPUs spanning four architectures and 15 years of hardware evolution
> (2009–2024). All results collected with this project's particle simulation
> benchmark.

### Test Hardware

| GPU | Architecture | Generation | CUs / SPs | Core Clock | FP32 TFLOPS | Memory | Bandwidth | Platform | API Coverage |
|-----|-------------|-----------|-----------|-----------|-------------|--------|-----------|----------|-------------|
| HD 5770 | TeraScale 2 | 2009 | 800 SPs (VLIW5) | 850 MHz | ~1.36 | 1 GB GDDR5 | 76.8 GB/s | Windows | DX11, OpenGL |
| FirePro D700 ×2 | GCN 1.0 (Tahiti) | 2013 | 2048 SPs | 850 MHz | ~3.5 | 6 GB GDDR5 | 264 GB/s | macOS | Metal |
| RX 580 | GCN 4 (Polaris) | 2017 | 36 CUs | 1,340 MHz | ~6.2 | 8 GB GDDR5 | 256 GB/s | Windows | Vulkan, DX12, DX11, OpenGL |
| Vega Frontier Edition | GCN 5 (Vega) | 2017 | 64 CUs | 1,600 MHz | ~13.1 | 16 GB HBM2 | 483 GB/s | Windows | Vulkan, DX12, DX11, OpenGL |
| RX 6600 XT | RDNA 2 | 2021 | 32 CUs | 2,589 MHz | ~10.6 | 8 GB GDDR6 | 256 GB/s | Windows | Vulkan, DX12, DX11, OpenGL |
| RX 6900 XT | RDNA 2 | 2020 | 80 CUs | 2,250 MHz | ~23.0 | 16 GB GDDR6 | 512 GB/s | Windows | Vulkan, DX12, DX11, OpenGL |
| Ryzen 9800X3D iGPU | RDNA 2 | 2024 | 2 CUs | 2,200 MHz | ~0.56 | Shared DDR5 | ~83 GB/s | Windows | Vulkan, DX12, DX11, OpenGL |

> **Note:** FirePro D700 data is from macOS Metal only. The Mac Pro 2013
> does not run Windows natively, so no DX/Vulkan comparison is available
> for GCN 1.0. All other GPUs are tested on the same Windows 11 platform.

---

### 13a. Raw Results — All AMD GPUs, 1M Particles (Medium)

**Best API per GPU (highest FPS):**

| # | GPU | Architecture | Best API | Avg FPS | Compute (ms) | Render (ms) | Total GPU (ms) | Bottleneck |
|---|-----|-------------|----------|---------|-------------|------------|---------------|------------|
| 1 | RX 6900 XT | RDNA 2 (80 CU) | | | | | | |
| 2 | RX 6600 XT | RDNA 2 (32 CU) | | | | | | |
| 3 | Vega FE | GCN 5 (64 CU) | | | | | | |
| 4 | RX 580 | GCN 4 (36 CU) | | | | | | |
| 5 | FirePro D700 | GCN 1.0 | Metal | | | | | |
| 6 | iGPU (2 CU) | RDNA 2 | | | | | | |
| 7 | HD 5770 | TeraScale 2 | | | | | | |
| 8 | WARP (CPU) | Software | | | | | | |

**All API results per GPU:**

<!-- Fill in after running all benchmarks. One row per GPU × API combination. -->

| GPU | Vulkan | DX12 | DX11 | OpenGL | Metal |
|-----|--------|------|------|--------|-------|
| RX 6900 XT | — FPS | — FPS | — FPS | — FPS | N/A |
| RX 6600 XT | — FPS | — FPS | — FPS | — FPS | N/A |
| Vega FE | — FPS | — FPS | — FPS | — FPS | N/A |
| RX 580 | — FPS | — FPS | — FPS | — FPS | N/A |
| FirePro D700 | N/A | N/A | N/A | N/A | — FPS |
| iGPU (2 CU) | — FPS | — FPS | — FPS | — FPS | N/A |
| HD 5770 | N/A | N/A | — FPS | — FPS | N/A |

---

### 13b. Per-CU Compute Efficiency

Normalise compute shader performance to **per-CU throughput** to isolate
architectural efficiency from raw CU count.

| GPU | Architecture | CUs | Compute Time (ms) | Per-CU Throughput (relative) | Per-CU vs RX 580 |
|-----|-------------|-----|--------------------|------------------------------|-------------------|
| HD 5770 | TeraScale 2 | ~10 equiv | | | |
| FirePro D700 | GCN 1.0 | 32 | | | |
| RX 580 | GCN 4 | 36 | | **1.00×** | — |
| Vega FE | GCN 5 | 64 | | | |
| RX 6600 XT | RDNA 2 | 32 | | | |
| RX 6900 XT | RDNA 2 | 80 | | | |
| iGPU (2 CU) | RDNA 2 | 2 | | | |

> **HD 5770 CU equivalence:** TeraScale 2 does not have CUs. 800 VLIW5
> stream processors are roughly grouped into 10 SIMD engines. This mapping
> is approximate.

**Analysis to write:**

<!-- After filling data:
- How much does per-CU efficiency improve from GCN 1.0 → GCN 4 → GCN 5 → RDNA 2?
- Is the per-CU improvement consistent, or are there architecture jumps?
- Does the iGPU (2 CU) match per-CU performance of the 6900 XT (80 CU)?
  If yes, RDNA 2 scales linearly. If no, explain why (memory bandwidth,
  cache contention, occupancy).
-->

---

### 13c. CU Scaling Within RDNA 2

Three RDNA 2 GPUs at vastly different CU counts allow direct measurement of
how compute performance scales with CU count within the same architecture.

| GPU | CUs | FPS | Compute (ms) | Scaling vs iGPU (2 CU) | Ideal Scaling (CU ratio) | Efficiency |
|-----|-----|-----|-------------|------------------------|--------------------------|------------|
| iGPU (2 CU) | 2 | | | 1.00× | 1.00× | |
| RX 6600 XT | 32 | | | | 16.0× | |
| RX 6900 XT | 80 | | | | 40.0× | |

**Analysis to write:**

<!-- After filling data:
- Does the 6600 XT achieve 16× the iGPU's performance (CU ratio)?
- Does the 6900 XT achieve 40× the iGPU's performance?
- If scaling is sub-linear, identify the bottleneck: memory bandwidth,
  cache hierarchy, or dispatch overhead.
- If scaling is super-linear, explain: larger L2 cache, higher bandwidth,
  better occupancy at higher CU counts.
-->

---

### 13d. Architecture Generational Progression

Normalise all GPUs to the **RX 580 (GCN 4) = 1.00×** baseline for
generational comparison.

| GPU | Architecture | Year | FPS | vs RX 580 | Memory BW | BW vs RX 580 |
|-----|-------------|------|-----|-----------|-----------|-------------|
| HD 5770 | TeraScale 2 | 2009 | | | 76.8 GB/s | 0.30× |
| FirePro D700 | GCN 1.0 | 2013 | | | 264 GB/s | 1.03× |
| **RX 580** | **GCN 4** | **2017** | | **1.00×** | **256 GB/s** | **1.00×** |
| Vega FE | GCN 5 | 2017 | | | 483 GB/s | 1.89× |
| RX 6600 XT | RDNA 2 | 2021 | | | 256 GB/s | 1.00× |
| RX 6900 XT | RDNA 2 | 2020 | | | 512 GB/s | 2.00× |

**Analysis to write:**

<!-- After filling data:
- Does the performance ranking follow the memory bandwidth ranking?
  (This benchmark is bandwidth-bound at 1M particles.)
- Vega FE has 1.89× the bandwidth of RX 580 — does it achieve ~1.89× FPS?
- RX 6600 XT has the same bandwidth as RX 580 but newer architecture —
  how much does RDNA 2 efficiency contribute beyond bandwidth?
- How does the FPS progression compare to the TFLOPS progression?
  (Continues the Section 12 finding that TFLOPS is a poor predictor.)
-->

---

### 13e. Cross-API Performance Variation by Architecture

Does the API ranking (DX11 > DX12 > Vulkan > OpenGL) observed on RTX 5090
hold across all AMD architectures, or does it change?

| GPU | Architecture | Fastest API | DX11 FPS | DX12 FPS | Vulkan FPS | OpenGL FPS | DX11 vs Vulkan Gap |
|-----|-------------|------------|----------|----------|------------|------------|-------------------|
| HD 5770 | TeraScale 2 | | | N/A | N/A | | |
| RX 580 | GCN 4 | | | | | | |
| Vega FE | GCN 5 | | | | | | |
| RX 6600 XT | RDNA 2 | | | | | | |
| RX 6900 XT | RDNA 2 | | | | | | |
| iGPU (2 CU) | RDNA 2 | | | | | | |

**Analysis to write:**

<!-- After filling data:
- Is DX11 still fastest on AMD GPUs, or does AMD's Vulkan driver
  (open-source Mesa RADV or proprietary AMDVLK) change the ranking?
- Does the DX11-vs-Vulkan CPU overhead gap shrink on AMD compared to
  NVIDIA? (AMD's DX11 driver is historically less optimised than NVIDIA's.)
- On older GPUs (HD 5770, RX 580), is OpenGL competitive with DX11?
- Does the API ranking change when the GPU is the bottleneck (high
  particle count) vs when the CPU is the bottleneck (low particle count)?
-->

---

### 13f. Memory Bandwidth as Performance Predictor

Plot FPS against memory bandwidth to test the hypothesis that this benchmark
is bandwidth-bound.

| GPU | Memory BW (GB/s) | FPS (best API) | FPS / (GB/s) |
|-----|-------------------|----------------|---------------|
| HD 5770 | 76.8 | | |
| FirePro D700 | 264 | | |
| RX 580 | 256 | | |
| Vega FE (HBM2) | 483 | | |
| RX 6600 XT | 256 | | |
| RX 6900 XT | 512 | | |
| iGPU (DDR5) | ~83 | | |

**Analysis to write:**

<!-- After filling data:
- Calculate R² of FPS vs memory bandwidth across all AMD GPUs.
- If R² > 0.85, the benchmark is confirmed bandwidth-bound on AMD hardware.
- Identify outliers: does Vega FE's HBM2 over-perform or under-perform
  relative to its bandwidth? (HBM2 has higher effective bandwidth due to
  wider bus and lower latency.)
- Does the iGPU's shared DDR5 behave differently from dedicated GDDR?
  (Shared memory competes with CPU traffic.)
-->

---

### 13g. Particle Count Scaling — GPU-Bound vs CPU-Bound Crossover

Run each GPU at multiple particle counts to find the crossover point where
the bottleneck shifts from CPU to GPU.

| GPU | 65K FPS | 1M FPS | 4M FPS | 16M FPS | CPU→GPU Crossover |
|-----|---------|--------|--------|---------|-------------------|
| RX 6900 XT | | | | | |
| RX 6600 XT | | | | | |
| Vega FE | | | | | |
| RX 580 | | | | | |
| HD 5770 | | | | N/A (OOM?) | |
| iGPU (2 CU) | | | | N/A (OOM?) | |

**Analysis to write:**

<!-- After filling data:
- At 65K particles, all GPUs should be CPU-bound — does FPS vary by API
  more than by GPU?
- At what particle count does each GPU become GPU-bound?
  (When FPS starts dropping proportionally with particle count.)
- Faster GPUs should stay CPU-bound longer — does the 6900 XT crossover
  at a higher particle count than the RX 580?
- HD 5770 has 1 GB VRAM — at what particle count does it run out of memory?
  (Each particle = 32 bytes; 16M particles = 512 MB.)
-->

---

### 13h. RenderDoc Cross-GPU Frame Analysis

Capture one Vulkan frame on each AMD GPU (where Vulkan is available) and
compare per-event GPU timing, barrier cost, and command structure.

> Captures generated via `--capture 5` (auto-capture at 5 seconds).
> Analysis automated with `scripts/rdoc_analyse.py` and
> `scripts/rdoc_export_timing.py`.

#### Per-Event GPU Timing Comparison

| GPU | Architecture | Compute Dispatch (ms) | Barrier (ms) | Render Pass (ms) | Total GPU (ms) |
|-----|-------------|-----------------------|-------------|-------------------|----------------|
| RX 6900 XT | RDNA 2 (80 CU) | | | | |
| RX 6600 XT | RDNA 2 (32 CU) | | | | |
| Vega FE | GCN 5 (64 CU) | | | | |
| RX 580 | GCN 4 (36 CU) | | | | |
| iGPU (2 CU) | RDNA 2 | | | | |

> HD 5770 and FirePro D700 excluded — no Vulkan support.

#### App Timestamp vs RenderDoc Cross-Validation

| GPU | Metric | App Timestamp (ms) | RenderDoc (ms) | Deviation |
|-----|--------|-------------------|----------------|-----------|
| RX 6900 XT | Compute | | | |
| RX 6900 XT | Render | | | |
| RX 6600 XT | Compute | | | |
| RX 6600 XT | Render | | | |
| RX 580 | Compute | | | |
| RX 580 | Render | | | |

> Target: < 5% deviation across all GPUs. Larger deviations indicate
> driver-specific timestamp query behaviour.
>
> Generated with: `python scripts/compare_rdoc_timing.py <rdoc_json_files>`

#### Barrier Cost Comparison

| GPU | Architecture | Memory Type | Barrier Duration (ms) | Notes |
|-----|-------------|-------------|----------------------|-------|
| RX 6900 XT | RDNA 2 | 16 GB GDDR6 | | L2 writeback required |
| RX 6600 XT | RDNA 2 | 8 GB GDDR6 | | Smaller L2 → faster flush? |
| Vega FE | GCN 5 | 16 GB HBM2 | | HBM2 lower latency |
| RX 580 | GCN 4 | 8 GB GDDR5 | | |
| iGPU (2 CU) | RDNA 2 | Shared DDR5 | | Unified memory → near-zero? |

**Analysis to write:**

<!-- After filling data:
- Does barrier cost scale with cache size or memory bandwidth?
- Is the iGPU barrier effectively zero (unified memory, no cache flush)?
- Does HBM2 (Vega FE) show lower barrier latency than GDDR6?
- Are there any driver-inserted implicit barriers on AMD that don't
  appear on NVIDIA? (Check RenderDoc event count per GPU.)
-->

#### Per-Frame Event Count by GPU

| GPU | Total Events | Frame Events | Dispatches | Draw Calls | Barriers | Driver-Inserted Barriers |
|-----|------------:|-------------:|-----------:|-----------:|---------:|------------------------:|
| RX 6900 XT | | | 1 | 1 | | |
| RX 6600 XT | | | 1 | 1 | | |
| Vega FE | | | 1 | 1 | | |
| RX 580 | | | 1 | 1 | | |
| iGPU (2 CU) | | | 1 | 1 | | |

> Compare against NVIDIA RTX 5090 baseline (Section 3): 133 total events,
> 28 frame events, 1 barrier. Do AMD GPUs produce different event counts
> due to driver differences (AMDVLK vs RADV vs NVIDIA)?
>
> Generated with: `python scripts/rdoc_analyse.py <rdc_files>`

---

### 13i. 3DMark Cross-Validation — AMD GPU Fleet

Cross-validate this benchmark's AMD GPU rankings against 3DMark Time Spy
(DX12) and Fire Strike (DX11) to confirm the results reflect real-world
performance scaling.

> Data source: `scripts/3dmark_scores.json`
> Charts generated with: `python scripts/compare_3dmark.py --save docs/images`

#### Normalised Performance (Baseline: RX 580 = 1.00×)

| GPU | Architecture | This Benchmark | 3DMark Time Spy | 3DMark Fire Strike | Deviation (TS) | Deviation (FS) |
|-----|-------------|---------------|----------------|-------------------|----------------|----------------|
| RX 6900 XT | RDNA 2 (80 CU) | | 4.25× | 3.52× | | |
| RX 6600 XT | RDNA 2 (32 CU) | | 2.09× | 1.87× | | |
| Vega FE | GCN 5 (64 CU) | | 1.26× | 1.37× | | |
| **RX 580** | **GCN 4 (36 CU)** | **1.00×** | **1.00×** | **1.00×** | **—** | **—** |
| iGPU (2 CU) | RDNA 2 | | 0.13× | 0.13× | | |
| HD 5770 | TeraScale 2 | | N/A | 0.21× | | |

> **Deviation** = `(This Benchmark ratio / 3DMark ratio) − 1`.
> Positive = our benchmark favours that GPU more; negative = less.

#### Expected Deviations and Why

| GPU | Expected Deviation | Reason |
|-----|-------------------|--------|
| RX 6900 XT | Negative (−10–20%) | 80 CU + 512 GB/s benefits complex multi-pass scenes more than a single dispatch |
| RX 6600 XT | Near zero | Mid-range GPU; balanced for both workload types |
| Vega FE | Positive (+10–20%?) | HBM2's 483 GB/s bandwidth disproportionately benefits bandwidth-bound compute workloads |
| iGPU (2 CU) | Near zero | Both benchmarks GPU-limited at 2 CU regardless of workload |
| HD 5770 | N/A for Time Spy | TeraScale 2 has no DX12; Fire Strike only |

#### Correlation Analysis

<!-- After filling data:
- Calculate R² of this benchmark's FPS vs 3DMark Time Spy across all AMD GPUs.
- R² > 0.90 = strong linear correlation, validates the benchmark.
- Does Vega FE's HBM2 create an outlier? (Higher bandwidth → this benchmark
  may over-rate Vega FE relative to 3DMark.)
- Plot: `python scripts/compare_3dmark.py --save docs/images`
  → `docs/images/3dmark_correlation_amd.png`
-->

#### This Benchmark vs 3DMark — What They Measure Differently

| | This Benchmark | 3DMark Time Spy | 3DMark Fire Strike |
|--|---------------|----------------|-------------------|
| **Workload** | Single compute dispatch + single draw call | Multi-pass rasterisation, tessellation, post-FX | Multi-pass rasterisation, particle physics |
| **Draw calls / frame** | 1 | Thousands | Thousands |
| **Bottleneck (fast GPU)** | CPU overhead | GPU (texture, geometry, shading) | GPU (texture, shading) |
| **Memory access** | Sequential SSBO read/write | Random texture fetches, render targets | Random texture fetches |
| **Benefits from** | Memory bandwidth, compute scheduler | Shader count, TMUs, ROPs, driver DX12 path | Shader count, TMUs, ROPs, driver DX11 path |

This difference explains why deviations exist — and why both benchmarks are
needed for a complete GPU performance picture.

---

### 13j. Summary — AMD Generational Findings

| Observation | Explanation |
|-------------|-------------|
| <!-- e.g. Per-CU efficiency: RDNA 2 is X× more efficient than GCN 1.0 --> | <!-- why --> |
| <!-- e.g. RDNA 2 CU scaling is near-linear up to 32 CUs, sub-linear at 80 --> | <!-- bandwidth saturation --> |
| <!-- e.g. Memory bandwidth predicts FPS with R² = 0.XX --> | <!-- bandwidth-bound workload --> |
| <!-- e.g. API ranking differs on AMD vs NVIDIA --> | <!-- driver differences --> |
| <!-- e.g. Vega FE's HBM2 provides X% more FPS than bandwidth ratio predicts --> | <!-- HBM latency advantage --> |
| <!-- e.g. TeraScale 2 VLIW5 achieves only X% of theoretical per-SP throughput --> | <!-- VLIW utilisation --> |

---

## 14. Summary

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
| RDNA 2 iGPU (0.56 TFLOPS) beats HD 5770 (1.36 TFLOPS) by 42–71% | VLIW5 utilisation losses, immature compute scheduler, and stale drivers reduce TeraScale 2's effective throughput well below its theoretical peak |
| TFLOPS is a poor predictor of compute shader performance | Architectural efficiency (SIMD vs VLIW), driver maturity, and compute scheduler design dominate raw ALU count |
| HD 5770 would likely win a gaming benchmark | Traditional rasterisation relies on fixed-function units (TMUs, ROPs) where the HD 5770 has 4–6× more hardware than the iGPU |

These results demonstrate that **API overhead, memory placement, and hardware architecture** all significantly affect GPU compute performance — and that the optimal configuration depends on workload complexity and hardware topology.
