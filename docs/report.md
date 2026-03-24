# Multi-Graphics API GPU Benchmark Report

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
| Vulkan 1.1+ | Explicit, low-level | GLSL → SPIR-V | Windows, Linux, macOS (MoltenVK) |
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
| RTX 5090 | Blackwell (170 SM) | *fill* | 8.26× | 6.30× | *fill* |
| RX 9070 XT | RDNA 4 (32 CU) | *fill* | 6.19× | 4.97× | *fill* |
| RX 6900 XT | RDNA 2 (80 CU) | *fill* | 4.25× | 3.52× | *fill* |
| RX 6600 XT | RDNA 2 (32 CU) | *fill* | 2.09× | 1.87× | *fill* |
| Vega FE | GCN 5 (64 CU) | *fill* | 1.26× | 1.37× | *fill* |
| **RX 580** | **GCN 4 (36 CU)** | **1.00×** | **1.00×** | **1.00×** | **—** |
| iGPU (2 CU) | RDNA 2 | *fill* | 0.13× | 0.13× | *fill* |
| HD 5770 | TeraScale 2 | *fill* | N/A (no DX12) | 0.21× | *fill* |

> **Deviation (TS)** = how much this project's relative performance differs
> from 3DMark Time Spy's relative ranking. Positive means our benchmark
> favours that GPU more than 3DMark; negative means less.

### Expected Deviations and Why

This project runs a **single compute dispatch + single draw call** per frame.
3DMark runs complex multi-pass rasterisation with thousands of draw calls,
tessellation, post-processing, and full-screen effects. Expected differences:

| GPU | Expected Deviation | Reason |
|-----|-------------------|--------|
| RX 9070 XT | Negative (−10–20%) | 3DMark's complex multi-pass workload benefits from RDNA 4's full feature set (mesh shaders, ray tracing units) that this single-dispatch benchmark cannot exercise |
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

> **AMD comparison:** The RX 9070 XT (RDNA 4) shows a very different API ranking:
> DX11 ≈ Vulkan (1,774 / 1,751 FPS) > DX12 (1,609 FPS) >> OpenGL (253 FPS).
> The DX11-over-Vulkan advantage shrinks from **2.5×** on NVIDIA to **1.01×** on AMD,
> reflecting AMD's less optimised DX11 driver. See Section 16 for the full
> RX 9070 XT cross-API analysis.

---

## 7. Cross-GPU Comparison — Vulkan, 1M Particles (Medium), Device-local

| Metric | RTX 5090 (Discrete) | RX 9070 XT (Discrete) | AMD Radeon iGPU (Integrated) |
|--------|--------------------|-----------------------|-----------------------------|
| **Avg FPS** | 2,700+ | 1,751 | ~320 |
| **Compute** | 0.035 ms | 0.033 ms | 1.47 ms |
| **Render** | 0.045 ms | 0.408 ms | 1.5 ms |
| **Total GPU** | 0.08 ms | 0.446 ms | ~3.0 ms |
| **Ratio** | 1× | ~5.6× slower | ~37× slower |

The RTX 5090 (21,760 CUDA cores, ~3,000 GB/s bandwidth) outperforms the Zen 4 iGPU (128 shaders, ~50 GB/s shared DDR5) by approximately **37×** in GPU execution time. This aligns with the memory bandwidth ratio (~60×), confirming the particle simulation is **bandwidth-bound** rather than compute-bound at this scale.

The RX 9070 XT sits between these extremes: its compute time (0.033 ms) is comparable to the RTX 5090 (0.035 ms), but its total GPU time (0.446 ms) is 5.6× higher due to **swapchain semaphore wait pollution** inflating the render timestamp (see Section 17). In headless mode, the 9070 XT achieves 21,260 FPS — only ~15% behind the RTX 5090's headless throughput.

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

## 13. AMD GPU Generational Analysis — TeraScale 2 → GCN → RDNA 2 → RDNA 4

> Cross-generational compute shader performance comparison across eight AMD
> GPUs spanning five architectures and 16 years of hardware evolution
> (2009–2025). All results collected with this project's particle simulation
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
| RX 9070 XT | RDNA 4 | 2025 | 32 CUs | 2,805 MHz | ~24.6 | 16 GB GDDR6 | 512 GB/s | Windows | Vulkan, DX12, DX11, OpenGL |
| Ryzen 9800X3D iGPU | RDNA 2 | 2024 | 2 CUs | 2,200 MHz | ~0.56 | Shared DDR5 | ~83 GB/s | Windows | Vulkan, DX12, DX11, OpenGL |

> **Note:** FirePro D700 data is from macOS Metal only. The Mac Pro 2013
> does not run Windows natively, so no DX/Vulkan comparison is available
> for GCN 1.0. All other GPUs are tested on the same Windows 11 platform.

---

### 13a. Raw Results — All AMD GPUs, 1M Particles (Medium)

**Best API per GPU (highest FPS):**

| # | GPU | Architecture | Best API | Avg FPS | Compute (ms) | Render (ms) | Total GPU (ms) | Bottleneck |
|---|-----|-------------|----------|---------|-------------|------------|---------------|------------|
| 1 | RX 9070 XT | RDNA 4 (32 CU) | DX11 | 1,773.7 | 0.047 | 0.451 | 0.542 | GPU-bound |
| 2 | RX 6900 XT | RDNA 2 (80 CU) | | | | | | |
| 3 | RX 6600 XT | RDNA 2 (32 CU) | | | | | | |
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
| RX 9070 XT | 1,751 FPS | 1,609 FPS | 1,774 FPS | 253 FPS | N/A |
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
| RX 9070 XT | RDNA 4 | 32 | 0.033 | | |
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
| RX 9070 XT | RDNA 4 | 2025 | 1,751 | | 512 GB/s | 2.00× |

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
| RX 9070 XT | RDNA 4 | DX11 | 1,774 | 1,609 | 1,751 | 253 | +1.3% |
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
| RX 9070 XT | 512 | 1,751 | |
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
| RX 9070 XT | | 1,751 | | 111 | |
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
| RX 9070 XT | RDNA 4 (32 CU) | | | | |
| RX 6900 XT | RDNA 2 (80 CU) | | | | |
| RX 6600 XT | RDNA 2 (32 CU) | | | | |
| Vega FE | GCN 5 (64 CU) | | | | |
| RX 580 | GCN 4 (36 CU) | | | | |
| iGPU (2 CU) | RDNA 2 | | | | |

> HD 5770 and FirePro D700 excluded — no Vulkan support.

#### App Timestamp vs RenderDoc Cross-Validation

| GPU | Metric | App Timestamp (ms) | RenderDoc (ms) | Deviation |
|-----|--------|-------------------|----------------|-----------|
| RX 9070 XT | Compute | 0.033 | | |
| RX 9070 XT | Render | 0.408 | | |
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
| RX 9070 XT | RDNA 4 | 16 GB GDDR6 | | RDNA 4 cache hierarchy |
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
| RX 9070 XT | | | 1 | 1 | | |
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
| RX 9070 XT | RDNA 4 (32 CU) | | 6.19× | 4.97× | | |
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
| RX 9070 XT | Negative (−10–20%) | RDNA 4's ray tracing and mesh shader hardware unused in this single-dispatch benchmark; 3DMark exercises full GPU feature set |
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
| RDNA 4 (9070 XT) achieves 8.2× better per-CU compute than RDNA 2 (6600 XT) at same 32 CU count | Higher clocks (1.08×) + architectural improvements in scheduler, cache hierarchy, and driver codegen account for the remaining ~7.5× |
| 9070 XT outperforms 80-CU RX 6900 XT in compute (0.033 vs 0.063 ms) despite 40% CU count | RDNA 4's per-CU efficiency leap is large enough to overcome the 2.5× CU disadvantage |
| API ranking on 9070 XT (DX11 ≈ Vulkan > DX12 > OpenGL) differs from RTX 5090 (DX11 >> DX12 > Vulkan > OpenGL) | AMD's DX11 driver is less optimised than NVIDIA's; the gap between explicit and implicit APIs is much smaller on AMD |
| OpenGL compute overhead persists on RDNA 4 (~2.6 ms) at similar levels to RDNA 2 (~2.7 ms) | AMD's OpenGL-to-Vulkan translation layer has not improved compute dispatch overhead across generations |
| 9070 XT compute scales 59× for 16× particle increase (1M → 16M) | Super-linear: at 1M particles GPU is underutilised, per-dispatch overhead dominates; at 16M the ALUs and bandwidth are fully saturated |
| TeraScale 2 VLIW5 achieves only ~50–70% of theoretical per-SP throughput | VLIW5 slot packing inefficiency in compute shaders with irregular control flow |
| All APIs converge to 0.034 ms compute in headless mode on 9070 XT | Proves GPU compute hardware is identical across APIs; windowed differences are entirely presentation/driver overhead |

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
| Fast GPUs report inflated "render time" in windowed mode | Vulkan's `COLOR_ATTACHMENT_OUTPUT_BIT` timestamp includes swapchain semaphore wait; on fast GPUs (9070 XT), ~90% of reported render time is idle wait |
| Increasing swapchain BufferCount does not fix timestamp pollution | The semaphore wait is inherent to the Vulkan presentation model, independent of buffer pool size |
| Headless mode achieves 10× FPS over windowed mode | Removing swapchain/render/present eliminates presentation overhead; all APIs converge to ~0.034 ms compute time |
| DX11 headless requires workarounds for timestamp queries | Without `Present()` as a frame boundary, DX11's timestamp pipeline produces garbage values; sanity filtering discards ~3–4% of samples |
| OpenGL headless requires periodic `glFinish()` on AMD | AMD's driver doesn't actively process commands for hidden windows; periodic full sync every 16 frames restores timestamp availability |
| 3DMark Unlimited ≠ headless compute | 3DMark Unlimited renders offscreen (full pipeline); this benchmark's headless mode skips rendering entirely (compute only) |
| RX 9070 XT (RDNA 4) has 8.2× better per-CU compute than RX 6600 XT (RDNA 2) | Higher clocks (1.08×) account for only a fraction; the rest is architectural improvement in scheduler, cache, and driver codegen |
| 9070 XT compute scales 59× for 16× particles (1M → 16M) | Super-linear scaling due to GPU underutilisation at 1M; at 16M the GPU is fully saturated |
| 9070 XT headless achieves 21K FPS across all APIs | All APIs converge to 0.034 ms compute; windowed mode's 1.7K FPS is 12× slower due to presentation overhead |

These results demonstrate that **API overhead, memory placement, and hardware architecture** all significantly affect GPU compute performance — and that the optimal configuration depends on workload complexity and hardware topology.

---

## 15. OpenGL Compute Shader Performance on AMD GPUs

### Observation

OpenGL compute shader performance on AMD GPUs is significantly lower than Vulkan / DX12 / DX11, with older architectures affected most severely.

| GPU | Architecture | OpenGL Compute ms | Vulkan Compute ms | Ratio |
|-----|-------------|-------------------|-------------------|-------|
| RTX 5090 (reference) | Blackwell | 0.019 | 0.019 | 1.0× |
| Radeon Graphics (iGPU) | RDNA 2 | 1.489 | 0.758 | 2.0× |
| RX 9070 XT | RDNA 4 | 2.612 | 0.033 | 79.2× |
| RX 6900 XT | RDNA 2 | 2.742 | 0.184 | 14.9× |
| RX 6600 XT | RDNA 2 | 2.719 | 0.240 | 11.3× |
| Vega Frontier Edition | Vega (GCN 5) | 3.321 | 0.368 | 9.0× |
| RX 580 | Polaris (GCN 4) | 18.913 | 0.362 | 52.3× |

On NVIDIA, OpenGL and Vulkan compute times are nearly identical. On AMD, OpenGL compute is 9–52× slower depending on architecture generation.

### FPS Impact

| GPU | OpenGL FPS | Vulkan FPS | DX11 FPS |
|-----|-----------|------------|----------|
| RX 9070 XT | 253 | 1,751 | 1,774 |
| RX 6900 XT | 229 | 2866 | 4107 |
| RX 6600 XT | 180 | — | — |
| RX 580 | 42 | 783 | 755 |

The RX 580's OpenGL score (42 FPS) is lower than the Ryzen 5 7600 CPU-based WARP software renderer running DX11 (44–53 FPS).

### Root Cause Analysis

This is a well-documented AMD Windows OpenGL driver limitation, not a code issue:

- **Same code, different results**: The identical OpenGL compute path achieves 2062 FPS on RTX 5090 (GPU time 0.019 ms), confirming the shader and API usage are correct.
- **Observed per-dispatch overhead**: On all GCN/RDNA GPUs tested in this benchmark, OpenGL `glDispatchCompute` exhibits a consistent overhead of ~2.7 ms (RDNA 2) to ~18.9 ms (GCN 4), independent of GPU compute capability — the RX 6900 XT and RX 6600 XT show nearly identical compute times despite having 80 vs 32 CUs. While AMD's general OpenGL performance issues are well-documented (see below), **specific quantification of per-dispatch compute overhead does not appear to have been published elsewhere** — this benchmark may be the first to isolate and measure it. This is demonstrated by the following comparison:

  | Metric | HD 5770 OpenGL | RX 6600 XT OpenGL | RX 6600 XT Vulkan |
  |--------|---------------|-------------------|-------------------|
  | Compute | 1.794 ms | 2.719 ms | 0.270 ms |
  | Render | 3.018 ms | 2.322 ms | 0.379 ms |
  | Total GPU | 4.818 ms | 5.148 ms | 0.649 ms |
  | FPS | 188 | 180 | 1239 |

  The RX 6600 XT's Vulkan compute time (0.270 ms) proves the hardware is 10× faster than what OpenGL reports (2.719 ms). The ~2.7 ms figure appears to be a **driver-level overhead floor** observed consistently across all GCN/RDNA hardware tested, rather than a reflection of GPU capability. The HD 5770 — a vastly weaker GPU from 2009 — achieves lower OpenGL compute times (1.794 ms) and higher FPS (188 vs 180) than the RX 6600 XT simply because it uses a completely different TeraScale driver stack that does not exhibit this overhead.

- **Known industry issue**: Multiple major projects and community reports have documented AMD's OpenGL performance gap on Windows:
  - RPCS3 (PS3 emulator) filed [issue #11197 "radeon: Poor state of Windows OpenGL drivers"](https://github.com/RPCS3/rpcs3/issues/11197), describing AMD's OpenGL performance as "disastrous."
  - PCSX2 (PS2 emulator) documented the problem in their wiki: ["OpenGL and AMD GPUs - All you need to know"](https://github.com/PCSX2/pcsx2/wiki/OpenGL-and-AMD-GPUs---All-you-need-to-know), noting OpenGL runs 10–70% slower compared to Direct3D on AMD GPUs.
  - The Khronos Community Forums contain threads reporting [execution differences between NVIDIA and AMD GPUs in compute shaders](https://community.khronos.org/t/difference-between-execution-compute-shader-on-nvidia-and-amd-gpu/110679), with RX 580 and RX 5700 exhibiting issues during `dispatchCompute`.
  - [Tom's Hardware Forum](https://forums.tomshardware.com/threads/amd-opengl-performance-is-horrible.3578989/) and [AMD Community](https://community.amd.com/t5/pc-drivers-software/poor-opengl-performance/td-p/528535) have numerous user reports of poor AMD OpenGL performance.
  - AMD acknowledged the issue and [rewrote their OpenGL driver to internally translate calls to Vulkan](https://www.neowin.net/news/amds-windows-11-22h2-wddm-31-driver-finally-fixes-radeons-poor-opengl-performance/) (Adrenalin 22.7.1 / WDDM 3.1), achieving [up to 55% improvement in Unigine Valley and 79% in Minecraft](https://www.neowin.net/news/amd-2271-driver-has-major-opengl-optimizations-and-windows-11-22h2-support/). [VideoCardz reported up to 92% improvement in Minecraft Java Edition](https://videocardz.com/newz/amds-new-driver-features-noise-suppression-technology-and-up-to-92-better-opengl-performance-in-minecraft), and [independent benchmarks by Nemez](https://nemez.net/posts/20220805-radeon-new-opengl-benchmarked-minecraft/) and [OC3D](https://overclock3d.net/reviews/software/amd_radeon_22_7_1_opengl_optimisations_tested_huge_gains/) confirmed AMD even surpassed NVIDIA in some Minecraft scenarios after the driver update.
  - **However, the translation layer primarily optimises the rendering path (draw calls), not compute dispatch.** The applications that saw 79–92% improvements — Minecraft, Unigine — are rendering-bound workloads dominated by vertex/fragment shaders and draw calls. Our benchmark's bottleneck is in `glDispatchCompute`, a relatively niche OpenGL usage pattern. This explains why even on the latest AMD drivers (Adrenalin 26.3.1, tested on the RX 6600 XT), the OpenGL compute overhead floor of ~2.7 ms persists — the translation layer offers limited benefit for this code path. Applications requiring GPU compute have largely migrated to Vulkan, DX12, or CUDA, reducing the incentive for AMD to optimise OpenGL compute dispatch specifically.
- **GCN/Polaris most affected**: Since Adrenalin 23.9, AMD [moved GCN (Polaris/Vega) to a maintenance-only driver branch](https://it.slashdot.org/story/23/11/09/1820227/amd-begins-polaris-and-vega-gpu-retirement-process-reduces-ongoing-driver-support) with no new performance optimisations. The OpenGL-to-Vulkan translation layer improvements may not have been fully applied to these legacy architectures, explaining the extreme 52× gap on the RX 580.
- **TeraScale counterexample**: The HD 5770 (TeraScale 2 / Evergreen) uses a completely different, legacy driver stack and does **not** exhibit this OpenGL compute overhead. As shown in the table above, it outperforms GCN/RDNA 2 cards in OpenGL despite being vastly inferior hardware. According to [Chips and Cheese's architectural analysis](https://chipsandcheese.com/p/gcn-amds-gpu-architecture-modernization), GCN completely rewrote AMD's GPU architecture and driver stack from TeraScale's VLIW design to a scalar SIMD model — meaning the OpenGL driver codebases are entirely separate. The overhead problem was introduced in the GCN/RDNA driver branch, not inherited from TeraScale. Notably, the HD 5770 shows the **opposite** pattern on DX11: compute + render = 3.87 ms, but total GPU time = 9.15 ms — a 5.3 ms synchronisation overhead between the compute and render stages, likely due to immature compute shader support on this early DX11-era architecture.

### Can This Be Optimised at the Application Level?

The OpenGL compute path in this benchmark is already minimal — one `glDispatchCompute` call and one `glMemoryBarrier` per frame. There is no room to reduce dispatch frequency, batch operations, or eliminate synchronisation. Alternative approaches such as `glDispatchComputeIndirect` or persistent mapped buffers do not address the bottleneck, as the overhead originates within the driver's internal dispatch path, not in data transfer or API call volume.

This suggests that **when the performance bottleneck is a driver-level fixed cost, application-level optimisation cannot break through the ceiling**. The only effective solution is to use an API that avoids this overhead entirely — Vulkan achieves 0.270 ms for the same compute workload that takes 2.719 ms through OpenGL on the same hardware (RX 6600 XT), a 10× improvement with identical shader logic.

### Why Did Minecraft See 79–92% Improvement but This Benchmark Did Not?

AMD's OpenGL-to-Vulkan translation layer excels at optimising **high-volume rendering workloads**. Minecraft Java Edition issues thousands of draw calls per frame (blocks, entities, particles, UI), each carrying CPU-side overhead for state changes and submission. The translation layer batches these into efficient Vulkan command buffers, dramatically reducing the per-call cost — e.g., 1000 draw calls × 0.1 ms overhead each = 100 ms, batched down to a few grouped submissions at ~10 ms total.

This benchmark has the opposite profile: **one single `glDispatchCompute` call per frame** with a ~2.7 ms observed driver overhead. The translation layer's batching strategy cannot help here — there is nothing to batch. The overhead appears to be a per-dispatch fixed cost within the driver, not an accumulation of many small costs that can be amortised. This is why Minecraft saw up to 92% improvement while our compute workload on the same latest drivers (Adrenalin 26.3.1) shows no meaningful change.

> **Note:** The Khronos Community Forums contain a [thread on `glDispatchCompute` calling overhead](https://community.khronos.org/t/gldispatchcompute-calling-overhead/71774), but that discussion reports ~0.2 ms overhead on older NVIDIA GPUs (GTX 560/470), not AMD. The ~2.7 ms overhead observed in this benchmark on AMD GCN/RDNA hardware is an order of magnitude larger and does not appear to have been specifically documented elsewhere.

### GTX 970 (Maxwell): DX11 Compute–Render Synchronisation Overhead

The GTX 970 exhibits a striking reversal in API performance ranking compared to the RTX 5090:

| API | Compute | Render | Compute+Render | **Total GPU** | FPS | Bottleneck |
|-----|---------|--------|----------------|---------------|-----|------------|
| Vulkan | 0.434 ms | 0.663 ms | 1.097 ms | **1.098 ms** | 718.8 | Balanced |
| OpenGL | 0.431 ms | 0.791 ms | 1.222 ms | **1.226 ms** | 642.2 | Balanced |
| DX12 | 0.443 ms | 0.535 ms | 0.978 ms | **0.977 ms** | 291.1 | CPU-bound |
| DX11 | 0.435 ms | 0.873 ms | 1.308 ms | **3.355 ms** | 280.3 | GPU-bound |

On the RTX 5090, DX11 is the fastest API (7736 FPS). On the GTX 970, it is the **slowest** (280 FPS). The cause is visible in the numbers:

- **DX11**: Compute + render sum to only 1.308 ms, but total GPU time is 3.355 ms — a **~2 ms synchronisation overhead** between the compute and render stages. Maxwell's DX11 driver appears to insert a costly pipeline flush/barrier when transitioning from compute dispatch to draw calls. The RTX 5090 (Blackwell) shows no such overhead (compute + render ≈ total GPU time).
- **DX12**: GPU time is actually the fastest (0.977 ms), but FPS is only 291.1 — a massive **2.5 ms CPU overhead** (frame time 3.435 ms − GPU 0.977 ms). This reflects the high CPU-side cost of DX12 command recording on an older driver/architecture combination.
- **Vulkan**: Best overall balance — GPU time 1.098 ms with only 0.293 ms CPU overhead. The explicit API model with pre-recorded command buffers works well even on older hardware.
- **OpenGL**: NVIDIA's OpenGL driver performs well (unlike AMD), with GPU time 1.226 ms and minimal CPU overhead.

This demonstrates that **API performance rankings are not universal — they depend on GPU architecture and driver maturity**. DX11's implicit driver model excels on modern NVIDIA hardware (where the driver has been refined over a decade) but introduces overhead on older architectures where compute–render transitions were not as optimised. The RX 9070 XT (RDNA 4) shows yet another pattern: DX11 ≈ Vulkan ≈ DX12, with only OpenGL significantly behind — AMD's DX11 advantage over explicit APIs is negligible compared to NVIDIA's.

### Pre-Compute-Shader GPUs: Why the GT 120 / 9500 GT Cannot Run This Benchmark

During testing, a GeForce 9500 GT (G96, 2008) was detected with only **DX11 API at Feature Level 10_0**. The benchmark failed immediately:

```
Feature Level: 10_0
FAILED: CreateComputeShader failed
```

Compute shaders require **Feature Level 11_0** (DX11-class hardware). The 9500 GT, despite having a **unified shader architecture** (introduced with G80 / GeForce 8800 in 2006), lacks the hardware features that compute shaders need:

- **UAV (Unordered Access Views)**: random read/write to arbitrary buffer addresses
- **Thread Group Shared Memory**: fast on-chip memory shared between threads in a workgroup
- **Atomic operations**: thread-safe read-modify-write to shared data
- **Independent dispatch**: ability to execute work outside the rendering pipeline

The evolution of GPU programmability, rendering pipeline, and Shader Model follows a clear progression:

| Era | DirectX | Shader Model | Pipeline | Key Capability | NVIDIA Example | AMD Example |
|-----|---------|-------------|----------|---------------|----------------|-------------|
| Fixed-function | DX5–7 | — | Fixed T&L | Parameters only, no programmable shaders | GeForce 256 (1999) | Radeon DDR (2000) |
| Early programmable | DX8 | SM 1.x | Programmable VS/PS (dedicated HW) | Simple vertex/pixel shaders, limited instructions | GeForce 3 (2001) | Radeon 8500 (2001) |
| Full programmable | DX9 | SM 2.0 | Programmable VS/PS (dedicated HW) | Branching, longer programs, FP32 pixel shaders | GeForce FX (2003) | Radeon 9700 (2002) |
| Advanced shaders | DX9.0c | SM 3.0 | Programmable VS/PS (dedicated HW) | Dynamic branching, HDR, vertex texture fetch | GeForce 6800 (2004) | Radeon X1800 (2005) |
| Unified + geometry | DX10 | SM 4.0 | **Unified architecture** + geometry shader | VS/PS/GS share same ALUs, stream output | GeForce 8800 (2006) | Radeon HD 2900 (2007) |
| Compute shaders | DX10.1 | SM 4.1 | Unified + limited compute | Gather4, MSAA improvements | GeForce GT 120 (2008) | Radeon HD 3870 (2007) |
| Full compute | **DX11** | **SM 5.0** | **Unified + compute shader** | UAV, thread group shared memory, atomics, tessellation | GeForce GTX 480 (2010) | Radeon HD 5870 (2009) |
| Explicit APIs | DX12 / Vulkan | SM 5.1 / SPIR-V | Explicit command recording | Multi-queue, async compute, low CPU overhead | GeForce GTX 900+ (2014) | Radeon GCN 1.0+ (2012) |
| Mesh shaders | DX12 Ultimate | SM 6.5+ | Mesh + amplification shaders | Mesh shading, ray tracing, variable rate shading | GeForce RTX 20+ (2018) | Radeon RX 6000+ (2020) |

The **9500 GT** (SM 4.0, DX10 Feature Level 10_0) sits at the "unified architecture" stage. While its shader processors can flexibly run vertex, pixel, or geometry shaders on the same ALUs, the hardware predates the memory access model (UAV, shared memory, atomics) and independent dispatch mechanism required by compute shaders (SM 5.0 / Feature Level 11_0).

Note: NVIDIA's proprietary **CUDA** (2007) brought general-purpose compute to unified architectures two years before DX11 standardised compute shaders. The G80 (GeForce 8800) could run CUDA compute workloads despite being a DX10/SM 4.0 GPU — CUDA bypasses the graphics API entirely and accesses the hardware directly. However, this benchmark targets cross-vendor standards (Vulkan, DX12, DX11, OpenGL) rather than vendor-specific APIs.

Even if a pixel-shader fallback were implemented to simulate compute on FL 10.0 hardware, the results would be **meaningless for comparison**: the workload would run through the rendering pipeline (rasterisation → fragment output) rather than as a true compute dispatch, measuring an entirely different code path with different bottlenecks. The benchmark's value lies in comparing the **same compute workload** across APIs and hardware — a pixel-shader workaround would break that comparability.

#### DX11 API vs. DX11 Hardware: A Common Misconception

A point of confusion encountered during testing: Windows' `dxdiag` utility reports the 9500 GT as supporting "DirectX 11", yet it cannot run DX11-class workloads like compute shaders or 3DMark Fire Strike. This is because **DX11 API support ≠ DX11 hardware capability**:

- **dxdiag reports "DirectX 11"** = the Windows operating system has the **DX11 runtime** installed. This is a software component, not a hardware feature.
- **GPU actual capability** = **Feature Level 10_0** (DX10-class hardware). This is what the GPU silicon physically supports.

The DX11 API is **backwards-compatible by design**: it can create a device on any GPU from FL 9_1 upwards, but the available features are limited to what the hardware's Feature Level supports. This benchmark successfully creates a DX11 device on the 9500 GT (the device reports FL 10_0), but `CreateComputeShader` fails because compute shaders require FL 11_0.

```
D3D11CreateDevice()          → succeeds (DX11 API can drive FL 10_0 hardware)
CreateComputeShader()        → FAILS   (compute shaders need FL 11_0)
CreateVertexShader()         → succeeds (vertex shaders available since FL 9_1)
CreatePixelShader()          → succeeds (pixel shaders available since FL 9_1)
CreateGeometryShader()       → succeeds (geometry shaders available since FL 10_0)
CreateHullShader()           → would FAIL (tessellation needs FL 11_0)
```

This same distinction explains 3DMark compatibility: tests like **Fire Strike** (DX11) require FL 11_0 features (tessellation, compute-based post-processing) and will not run on the 9500 GT. Tests like **Cloud Gate** (DX10/FL 10_0) use only vertex, pixel, and geometry shaders, and run successfully. The "DX11" label in both cases refers to the API version used for rendering, not the minimum hardware requirement — which is determined by the Feature Level.

### Shader Model, Shading Languages, and the Rendering Pipeline

Three concepts are often conflated but serve distinct roles:

- **Rendering pipeline** — the GPU's processing stages (vertex → rasterisation → fragment → output, plus optional stages like geometry, tessellation, and compute). This defines **what stages exist** and how data flows between them.
- **Shader Model (SM)** — a hardware capability specification defining **what code can run** at each programmable stage. Higher SM versions unlock more instructions, longer programs, new memory access patterns, and new pipeline stages.
- **Shading languages** — the programming languages developers use to write shader code for each stage.

These three evolve together: a new pipeline stage (e.g., compute shader) requires new hardware capability (SM 5.0), which is then exposed through shading language features (HLSL `[numthreads]`, GLSL `layout(local_size_x)`).

#### Shading Languages and Compilation

Each graphics API defines its own shading language:

| Language | API | Compilation | Target |
|----------|-----|-------------|--------|
| **HLSL** (High-Level Shading Language) | DirectX 9–12 | `fxc` (SM 2.0–5.0) / `dxc` (SM 6.0+) | DXBC / DXIL bytecode |
| **GLSL** (OpenGL Shading Language) | OpenGL / Vulkan | `glslang` / `glslc` | OpenGL: driver compiles at runtime; Vulkan: pre-compiled to SPIR-V |
| **MSL** (Metal Shading Language) | Metal | Metal compiler | Apple IR |
| **SPIR-V** | Vulkan | Intermediate representation | Consumed by Vulkan driver, compiled to GPU-native ISA |

The compilation pipeline in this benchmark:

```
Vulkan:   GLSL (.comp/.vert/.frag)  ──→  glslc  ──→  SPIR-V (.spv)  ──→  Vulkan driver  ──→  GPU ISA
DX12:     HLSL (.hlsl)              ──→  fxc    ──→  DXBC bytecode   ──→  DX12 driver    ──→  GPU ISA
DX11:     HLSL (.hlsl)              ──→  fxc    ──→  DXBC bytecode   ──→  DX11 driver    ──→  GPU ISA
OpenGL:   GLSL (embedded strings)   ──→  driver compiles at runtime  ──→  GPU ISA
```

Despite using different languages, all backends compile down to the **same GPU instruction set** (ISA) for a given GPU. The shader logic is equivalent across all four backends — position update in the compute shader, point-sprite rendering in the vertex/fragment shaders. The only differences are syntax and API-specific boilerplate.

This is why cross-API comparisons in this benchmark are meaningful: the **same algorithm** runs through different API/driver paths to the same hardware, isolating the API and driver overhead from the shader workload itself.

#### Shader Model and Feature Level Mapping

The Shader Model version determines what a GPU can do, but different APIs expose this through different naming:

| SM | DX Feature Level | OpenGL Version | GLSL Version | Key Addition |
|----|-----------------|----------------|--------------|-------------|
| SM 2.0 | 9_1 – 9_3 | 2.1 | 120 | Basic VS/PS, FP32 |
| SM 3.0 | — | 3.0 | 130 | Dynamic branching |
| SM 4.0 | 10_0 | 3.3 | 330 | Unified shaders, geometry shader, integer ops |
| SM 4.1 | 10_1 | — | — | Gather4, MSAA read |
| SM 5.0 | **11_0** | **4.3** | **430** | **Compute shader, UAV, tessellation** |
| SM 5.1 | 11_1 / 12_0 | 4.5+ | 450 | Bindless-style resource indexing |
| SM 6.0+ | 12_0+ | — (Vulkan SPIR-V) | — | Wave intrinsics, ray tracing, mesh shaders |

This benchmark requires **SM 5.0 / Feature Level 11_0 / OpenGL 4.3** as the minimum — the point where compute shaders became a standard, cross-vendor feature. The 9500 GT's SM 4.0 / FL 10_0 hardware falls one generation short of this requirement.

### Conclusion

These results quantitatively demonstrate that **modern APIs (Vulkan, DX12) are essential for realising the full compute potential of AMD hardware**. The OpenGL compute path carries significant driver overhead on AMD, particularly on legacy architectures, reinforcing the industry trend towards explicit, low-overhead graphics APIs. Choosing the right API is more impactful than optimising application code when driver-level overhead dominates.

The RX 9070 XT (RDNA 4) is the most extreme example: OpenGL compute takes 2.612 ms vs Vulkan's 0.033 ms — a **79× penalty** — the largest ratio observed in any GPU tested. Despite being AMD's newest architecture, the OpenGL compute dispatch overhead has not improved from RDNA 2 levels (~2.7 ms), confirming that AMD's driver team has deprioritised OpenGL compute optimisation.

Additionally, API performance rankings are architecture-dependent: DX11 leads on modern NVIDIA GPUs but falls behind Vulkan on older Maxwell hardware due to compute–render synchronisation costs. On the RX 9070 XT, DX11 and Vulkan are nearly tied (1,774 vs 1,751 FPS), reflecting AMD's less mature DX11 optimisation path. This underscores that **no single API is universally optimal** — the best choice depends on the target hardware generation and driver maturity.

---

## 16. RX 9070 XT (RDNA 4) — Cross-API, Particle Scaling, and Headless Analysis

> AMD's first RDNA 4 discrete GPU, tested across three scenarios: standard 1M
> windowed, maximum 16M windowed, and headless compute. The 9070 XT provides a
> unique lens into swapchain throttling behaviour due to its very fast compute
> throughput relative to presentation overhead.

### Test Hardware

| Component | Specification |
|-----------|--------------|
| CPU | AMD Ryzen 5 7600 6-Core Processor |
| GPU | AMD Radeon RX 9070 XT (RDNA 4, 32 CU, 16 GB GDDR6, 512 GB/s) |
| Driver | AMD Adrenalin 26.3.1 (LLPC) |
| OS | Windows 11 (NT 10.0.26200) |
| Resolution | 1280 × 720 |
| V-Sync | OFF |
| Memory Mode | Device-local |

### 16a. Cross-API Comparison — 1M Particles (Medium), Windowed

| # | API | Avg FPS | Compute (ms) | Render (ms) | Total GPU (ms) | GPU Util | Bottleneck |
|---|-----|---------|-------------|------------|---------------|----------|------------|
| 1 | DX11 | 1,773.7 | 0.047 | 0.451 | 0.542 | 100% | GPU-bound |
| 2 | Vulkan | 1,750.6 | 0.033 | 0.408 | 0.446 | 80% | Balanced |
| 3 | DX12 | 1,608.7 | 0.034 | 0.399 | 0.434 | 70% | Balanced |
| 4 | OpenGL | 253.4 | 2.612 | 0.792 | 3.658 | 90% | GPU-bound |

**Key observations:**

- **DX11 and Vulkan are nearly tied** at ~1750 FPS, with DX11 slightly ahead. Unlike on RTX 5090 where DX11 dominates (8955 FPS vs 3611 Vulkan), the gap is much smaller on 9070 XT — AMD's DX11 driver is less optimised than NVIDIA's.
- **DX12 is slightly behind** at 1609 FPS despite having the lowest total GPU time (0.434 ms). CPU overhead pulls it below DX11/Vulkan.
- **OpenGL is severely penalised** — the AMD OpenGL compute overhead issue (Section 15) causes 2.612 ms compute time vs 0.033 ms on Vulkan, a **79× penalty**.
- **All APIs show inflated render times** (0.4–0.8 ms) due to swapchain semaphore wait pollution (detailed in Section 17). The actual render work is ~0.04 ms.

### 16b. Cross-API Comparison — 16M Particles (Ultra), Windowed

| # | API | Avg FPS | Compute (ms) | Render (ms) | Total GPU (ms) | GPU Util | Bottleneck |
|---|-----|---------|-------------|------------|---------------|----------|------------|
| 1 | Vulkan | 110.9 | 1.963 | 6.440 | 8.418 | 90% | GPU-bound |
| 2 | DX12 | 105.7 | 1.940 | 6.126 | 8.069 | 90% | GPU-bound |
| 3 | DX11 | 93.5 | 1.908 | 6.152 | 9.904 | 90% | GPU-bound |
| 4 | OpenGL | 15.7 | 47.959 | 10.073 | 58.473 | 90% | GPU-bound |

**16× particle scaling analysis (1M → 16M):**

| API | 1M Compute | 16M Compute | Scaling (expected 16×) | 1M FPS | 16M FPS | FPS Ratio |
|-----|-----------|-----------|----------------------|--------|---------|-----------|
| Vulkan | 0.033 | 1.963 | **59.5×** | 1,750.6 | 110.9 | 15.8× |
| DX12 | 0.034 | 1.940 | **57.1×** | 1,608.7 | 105.7 | 15.2× |
| DX11 | 0.047 | 1.908 | **40.6×** | 1,773.7 | 93.5 | 19.0× |
| OpenGL | 2.612 | 47.959 | **18.4×** | 253.4 | 15.7 | 16.1× |

- **Compute time scales super-linearly** (57–60× for 16× particles on Vulkan/DX12). This is expected: at 1M particles the GPU is underutilised and the per-dispatch overhead dominates; at 16M particles the ALUs and memory bandwidth are fully saturated.
- **FPS scales roughly linearly** (~16× reduction) because at 16M particles all APIs are GPU-bound — CPU overhead is negligible relative to GPU execution time.
- **DX11's compute–render gap widens**: total GPU (9.904 ms) exceeds compute + render sum (1.908 + 6.152 = 8.060 ms) by 1.844 ms, suggesting pipeline synchronisation overhead similar to what was observed on the GTX 970 (Section 15).
- **OpenGL's compute time explodes to 47.959 ms** — the AMD OpenGL dispatch overhead scales worse than linearly with particle count, making OpenGL completely impractical for high particle counts on AMD.
- **Render time dominates** at 16M: ~6 ms for Vulkan/DX12/DX11 vs ~0.4 ms at 1M. This is real render work (16M point sprites), not semaphore wait — at 16M particles the GPU is genuinely busy rendering, unlike at 1M where the semaphore wait dominated.

### 16c. Headless Compute — 1M Particles

| # | API | Avg FPS | Compute (ms) | Render (ms) | Total GPU (ms) | GPU Util | Bottleneck |
|---|-----|---------|-------------|------------|---------------|----------|------------|
| 1 | DX12 | 21,354.0 | 0.034 | 0.0 | 0.035 | 70% | Balanced |
| 2 | Vulkan | 21,259.6 | 0.034 | 0.0 | 0.034 | 70% | Balanced |
| 3 | OpenGL | 20,298.3 | 0.034 | 0.0 | 0.034 | 70% | Balanced |
| 4 | DX11 | 16,937.8 | 0.034 | 0.0 | 0.034 | 60% | Balanced |

**Windowed vs Headless comparison:**

| API | Windowed FPS | Headless FPS | Speedup | Windowed Compute | Headless Compute |
|-----|-------------|-------------|---------|-----------------|-----------------|
| Vulkan | 1,750.6 | 21,259.6 | **12.1×** | 0.033 ms | 0.034 ms |
| DX12 | 1,608.7 | 21,354.0 | **13.3×** | 0.034 ms | 0.034 ms |
| DX11 | 1,773.7 | 16,937.8 | **9.5×** | 0.047 ms | 0.034 ms |
| OpenGL | 253.4 | 20,298.3 | **80.1×** | 2.612 ms | 0.034 ms |

- **All four APIs converge to identical compute time (0.034 ms)** in headless mode, proving the GPU compute hardware is equivalent regardless of API.
- **OpenGL's 80× speedup** is the most dramatic — headless mode bypasses the AMD OpenGL compute dispatch overhead entirely, as the dispatch path is simpler without a rendering context.
- **DX11 compute drops from 0.047 to 0.034 ms** in headless, suggesting the DX11 driver's implicit state management adds ~0.013 ms overhead even to compute dispatch when a swapchain is present.
- The remaining FPS differences (21K vs 17K for DX11) reflect pure CPU-side overhead differences between APIs.

### 16d. Flights Test — 1M Particles, Windowed (2 vs 3 Frames-in-Flight)

| API | Flights=2 FPS | Flights=3 FPS | Change | Flights=2 Render | Flights=3 Render |
|-----|-------------|-------------|--------|-----------------|-----------------|
| Vulkan | 1,750.6 | 1,736.9 | −0.8% | 0.408 ms | 0.409 ms |
| DX12 | 1,608.7 | 1,964.5 | **+22.1%** | 0.399 ms | 0.400 ms |
| DX11 | 1,773.7 | 1,956.3 | +10.3% | 0.451 ms | 0.399 ms |
| OpenGL | 253.4 | 256.1 | +1.1% | 0.792 ms | 0.781 ms |

- **DX12 benefits most** from an extra frame-in-flight (+22%), suggesting its command pipeline can overlap more work with 3 buffers.
- **Vulkan shows no improvement** — its presentation engine already manages buffering efficiently at 2 frames.
- **Render times remain unchanged** across both flight counts, confirming that swapchain semaphore wait pollution is not reduced by adding more swapchain images (as discussed in Section 17d).

### 16e. RX 9070 XT vs Other GPUs — 1M Particles, Vulkan

| GPU | Architecture | Compute (ms) | Render (ms) | Total GPU (ms) | FPS |
|-----|-------------|-------------|------------|---------------|-----|
| RTX 5090 | Blackwell (170 SM) | 0.025 | 0.064 | 0.090 | 3,611 |
| **RX 9070 XT** | **RDNA 4 (32 CU)** | **0.033** | **0.408** | **0.446** | **1,751** |
| RX 6900 XT | RDNA 2 (80 CU) | 0.063 | 0.134 | 0.197 | 2,866 |
| RX 6600 XT | RDNA 2 (32 CU) | 0.270 | 0.379 | 0.649 | 1,239 |
| Vega FE | GCN 5 (64 CU) | 0.368 | — | — | — |
| RX 580 | GCN 4 (36 CU) | 0.362 | — | — | — |

**Compute performance ranking** (lower is better):

| GPU | Compute (ms) | vs RX 9070 XT | Per-CU Efficiency vs 9070 XT |
|-----|-------------|--------------|------------------------------|
| RTX 5090 | 0.025 | 0.76× | N/A (different arch) |
| **RX 9070 XT** | **0.033** | **1.00×** | **1.00×** |
| RX 6900 XT | 0.063 | 1.91× | 0.21× (80 CU / 0.063 ms → much lower per-CU) |
| RX 6600 XT | 0.270 | 8.18× | 0.12× (same 32 CU, 8× slower → RDNA 4 >> RDNA 2 per-CU) |
| RX 580 | 0.362 | 10.97× | 0.08× (36 CU) |

The 9070 XT demonstrates **8.2× better compute throughput per CU than the 6600 XT** (same 32 CU count), showcasing RDNA 4's architectural improvements over RDNA 2:
- Higher clock speed (2,805 MHz vs 2,589 MHz) accounts for only ~1.08×
- The remaining **~7.5× improvement** comes from architectural changes: improved compute scheduler, better cache hierarchy, wider memory interface utilisation, and mature RDNA 4 driver code generation

The 9070 XT even outperforms the 80-CU RX 6900 XT in compute (0.033 vs 0.063 ms) despite having only 40% of its CU count, making it the fastest AMD compute GPU tested in this benchmark.

---

## 17. Swapchain Throttling, Timestamp Pollution, and Headless Compute

### 16a. The Problem: Why Fast GPUs Appear Slower Than Expected

During windowed benchmark runs, the RX 9070 XT exhibited an unexpected anomaly: despite computing 2× faster than the RX 6900 XT, its reported **render time was higher**, resulting in lower-than-expected total GPU time efficiency.

| GPU | Compute (ms) | Render (ms) | Total GPU (ms) | FPS |
|-----|-------------|------------|---------------|-----|
| RTX 5090 | 0.025 | 0.064 | 0.090 | 3,611 |
| RX 6900 XT | 0.063 | 0.134 | 0.197 | 2,866 |
| **RX 9070 XT** | **0.033** | **0.408** | **0.441** | 1,981 |

The 9070 XT's render time (0.408 ms) is 3× that of the 6900 XT (0.134 ms), despite rendering the same single draw call of point sprites. Investigation revealed this is not a GPU performance issue but a **measurement artefact caused by swapchain semaphore wait pollution in timestamps**.

### 16b. Root Cause: Semaphore Wait in Vulkan Timestamps

In the Vulkan backend, timestamp T3 is written at `VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT`. This pipeline stage does not begin until the presentation engine releases a swapchain image — signalled via the `imageAvailableSemaphore` acquired from `vkAcquireNextImageKHR`.

```
T2 (TOP_OF_PIPE)  ──→  Vertex Shader  ──→  Rasterisation  ──→  Fragment Shader
                                                                      │
                                                        ┌─────────────┘
                                                        ▼
                                             COLOR_ATTACHMENT_OUTPUT
                                             ┌──────────────────────┐
                                             │  Wait for semaphore  │ ← swapchain image availability
                                             │  (variable delay)    │
                                             │  Actual pixel write  │
                                             └──────────────────────┘
                                                        │
                                                        ▼
                                                   T3 (timestamp)
```

The **render time** (T3 − T2) therefore measures: `actual render work + semaphore wait for swapchain image`. On fast GPUs that finish compute + render in under 1 ms, the GPU spends most of its time **idle, waiting for the presentation engine to recycle a swapchain image** from the previous frame's `vkQueuePresentKHR`.

### 16c. Why Different GPUs Are Affected Differently

| GPU | Situation | Effect on Render Timestamp |
|-----|-----------|---------------------------|
| **RTX 5090** | CPU-bound (30% GPU util). GPU finishes early, waits for next CPU submission. Semaphore wait is hidden within CPU stall. | Minimal pollution — GPU idle time is between frames, not during render stage |
| **RX 6900 XT** | Balanced. Compute takes long enough (0.063 ms) that the presentation engine has time to release the swapchain image before render stage begins. | Low pollution — semaphore is usually already signalled |
| **RX 9070 XT** | Compute finishes very fast (0.033 ms), reaches `COLOR_ATTACHMENT_OUTPUT` before the presentation engine has released the image from the previous present. GPU stalls waiting for semaphore. | **High pollution** — ~0.37 ms of the 0.408 ms "render time" is semaphore wait |

The 9070 XT's actual render work is approximately **0.04 ms** (similar to other GPUs), but the timestamp reports 0.408 ms because it includes the swapchain image wait.

### 16d. Swapchain BufferCount vs VSync

Two frequently confused concepts that both affect frame pacing:

| | Swapchain BufferCount | VSync |
|--|----------------------|-------|
| **What it controls** | How many swapchain images exist in the pool | When completed frames are shown on the display |
| **Set by** | `VkSwapchainCreateInfoKHR::minImageCount` (Vulkan), `DXGI_SWAP_CHAIN_DESC::BufferCount` (DX11/12) | `glfwSwapInterval()` (OpenGL), `Present(syncInterval)` (DX), `VK_PRESENT_MODE_*` (Vulkan) |
| **Effect on FPS** | More buffers → less GPU idle time waiting for image availability, but diminishing returns beyond 3 | VSync ON → FPS capped to display refresh rate; VSync OFF → uncapped |
| **Effect on input lag** | More buffers → higher input lag (more pre-rendered frames queued) | VSync ON → adds up to one frame of latency |

Increasing `BufferCount` from 2 to 3 was tested (via `--flights 3`) but showed **no meaningful improvement** in timestamp pollution. The semaphore wait is inherent to the Vulkan presentation model — the timestamp at `COLOR_ATTACHMENT_OUTPUT_BIT` will always include the wait, regardless of how many images are in the pool, because the GPU must still wait for at least one image to become available from the presentation engine.

### 16e. Headless Compute Mode — Eliminating Presentation Overhead

To measure pure GPU compute performance without any swapchain, rendering, or presentation interference, a **headless compute mode** (`--headless`) was implemented:

| Component | Windowed Mode | Headless Mode |
|-----------|--------------|---------------|
| Window | GLFW visible window | No window (OpenGL: hidden window for context) |
| Swapchain | Created, images acquired/presented | Not created |
| Render pass | Full vertex + fragment pipeline | Skipped entirely |
| Present | `vkQueuePresentKHR` / `Present()` / `glfwSwapBuffers` | Skipped |
| Timestamps | T0–T3 (compute + render) | T0–T1 (compute only), T2=T3 mirrored |
| GPU utilisation | Limited by presentation engine | Limited only by compute throughput |

#### Headless Results — RX 9070 XT, 1M Particles

| API | FPS | Compute (ms) | GPU Util |
|-----|-----|-------------|----------|
| Vulkan | 21,260 | 0.034 | 70% |
| DX12 | 21,354 | 0.034 | 70% |
| DX11 | 16,938 | 0.034 | 60% |
| OpenGL | 20,298 | 0.034 | 70% |

All four APIs converge to nearly identical compute times (0.034 ms), confirming that the GPU-side compute workload is equivalent across APIs. The FPS difference reflects only **CPU-side overhead** — DX11 is slightly slower due to its implicit driver model requiring more CPU work per frame without a Present() call to batch around.

Compare with windowed mode:

| API | Windowed FPS | Headless FPS | Speedup |
|-----|-------------|-------------|---------|
| Vulkan | 1,981 | 21,260 | **10.7×** |
| DX12 | 2,100 | 21,354 | **10.2×** |
| DX11 | 3,500 | 16,938 | **4.8×** |
| OpenGL | 1,800 | 20,298 | **11.3×** |

The 10× speedup confirms that windowed mode performance is **dominated by presentation overhead**, not by compute or render workload.

### 16f. API-Specific Headless Implementation Challenges

#### DX11: No Frame Boundary Without Present()

DX11's implicit driver model uses `Present()` as an implicit frame boundary for command batching and timestamp query resolution. Without it:

- **Problem 1**: `CollectTimestampResults()` used `Sleep(1)` retries waiting for query resolution. Without Present(), queries never resolved promptly, causing each frame to take 4+ ms (Sleep granularity).
- **Fix**: Removed Sleep in headless mode; spin-wait only.
- **Problem 2**: Even with spin-wait, timestamp values were occasionally garbage (e.g., 805534675707 ms) because DX11 lacks proper frame boundaries without Present().
- **Fix**: Added `context_->Flush()` after compute dispatch to force command submission, plus sanity filter discarding timestamps > 1000 ms. Approximately 3–4% of frames produce garbage values and are discarded (e.g., 212531/220193 valid samples).

#### OpenGL: AMD Driver Requires Explicit Flush for Hidden Windows

OpenGL requires a window (even hidden) to create a GL context. On AMD drivers, `glFlush()` alone is insufficient to process commands for hidden windows — the driver does not actively schedule GPU work without a visible surface.

- **Attempt 1**: `glFlush()` only → timestamps never resolve (0 valid samples).
- **Attempt 2**: `glFinish()` every frame → timestamps work, but FPS drops from 22,000 to 8,000 (CPU stalls waiting for GPU).
- **Final solution**: `glFinish()` every 16th frame (forces command processing) + `glFenceSync` + `glFlush()` on other frames (non-blocking). This achieves 20,298 FPS with ~25% timestamp sample rate (65985/263889 valid samples).

#### Vulkan and DX12: Clean Headless

Both explicit APIs handle headless cleanly:
- Skip swapchain, render pass, and present calls
- Compute dispatch + fence sync is sufficient
- 100% timestamp sample rate, no workarounds needed

### 16g. Comparison with 3DMark Unlimited Mode

3DMark offers an **Unlimited** mode that removes VSync and frame rate caps. This is often confused with headless compute, but they are fundamentally different:

| | 3DMark Unlimited | This Benchmark Headless |
|--|-----------------|------------------------|
| **Rendering** | Full offscreen rendering (all geometry, textures, post-FX) | **No rendering** — compute dispatch only |
| **Target** | Offscreen render target (no swapchain present) | No render target at all |
| **Measures** | Combined compute + render + post-processing GPU throughput, uncapped | Pure compute shader throughput |
| **Presentation** | Skipped (no VSync, no Present) | Skipped |
| **Use case** | Cross-device comparison without display refresh rate bias | Isolating compute performance from presentation overhead |
| **Analogy** | Running the full game engine but rendering to a texture instead of screen | Running only the physics engine with no rendering at all |

3DMark Unlimited is equivalent to **rendering to an offscreen framebuffer** — the full GPU pipeline (vertex → rasterisation → fragment → post-processing) executes, but the final present/flip is skipped. This benchmark's headless mode is more aggressive: it **eliminates the entire graphics pipeline**, measuring only the compute dispatch that updates particle positions.

If a 3DMark Unlimited-style mode were added to this benchmark, it would involve:
1. Creating an offscreen framebuffer (VkFramebuffer / ID3D11RenderTargetView / FBO)
2. Running the full compute + render pipeline to that framebuffer
3. Skipping only `vkQueuePresentKHR` / `Present()` / `glfwSwapBuffers`
4. Timestamp T3 would measure actual render completion without semaphore wait pollution

This would provide a middle ground between windowed (presentation-throttled) and headless (compute-only) modes, and would be the most direct comparison point with 3DMark Unlimited scores.

---

## Appendix: Dual Identical GPU Behaviour (Mac Pro 2013 — 2× FirePro D700)

The Mac Pro (Late 2013) contains two identical AMD FirePro D700 GPUs (GCN 1.0, Tahiti XT). Testing under Windows 11 via Boot Camp revealed notable differences in how each graphics API handles multi-GPU selection for identical cards:

### Per-API GPU Addressability

| API | Sees both GPUs? | Can run on GPU #2 independently? |
|-----|----------------|----------------------------------|
| Vulkan | Yes (2 `VkPhysicalDevice`) | **Yes** — Task Manager confirms GPU #2 load |
| DirectX 12 | Yes (2 DXGI adapters, distinct LUIDs) | **No** — driver routes work to GPU #1 |
| DirectX 11 | Yes (2 DXGI adapters, distinct LUIDs) | **No** — driver routes work to GPU #1 |
| OpenGL | No (single context, OS-assigned GPU) | No |

### Key Findings

- **DXGI enumerates both D700s with different LUIDs**, and both report DX12 Feature Level 11_1 support. Creating a D3D12/D3D11 device on either adapter succeeds. However, **the AMD driver routes all DX11/DX12 compute and rendering work to GPU #1** regardless of which adapter was selected. Windows Task Manager shows zero utilisation on GPU #2 during DX11/DX12 benchmarks targeting the second adapter.

- **Only Vulkan can genuinely dispatch work to GPU #2.** When the benchmark selects `VkPhysicalDevice[1]`, Task Manager confirms GPU #2 shows compute and 3D load while GPU #1 remains idle (aside from display output).

- **This is an AMD driver limitation, not a DX11/DX12 API limitation.** The DX11/DX12 APIs fully support multi-GPU independent addressing — DXGI enumerates both adapters with distinct LUIDs, and `D3D12CreateDevice` / `D3D11CreateDevice` succeed on both. The API layer does its job correctly. However, the AMD Windows driver internally routes all DX11/DX12 work to the primary GPU regardless of which adapter was selected. AMD's own Vulkan driver on the same hardware correctly dispatches to GPU #2, proving the hardware is capable. The FirePro D700's Windows driver has been EOL since March 2019, so this behaviour is unlikely to ever be fixed. This is also consistent with 3DMark behaviour: 3DMark Time Spy cannot select between identical GPUs on this system either.

- **Performance is near-identical between the two cards** when properly addressed via Vulkan: GPU #1 averages ~554 FPS while GPU #2 averages ~448 FPS. The ~20% gap is likely due to GPU #1 handling display output overhead being offset by its position as the "primary" adapter, or minor thermal/power delivery asymmetry in the Mac Pro chassis.

### Technical Details

The benchmark uses **DXGI adapter LUID** (Locally Unique Identifier) to match GPUs across different DXGI factory instances. This was necessary because:
1. DXGI `EnumAdapters1` may return different adapter counts or ordering across factory instances (e.g., the DX12 backend's factory only sees one D700 adapter while the detection-phase factory sees both).
2. The previous approach of deduplicating by `VendorId + DeviceId + SubSysId` incorrectly merged both D700s into a single entry.
3. Passing a gpus-array index to backends failed because the array could be reordered by Vulkan device insertion, causing index mismatches (e.g., index 1 pointing to Basic Render Driver instead of GPU #2).

The LUID-based selection resolves all three issues — the benchmark now correctly creates a device on the intended DXGI adapter, even though the driver ultimately routes DX11/DX12 work to the primary GPU.

---

## Appendix: ATI, AMD, and Qualcomm Adreno — A Shared GPU Heritage

> The Adreno 640 tested in this benchmark shares a direct lineage with the
> Radeon GPUs it is compared against. This section traces the corporate and
> technical connections from ATI Technologies through AMD to Qualcomm's
> Adreno mobile GPU division.

### Corporate Lineage

| Year | Event |
|------|-------|
| 1985 | **Array Technology Inc. (ATI)** founded in Markham, Ontario, Canada |
| 1987 | First product: EGA Wonder — ISA graphics card for IBM PCs |
| 1991 | ATI enters the dedicated 2D accelerator market (Mach 8, Mach 32) |
| 1996 | **3D Rage** — ATI's first 3D-capable GPU. Competed with 3dfx Voodoo and S3 ViRGE |
| 2000 | **Radeon DDR (R100)** — ATI's first GPU under the Radeon brand. Hardware T&L, competed with NVIDIA GeForce 2 |
| 2002 | **Radeon 9700 Pro (R300)** — first DirectX 9 GPU, SM 2.0, outperformed GeForce FX. Widely considered ATI's finest moment |
| 2006 | **AMD acquires ATI Technologies** for $5.4 billion. ATI's GPU division becomes AMD Graphics |
| 2008 | AMD sells its mobile GPU division (Imageon) to **Qualcomm** for $65 million |
| 2009 | Qualcomm renames Imageon to **Adreno** (anagram of "Radeon"). First product: Adreno 200 in Snapdragon QSD8250 |
| 2013 | AMD rebrands consumer GPUs from "Radeon HD" to "Radeon R" series, then later "Radeon RX" |
| 2017 | AMD launches Vega architecture (GCN 5). "ATI" name fully phased out from all products |
| 2020 | AMD launches RDNA 2 (RX 6000 series). Qualcomm launches Adreno 660 (Snapdragon 888) |
| 2024 | Qualcomm launches Snapdragon X Elite with Adreno X1 GPU for Windows on ARM laptops |
| 2025 | AMD launches RDNA 4 (RX 9070 XT). Qualcomm's Adreno GPUs power the majority of Android devices and Windows on ARM PCs |

### Technical Connection: Imageon → Adreno

ATI's **Imageon** was a low-power mobile GPU line designed for handheld devices and embedded systems (PDAs, early smartphones). When AMD acquired ATI in 2006, Imageon became part of AMD's portfolio but was considered non-core — AMD's focus was on discrete desktop/laptop GPUs (Radeon) and professional workstation GPUs (FirePro).

In 2008, AMD divested the Imageon mobile GPU division to Qualcomm for $65 million — a fraction of the $5.4 billion AMD paid for all of ATI. Qualcomm integrated Imageon into its Snapdragon SoC platform and renamed it **Adreno** — an anagram of "Radeon" that preserves the ATI heritage while establishing a distinct brand.

Adreno's architecture has since diverged significantly from Radeon. By the Adreno 600 series (2018), the GPU shares no meaningful silicon design with contemporary Radeon GPUs — the instruction set, memory hierarchy, shader core layout, and driver stack are entirely Qualcomm-designed. However, foundational concepts from ATI's Imageon era (tile-based rendering optimisations, unified shader architecture for mobile power budgets) persist in Adreno's design philosophy.

### GPUs Tested in This Benchmark — Family Tree

```
ATI Technologies (1985)
├── Radeon R100 (2000)
│   └── Radeon 9700 (R300, 2002) — first DX9 GPU
│       └── Radeon X1800 (R520, 2005) — last ATI-only design
│
├── Imageon (mobile GPU line, 2002–2008)
│   └── [Sold to Qualcomm, 2008]
│       └── Adreno 200 (2009) — renamed from Imageon
│           └── Adreno 3xx/4xx/5xx/6xx
│               └── Adreno 640 (2019) ← TESTED: Xiaomi Pad 5 / Snapdragon 860
│                   └── Adreno X1 (2024) — Snapdragon X Elite for WoA
│
└── [AMD acquires ATI, 2006]
    └── AMD Radeon
        ├── HD 5770 (TeraScale 2, 2009) ← TESTED
        ├── FirePro D700 (GCN 1.0, 2013) ← TESTED
        ├── RX 580 (GCN 4, 2017) ← TESTED
        ├── Vega FE (GCN 5, 2017) ← TESTED
        ├── RX 6600 XT / 6900 XT (RDNA 2, 2020–2021) ← TESTED
        └── RX 9070 XT (RDNA 4, 2025) ← TESTED
```

### What This Means for the Benchmark

The Adreno 640 in this benchmark is, in a historical sense, a distant cousin of the Radeon GPUs it is compared against. Both trace their origins to ATI Technologies, but their architectures diverged completely after the 2008 sale to Qualcomm. Comparing them side-by-side in the same benchmark highlights:

1. **How far mobile GPUs have come**: The Adreno 640 (a 2019 mobile GPU in a tablet SoC) can run the same Vulkan 1.1 compute + render pipeline as desktop GPUs, achieving 106 FPS — slower than a discrete desktop GPU, but functional and measurable with identical code.

2. **The power/performance trade-off**: The Adreno 640 operates within a ~3W thermal envelope (tablet SoC), while the RX 9070 XT consumes ~300W. The 9070 XT is ~17× faster (1,751 vs 106 FPS), but uses ~100× more power — making the Adreno 640 significantly more performance-per-watt efficient for this workload.

3. **Driver maturity gap**: Qualcomm's Windows Vulkan driver is relatively young (first WoA devices shipped 2023), while AMD's Radeon Vulkan drivers have been refined since 2016. This is reflected in the Adreno 640's higher render times and less efficient presentation path.

---

## Appendix: Qualcomm Adreno 640 — Windows on ARM Benchmark Results

> First-ever inclusion of a mobile Qualcomm GPU in this benchmark suite.
> The Adreno 640 runs natively on Windows 11 ARM64 via Qualcomm's WoA
> Vulkan/DX12/DX11 drivers — no emulation or translation layer for the
> GPU workload itself.

### Why Include an Adreno GPU in a Desktop GPU Benchmark?

The test device is a **Xiaomi Pad 5** (小米平板5) — an Android tablet originally shipping with MIUI based on Android 11. Through community-developed custom firmware (Project Renegade / Windows on ARM for Snapdragon 855/860 tablets), Windows 11 ARM64 was installed on this device, replacing the Android operating system entirely. The tablet boots directly into Windows 11, with Qualcomm-provided WDDM drivers exposing the Adreno 640 as a standard Windows GPU — complete with Vulkan, DirectX 12, and DirectX 11 support.

**The motivation for testing this device is rooted in the ATI → AMD → Qualcomm lineage documented in the previous section.** The Adreno GPU traces its origin to ATI's Imageon mobile GPU division, which AMD sold to Qualcomm in 2008. Qualcomm renamed Imageon to Adreno — literally an anagram of "Radeon." In a historical sense, the Adreno 640 is a distant descendant of the same ATI family tree as every Radeon GPU in this benchmark. It is, loosely speaking, still an "A-card" (A卡) — making it a fitting, if unconventional, addition to a benchmark suite that already spans ATI/AMD GPUs from TeraScale 2 (2009) through RDNA 4 (2025).

Including the Adreno 640 also provides unique data points not available from any desktop GPU:

1. **Mobile vs desktop GPU scaling**: How does a 3W tablet SoC GPU compare against 75–300W discrete desktop GPUs running the exact same compute + render workload?
2. **Qualcomm driver maturity**: Qualcomm's Windows GPU drivers are relatively new (first WoA devices shipped 2023). How do they perform compared to AMD and NVIDIA's decade-old Windows driver stacks?
3. **ARM64 vs x64 binary translation**: The same benchmark compiled as ARM64 (native) and x64 (Prism emulation) on identical hardware reveals the exact CPU-side overhead of Microsoft's binary translation layer — with GPU times serving as a constant control variable.

### Test Hardware

| Component | Specification |
|-----------|--------------|
| Device | Xiaomi Pad 5 (nabu) — Android tablet running Windows 11 ARM64 via [Renegade Project](https://github.com/edk2-porting) / [Port-Windows-11-Xiaomi-Pad-5](https://github.com/erdilS/Port-Windows-11-Xiaomi-Pad-5) |
| Original OS | Android 11 (MIUI 12.5) |
| Current OS | Windows 11 Pro ARM64 (NT 10.0.26100) |
| SoC | Qualcomm Snapdragon 860 (SM8150-AC) — a binned Snapdragon 855+ |
| CPU | Kryo 585: 1× A77 @ 2.96 GHz (prime) + 3× A77 @ 2.42 GHz (performance) + 4× A55 @ 1.80 GHz (efficiency) |
| GPU | Qualcomm Adreno 640, ~585 MHz boost, 384 ALUs |
| RAM | 6 GB LPDDR4X (shared between CPU and GPU) |
| VRAM | Shared (reported as 1 MB by driver — a Qualcomm WDDM driver reporting limitation, not actual VRAM size) |
| Vulkan | 1.1.276 (Qualcomm proprietary driver, build 2023-10-23) |
| DX12 | Feature Level 12_1 (driver 27.20.2060.0) |
| DX11 | Supported (driver 27.20.2060.0) |
| OpenGL | **Not supported** — Qualcomm's Windows driver does not expose desktop OpenGL. Microsoft's OpenCL/OpenGL Compatibility Pack provides only GL 3.3 via Mesa-on-DX12 translation, below this benchmark's GL 4.3 requirement. (The same hardware supports OpenGL ES 3.2 on Android, and the open-source Mesa Freedreno driver achieves full OpenGL 4.6 on Linux.) |
| Display | 11" 2560×1600 IPS 120 Hz (benchmark runs at 1280×720) |
| Thermal | Passive cooling only (tablet form factor, no fan) |
| OS | Windows 11 Pro ARM64 (NT 10.0.26100) |
| Resolution | 1280 × 720 |
| V-Sync | OFF |

### Cross-API Results — 1M Particles (Medium)

| # | API | Avg FPS | Compute (ms) | Render (ms) | Total GPU (ms) | Frame Time (ms) | GPU Util | Bottleneck |
|---|-----|---------|-------------|------------|---------------|-----------------|----------|------------|
| 1 | DX12 | 116.7 | 3.281 | 4.767 | 8.053 | 8.57 | 94% | GPU-bound |
| 2 | Vulkan | 105.8 | 3.299 | 5.704 | 9.002 | 9.45 | 95% | GPU-bound |
| 3 | DX11 | 83.2 | 4.374 | 4.579 | 11.775 | 12.02 | 98% | GPU-bound |

### Key Observations

**1. DX12 is the fastest API on Adreno 640**

Unlike desktop GPUs where DX11 or Vulkan often lead, DX12 is the clear winner on this mobile GPU. This suggests Qualcomm's DX12 driver path is more optimised than their Vulkan or DX11 paths — plausible given that DX12 is the primary API for Windows on ARM gaming and application compatibility.

**2. Vulkan render time is inflated**

Vulkan's render time (5.704 ms) is 20% higher than DX12's (4.767 ms), despite both rendering the same workload. This likely reflects immaturity in Qualcomm's Vulkan presentation/swapchain path on Windows, similar to the semaphore wait pollution observed on desktop GPUs (Section 17) but more pronounced.

**3. DX11 has the highest compute overhead**

DX11 compute time (4.374 ms) is 33% higher than Vulkan/DX12 (~3.3 ms). Combined with a large total GPU time (11.775 ms), DX11 is clearly the least efficient path. The implicit driver overhead that helps DX11 on mature desktop drivers (NVIDIA, AMD) does not translate to Qualcomm's younger driver stack.

**4. All APIs are GPU-bound at 95%+ utilisation**

The Adreno 640 is fully saturated at 1M particles — there is no CPU overhead headroom. This contrasts sharply with desktop GPUs where most APIs are CPU-bound at this particle count (e.g., RTX 5090 at 33% GPU utilisation). The mobile GPU's lower compute throughput means it hits the GPU-bound regime much earlier.

### Adreno 640 vs Desktop GPUs — Cross-Platform Comparison

| GPU | Best API | Best FPS | Compute (ms) | Total GPU (ms) | vs Adreno 640 |
|-----|----------|---------|-------------|---------------|---------------|
| RTX 5090 | DX12 | 5,603 | 0.014 | 0.065 | **48× faster** |
| RX 9070 XT | DX11 | 1,774 | 0.047 | 0.542 | **15× faster** |
| RX 6600 XT | DX12 | 1,834 | 0.190 | 0.406 | **16× faster** |
| Vega FE | DX12 | 1,716 | 0.219 | 0.452 | **15× faster** |
| RX 580 | DX12 | 912 | 0.362 | 0.930 | **8× faster** |
| GTX 970 | Vulkan | 719 | 0.434 | 1.098 | **6× faster** |
| FirePro D700 | Vulkan | 555 | 0.589 | 1.473 | **5× faster** |
| Radeon iGPU (2 CU) | DX12 | 324 | 1.480 | 2.953 | **3× faster** |
| HD 5770 | OpenGL | 188 | 1.794 | 4.818 | **1.6× faster** |
| **Adreno 640** | **DX12** | **117** | **3.281** | **8.053** | **1.0× (baseline)** |
| WARP (CPU) | DX12 | 83 | — | 11.7 | 0.7× (slower) |

The Adreno 640 sits between the HD 5770 (a 2009 discrete desktop GPU) and the WARP software renderer in absolute performance. It outperforms WARP by 40%, confirming it is a real hardware GPU despite its mobile origins.

### API Support Limitations on Windows ARM

| API | Adreno 640 (WoA) | Desktop GPU (x64) | Notes |
|-----|-------------------|-------------------|-------|
| Vulkan | 1.1 (native driver) | 1.3+ | Qualcomm provides a native ARM64 Vulkan ICD |
| DX12 | FL 12_1 (native driver) | FL 12_1–12_2 | Full native support via WDDM driver |
| DX11 | Supported (native driver) | Supported | Full native support |
| OpenGL | **3.3 max** (compatibility pack) | 4.6 | No native desktop OpenGL; Microsoft's compatibility pack (Mesa → DX12 translation) maxes out at GL 3.3, below the 4.3 required by this benchmark |
| Metal | N/A | N/A (macOS only) | — |

The lack of OpenGL 4.3 support means the Adreno 640 cannot run this benchmark's OpenGL backend. This is a platform limitation, not a hardware one — the same Adreno 640 on Android supports OpenGL ES 3.2 (roughly equivalent to desktop GL 4.3 in capability), and on Linux the open-source Mesa Freedreno driver achieves full OpenGL 4.6 on Adreno 600-series hardware.

---

## Appendix: ARM64 Native vs x64 Emulated — Performance Comparison on Adreno 640

> Windows on ARM runs native ARM64 binaries at full speed, but can also
> execute x86/x64 applications through Microsoft's built-in binary
> translation layer (Prism). This section compares the same benchmark
> compiled as ARM64 vs x64 on identical hardware.

### What is Prism (x64 Emulation on ARM)?

Windows 11 on ARM includes **Prism**, a binary translation layer that converts x86/x64 instructions to ARM64 at runtime. This allows unmodified x64 Windows applications to run on ARM hardware with a performance penalty. Prism translates code JIT (just-in-time), caching translated blocks for reuse.

Key characteristics:
- **CPU code is translated**: all C++ application logic (particle initialisation, frame loop, API calls) runs through the translation layer
- **GPU code is NOT translated**: shaders (SPIR-V, HLSL, GLSL) execute natively on the GPU regardless of the host binary's architecture
- **API calls pass through**: Vulkan/DX12/DX11 driver calls from the x64 binary reach the same native ARM64 GPU driver via interop thunks

### Why Compare ARM64 vs x64?

1. **RenderDoc compatibility**: RenderDoc only ships as x64. An x64 build is required for RenderDoc frame capture on WoA hardware. Measuring the emulation penalty tells us whether x64 benchmark results are still meaningful for comparison.
2. **Real-world relevance**: Many Windows applications remain x64-only. Understanding the GPU benchmark impact of emulation helps assess whether WoA devices can be trusted for performance-sensitive x64 workloads.
3. **Isolating CPU vs GPU overhead**: Since shaders run natively regardless of binary architecture, any performance difference is purely CPU-side (API call overhead, frame loop, buffer management). This cleanly separates translation overhead from GPU execution time.

### Results — ARM64 Native vs x64 Emulated (Adreno 640, 1M Particles, Medium)

| Metric | ARM64 Native | x64 Emulated | Delta |
|--------|-------------|-------------|-------|
| **Vulkan** | | | |
| Avg FPS | 105.8 | 78.6 | **−25.7%** |
| Compute (ms) | 3.299 | 3.119 | −5.5% |
| Render (ms) | 5.704 | 5.586 | −2.1% |
| Total GPU (ms) | 9.002 | 8.706 | −3.3% |
| Frame Time (ms) | 9.448 | 12.726 | **+34.7%** |
| **DX12** | | | |
| Avg FPS | 116.7 | 98.3 | **−15.8%** |
| Compute (ms) | 3.281 | 3.257 | −0.7% |
| Render (ms) | 4.767 | 4.368 | −8.4% |
| Total GPU (ms) | 8.053 | 7.630 | −5.3% |
| Frame Time (ms) | 8.570 | 10.169 | **+18.7%** |
| **DX11** | | | |
| Avg FPS | 83.2 | 75.2 | **−9.6%** |
| Compute (ms) | 4.374 | 4.381 | +0.2% |
| Render (ms) | 4.579 | 4.470 | −2.4% |
| Total GPU (ms) | 11.775 | 11.678 | −0.8% |
| Frame Time (ms) | 12.024 | 13.293 | **+10.6%** |

### Analysis

**GPU compute/render times are virtually identical** between ARM64 and x64 builds — within ±5% across all APIs, well within run-to-run variance. This confirms the prediction: GPU shaders execute natively on the Adreno 640 regardless of the host binary's architecture. The translation layer does not affect GPU-side workload execution.

**FPS is 10–26% lower on x64**, with the penalty varying by API:

| API | FPS Penalty | Frame Time Increase | CPU Overhead Added |
|-----|------------|--------------------|--------------------|
| Vulkan | −25.7% | +3.28 ms | ~3.3 ms per frame |
| DX12 | −15.8% | +1.60 ms | ~1.6 ms per frame |
| DX11 | −9.6% | +1.27 ms | ~1.3 ms per frame |

**Why Vulkan is penalised most heavily:**

Vulkan has the highest per-frame CPU call count of the three APIs — explicit command buffer recording, descriptor set binding, fence management, and swapchain acquisition each require individual API calls that pass through the Prism translation layer. Each translated call adds a small overhead (~microseconds), but at 100+ calls per frame, the total accumulates to ~3.3 ms.

DX12 has slightly fewer per-frame CPU calls due to its command list model, resulting in a smaller 1.6 ms penalty. DX11's implicit driver handles most resource management internally (fewer API calls from the application), so it suffers the least translation overhead at 1.3 ms.

**Why GPU times are slightly _lower_ on x64 (counter-intuitive):**

The x64 build's GPU compute and render times are marginally lower (by 1–5%) than ARM64. This is not because x64 code makes the GPU faster — it is an artefact of the **higher frame time**. With longer gaps between frame submissions (due to CPU translation overhead), the GPU has slightly more time to process each frame without contention, resulting in marginally cleaner timestamps. The difference is within measurement noise and should not be interpreted as a real GPU performance improvement.

### Conclusions

1. **GPU benchmark data from x64 builds is valid.** GPU compute and render times are unaffected by Prism translation — x64 results can be directly compared against ARM64 results for GPU performance analysis.

2. **FPS comparisons require a correction factor.** x64 FPS is 10–26% lower than ARM64 native due to CPU-side translation overhead. When comparing Adreno 640 FPS against desktop GPUs (which run x64 natively), the ARM64 native results should be used as the true performance baseline.

3. **Prism overhead is workload-dependent.** API-heavy workloads (Vulkan, DX12) are penalised more than API-light workloads (DX11). For GPU-bound scenarios (this benchmark at 1M particles on Adreno 640), the penalty is modest because most frame time is spent on GPU execution, not CPU API calls.

4. **RenderDoc x64 captures on WoA are viable.** Since the x64 build produces identical GPU behaviour with only a CPU overhead penalty, RenderDoc frame captures from x64 builds are representative of true GPU workload behaviour — the captured GPU commands, timings, and resource state will match what the ARM64 native build would produce.
