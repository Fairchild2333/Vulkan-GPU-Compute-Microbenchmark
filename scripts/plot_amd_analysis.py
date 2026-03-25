#!/usr/bin/env python3
"""
plot_amd_analysis.py — Generate AMD GPU analysis charts for Section 13 of the report.

Produces 4 charts from hardcoded report data:
  1. Per-CU Compute Efficiency (Section 13b)
  2. Generational Performance Progression (Section 13d)
  3. Memory Bandwidth vs FPS Correlation (Section 13f)
  4. Barrier Cost Comparison (Section 13h)

Usage:
    python scripts/plot_amd_analysis.py                    # show interactively
    python scripts/plot_amd_analysis.py --save docs/images # save PNGs
"""

import argparse
import sys
from pathlib import Path

try:
    import matplotlib
    import matplotlib.pyplot as plt
    import matplotlib.ticker as ticker
    import numpy as np
except ImportError:
    print("ERROR: matplotlib and numpy are required.\n"
          "  pip install matplotlib numpy", file=sys.stderr)
    sys.exit(1)


# ── Dark theme (matching plot_results.py) ─────────────────────────────────

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


# Architecture → colour mapping (warm→cool = old→new)
ARCH_COLOURS = {
    "TeraScale 2": "#FF6B6B",
    "GCN 1.0":     "#E67E22",
    "GCN 4":       "#F1C40F",
    "GCN 5":       "#2ECC71",
    "RDNA 2":      "#3498DB",
    "RDNA 4":      "#9B59B6",
}


# ── Chart 1: Per-CU Compute Efficiency (13b) ─────────────────────────────

def chart_per_cu_efficiency(save_dir):
    gpus = [
        ("HD 5770",      "TeraScale 2", 0.73),
        ("FirePro D700", "GCN 1.0",     0.69),
        ("RX 580",       "GCN 4",       1.00),
        ("Vega FE",      "GCN 5",       0.93),
        ("RX 6600 XT",   "RDNA 2",      1.51),
        ("RX 6900 XT",   "RDNA 2",      2.59),
        ("iGPU (2 CU)",  "RDNA 2",      5.19),
        ("RX 9070 XT",   "RDNA 4",     12.35),
    ]

    names  = [g[0] for g in gpus]
    archs  = [g[1] for g in gpus]
    values = [g[2] for g in gpus]
    colours = [ARCH_COLOURS.get(a, "#888") for a in archs]

    fig, ax = plt.subplots(figsize=(12, 6))
    bars = ax.barh(names, values, color=colours, edgecolor="#333", linewidth=0.5)

    # Value labels
    for bar, v in zip(bars, values):
        ax.text(v + 0.15, bar.get_y() + bar.get_height() / 2,
                f"{v:.2f}×", va="center", fontsize=10, fontweight="bold")

    # Baseline line at 1.00×
    ax.axvline(x=1.0, color="#F1C40F", linestyle="--", linewidth=1.2, alpha=0.7)
    ax.text(1.05, len(gpus) - 0.5, "RX 580 baseline", fontsize=9, color="#F1C40F",
            alpha=0.8)

    ax.set_xlabel("Per-CU Efficiency (normalised to RX 580 = 1.00×)")
    ax.set_title("Per-CU Compute Efficiency Across AMD Architectures",
                 fontsize=14, fontweight="bold", pad=12)
    ax.set_xlim(0, max(values) * 1.15)
    ax.grid(axis="x", alpha=0.3)

    # Legend for architectures
    from matplotlib.patches import Patch
    seen = {}
    for a in archs:
        if a not in seen:
            seen[a] = ARCH_COLOURS.get(a, "#888")
    legend_handles = [Patch(facecolor=c, edgecolor="#333", label=a)
                      for a, c in seen.items()]
    ax.legend(handles=legend_handles, loc="lower right", fontsize=9)

    fig.tight_layout()
    if save_dir:
        fig.savefig(save_dir / "amd_per_cu_efficiency.png", bbox_inches="tight")
        print(f"  Saved {save_dir / 'amd_per_cu_efficiency.png'}")
    else:
        plt.show()
    plt.close(fig)


# ── Chart 2: Generational Progression (13d) ──────────────────────────────

def chart_generational(save_dir):
    # Windowed 1M data (presentation-limited for fast GPUs)
    gpus_windowed = [
        ("HD 5770",      "TeraScale 2", 2009, 0.21),
        ("FirePro D700", "GCN 1.0",     2013, 0.61),
        ("RX 580",       "GCN 4",       2017, 1.00),
        ("Vega FE",      "GCN 5",       2017, 1.88),
        ("RX 6600 XT",   "RDNA 2",      2021, 2.01),
        ("RX 6900 XT",   "RDNA 2",      2020, 4.52),
        ("RX 9070 XT",   "RDNA 4",      2025, 1.95),
    ]
    # Headless 1M data (true compute, no presentation overhead)
    # Normalised to RX 580 = 912 FPS (windowed baseline)
    gpus_headless = [
        ("RX 6900 XT",   "RDNA 2",      2020, 15950 / 912),   # 17.49×
        ("RX 9070 XT",   "RDNA 4",      2025, 21354 / 912),   # 23.41×
    ]

    fig, ax = plt.subplots(figsize=(12, 6))

    # --- Windowed points (filled circles) ---
    w_names   = [g[0] for g in gpus_windowed]
    w_archs   = [g[1] for g in gpus_windowed]
    w_years   = [g[2] for g in gpus_windowed]
    w_ratios  = [g[3] for g in gpus_windowed]
    w_colours = [ARCH_COLOURS.get(a, "#888") for a in w_archs]

    ax.scatter(w_years, w_ratios, c=w_colours, s=160, zorder=5,
               edgecolors="#e0e0e0", linewidths=1.2, marker="o")

    # Connect windowed with dashed line
    sorted_w = sorted(zip(w_years, w_ratios), key=lambda p: (p[0], p[1]))
    ax.plot([p[0] for p in sorted_w], [p[1] for p in sorted_w],
            color="#e0e0e0", alpha=0.3, linewidth=1.5, linestyle="--", zorder=1)

    # Labels for windowed
    offsets_w = {
        "HD 5770": (8, 10), "FirePro D700": (8, 10), "RX 580": (8, -18),
        "Vega FE": (8, 8), "RX 6600 XT": (8, -18), "RX 6900 XT": (-95, -10),
        "RX 9070 XT": (-95, -18),
    }
    for name, year, ratio, arch in zip(w_names, w_years, w_ratios, w_archs):
        ox, oy = offsets_w.get(name, (8, 8))
        ax.annotate(f"{name}\n{ratio:.2f}×",
                    xy=(year, ratio), xytext=(ox, oy),
                    textcoords="offset points", fontsize=8,
                    color=ARCH_COLOURS.get(arch, "#e0e0e0"),
                    fontweight="bold",
                    arrowprops=dict(arrowstyle="-", color="#666", lw=0.8)
                    if abs(ox) > 30 or abs(oy) > 30 else None)

    # --- Headless points (star markers, larger) ---
    h_names   = [g[0] for g in gpus_headless]
    h_archs   = [g[1] for g in gpus_headless]
    h_years   = [g[2] for g in gpus_headless]
    h_ratios  = [g[3] for g in gpus_headless]
    h_colours = [ARCH_COLOURS.get(a, "#888") for a in h_archs]

    ax.scatter(h_years, h_ratios, c=h_colours, s=250, zorder=6,
               edgecolors="#e0e0e0", linewidths=1.5, marker="*")

    # Arrow from windowed to headless for each GPU
    for wg in gpus_windowed:
        for hg in gpus_headless:
            if wg[0] == hg[0]:
                ax.annotate("",
                            xy=(hg[2], hg[3]), xytext=(wg[2], wg[3]),
                            arrowprops=dict(arrowstyle="->", color=ARCH_COLOURS.get(wg[1], "#888"),
                                            lw=1.8, alpha=0.6))

    # Labels for headless
    offsets_h = {
        "RX 6900 XT": (8, 8),
        "RX 9070 XT": (8, 8),
    }
    for name, year, ratio, arch in zip(h_names, h_years, h_ratios, h_archs):
        ox, oy = offsets_h.get(name, (8, 8))
        ax.annotate(f"{name} (headless)\n{ratio:.1f}×",
                    xy=(year, ratio), xytext=(ox, oy),
                    textcoords="offset points", fontsize=9,
                    color=ARCH_COLOURS.get(arch, "#e0e0e0"),
                    fontweight="bold")

    # Baseline
    ax.axhline(y=1.0, color="#F1C40F", linestyle=":", linewidth=1, alpha=0.5)

    ax.set_xlabel("Release Year")
    ax.set_ylabel("Performance (normalised to RX 580 = 1.00×)")
    ax.set_title("AMD GPU Generational Progression — Windowed vs Headless",
                 fontsize=14, fontweight="bold", pad=12)
    ax.set_xlim(2007, 2027)
    ax.set_ylim(0, max(h_ratios) * 1.25)
    ax.xaxis.set_major_locator(ticker.MultipleLocator(2))
    ax.grid(alpha=0.3)

    # Legend
    from matplotlib.patches import Patch
    from matplotlib.lines import Line2D
    seen = {}
    for a in w_archs + h_archs:
        if a not in seen:
            seen[a] = ARCH_COLOURS.get(a, "#888")
    legend_handles = [Patch(facecolor=c, edgecolor="#333", label=a)
                      for a, c in seen.items()]
    legend_handles.append(Line2D([0], [0], marker="o", color="#888", markersize=8,
                                  linestyle="", label="Windowed 1M"))
    legend_handles.append(Line2D([0], [0], marker="*", color="#888", markersize=12,
                                  linestyle="", label="Headless 1M"))
    ax.legend(handles=legend_handles, loc="upper left", fontsize=9)

    fig.tight_layout()
    if save_dir:
        fig.savefig(save_dir / "amd_generational_progression.png",
                    bbox_inches="tight")
        print(f"  Saved {save_dir / 'amd_generational_progression.png'}")
    else:
        plt.show()
    plt.close(fig)


# ── Chart 3: Memory Bandwidth vs FPS Correlation (13f) ───────────────────

def chart_bandwidth_correlation(save_dir):
    gpus = [
        ("HD 5770",      "TeraScale 2",  76.8,   188, 2.45),
        ("FirePro D700", "GCN 1.0",     264,     555, 2.10),
        ("RX 580",       "GCN 4",       256,     912, 3.56),
        ("Vega FE",      "GCN 5",       483,    1716, 3.55),
        ("RX 6600 XT",   "RDNA 2",      256,    1834, 7.16),
        ("RX 6900 XT",   "RDNA 2",      512,    4068, 7.95),
        ("RX 9070 XT",   "RDNA 4",      512,    1774, 3.46),
        ("iGPU (2 CU)",  "RDNA 2",       83,     324, 3.90),
    ]

    names   = [g[0] for g in gpus]
    archs   = [g[1] for g in gpus]
    bws     = np.array([g[2] for g in gpus])
    fpss    = np.array([g[3] for g in gpus])
    colours = [ARCH_COLOURS.get(a, "#888") for a in archs]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(16, 6))

    # Left: BW vs FPS scatter
    ax1.scatter(bws, fpss, c=colours, s=180, zorder=5,
                edgecolors="#e0e0e0", linewidths=1.2)

    # Trend line (excluding 9070 XT which is presentation-limited)
    mask = np.array([n != "RX 9070 XT" for n in names])
    if mask.sum() >= 2:
        z = np.polyfit(bws[mask], fpss[mask], 1)
        p = np.poly1d(z)
        x_trend = np.linspace(50, 550, 100)
        ax1.plot(x_trend, p(x_trend), color="#E74C3C", linestyle="--",
                 linewidth=1.5, alpha=0.6, label=f"Trend (excl. 9070 XT)")

    for name, bw, fps, arch in zip(names, bws, fpss, archs):
        ox, oy = (8, 10)
        if name == "RX 580":
            ox, oy = (-50, -20)
        elif name == "RX 9070 XT":
            ox, oy = (-90, 10)
        elif name == "iGPU (2 CU)":
            ox, oy = (8, -18)
        elif name == "RX 6600 XT":
            ox, oy = (-80, -18)
        ax1.annotate(name, xy=(bw, fps), xytext=(ox, oy),
                     textcoords="offset points", fontsize=8.5,
                     color=ARCH_COLOURS.get(arch, "#e0e0e0"), fontweight="bold")

    ax1.set_xlabel("Memory Bandwidth (GB/s)")
    ax1.set_ylabel("FPS (best API)")
    ax1.set_title("Memory Bandwidth vs FPS", fontsize=13, fontweight="bold")
    ax1.grid(alpha=0.3)
    ax1.legend(fontsize=9)

    # Right: FPS per GB/s (efficiency) bar chart
    efficiency = fpss / bws
    sorted_idx = np.argsort(efficiency)
    sorted_names = [names[i] for i in sorted_idx]
    sorted_eff = efficiency[sorted_idx]
    sorted_colours = [colours[i] for i in sorted_idx]

    bars = ax2.barh(sorted_names, sorted_eff, color=sorted_colours,
                    edgecolor="#333", linewidth=0.5)
    for bar, v in zip(bars, sorted_eff):
        ax2.text(v + 0.1, bar.get_y() + bar.get_height() / 2,
                 f"{v:.2f}", va="center", fontsize=9, fontweight="bold")

    ax2.set_xlabel("FPS per GB/s of Bandwidth")
    ax2.set_title("Bandwidth Efficiency (FPS / GB/s)",
                  fontsize=13, fontweight="bold")
    ax2.grid(axis="x", alpha=0.3)

    # Shared legend
    from matplotlib.patches import Patch
    seen = {}
    for a in archs:
        if a not in seen:
            seen[a] = ARCH_COLOURS.get(a, "#888")
    legend_handles = [Patch(facecolor=c, edgecolor="#333", label=a)
                      for a, c in seen.items()]
    ax2.legend(handles=legend_handles, loc="lower right", fontsize=8)

    fig.suptitle("Memory Bandwidth as Performance Predictor",
                 fontsize=15, fontweight="bold", y=1.02)
    fig.tight_layout()
    if save_dir:
        fig.savefig(save_dir / "amd_bandwidth_correlation.png",
                    bbox_inches="tight")
        print(f"  Saved {save_dir / 'amd_bandwidth_correlation.png'}")
    else:
        plt.show()
    plt.close(fig)


# ── Chart 4: Barrier Cost Comparison (13h) ────────────────────────────────

def chart_barrier_cost(save_dir):
    # "< 0.001" → 0.0005 for visualisation (shown as < 0.001 in label)
    gpus = [
        ("RX 9070 XT\n(RDNA 4, GDDR6)",      0.005,  False),
        ("RX 6900 XT\n(RDNA 2, GDDR6)",       0.0005, True),
        ("RX 6600 XT\n(RDNA 2, GDDR6)",       0.0005, True),
        ("Vega FE\n(GCN 5, HBM2)",            0.001,  False),
        ("RX 580\n(GCN 4, GDDR5)",            0.006,  False),
        ("FirePro D700\n(GCN 1.0, GDDR5)",    0.003,  False),
        ("iGPU 2 CU\n(RDNA 2, DDR5)",         0.0005, True),
    ]

    names   = [g[0] for g in gpus]
    values  = [g[1] for g in gpus]
    below   = [g[2] for g in gpus]

    # Colour by memory type
    mem_colours = {
        "GDDR6": "#3498DB",
        "GDDR5": "#F1C40F",
        "HBM2":  "#2ECC71",
        "DDR5":  "#E74C3C",
    }
    colours = []
    for n in names:
        if "HBM2" in n:
            colours.append(mem_colours["HBM2"])
        elif "DDR5" in n:
            colours.append(mem_colours["DDR5"])
        elif "GDDR5" in n:
            colours.append(mem_colours["GDDR5"])
        else:
            colours.append(mem_colours["GDDR6"])

    fig, ax = plt.subplots(figsize=(12, 5))
    bars = ax.bar(names, values, color=colours, edgecolor="#333", linewidth=0.5,
                  width=0.65)

    # Value labels
    for bar, v, b in zip(bars, values, below):
        label = "< 0.001" if b else f"{v:.3f}"
        ax.text(bar.get_x() + bar.get_width() / 2, v + 0.0002,
                label, ha="center", va="bottom", fontsize=10, fontweight="bold")

    # Threshold line
    ax.axhline(y=0.001, color="#E74C3C", linestyle=":", linewidth=1, alpha=0.6)
    ax.text(len(gpus) - 0.5, 0.0012, "timestamp resolution limit",
            fontsize=8, color="#E74C3C", alpha=0.8, ha="right")

    ax.set_ylabel("Barrier Duration (ms)")
    ax.set_title("Pipeline Barrier Cost: Compute → Vertex Input\n"
                 "(vkCmdPipelineBarrier, Vulkan, 1M particles)",
                 fontsize=13, fontweight="bold", pad=12)
    ax.set_ylim(0, max(values) * 1.5)
    ax.yaxis.set_major_formatter(ticker.FormatStrFormatter("%.3f"))
    ax.grid(axis="y", alpha=0.3)

    # Legend by memory type
    from matplotlib.patches import Patch
    legend_handles = [Patch(facecolor=c, edgecolor="#333", label=m)
                      for m, c in mem_colours.items()]
    ax.legend(handles=legend_handles, loc="upper left", fontsize=9)

    fig.tight_layout()
    if save_dir:
        fig.savefig(save_dir / "amd_barrier_cost.png", bbox_inches="tight")
        print(f"  Saved {save_dir / 'amd_barrier_cost.png'}")
    else:
        plt.show()
    plt.close(fig)


# ── Main ──────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Generate AMD GPU analysis charts for report Section 13.")
    parser.add_argument("--save", type=Path, default=None,
                        help="Directory to save PNGs (e.g. docs/images)")
    args = parser.parse_args()

    if args.save:
        args.save.mkdir(parents=True, exist_ok=True)
        matplotlib.use("Agg")

    apply_style()

    print("Generating AMD analysis charts...")
    chart_per_cu_efficiency(args.save)
    chart_generational(args.save)
    chart_bandwidth_correlation(args.save)
    chart_barrier_cost(args.save)
    print("Done.")


if __name__ == "__main__":
    main()
