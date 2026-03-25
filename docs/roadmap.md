# Roadmap

## Completed

- [x] Vulkan compute + graphics pipeline with particle simulation
- [x] DirectX 12 / DirectX 11 / Metal backend ports
- [x] GPU timestamp profiling (all backends)
- [x] Benchmark mode with standardised report output
- [x] Multi-GPU selection (`--gpu` flag)
- [x] HarmonyOS (OHOS) Vulkan port via XComponent
- [x] Benchmark result history — auto-save, list, compare, delete, CSV export
- [x] Interactive main menu with quick run / custom run / comparison / delete
- [x] OpenGL 4.3 compute shader backend with GLAD2 loader
- [x] `VK_EXT_debug_utils` integration — debug labels and object names for RenderDoc
- [x] RenderDoc In-Application API — auto-detect, F12 capture, `--capture <seconds>` CLI
- [x] Python benchmark tooling — chart generation, batch runner, markdown/HTML report export
- [x] Headless compute mode (`--headless`) — pure GPU compute without window/swapchain/rendering
- [x] Configurable frames-in-flight (`--flights N`) — test swapchain depth impact
- [x] Configurable particle count — preset sizes (65K–16M) or custom values
- [x] RX 9070 XT (RDNA 4) benchmarks — cross-API, particle scaling, headless analysis

### Benchmark Result History

Results are automatically saved to `~/.gpu_bench/results.json` after each run.
Full metrics are persisted: graphics API, GPU, CPU, particle count, difficulty,
timing breakdown (compute / render / total GPU), FPS, and bottleneck analysis.

| Feature | Interactive | CLI |
|---------|-----------|-----|
| List all results | Main menu → Delete results | `--results` |
| Compare (ranked table) | Main menu → Compare results | `--compare` |
| Detailed side-by-side | Compare → enter two rank #s | `--compare <id1> <id2>` |
| Delete one result | Delete → enter ID | `--results-delete <id>` |
| Clear all results | Delete → type `all` | `--results-clear` |
| Export to CSV | — | `--results-export <file.csv>` |

### Interactive Main Menu

On startup (when no CLI flags are given), the application presents:

```
========== GPU Benchmark ==========
  [0] Quick run (Vulkan 1.2 / RTX 5090 / Medium)  <- default
  [1] Custom run (choose API / GPU / difficulty)
  [2] Run again (same settings)
  [3] Compare results (N saved)
  [4] Delete results
  [5] Full analysis — one GPU (all APIs + RenderDoc + charts)
  [6] Full analysis — all GPUs x APIs (+ RenderDoc + charts)
  [7] Flights test — one GPU (all APIs + RenderDoc, custom flights)
  [8] Particle count test — one GPU (all APIs + RenderDoc, custom particles)
  [9] Headless compute — one GPU (all APIs, pure compute, no rendering)
  [10] Exit
====================================
```

- **Quick run** auto-selects the best GPU (discrete > integrated > software)
  and the best API that GPU supports (Vulkan > DX12 > DX11 > Metal).
- **Run again** appears after the first run and reuses the previous settings.
- **Full analysis** [5]/[6] — one-click workflow that:
  1. **[5]** selects one GPU (default: best dGPU); **[6]** runs every GPU.
  2. Benchmarks every supported API on the selected GPU(s) (1M particles, 15s).
  3. Triggers a RenderDoc frame capture at the 5-second mark on each run (if
     RenderDoc is attached or the in-app API detects the DLL).
  4. After all runs, automatically calls Python scripts to generate:
     - FPS comparison charts → `docs/images/`
     - Markdown results table → `docs/results-table.md`
     - Standalone HTML report → `docs/report.html`
  5. Prints a final comparison table to the console.

  Requires `pip install -r scripts/requirements.txt` for the Python step.
- **Flights test** [7] — benchmarks with a custom number of frames-in-flight
  (swapchain images). Tests how swapchain depth affects throughput. RenderDoc
  capture filenames include the flights count (e.g. `_flights3`).
- **Particle count test** [8] — benchmarks with a custom particle count
  (preset sizes from 65K to 16M, or enter any number). Tests GPU scaling
  behaviour. RenderDoc capture filenames include the particle count.
- **Headless compute** [9] — pure GPU compute benchmark with no window,
  swapchain, rendering, or presentation. Eliminates swapchain throttling and
  semaphore wait pollution, revealing true compute throughput. On the
  RX 9070 XT, headless mode achieves **21,000+ FPS** across all APIs
  (vs ~1,750 FPS windowed) — a 12× speedup.
- After each run the menu reappears — no need to restart the application.

---

## In Progress / Planned

### RenderDoc Capture & Cross-GPU Analysis (P0 — next up)

End-to-end RenderDoc profiling on multiple GPUs: **RX 9070 XT** (RDNA 4, 64 CU),
**RX 6900 XT** (RDNA 2, 80 CU), and the AMD iGPU (2 CU) as baseline.
Full step-by-step guide: [`renderdoc-capture-guide.md`](renderdoc-capture-guide.md).

- [x] Run baseline benchmarks (9070 XT + 6900 XT + iGPU, Vulkan + DX11) without RenderDoc.
- [x] Capture one Vulkan frame on each GPU via `--capture 5` (at 5s mark).
- [x] Take 7 annotated screenshots (event list, compute pipeline, SSBO data,
      graphics pipeline, barrier, render output, per-event timing).
- [x] Cross-validate app timestamp queries against RenderDoc GPU timing (< 5 %
      deviation target).
- [x] Write cross-GPU comparison (CU scaling, memory bandwidth, barrier cost).
- [x] Compare RDNA 4 vs RDNA 2 per-CU efficiency via RenderDoc per-event timing.
- [x] ~~Propose optimisations~~ — deferred; current analysis is sufficient.
- [x] Fill in [`renderdoc-analysis.md`](renderdoc-analysis.md) and
      update [`report.md`](report.md).

Code integration already complete:

- `VK_EXT_debug_utils` labels and object names in the Vulkan backend.
- RenderDoc In-Application API: auto-detect on launch, **F12** manual capture,
  `--capture <seconds>` for time-based unattended capture.

### Python Benchmark Tooling (P0 — done)

A `scripts/` directory with Python utilities for automated benchmark data
analysis, satisfying the JD's *"Scripting languages — Python, Perl, shell"*
requirement.

| Script | Purpose |
|--------|---------|
| [`scripts/plot_results.py`](../scripts/plot_results.py) | Read `~/.gpu_bench/results.json` and generate 4 charts: FPS by GPU × API, GPU time breakdown (compute/render), CPU overhead, particle-count scaling |
| [`scripts/batch_benchmark.py`](../scripts/batch_benchmark.py) | Automate batch runs across all GPU × API × particle-count combinations, with `--dry-run` preview |
| [`scripts/export_report.py`](../scripts/export_report.py) | Export results as markdown tables (for docs) or a standalone sortable HTML report |
| [`scripts/compare_3dmark.py`](../scripts/compare_3dmark.py) | Cross-validate project FPS against 3DMark Time Spy / Fire Strike scores — normalised bar chart, R² correlation scatter plot, deviation table |
| [`scripts/rdoc_export_timing.py`](../scripts/rdoc_export_timing.py) | Export per-event GPU timing from a RenderDoc `.rdc` capture to JSON — works in RenderDoc GUI Python Shell or standalone via `renderdoc` module |
| [`scripts/compare_rdoc_timing.py`](../scripts/compare_rdoc_timing.py) | Cross-validate app timestamp queries vs RenderDoc GPU timing — side-by-side comparison, deviation analysis, per-event breakdown |
| [`scripts/3dmark_scores.json`](../scripts/3dmark_scores.json) | 3DMark reference scores (edit with your own or public data) |
| [`scripts/requirements.txt`](../scripts/requirements.txt) | Python dependencies (`matplotlib`, `numpy`) |

```bash
# Install dependencies
pip install -r scripts/requirements.txt

# Generate charts as PNGs
python scripts/plot_results.py --save docs/images

# Batch-run all GPU × API combos (dry-run first)
python scripts/batch_benchmark.py --gpus 0 1 --dry-run
python scripts/batch_benchmark.py --gpus 0 1 --frames 500

# Export markdown table
python scripts/export_report.py --md docs/results-table.md

# Export standalone HTML report
python scripts/export_report.py --html docs/report.html

# Cross-validate against 3DMark
# Option A: auto-import from .3dmark-result files (saved by 3DMark GUI)
python scripts/compare_3dmark.py --import-3dmark "C:/Users/*/Documents/3DMark/*.3dmark-result"

# Option B: auto-import from exported XML (3DMark Professional --export)
python scripts/compare_3dmark.py --import-xml path/to/timespy.xml path/to/firestrike.xml

# Option C: manually edit scripts/3dmark_scores.json, then:
python scripts/compare_3dmark.py --save docs/images   # generate charts
python scripts/compare_3dmark.py --md                  # markdown table to stdout

# RenderDoc timing export & cross-validation
# Step 1: Export timing from .rdc capture (inside RenderDoc Python Shell)
#   exec(open('scripts/rdoc_export_timing.py').read())
# Step 1 (alt): Standalone (requires renderdoc on PYTHONPATH)
python scripts/rdoc_export_timing.py capture_6900xt.rdc -o rdoc_6900xt.json
python scripts/rdoc_export_timing.py capture_igpu.rdc   -o rdoc_igpu.json

# Step 2: Compare app timestamps vs RenderDoc timing
python scripts/compare_rdoc_timing.py rdoc_6900xt.json rdoc_igpu.json
```

### Web Backend — WebGL / WebGPU

Browser-based port of the particle benchmark, inspired by projects such as
[Volume Shader BM](https://volumeshader-bm.com/). Goals:

- **WebGPU** compute shader path (WGSL) for browsers with WebGPU support.
- **WebGL 2.0** fallback using transform feedback for particle updates.
- Hosted as a static site so anyone can run the benchmark without installing
  drivers or SDKs.
- Cross-platform, cross-system league table comparing results from native
  backends (Vulkan / DX / Metal) against web backends on the same hardware.

### Cross-Platform & Cross-GPU Performance Comparison

Written analysis document comparing frame rates and GPU timings across a
range of AMD and NVIDIA hardware spanning 16 years (2009–2025):

| GPU | Architecture | CUs / SPs | VRAM | Platform | Notes |
|-----|-------------|-----------|------|----------|-------|
| **RTX 5090** | Blackwell (NVIDIA) | 170 SMs | 32 GB GDDR7 | Windows | Current flagship — reference for cross-vendor comparison |
| **RX 9070 XT** | RDNA 4 (AMD) | 64 CUs | 16 GB GDDR6 | Windows | Latest AMD architecture — fastest per-CU compute tested |
| GTX 970 | Maxwell (NVIDIA) | 13 SMs | 4 GB GDDR5 | Windows | Older NVIDIA — reveals API ranking reversal vs modern GPUs |
| RX 6900 XT | RDNA 2 | 80 CUs | 16 GB | Windows | Flagship RDNA 2 |
| RX 6600 XT | RDNA 2 | 32 CUs | 8 GB | Windows | Mid-range RDNA 2 — half the CU count of 9070 XT, enables per-CU comparison |
| Vega Frontier Edition | Vega (GCN 5) | 64 CUs | 16 GB HBM2 | Windows | Prosumer / compute |
| RX 580 | Polaris (GCN 4) | 36 CUs | 8 GB | Windows | Mid-range GCN — baseline for normalised comparisons |
| FirePro D700 | Tahiti (GCN 1.0) | 2048 SPs | 6 GB | Windows | Mac Pro 2013 dual-GPU — each card benchmarked independently |
| HD 5770 | Evergreen (TeraScale 2, before GCN) | 800 SPs | 1 GB | Windows (DX11) | Legacy DX11-era GPU |
| Ryzen 7 9800X3D iGPU | RDNA 2 | 2 CUs | Shared | Windows | Integrated graphics |
| Ryzen 7 9800X3D (WARP) | Software | — | System RAM | Windows | Microsoft WARP software rasteriser on AMD CPU |

> **HD 5770 note:** Evergreen does not support Vulkan. Testing will use the
> DX11 backend only (Feature Level 11_0).
>
> **FirePro D700 note:** The Mac Pro (Late 2013) has two identical D700
> GPUs (GCN 1.0, Tahiti XT). macOS does **not** support CrossFire; each GPU
> is an independent `MTLDevice`. One GPU handles display output while the
> other is dedicated to compute. Both cards will be benchmarked individually
> via Metal, and optionally via MoltenVK (Vulkan→Metal) or Boot Camp DX11.
> This provides the only GCN 1.0 data point in the comparison.
> Although the D700 can create a DX12 device (Feature Level 11_0), its
> compute shader performance under DX12 is identical to WARP software
> rendering (~29 FPS vs ~28 FPS), indicating the driver does not
> accelerate DX12 compute on GCN 1.0. Vulkan 1.1 and DX11 run on the GPU
> normally (~570–600 FPS).
>
> **WARP note:** The Windows Advanced Rasterization Platform (WARP) is a
> high-performance software renderer included in DirectX. Running the DX11 /
> DX12 backends on WARP with an AMD CPU provides a pure-software baseline,
> isolating CPU compute throughput from GPU hardware.

The document covers:

- Per-backend (Vulkan / DX11 / DX12 / Metal / OpenGL / WARP) frame-rate comparison.
- Scaling behaviour when increasing particle count (65 K → 1 M → 16 M).
- Compute vs render timing breakdown per GPU (and CPU via WARP).
- Hardware vs software rendering comparison (discrete GPU vs WARP baseline).
- RX 9070 XT (RDNA 4) vs RX 6600 XT (RDNA 2): **4.1× per-CU compute improvement**
  with 2× the CU count (64 vs 32), demonstrating generational architectural gains.
- Headless compute mode: removing swapchain/render/present reveals true compute
  throughput — RX 9070 XT achieves 21,000+ FPS in headless vs 1,750 windowed.
- Swapchain semaphore wait pollution analysis — why fast GPUs (9070 XT, RTX 5090)
  report inflated render timestamps in windowed mode.
- Cross-validation against 3DMark Time Spy and Fire Strike across all GPUs.
- OpenGL compute dispatch overhead on AMD GPUs (2.6–48 ms, vs 0.03 ms on Vulkan).
- Generational progression from TeraScale 2 → GCN 1.0 → GCN 4 → GCN 5 → RDNA 2 → RDNA 4.
- Mac Pro 2013 dual-GPU analysis: display GPU vs headless GPU performance
  isolation, and macOS Metal vs Boot Camp DX11 cross-platform comparison.

### Workgroup Size Tuning Experiments

Sweep `local_size_x` across powers of two (32, 64, 128, 256, 512, 1024) and
measure the impact on compute dispatch time for each GPU above. Publish
findings as an analysis document covering:

- Optimal workgroup size per architecture (GCN vs RDNA 2 vs RDNA 4 vs Maxwell vs Blackwell).
- Occupancy and wavefront utilisation implications.
- Correlation with CU count and cache hierarchy.

### Memory Allocation Strategy Comparison

Benchmark and document the performance difference between:

| Strategy | Vulkan Flags | Use Case |
|----------|-------------|----------|
| Host-visible / host-coherent | `HOST_VISIBLE \| HOST_COHERENT` | Current approach — simple, CPU-mappable |
| Device-local + staging buffer | `DEVICE_LOCAL` + staging copy | Optimal for discrete GPUs |
| Persistent mapping | `HOST_VISIBLE \| HOST_COHERENT` + persistent `vkMapMemory` | Avoids repeated map/unmap |
| Device-local host-visible (ReBAR) | `DEVICE_LOCAL \| HOST_VISIBLE` | AMD SAM / ReBAR on supported GPUs |

Measure particle-buffer throughput and compute dispatch latency for each
strategy across integrated and discrete GPUs.

### Multi-Draw-Call Stress Test

The current benchmark issues a single compute dispatch and a single draw call
per frame — a workload profile that favours DX11's highly optimised implicit
driver path over DX12/Vulkan's explicit model (see
[`report.md`](report.md) for measured data).

To demonstrate the scalability advantage of explicit APIs, add an optional
**multi-draw-call mode**:

- Render particles in batches (e.g. 1 draw call per 1 024 particles), producing
  1 000+ draw calls per frame at default particle counts.
- Record draw commands across **4–8 threads** on DX12 (secondary command lists)
  and Vulkan (secondary command buffers), then submit in a single primary.
- Compare single-threaded vs multi-threaded CPU submission time per API.
- Expected outcome: DX12/Vulkan overtake DX11 when draw-call count is high
  enough for the driver's single-threaded path to become the bottleneck.

This will complete the cross-API analysis by showing both sides of the
implicit-vs-explicit trade-off.

### Advanced Particle Interactions

Extend the compute shader to support richer physics:

- **Gravitational attraction** — N-body style pairwise forces (shared-memory
  tiling for O(N log N) or O(N²) with optimisation notes).
- **Simple collision response** — spatial hashing or grid-based broad phase.
- **Attractors / repulsors** — mouse-driven interactive forces.

This demonstrates more complex compute shader design, including shared-memory
optimisation and synchronisation within workgroups.

### HIP / ROCm Headless Compute Backend

Add a headless (no rendering) compute benchmark using AMD's
[HIP](https://github.com/ROCm/HIP) runtime:

- Port the particle-update kernel from GLSL/HLSL to a HIP kernel.
- Time kernel dispatch with `hipEvent` and output the same standardised
  benchmark report as the graphics backends.
- Compare HIP kernel throughput against Vulkan/DX compute shader dispatch on
  identical AMD hardware.
- HIP compiles for both AMD (ROCm) and NVIDIA (CUDA back-end) GPUs, so the
  same source covers both vendors.

### CUDA Headless Compute Backend

Equivalent headless compute benchmark targeting NVIDIA GPUs natively:

- Port the particle-update kernel to a CUDA kernel (`.cu`).
- Time with `cudaEvent` and produce the same report format.
- Compare CUDA kernel throughput against Vulkan compute and the HIP path on
  NVIDIA hardware.

### Explicit Multi-GPU — Split Compute Across Dual GPUs

Implement explicit multi-GPU support, splitting the particle compute workload
across two physical GPUs and merging results for rendering. Target hardware:
**Mac Pro 2013 dual FirePro D700** (GCN 1.0, 6 GB each).

| API | Mechanism | Status |
|-----|-----------|--------|
| **Metal** (primary) | `MTLCopyAllDevices()` → two `MTLDevice` / `MTLCommandQueue`, split particle buffer, `MTLSharedEvent` cross-GPU sync | Planned — most feasible path; macOS natively exposes both D700s |
| **DX12** | `IDXGIFactory6::EnumAdapters` → Linked or Unlinked Explicit Multi-Adapter, `ID3D12Fence` cross-GPU sync | Long-term — requires Boot Camp + working DX12 driver for D700 |
| **Vulkan** | `VK_KHR_device_group` / `VK_KHR_device_group_creation`, sub-allocate per-device memory, semaphore sync | Long-term — needs dual Vulkan ICDs on the same machine |

Tasks:

- [ ] Metal: enumerate both D700s, create per-device command queues and
      particle buffers (each device owns half the particles).
- [ ] Metal: dispatch compute on both devices in parallel, synchronise with
      `MTLSharedEvent`, blit results to the display-GPU buffer.
- [ ] Metal: render merged particle buffer on the display GPU.
- [ ] Benchmark single-GPU vs dual-GPU throughput (ideal ≈ 2× compute, less
      for render due to data transfer overhead).
- [ ] Write analysis document: scaling efficiency, PCIe transfer cost,
      synchronisation overhead, comparison with implicit CrossFire AFR.
- [ ] (Optional) DX12 Explicit Multi-Adapter implementation on Boot Camp
      Windows, if D700 drivers support DX12.
- [ ] (Optional) Vulkan `VK_KHR_device_group` implementation on a system with
      two discrete Vulkan-capable GPUs.
