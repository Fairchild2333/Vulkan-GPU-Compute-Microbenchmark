# RenderDoc Frame-Capture Analysis

> Industry-standard GPU profiling of the Vulkan particle compute + render pipeline.

## Environment

| Item | Value |
|------|-------|
| **RenderDoc Version** | <!-- e.g. 1.35 --> |
| **GPU** | <!-- e.g. NVIDIA GeForce RTX 5090 / AMD RX 6600 XT --> |
| **Driver** | <!-- e.g. 572.xx / Adrenalin 24.x --> |
| **OS** | <!-- e.g. Windows 11 25H2 --> |
| **Backend** | Vulkan 1.2 |
| **Particles** | <!-- e.g. 16,777,216 --> |
| **Difficulty** | <!-- e.g. 3 (Extreme) --> |

---

## 1. Launching with RenderDoc

### Steps

1. Open RenderDoc.
2. **Launch Application** tab → set **Executable Path** to:
   ```
   build\Release\gpu_benchmark.exe
   ```
3. Set **Working Directory** to the project root.
4. Set **Command-line Arguments** (recommended):
   ```
   --backend vulkan --benchmark 100
   ```
   This forces Vulkan and runs 100 measured frames with V-Sync off, providing a
   short but representative capture window.
5. Click **Launch**.
6. Once the particle window appears, press **F12** (or **Print Screen**) to
   capture a single frame.

### Expected Outcome

RenderDoc should show a capture with three clearly labelled debug regions:

- **Particle Compute** (green label)
- **SSBO Barrier (Compute → Vertex)** (yellow label)
- **Particle Render** (blue label)

> **Screenshot**: <!-- Insert RenderDoc event browser screenshot here -->

---

## 2. Event List Walkthrough

The captured frame's command buffer should contain the following sequence:

| # | Event | Debug Label | Description |
|---|-------|-------------|-------------|
| 1 | `vkCmdResetQueryPool` | — | Reset timestamp query slots for this frame |
| 2 | `vkCmdWriteTimestamp` (TOP_OF_PIPE) | — | Timestamp T0: frame start |
| 3 | `vkCmdBindPipeline` (COMPUTE) | **Particle Compute** | Bind the compute pipeline |
| 4 | `vkCmdBindDescriptorSets` | | Bind SSBO descriptor |
| 5 | `vkCmdPushConstants` | | Push `deltaTime` + `damping` |
| 6 | `vkCmdDispatch` | | Dispatch N/256 workgroups |
| 7 | `vkCmdWriteTimestamp` (COMPUTE_SHADER) | — | Timestamp T1: compute end |
| 8 | `vkCmdPipelineBarrier` | **SSBO Barrier** | `SHADER_WRITE → VERTEX_ATTRIBUTE_READ` |
| 9 | `vkCmdWriteTimestamp` (TOP_OF_PIPE) | — | Timestamp T2: render start |
| 10 | `vkCmdBeginRenderPass` | **Particle Render** | Clear to dark blue (0.04, 0.08, 0.14) |
| 11 | `vkCmdBindPipeline` (GRAPHICS) | | Bind the graphics pipeline |
| 12 | `vkCmdBindVertexBuffers` | | Bind particle SSBO as vertex buffer |
| 13 | `vkCmdDraw` | | Draw N particles as points |
| 14 | `vkCmdEndRenderPass` | | Finish render pass |
| 15 | `vkCmdWriteTimestamp` (COLOR_OUTPUT) | — | Timestamp T3: render end |

> **Screenshot**: <!-- Insert event list screenshot showing all 15 events -->

### Analysis

<!-- Describe what you observe:
- Are there any unexpected events (e.g. implicit barriers inserted by the driver)?
- How does the event ordering match the expected compute → barrier → render flow?
- Any pipeline bubbles visible in the timing view?
-->

---

## 3. SSBO Inspection — Particle Data

### Viewing Buffer Contents

1. Click on the `vkCmdDispatch` event (event #6).
2. In the **Pipeline State** panel, navigate to **Compute Shader → Descriptor
   Set 0 → Binding 0**.
3. Click on the buffer resource — RenderDoc will show the raw buffer contents.
4. Set the buffer format to match the `Particle` struct:

   ```
   float2 position;
   float2 velocity;
   float4 colour;
   ```

   (In RenderDoc's buffer viewer, use custom struct format:
   `float2 pos; float2 vel; float4 col;`)

### Expected Data

| Field | Expected Range | Notes |
|-------|---------------|-------|
| `position.xy` | [-1.0, 1.0] | Normalised screen coordinates |
| `velocity.xy` | [-2.0, 2.0] | Units per second |
| `colour.rgba` | [0.0, 1.0] | HSV-derived, alpha = 1.0 |

> **Screenshot**: <!-- Insert buffer viewer screenshot showing particle data -->

### Observations

<!-- Describe:
- Are positions within expected bounds?
- Do velocities look reasonable (not NaN, not exploding)?
- Is colour data as expected from the initialisation?
- Compare buffer state BEFORE and AFTER the compute dispatch (use the timeline).
-->

---

## 4. Pipeline State Inspection

### 4.1 Compute Pipeline

1. Select the `vkCmdDispatch` event.
2. Open **Pipeline State → Compute Shader** tab.

| Setting | Expected Value |
|---------|---------------|
| Shader entry | `main` |
| Local size | (256, 1, 1) |
| Descriptor set 0, binding 0 | Particle SSBO (read/write) |
| Push constants | `deltaTime` (float), `damping` (float) |

> **Screenshot**: <!-- Insert compute pipeline state screenshot -->

### 4.2 Graphics Pipeline

1. Select the `vkCmdDraw` event.
2. Open **Pipeline State** tab.

| Stage | Setting | Expected Value |
|-------|---------|---------------|
| **Vertex Input** | Binding 0 stride | `sizeof(Particle)` = 32 bytes |
| | Attribute 0 (position) | offset 0, `R32G32_SFLOAT` |
| | Attribute 1 (colour) | offset 16, `R32G32B32A32_SFLOAT` |
| **Vertex Shader** | Entry | `main` |
| **Fragment Shader** | Entry | `main` |
| **Rasteriser** | Topology | `POINT_LIST` |
| | Point size | 2.0 (set in vertex shader) |
| **Colour Blend** | Blend enable | Additive (`SRC_ALPHA + ONE`) |
| **Render Pass** | Colour attachment | Swapchain image (B8G8R8A8_SRGB) |

> **Screenshot**: <!-- Insert graphics pipeline state screenshot -->

---

## 5. Barrier Analysis

### The SSBO Barrier

| Property | Value |
|----------|-------|
| **Source stage** | `COMPUTE_SHADER_BIT` |
| **Destination stage** | `VERTEX_INPUT_BIT` |
| **Source access** | `SHADER_WRITE_BIT` |
| **Destination access** | `VERTEX_ATTRIBUTE_READ_BIT` |
| **Scope** | Single buffer (`Particle SSBO`), whole size |

### Correctness Check

- The barrier ensures all compute shader writes to the SSBO are visible before
  the vertex shader reads particle positions.
- No image layout transitions are needed (the SSBO is a buffer, not an image).
- There are no redundant barriers — exactly one `vkCmdPipelineBarrier` between
  compute and render.

### Potential Improvements

<!-- After inspecting:
- Are there any implicit barriers inserted by the driver? (visible in RenderDoc's
  "Resource Usage" timeline)
- Could the barrier be tighter (e.g. using VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT
  if applicable)?
- Would a memory barrier (VkMemoryBarrier) suffice instead of a buffer barrier?
-->

---

## 6. GPU Timing (RenderDoc vs Application)

Compare RenderDoc's built-in timing with the application's own timestamp queries:

| Metric | Application (timestamp queries) | RenderDoc |
|--------|-------------------------------|-----------|
| Compute dispatch | <!-- ms --> | <!-- ms --> |
| Render pass | <!-- ms --> | <!-- ms --> |
| Total GPU time | <!-- ms --> | <!-- ms --> |
| Frame time | <!-- ms --> | <!-- ms --> |

### Discrepancy Analysis

<!-- If RenderDoc's times differ from the application's timestamps, explain why:
- RenderDoc may add overhead from its interception layer
- Timestamp query precision vs RenderDoc's GPU counter precision
- Any serialisation effects from RenderDoc's capture mode
-->

---

## 7. Framebuffer Output

1. Navigate to the final `vkCmdEndRenderPass` event.
2. Open the **Texture Viewer** → select the colour attachment.

> **Screenshot**: <!-- Insert the rendered frame showing particles -->

The framebuffer should show:

- Dark blue background (clear colour: 0.04, 0.08, 0.14).
- Millions of coloured point-sprites distributed across the screen.
- Bright clusters where particles overlap (additive blending).

---

## 8. Findings & Recommendations

### What Works Well

<!-- e.g.:
- Clean compute → barrier → render pipeline with no redundant synchronisation.
- SSBO data is correctly updated each frame.
- Additive blending produces expected visual output.
-->

### Potential Optimisations

<!-- e.g.:
- Consider using VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT (Vulkan 1.3)
  for more precise barrier scoping.
- The single dispatch covers all particles — for very large counts, consider
  indirect dispatch to allow GPU-driven workload sizing.
- Point size is hardcoded in the vertex shader; could be a push constant for
  dynamic adjustment.
-->

### Issues Found

<!-- e.g.:
- None / describe any unexpected behaviours observed in RenderDoc.
-->

---

## Appendix: RenderDoc Capture Settings

Recommended RenderDoc settings for this project:

| Setting | Value | Reason |
|---------|-------|--------|
| **API Validation** | Enabled | Catch any Vulkan usage errors |
| **Ref All Resources** | Enabled | Ensure all buffers are captured, not just referenced ones |
| **Capture All Cmd Lists** | Enabled | Capture the full command buffer |
| **Allow Fullscreen** | Disabled | Easier to capture in windowed mode |

---

*Document generated as part of the GPU Compute Microbenchmark project.
See [README](../README.md) for build instructions.*
