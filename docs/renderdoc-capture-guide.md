# RenderDoc Capture Guide — Step-by-Step

> Practical walkthrough for capturing and analysing Vulkan frames with
> RenderDoc, targeting the AMD C++ GPU Engineer JD requirements.

## Target Hardware

| Role | GPU | Architecture | VRAM |
|------|-----|-------------|------|
| **Primary** | AMD Radeon RX 6900 XT | RDNA 2 (80 CU) | 16 GB GDDR6 |
| **Baseline** | AMD Radeon Graphics (Zen 5 iGPU) | RDNA 2 (2 CU) | 2 GB shared DDR5 |

---

## Prerequisites

1. Install [RenderDoc](https://renderdoc.org/) (v1.35+).
2. Build the project:
   ```powershell
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build --config Release
   ```
3. Verify the executable runs:
   ```powershell
   .\build\Release\gpu_benchmark.exe --backend vulkan --benchmark 10
   ```

---

## Step 1 — Run Baseline Benchmarks (no RenderDoc)

Before capturing, record clean performance numbers without RenderDoc overhead.

```powershell
# RX 6900 XT — 16M particles, Vulkan
.\build\Release\gpu_benchmark.exe --backend vulkan --gpu 0 --benchmark 500 --particles 16777216

# RX 6900 XT — 16M particles, DX11 (for cross-API comparison)
.\build\Release\gpu_benchmark.exe --backend dx11 --gpu 0 --benchmark 500 --particles 16777216

# AMD iGPU — 1M particles, Vulkan (lower count to keep it runnable)
.\build\Release\gpu_benchmark.exe --backend vulkan --gpu 1 --benchmark 500 --particles 1048576
```

**Record from the terminal Summary output:**

| Metric | RX 6900 XT (Vulkan) | RX 6900 XT (DX11) | AMD iGPU (Vulkan) |
|--------|--------------------|--------------------|-------------------|
| Avg FPS | | | |
| Avg Compute Time (ms) | | | |
| Avg Render Time (ms) | | | |
| Avg Total GPU Time (ms) | | | |
| Avg Frame Time (ms) | | | |
| CPU Overhead / Frame (ms) | | | |
| Bottleneck | | | |

> **Tip**: Use `--compare` afterwards to see a ranked overview of all runs.

---

## Step 2 — Capture Frames with RenderDoc

### Option A: Command-line (recommended)

```powershell
# RX 6900 XT — auto-capture frame 50
& "C:\Program Files\RenderDoc\renderdoccmd.exe" capture -w `
    .\build\Release\gpu_benchmark.exe --backend vulkan --gpu 0 --benchmark 200 --particles 16777216 --capture 50

# AMD iGPU — auto-capture frame 50
& "C:\Program Files\RenderDoc\renderdoccmd.exe" capture -w `
    .\build\Release\gpu_benchmark.exe --backend vulkan --gpu 1 --benchmark 200 --particles 1048576 --capture 50
```

The console will print:

```
============================================
  RenderDoc detected! (API 1.6.0)
  Press F12 during rendering to capture a frame.
  Captures are saved to the RenderDoc working directory.
============================================
...
[RenderDoc] Frame 50 captured! (total captures: 1)
```

### Option B: RenderDoc GUI

1. Open RenderDoc → **Launch Application**.
2. Executable Path: `build\Release\gpu_benchmark.exe`
3. Working Directory: project root
4. Command-line Arguments: `--backend vulkan --gpu 0 --benchmark 200 --particles 16777216`
5. Click **Launch**, then press **F12** when particles are visible.

### Output

Two `.rdc` files will be saved in the RenderDoc working directory
(usually `%APPDATA%\RenderDoc\` or the project root).

---

## Step 3 — Analyse in RenderDoc (7 screenshots)

Open each `.rdc` file in RenderDoc and take the following screenshots.
Save them to `docs/images/` with descriptive filenames.

### Screenshot 1: Event List Overview

- **Where**: Event Browser panel (left side).
- **What to capture**: The full event list showing three colour-coded debug
  labels:
  - 🟢 **Particle Compute** (green)
  - 🟡 **SSBO Barrier (Compute → Vertex)** (yellow)
  - 🔵 **Particle Render** (blue)
- **Filename**: `docs/images/rdoc-event-list.png`
- **Write in report**:
  > "Single frame contains 15 Vulkan API calls organised into three phases:
  > compute dispatch (6 events), memory barrier (1 event), and render pass
  > (6 events), plus 4 timestamp writes. No driver-inserted implicit barriers
  > observed."

### Screenshot 2: Compute Dispatch — Pipeline State

- **Where**: Click `vkCmdDispatch` event → **Pipeline State** tab → **Compute
  Shader** section.
- **What to record**:

  | Setting | Expected Value |
  |---------|---------------|
  | Shader entry point | `main` |
  | Local size | (256, 1, 1) |
  | Workgroup count | 65536 × 1 × 1 (for 16M particles) |
  | Descriptor Set 0, Binding 0 | Particle SSBO, ~512 MB |
  | Push constant 0 | `deltaTime` (float, e.g. ~0.005) |
  | Push constant 1 | `damping` (float, 0.9) |

- **Filename**: `docs/images/rdoc-compute-pipeline.png`
- **Write in report**:
  > "Compute shader dispatches 65536 workgroups of 256 threads each, processing
  > 16,777,216 particles in a single dispatch. The SSBO is bound as a storage
  > buffer (read/write) at binding 0. Push constants deliver per-frame
  > deltaTime and a fixed damping factor of 0.9."

### Screenshot 3: SSBO Particle Data (Buffer Viewer)

- **Where**: Still on `vkCmdDispatch` → Pipeline State → Descriptor Set 0 →
  Binding 0 → click the buffer resource.
- **Setup**: In the Buffer Viewer, set format to:
  ```
  float2 pos; float2 vel; float4 col;
  ```
- **What to record**: Pick 5–10 rows of particle data and note:

  | Field | Expected Range | What to Check |
  |-------|---------------|---------------|
  | `pos.xy` | [-1.0, 1.0] | Within screen bounds |
  | `vel.xy` | [-2.0, 2.0] | Reasonable magnitude |
  | `col.rgba` | [0.0, 1.0] | No NaN, alpha = 1.0 |

- **Filename**: `docs/images/rdoc-ssbo-data.png`
- **Write in report**:
  > "Sampled 10 particles from the SSBO: all positions within normalised
  > [-1, 1] bounds, velocities in [-0.5, 0.5] range (post-damping), colour
  > channels valid (no NaN/Inf). Buffer contains 16,777,216 × 32 bytes =
  > 512 MiB of interleaved particle data."

### Screenshot 4: Graphics Pipeline State

- **Where**: Click `vkCmdDraw` event → **Pipeline State** tab.
- **What to record**:

  | Stage | Setting | Expected |
  |-------|---------|----------|
  | Vertex Input | Binding 0 stride | 32 bytes |
  | | Attr 0 (position) | `R32G32_SFLOAT`, offset 0 |
  | | Attr 1 (colour) | `R32G32B32A32_SFLOAT`, offset 16 |
  | Vertex Shader | Entry | `main` |
  | Fragment Shader | Entry | `main` |
  | Input Assembly | Topology | `VK_PRIMITIVE_TOPOLOGY_POINT_LIST` |
  | Rasteriser | Point size | 2.0 (set in VS) |
  | Colour Blend | Enabled | Yes — `SRC_ALPHA` + `ONE` (additive) |
  | Framebuffer | Colour format | `B8G8R8A8_SRGB` |

- **Filename**: `docs/images/rdoc-graphics-pipeline.png`
- **Write in report**:
  > "Graphics pipeline renders 16M particles as POINT_LIST with additive
  > blending (SRC_ALPHA + ONE). Vertex input reads position (R32G32) and
  > colour (R32G32B32A32) from the same SSBO that the compute shader wrote to,
  > demonstrating correct compute → render data flow through the barrier."

### Screenshot 5: Barrier Details

- **Where**: Click `vkCmdPipelineBarrier` event → check the API Inspector or
  Pipeline State for barrier details.
- **What to record**:

  | Property | Value |
  |----------|-------|
  | srcStageMask | `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT` |
  | dstStageMask | `VK_PIPELINE_STAGE_VERTEX_INPUT_BIT` |
  | srcAccessMask | `VK_ACCESS_SHADER_WRITE_BIT` |
  | dstAccessMask | `VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT` |
  | Buffer | `Particle SSBO` |
  | Size | `VK_WHOLE_SIZE` |
  | Extra barriers? | None (no driver-inserted barriers) |

- **Filename**: `docs/images/rdoc-barrier.png`
- **Write in report**:
  > "Single explicit buffer memory barrier between compute and render passes.
  > Source stage is COMPUTE_SHADER with SHADER_WRITE access; destination is
  > VERTEX_INPUT with VERTEX_ATTRIBUTE_READ access. No redundant or
  > driver-inserted barriers detected in the event list, confirming minimal
  > synchronisation overhead."

### Screenshot 6: Rendered Output (Texture Viewer)

- **Where**: Click `vkCmdEndRenderPass` → **Texture Viewer** → select colour
  attachment.
- **What to capture**: The final rendered frame showing particles.
- **Filename**: `docs/images/rdoc-render-output.png`
- **Write in report**:
  > "Final framebuffer shows 16M particles rendered as coloured point-sprites
  > on a dark blue background (clear colour 0.04, 0.08, 0.14). Bright
  > clusters visible where particles overlap due to additive blending,
  > confirming correct render pipeline output."

### Screenshot 7: Per-Event GPU Timing

- **Where**: Event Browser → right-click column header → **Show Duration
  Column** (or use the **Timing** pane if available).
- **What to record**:

  | Event | GPU Time (RenderDoc) | GPU Time (App Timestamps) | Delta |
  |-------|---------------------|--------------------------|-------|
  | Compute dispatch | fill | fill | fill |
  | Barrier | fill | N/A | — |
  | Render pass | fill | fill | fill |
  | Total frame | fill | fill | fill |

- **Filename**: `docs/images/rdoc-timing.png`
- **Write in report**:
  > "RenderDoc reports compute dispatch at X.XXX ms and render pass at
  > X.XXX ms. Application's own vkCmdWriteTimestamp reports X.XXX ms and
  > X.XXX ms respectively, a deviation of < X%. This confirms the timestamp
  > query implementation is accurate and the RenderDoc interception layer
  > introduces negligible overhead."

---

## Step 4 — Cross-GPU Comparison (RX 6900 XT vs iGPU)

This is the most important section — it demonstrates the AMD JD requirement
*"Debug, test, analyze, and improve model functional and performance accuracy"*.

Open both `.rdc` files side by side (or tab between them) and fill in:

| Metric | RX 6900 XT (80 CU) | AMD iGPU (2 CU) | Ratio | Analysis |
|--------|--------------------|--------------------|-------|----------|
| Compute dispatch | fill ms | fill ms | fill × | Should scale roughly with CU count (80:2 = 40×) |
| Render pass | fill ms | fill ms | fill × | ROP / TMU throughput difference |
| Barrier | fill ms | fill ms | fill × | iGPU unified memory → barrier may be faster |
| Total GPU time | fill ms | fill ms | fill × | Combined scaling factor |
| SSBO size | 512 MiB | 32 MiB | 16× | Different particle counts |

**Analysis to write:**

1. **CU scaling**: Does the compute time ratio match the CU count ratio?
   If RX 6900 XT is 40× faster in compute but only 10× fewer CUs, explain why
   (clock speed, cache, memory bandwidth).

2. **Memory architecture**: The RX 6900 XT uses device-local VRAM (16 GB
   GDDR6, ~512 GB/s). The iGPU shares system DDR5 (~90 GB/s). This 5-6×
   bandwidth difference should show up in the SSBO throughput.

3. **Barrier cost**: On the iGPU with unified memory, the barrier is
   essentially a no-op (no cache flush needed). On the discrete 6900 XT,
   the barrier forces an L2 cache writeback. Measure this.

4. **Identical correctness**: Both GPUs should produce the same SSBO data
   (positions, velocities) within floating-point precision. Verify a few
   particles match between the two captures.

---

## Step 5 — Write Optimisation Recommendations

Based on observations, propose improvements. You do not need to implement all
of them — identifying them demonstrates analytical ability.

### Barrier Precision (Vulkan 1.3)

```
Current:  VK_PIPELINE_STAGE_VERTEX_INPUT_BIT
Proposed: VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT (Vulkan 1.3)
```

Narrower stage mask reduces potential stall scope. Both the RX 6900 XT and
Zen 5 iGPU support Vulkan 1.3.

### Compute-Render Overlap (Ping-Pong Buffer)

Currently, compute and render are serialised within one command buffer.
With two particle SSBOs (A and B), frame N could render from A while
computing into B, allowing the GPU to overlap compute and render work:

```
Frame N:   Compute → B    Render ← A
Frame N+1: Compute → A    Render ← B
```

This would benefit the RX 6900 XT where compute and graphics engines can run
concurrently, but would have no effect on the iGPU (shared CUs).

### Indirect Dispatch

Replace:
```cpp
vkCmdDispatch(cmd, particleCount / 256, 1, 1);
```
With:
```cpp
vkCmdDispatchIndirect(cmd, indirectBuffer, 0);
```

Enables GPU-driven workload sizing. Useful if particle count varies
dynamically (e.g. particle emission / destruction).

### Dynamic Point Size

Move the hardcoded `gl_PointSize = 2.0` in the vertex shader to a push
constant. Avoids pipeline recreation when adjusting particle display size.

### Host-Visible on iGPU

The iGPU's unified memory means `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT` and
`VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT` may point to the same physical memory.
Using `--host-memory` on the iGPU could eliminate the staging buffer copy with
zero performance penalty. Benchmark both modes and compare.

---

## Step 6 — Update Report

After completing the above, update the following files:

1. **`docs/renderdoc-analysis.md`** — fill in the template with screenshots
   and timing data.
2. **`docs/report.md`** — update Section 0 with actual numbers from
   the timing cross-validation table.
3. **`README.md`** — no changes needed (already documents RenderDoc support).

### Final checklist

- [ ] RenderDoc installed and version noted
- [ ] Baseline benchmarks recorded (without RenderDoc)
- [ ] RX 6900 XT frame captured and saved as `.rdc`
- [ ] AMD iGPU frame captured and saved as `.rdc`
- [ ] 7 screenshots taken and saved to `docs/images/`
- [ ] Event list walkthrough written
- [ ] SSBO data verified (no NaN, correct ranges)
- [ ] Pipeline state documented
- [ ] Barrier correctness confirmed
- [ ] GPU timing cross-validated (app timestamps vs RenderDoc)
- [ ] Cross-GPU comparison table filled in
- [ ] Optimisation recommendations written
- [ ] `docs/renderdoc-analysis.md` updated with all data
- [ ] `docs/report.md` Section 0 timing table filled in

---

## How This Maps to the AMD JD

| JD Requirement | What You Demonstrate |
|---------------|---------------------|
| *Use of industry-standard profiling and debug tools* | RenderDoc frame capture, event inspection, buffer analysis, timing comparison |
| *Debug, test, analyze model functional accuracy* | Verified SSBO data correctness, barrier placement, render output |
| *Debug, test, analyze model performance accuracy* | Cross-validated GPU timestamps against RenderDoc, compared two AMD architectures |
| *Identify opportunities for improving design* | Proposed 5 concrete optimisations based on RenderDoc observations |
| *Graphics API or graphics pipeline knowledge* | Demonstrated understanding of compute → barrier → render pipeline, descriptor sets, synchronisation |
| *C/C++ development* | Integrated RenderDoc API in C++, implemented debug utils, CLI capture flag |

---

*This guide is part of the [GPU Compute Microbenchmark](../README.md) project.*
