#!/usr/bin/env python3
"""
compare_3dmark.py — Cross-validate benchmark results against 3DMark scores.

Compares this project's FPS results with 3DMark Time Spy / Fire Strike scores
to validate that the benchmark accurately reflects GPU performance differences.

Data sources:
  - Project results: ~/.gpu_bench/results.json (auto-loaded)
  - 3DMark scores:   scripts/3dmark_scores.json (manually or auto-populated)

Usage:
    python scripts/compare_3dmark.py                    # show charts
    python scripts/compare_3dmark.py --save docs/images # save PNGs
    python scripts/compare_3dmark.py --md               # print markdown table

    # Auto-import from .3dmark-result files (ZIP archives containing XML)
    python scripts/compare_3dmark.py --import-3dmark path/to/result1.3dmark-result path/to/result2.3dmark-result
    python scripts/compare_3dmark.py --import-3dmark C:/Users/*/Documents/3DMark/*.3dmark-result

    # Auto-import from exported XML files (3DMark Professional --export)
    python scripts/compare_3dmark.py --import-xml path/to/exported.xml

First run (manual):
    1. Edit scripts/3dmark_scores.json with your 3DMark scores.
    2. Run the project benchmark on each GPU.
    3. Run this script to generate the comparison.
"""

import argparse
import glob
import json
import os
import re
import sys
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

try:
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError:
    print("ERROR: matplotlib and numpy are required.\n"
          "  pip install matplotlib numpy", file=sys.stderr)
    sys.exit(1)


SCORES_FILE = Path(__file__).parent / "3dmark_scores.json"


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
    return data if isinstance(data, list) else []


def load_3dmark_scores() -> dict:
    if not SCORES_FILE.exists():
        print(f"ERROR: {SCORES_FILE} not found.\n"
              f"  Create it first — see the template in the file header.",
              file=sys.stderr)
        sys.exit(1)
    with open(SCORES_FILE, "r", encoding="utf-8") as f:
        return json.load(f)


# ── 3DMark file import ──────────────────────────────────────────────────────

def detect_benchmark_type(results: list[dict]) -> str:
    """Guess Time Spy / Fire Strike / etc. from result element names."""
    names = {r.get("name", "") for r in results}
    name_str = " ".join(names).lower()
    if "timespy" in name_str or "time spy" in name_str:
        return "TimeSpy"
    if "firestrike" in name_str or "fire strike" in name_str:
        return "FireStrike"
    if "portroyal" in name_str or "port royal" in name_str:
        return "PortRoyal"
    if "nightraid" in name_str or "night raid" in name_str:
        return "NightRaid"
    return "Unknown"


def extract_score_from_results(results: list[dict]) -> int:
    """Find the overall or graphics score from a list of result dicts."""
    score = 0
    for r in results:
        name = r.get("name", "").lower()
        val = r.get("value", 0)
        if not val:
            continue
        try:
            val = int(float(val))
        except (ValueError, TypeError):
            continue
        if "graphicsscore" in name or "graphics score" in name:
            return val
        if "overallscore" in name or "overall" in name:
            score = max(score, val)
        if "score" in name and val > score:
            score = val
    return score


def extract_fps_values(results: list[dict]) -> list[float]:
    fps = []
    for r in results:
        unit = r.get("unit", "")
        if unit == "fps":
            try:
                fps.append(float(r["value"]))
            except (ValueError, KeyError):
                pass
    return fps


def parse_arielle_xml(root) -> dict:
    """Parse arielle.xml (inside .3dmark-result ZIP) for scores and FPS."""
    results = []
    for elem in root.iter("result"):
        entry = {}
        name_el = elem.find("name")
        value_el = elem.find("value")
        pr_el = elem.find("primary_result")
        if name_el is not None and name_el.text:
            entry["name"] = name_el.text.strip()
        if value_el is not None and value_el.text:
            entry["value"] = value_el.text.strip()
        if pr_el is not None and pr_el.text:
            entry["value"] = pr_el.text.strip()
            entry["unit"] = pr_el.get("unit", "")
        if entry:
            results.append(entry)

    benchmark = detect_benchmark_type(results)
    score = extract_score_from_results(results)
    fps_vals = extract_fps_values(results)

    return {
        "benchmark": benchmark,
        "score": score,
        "avgFps": round(sum(fps_vals) / len(fps_vals), 2) if fps_vals else 0,
        "fpsValues": fps_vals,
        "resultCount": len(results),
    }


def parse_si_xml(root) -> dict:
    """Parse si.xml (inside .3dmark-result ZIP) for GPU/CPU info."""
    info = {}

    cpu = root.find(".//Direct_Query_Info/CPU")
    if cpu is not None and cpu.text:
        info["cpu"] = cpu.text.strip()

    gpu_card = root.find(".//GPUZ_Info/GPUs/GPU/CARD_NAME")
    if gpu_card is not None and gpu_card.text:
        info["gpu"] = gpu_card.text.strip()

    gpu_name = root.find(".//GPUZ_Info/GPUs/GPU/GPU_NAME")
    if gpu_name is not None and gpu_name.text:
        info["gpuChip"] = gpu_name.text.strip()

    vram = root.find(".//GPUZ_Info/GPUs/GPU/MEM_SIZE")
    if vram is not None and vram.text:
        try:
            info["vramMB"] = int(vram.text.strip())
        except ValueError:
            pass

    driver = root.find(".//GPUZ_Info/GPUs/GPU/DRIVER_VER")
    if driver is not None and driver.text:
        info["driver"] = driver.text.strip()

    os_ver = root.find(".//Direct_Query_Info/OS")
    if os_ver is not None and os_ver.text:
        info["os"] = os_ver.text.strip()

    return info


def import_3dmark_result(filepath: str) -> dict | None:
    """Import a .3dmark-result file (ZIP containing arielle.xml + si.xml)."""
    path = Path(filepath)
    if not path.exists():
        print(f"  SKIP: {path} not found", file=sys.stderr)
        return None

    try:
        with zipfile.ZipFile(path, "r") as zf:
            names = zf.namelist()

            arielle_path = next(
                (n for n in names if n.lower().endswith("arielle.xml")), None)
            si_path = next(
                (n for n in names if n.lower().endswith("si.xml")), None)

            result = {"source": str(path)}

            if arielle_path:
                with zf.open(arielle_path) as f:
                    tree = ET.parse(f)
                    result.update(parse_arielle_xml(tree.getroot()))

            if si_path:
                with zf.open(si_path) as f:
                    tree = ET.parse(f)
                    result["systemInfo"] = parse_si_xml(tree.getroot())

            return result
    except zipfile.BadZipFile:
        print(f"  SKIP: {path} is not a valid ZIP file", file=sys.stderr)
        return None
    except Exception as e:
        print(f"  SKIP: {path} — {e}", file=sys.stderr)
        return None


def import_export_xml(filepath: str) -> dict | None:
    """Import an XML file from 3DMark Professional --export."""
    path = Path(filepath)
    if not path.exists():
        print(f"  SKIP: {path} not found", file=sys.stderr)
        return None

    try:
        tree = ET.parse(path)
        root = tree.getroot()
        result = {"source": str(path)}
        result.update(parse_arielle_xml(root))
        return result
    except Exception as e:
        print(f"  SKIP: {path} — {e}", file=sys.stderr)
        return None


def merge_import_into_scores(imported: list[dict], scores: dict) -> dict:
    """Merge imported 3DMark results into the scores JSON structure."""
    existing = {e["name"]: e for e in scores.get("gpus", [])}

    for imp in imported:
        si = imp.get("systemInfo", {})
        gpu_name = short_gpu(si.get("gpu", "Unknown GPU"))
        bench = imp.get("benchmark", "Unknown")
        score = imp.get("score", 0)
        avg_fps = imp.get("avgFps", 0)

        if gpu_name not in existing:
            existing[gpu_name] = {
                "name": gpu_name,
                "architecture": si.get("gpuChip", "—"),
                "timeSpy": 0,
                "fireStrike": 0,
            }

        entry = existing[gpu_name]

        if bench == "TimeSpy" and score > 0:
            entry["timeSpy"] = score
            print(f"  {gpu_name}: Time Spy = {score:,}"
                  f" (avg {avg_fps:.1f} fps)", file=sys.stderr)
        elif bench == "FireStrike" and score > 0:
            entry["fireStrike"] = score
            print(f"  {gpu_name}: Fire Strike = {score:,}"
                  f" (avg {avg_fps:.1f} fps)", file=sys.stderr)
        else:
            print(f"  {gpu_name}: {bench} = {score:,} (unknown benchmark type,"
                  f" stored but not charted)", file=sys.stderr)

        if si.get("gpuChip") and entry.get("architecture", "—") == "—":
            entry["architecture"] = si["gpuChip"]

    scores["gpus"] = list(existing.values())
    return scores


def save_scores(scores: dict):
    with open(SCORES_FILE, "w", encoding="utf-8") as f:
        json.dump(scores, f, indent=2, ensure_ascii=False)
    print(f"  Updated {SCORES_FILE}", file=sys.stderr)


def short_gpu(name: str) -> str:
    for old, new in [("NVIDIA GeForce ", ""), ("AMD Radeon ", ""),
                     ("(TM)", ""), ("Graphics", "iGPU"),
                     ("Microsoft Basic Render Driver", "WARP")]:
        name = name.replace(old, new)
    return name.strip()


def best_fps_per_gpu(results: list[dict]) -> dict[str, float]:
    """Return the highest FPS per GPU (across all APIs)."""
    best = {}
    for r in results:
        gpu = short_gpu(r.get("deviceName", "?"))
        fps = r.get("avgFps", 0)
        if fps > best.get(gpu, 0):
            best[gpu] = fps
    return best


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


# ── Chart: Normalised performance comparison ────────────────────────────────

def chart_normalised(gpu_fps: dict, scores: dict, save_dir: Path | None):
    """
    Normalise all scores to a baseline GPU = 1.0x, then plot project FPS
    vs 3DMark side by side.
    """
    gpus_in_common = []
    for entry in scores.get("gpus", []):
        name = entry["name"]
        if name in gpu_fps:
            gpus_in_common.append(name)

    if len(gpus_in_common) < 2:
        print("  Need at least 2 GPUs with both project and 3DMark data.")
        return

    baseline = scores.get("baseline", gpus_in_common[0])
    if baseline not in gpus_in_common:
        baseline = gpus_in_common[0]

    score_map = {e["name"]: e for e in scores.get("gpus", [])}

    baseline_fps = gpu_fps.get(baseline, 1)
    baseline_ts = score_map[baseline].get("timeSpy", 1) or 1
    baseline_fs = score_map[baseline].get("fireStrike", 1) or 1

    labels = []
    norm_fps = []
    norm_ts = []
    norm_fs = []

    for gpu in gpus_in_common:
        labels.append(gpu)
        norm_fps.append(gpu_fps[gpu] / baseline_fps if baseline_fps else 0)
        ts = score_map[gpu].get("timeSpy", 0)
        fs = score_map[gpu].get("fireStrike", 0)
        norm_ts.append(ts / baseline_ts if ts and baseline_ts else 0)
        norm_fs.append(fs / baseline_fs if fs and baseline_fs else 0)

    x = np.arange(len(labels))
    width = 0.25

    fig, ax = plt.subplots(figsize=(max(10, len(labels) * 2.5), 6))

    bars1 = ax.bar(x - width, norm_fps, width * 0.9,
                   label="This Benchmark (Best FPS)", color="#E74C3C")
    bars2 = ax.bar(x, norm_ts, width * 0.9,
                   label="3DMark Time Spy (DX12)", color="#3498DB")
    bars3 = ax.bar(x + width, norm_fs, width * 0.9,
                   label="3DMark Fire Strike (DX11)", color="#2ECC71")

    for bars in [bars1, bars2, bars3]:
        for bar in bars:
            h = bar.get_height()
            if h > 0:
                ax.text(bar.get_x() + bar.get_width() / 2, h,
                        f"{h:.2f}x", ha="center", va="bottom", fontsize=8)

    ax.set_ylabel(f"Relative Performance (baseline: {baseline} = 1.0x)")
    ax.set_title("Cross-Validation: Project Benchmark vs 3DMark")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=20, ha="right")
    ax.axhline(y=1.0, color="#888", linestyle="--", linewidth=0.8)
    ax.legend(loc="upper left")
    ax.grid(axis="y")
    fig.tight_layout()

    if save_dir:
        fig.savefig(save_dir / "3dmark_comparison.png")
        print(f"  Saved {save_dir / '3dmark_comparison.png'}")
    else:
        plt.show()
    plt.close(fig)


# ── Chart: Correlation scatter ──────────────────────────────────────────────

def chart_correlation(gpu_fps: dict, scores: dict, save_dir: Path | None):
    """Scatter plot: project FPS vs 3DMark Time Spy, with R² value."""
    score_map = {e["name"]: e for e in scores.get("gpus", [])}
    xs, ys, labels = [], [], []
    for name, entry in score_map.items():
        ts = entry.get("timeSpy", 0)
        if name in gpu_fps and ts > 0:
            xs.append(ts)
            ys.append(gpu_fps[name])
            labels.append(name)

    if len(xs) < 2:
        print("  Need at least 2 data points for correlation.")
        return

    xs_arr = np.array(xs, dtype=float)
    ys_arr = np.array(ys, dtype=float)

    coeffs = np.polyfit(xs_arr, ys_arr, 1)
    poly = np.poly1d(coeffs)
    trend_x = np.linspace(min(xs_arr) * 0.9, max(xs_arr) * 1.1, 100)

    ss_res = np.sum((ys_arr - poly(xs_arr)) ** 2)
    ss_tot = np.sum((ys_arr - np.mean(ys_arr)) ** 2)
    r_squared = 1 - (ss_res / ss_tot) if ss_tot > 0 else 0

    fig, ax = plt.subplots(figsize=(8, 6))
    ax.scatter(xs_arr, ys_arr, color="#E74C3C", s=80, zorder=5)
    ax.plot(trend_x, poly(trend_x), "--", color="#888", linewidth=1,
            label=f"Linear fit (R² = {r_squared:.3f})")

    for xi, yi, lbl in zip(xs, ys, labels):
        ax.annotate(lbl, (xi, yi), textcoords="offset points",
                    xytext=(8, 6), fontsize=8, color="#aaa")

    ax.set_xlabel("3DMark Time Spy (Graphics Score)")
    ax.set_ylabel("Project Benchmark (Avg FPS)")
    ax.set_title("Correlation: Compute Benchmark vs 3DMark Time Spy")
    ax.legend()
    ax.grid(True)
    fig.tight_layout()

    if save_dir:
        fig.savefig(save_dir / "3dmark_correlation.png")
        print(f"  Saved {save_dir / '3dmark_correlation.png'}")
    else:
        plt.show()
    plt.close(fig)


# ── Markdown table ──────────────────────────────────────────────────────────

def print_markdown(gpu_fps: dict, scores: dict):
    score_map = {e["name"]: e for e in scores.get("gpus", [])}
    baseline = scores.get("baseline", "")

    baseline_fps = gpu_fps.get(baseline, 1) or 1
    baseline_ts = (score_map.get(baseline, {}).get("timeSpy", 1)) or 1
    baseline_fs = (score_map.get(baseline, {}).get("fireStrike", 1)) or 1

    print()
    print("## Cross-Validation: Project Benchmark vs 3DMark")
    print()
    print(f"Baseline GPU: **{baseline}** = 1.00x")
    print()
    print("| GPU | Architecture | Project FPS | Norm | "
          "Time Spy | Norm | Fire Strike | Norm | "
          "Deviation (TS) |")
    print("|-----|-------------|------------|------|"
          "----------|------|-------------|------|"
          "----------------|")

    for entry in scores.get("gpus", []):
        name = entry["name"]
        arch = entry.get("architecture", "—")
        ts = entry.get("timeSpy", 0)
        fs = entry.get("fireStrike", 0)
        fps = gpu_fps.get(name, 0)

        n_fps = fps / baseline_fps if baseline_fps else 0
        n_ts = ts / baseline_ts if ts and baseline_ts else 0
        n_fs = fs / baseline_fs if fs and baseline_fs else 0

        deviation = ""
        if n_fps > 0 and n_ts > 0:
            dev = ((n_fps / n_ts) - 1) * 100
            deviation = f"{dev:+.0f}%"

        fps_str = f"{fps:,.0f}" if fps else "—"
        ts_str = f"{ts:,}" if ts else "—"
        fs_str = f"{fs:,}" if fs else "—"

        print(f"| {name} | {arch} | {fps_str} | {n_fps:.2f}x | "
              f"{ts_str} | {n_ts:.2f}x | {fs_str} | {n_fs:.2f}x | "
              f"{deviation} |")

    print()
    print("> **Deviation (TS)**: how much this project's relative performance "
          "differs from 3DMark Time Spy's relative ranking.  ")
    print("> Positive = our benchmark favours this GPU more than 3DMark; "
          "negative = less.  ")
    print("> Small deviations (±15%) are expected due to workload differences "
          "(compute-heavy vs rasterisation-heavy).")
    print()


# ── Main ────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Cross-validate benchmark results against 3DMark scores")
    parser.add_argument("--json", type=Path, default=None,
                        help="Path to results.json")
    parser.add_argument("--save", type=Path, default=None,
                        help="Directory to save PNG charts")
    parser.add_argument("--md", action="store_true",
                        help="Print markdown comparison table to stdout")
    parser.add_argument("--import-3dmark", nargs="+", default=None,
                        metavar="FILE",
                        help=".3dmark-result files to import (supports globs)")
    parser.add_argument("--import-xml", nargs="+", default=None,
                        metavar="FILE",
                        help="Exported XML files to import (3DMark Professional --export)")
    args = parser.parse_args()

    # ── Import mode ─────────────────────────────────────────────────────
    if args.import_3dmark or args.import_xml:
        if SCORES_FILE.exists():
            with open(SCORES_FILE, "r", encoding="utf-8") as f:
                scores = json.load(f)
        else:
            scores = {"baseline": "", "gpus": []}

        imported = []

        if args.import_3dmark:
            expanded = []
            for pattern in args.import_3dmark:
                expanded.extend(glob.glob(pattern, recursive=True))
            if not expanded:
                expanded = args.import_3dmark
            print(f"Importing {len(expanded)} .3dmark-result file(s)...",
                  file=sys.stderr)
            for f in expanded:
                r = import_3dmark_result(f)
                if r:
                    imported.append(r)

        if args.import_xml:
            expanded = []
            for pattern in args.import_xml:
                expanded.extend(glob.glob(pattern, recursive=True))
            if not expanded:
                expanded = args.import_xml
            print(f"Importing {len(expanded)} XML file(s)...",
                  file=sys.stderr)
            for f in expanded:
                r = import_export_xml(f)
                if r:
                    imported.append(r)

        if imported:
            scores = merge_import_into_scores(imported, scores)
            save_scores(scores)
            print(f"\nImported {len(imported)} result(s) into "
                  f"{SCORES_FILE}", file=sys.stderr)
        else:
            print("No results imported.", file=sys.stderr)
        return

    # ── Chart / table mode ──────────────────────────────────────────────
    results_path = args.json or default_results_path()
    results = load_results(results_path)
    scores = load_3dmark_scores()
    gpu_fps = best_fps_per_gpu(results)

    print(f"Loaded {len(results)} benchmark result(s)", file=sys.stderr)
    print(f"Loaded {len(scores.get('gpus', []))} 3DMark GPU entries",
          file=sys.stderr)

    if args.md:
        print_markdown(gpu_fps, scores)
        return

    apply_style()

    if args.save:
        args.save.mkdir(parents=True, exist_ok=True)

    chart_normalised(gpu_fps, scores, args.save)
    chart_correlation(gpu_fps, scores, args.save)

    if args.save:
        print(f"\nCharts saved to {args.save}/")
    print("Done.")


if __name__ == "__main__":
    main()
