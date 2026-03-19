#!/usr/bin/env python3
"""
plot_results.py — Generate comparison charts from benchmark results.

Reads ~/.gpu_bench/results.json and produces:
  1. FPS bar chart grouped by GPU, coloured by API
  2. GPU time breakdown (compute / render) stacked bar chart
  3. CPU overhead comparison
  4. Scaling chart: FPS vs particle count per GPU

Usage:
    python scripts/plot_results.py                  # show charts interactively
    python scripts/plot_results.py --save docs/images  # save PNGs to folder
    python scripts/plot_results.py --json path.json    # use a custom results file
"""

import argparse
import json
import os
import sys
from pathlib import Path
from collections import defaultdict

try:
    import matplotlib
    import matplotlib.pyplot as plt
    import matplotlib.ticker as ticker
    import numpy as np
except ImportError:
    print("ERROR: matplotlib and numpy are required.\n"
          "  pip install matplotlib numpy", file=sys.stderr)
    sys.exit(1)


def default_results_path() -> Path:
    if sys.platform == "win32":
        home = os.environ.get("USERPROFILE", "")
    else:
        home = os.environ.get("HOME", "")
    return Path(home) / ".gpu_bench" / "results.json"


def load_results(path: Path) -> list[dict]:
    if not path.exists():
        print(f"ERROR: {path} not found.", file=sys.stderr)
        sys.exit(1)
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if isinstance(data, dict) and "results" in data:
        return data["results"]
    if isinstance(data, list):
        return data
    print("ERROR: unexpected JSON structure.", file=sys.stderr)
    sys.exit(1)


API_COLOURS = {
    "Vulkan":    "#E74C3C",
    "DX12":      "#3498DB",
    "DX11":      "#2ECC71",
    "OpenGL":    "#F39C12",
    "Metal":     "#9B59B6",
}

API_ORDER = ["Vulkan", "DX12", "DX11", "OpenGL", "Metal"]


def short_gpu_name(name: str) -> str:
    import re
    replacements = [
        ("NVIDIA GeForce ", ""),
        ("AMD Radeon ", ""),
        ("(TM)", ""),
        ("Graphics", "iGPU"),
        ("Microsoft Basic Render Driver", "WARP"),
        ("Microsoft WARP (CPU Software Renderer)", "WARP"),
    ]
    for old, new in replacements:
        name = name.replace(old, new)
    name = name.strip()
    # Strip DX12 Feature Level suffix, e.g. "(FL 12_1)", "(FL 11_0)"
    name = re.sub(r"\s*\(FL\s+\d+_\d+\)", "", name)
    # OpenGL reports renderer strings like "RTX 5090/PCIe/SSE2" — strip suffixes
    for suffix in ["/PCIe/SSE2", "/PCIe/SSE42", "/PCIe"]:
        if suffix in name:
            name = name[:name.index(suffix)]
    return name.strip()


def deduplicate(results: list[dict]) -> list[dict]:
    """Keep only the best (highest FPS) result per GPU x API x particleCount."""
    best: dict[tuple, dict] = {}
    for r in results:
        key = (short_gpu_name(r.get("deviceName", "")),
               normalise_api(r.get("graphicsApi", "")),
               r.get("particleCount", 0))
        prev = best.get(key)
        if prev is None or r.get("avgFps", 0) > prev.get("avgFps", 0):
            best[key] = r
    return list(best.values())


def normalise_api(api: str) -> str:
    mapping = {
        "vulkan": "Vulkan", "dx12": "DX12", "directx 12": "DX12",
        "dx11": "DX11", "directx 11": "DX11",
        "opengl": "OpenGL", "opengl 4.3": "OpenGL",
        "metal": "Metal",
    }
    return mapping.get(api.lower(), api)


def apply_style():
    plt.rcParams.update({
        "figure.facecolor": "#1a1a2e",
        "axes.facecolor":   "#16213e",
        "axes.edgecolor":   "#e0e0e0",
        "axes.labelcolor":  "#e0e0e0",
        "text.color":       "#e0e0e0",
        "xtick.color":      "#e0e0e0",
        "ytick.color":      "#e0e0e0",
        "grid.color":       "#2a2a4a",
        "grid.alpha":       0.5,
        "legend.facecolor": "#16213e",
        "legend.edgecolor": "#444",
        "font.size":        11,
        "figure.dpi":       150,
    })


# ── Chart 1: FPS by GPU × API ──────────────────────────────────────────────

def chart_fps_by_gpu(results: list[dict], save_dir: Path | None):
    gpu_api = defaultdict(dict)
    for r in results:
        gpu = short_gpu_name(r.get("deviceName", "Unknown"))
        api = normalise_api(r.get("graphicsApi", ""))
        fps = r.get("avgFps", 0)
        if fps > gpu_api[gpu].get(api, 0):
            gpu_api[gpu][api] = fps

    gpus = sorted(gpu_api.keys())
    apis = [a for a in API_ORDER if any(a in gpu_api[g] for g in gpus)]

    x = np.arange(len(gpus))
    width = 0.8 / max(len(apis), 1)

    fig, ax = plt.subplots(figsize=(max(10, len(gpus) * 2.5), 6))
    for i, api in enumerate(apis):
        vals = [gpu_api[g].get(api, 0) for g in gpus]
        bars = ax.bar(x + i * width, vals, width * 0.9,
                      label=api, color=API_COLOURS.get(api, "#888"))
        for bar, v in zip(bars, vals):
            if v > 0:
                ax.text(bar.get_x() + bar.get_width() / 2, v,
                        f"{v:,.0f}", ha="center", va="bottom", fontsize=8)

    ax.set_xlabel("GPU")
    ax.set_ylabel("Average FPS")
    ax.set_title("FPS Comparison — by GPU × Graphics API")
    ax.set_xticks(x + width * (len(apis) - 1) / 2)
    ax.set_xticklabels(gpus, rotation=20, ha="right")
    ax.legend(loc="upper right")
    ax.yaxis.set_major_formatter(ticker.FuncFormatter(lambda v, _: f"{v:,.0f}"))
    ax.grid(axis="y")
    fig.tight_layout()

    if save_dir:
        fig.savefig(save_dir / "fps_by_gpu.png")
        print(f"  Saved {save_dir / 'fps_by_gpu.png'}")
    else:
        plt.show()
    plt.close(fig)


# ── Chart 2: GPU Time Breakdown ─────────────────────────────────────────────

def chart_gpu_time(results: list[dict], save_dir: Path | None):
    deduped = deduplicate(results)
    entries = []
    for r in deduped:
        label = f"{short_gpu_name(r.get('deviceName', '?'))}\n{normalise_api(r.get('graphicsApi', '?'))}"
        compute = r.get("avgComputeMs", 0)
        render = r.get("avgRenderMs", 0)
        entries.append((label, compute, render))

    entries.sort(key=lambda e: e[1] + e[2])

    labels = [e[0] for e in entries]
    compute = [e[1] for e in entries]
    render = [e[2] for e in entries]

    x = np.arange(len(labels))
    fig, ax = plt.subplots(figsize=(max(10, len(labels) * 1.5), 6))
    ax.barh(x, compute, color="#E74C3C", label="Compute (ms)")
    ax.barh(x, render, left=compute, color="#3498DB", label="Render (ms)")

    for i, (c, r) in enumerate(zip(compute, render)):
        total = c + r
        if total > 0:
            ax.text(total + 0.01, i, f"{total:.3f}", va="center", fontsize=8)

    ax.set_yticks(x)
    ax.set_yticklabels(labels, fontsize=9)
    ax.set_xlabel("GPU Time (ms)")
    ax.set_title("GPU Time Breakdown — Compute vs Render")
    ax.legend(loc="lower right")
    ax.grid(axis="x")
    fig.tight_layout()

    if save_dir:
        fig.savefig(save_dir / "gpu_time_breakdown.png")
        print(f"  Saved {save_dir / 'gpu_time_breakdown.png'}")
    else:
        plt.show()
    plt.close(fig)


# ── Chart 3: CPU Overhead ───────────────────────────────────────────────────

def chart_cpu_overhead(results: list[dict], save_dir: Path | None):
    deduped = deduplicate(results)
    entries = []
    for r in deduped:
        frame_ms = r.get("avgFrameTimeMs", 0)
        gpu_ms = r.get("avgTotalGpuMs", 0)
        cpu_ms = max(frame_ms - gpu_ms, 0)
        label = f"{short_gpu_name(r.get('deviceName', '?'))}\n{normalise_api(r.get('graphicsApi', '?'))}"
        entries.append((label, cpu_ms, gpu_ms))

    entries.sort(key=lambda e: e[1] + e[2])

    labels = [e[0] for e in entries]
    cpu = [e[1] for e in entries]
    gpu = [e[2] for e in entries]

    x = np.arange(len(labels))
    fig, ax = plt.subplots(figsize=(max(10, len(labels) * 1.5), 6))
    ax.barh(x, cpu, color="#E67E22", label="CPU Overhead (ms)")
    ax.barh(x, gpu, left=cpu, color="#1ABC9C", label="GPU Time (ms)")

    ax.set_yticks(x)
    ax.set_yticklabels(labels, fontsize=9)
    ax.set_xlabel("Frame Time (ms)")
    ax.set_title("Frame Time Breakdown — CPU Overhead vs GPU Time")
    ax.legend(loc="lower right")
    ax.grid(axis="x")
    fig.tight_layout()

    if save_dir:
        fig.savefig(save_dir / "cpu_overhead.png")
        print(f"  Saved {save_dir / 'cpu_overhead.png'}")
    else:
        plt.show()
    plt.close(fig)


# ── Chart 4: Particle Count Scaling ─────────────────────────────────────────

def chart_scaling(results: list[dict], save_dir: Path | None):
    deduped = deduplicate(results)
    series = defaultdict(list)
    for r in deduped:
        key = f"{short_gpu_name(r.get('deviceName', '?'))} ({normalise_api(r.get('graphicsApi', '?'))})"
        series[key].append((r.get("particleCount", 0), r.get("avgFps", 0)))

    if not any(len(pts) >= 2 for pts in series.values()):
        print("  Skipping scaling chart (need ≥2 particle counts per config).")
        return

    fig, ax = plt.subplots(figsize=(10, 6))
    for label, pts in sorted(series.items()):
        pts.sort()
        counts = [p[0] for p in pts]
        fps = [p[1] for p in pts]
        if len(counts) >= 2:
            ax.plot(counts, fps, "o-", label=label, markersize=5)

    ax.set_xlabel("Particle Count")
    ax.set_ylabel("Average FPS")
    ax.set_title("Performance Scaling — FPS vs Particle Count")
    ax.set_xscale("log", base=2)
    ax.xaxis.set_major_formatter(ticker.FuncFormatter(
        lambda v, _: f"{v / 1e6:.1f}M" if v >= 1e6 else f"{v / 1e3:.0f}K"))
    ax.yaxis.set_major_formatter(ticker.FuncFormatter(lambda v, _: f"{v:,.0f}"))
    ax.legend(fontsize=9)
    ax.grid(True)
    fig.tight_layout()

    if save_dir:
        fig.savefig(save_dir / "scaling.png")
        print(f"  Saved {save_dir / 'scaling.png'}")
    else:
        plt.show()
    plt.close(fig)


# ── Main ────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Generate benchmark comparison charts from results.json")
    parser.add_argument("--json", type=Path, default=None,
                        help="Path to results.json (default: ~/.gpu_bench/results.json)")
    parser.add_argument("--save", type=Path, default=None,
                        help="Directory to save PNG charts (default: show interactively)")
    args = parser.parse_args()

    results_path = args.json or default_results_path()
    results = load_results(results_path)
    print(f"Loaded {len(results)} result(s) from {results_path}")

    if not results:
        print("No results to plot.")
        return

    apply_style()

    if args.save:
        args.save.mkdir(parents=True, exist_ok=True)

    chart_fps_by_gpu(results, args.save)
    chart_gpu_time(results, args.save)
    chart_cpu_overhead(results, args.save)
    chart_scaling(results, args.save)

    if args.save:
        print(f"\nAll charts saved to {args.save}/")
    print("Done.")


if __name__ == "__main__":
    main()
