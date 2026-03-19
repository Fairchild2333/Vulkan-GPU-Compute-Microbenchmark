#!/usr/bin/env python3
"""
compare_rdoc_timing.py — Cross-validate app timestamp queries vs RenderDoc GPU timing.

Reads:
  - App results from ~/.gpu_bench/results.json (application's own timestamps)
  - RenderDoc timing from rdoc_timing.json (exported by rdoc_export_timing.py)

Produces:
  - Side-by-side comparison table (markdown)
  - Deviation analysis

Usage:
    python scripts/compare_rdoc_timing.py rdoc_timing.json
    python scripts/compare_rdoc_timing.py rdoc_timing.json --json results.json
    python scripts/compare_rdoc_timing.py rdoc_timing_6900xt.json rdoc_timing_igpu.json
"""

import argparse
import json
import os
import sys
from pathlib import Path


def default_results_path() -> Path:
    if sys.platform == "win32":
        home = os.environ.get("USERPROFILE", "")
    else:
        home = os.environ.get("HOME", "")
    return Path(home) / ".gpu_bench" / "results.json"


def load_results(path: Path) -> list[dict]:
    if not path.exists():
        return []
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if isinstance(data, dict) and "results" in data:
        return data["results"]
    return data if isinstance(data, list) else []


def load_rdoc_timing(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def short_gpu(name: str) -> str:
    for old, new in [("NVIDIA GeForce ", ""), ("AMD Radeon ", ""),
                     ("(TM)", ""), ("Graphics", "iGPU"),
                     ("Microsoft Basic Render Driver", "WARP")]:
        name = name.replace(old, new)
    return name.strip()


def find_matching_result(results: list[dict], rdoc: dict) -> dict | None:
    """Find the app result that best matches the RenderDoc capture (Vulkan, same GPU)."""
    for r in results:
        api = r.get("graphicsApi", "").lower()
        if "vulkan" not in api:
            continue
        return r
    return results[0] if results else None


def deviation(app_val: float, rdoc_val: float) -> str:
    if app_val == 0 or rdoc_val == 0:
        return "N/A"
    pct = ((rdoc_val / app_val) - 1) * 100
    return f"{pct:+.1f}%"


def print_comparison(rdoc: dict, app_result: dict | None):
    summary = rdoc.get("summary", {})
    rdoc_compute = summary.get("computeMs", 0)
    rdoc_render = summary.get("renderMs", 0)
    rdoc_barrier = summary.get("barrierMs", 0)
    rdoc_total = summary.get("totalMs", 0)

    app_compute = app_result.get("avgComputeMs", 0) if app_result else 0
    app_render = app_result.get("avgRenderMs", 0) if app_result else 0
    app_total = app_result.get("avgTotalGpuMs", 0) if app_result else 0

    source = rdoc.get("source", "?")
    gpu = rdoc.get("gpu", "")
    if not gpu and app_result:
        gpu = short_gpu(app_result.get("deviceName", "?"))

    print()
    print(f"## Timestamp Cross-Validation: {gpu}")
    print()
    print(f"RenderDoc capture: `{source}`")
    if app_result:
        print(f"App result: {app_result.get('graphicsApi', '?')} / "
              f"{short_gpu(app_result.get('deviceName', '?'))} / "
              f"{app_result.get('particleCount', '?'):,} particles")
    print()
    print("| Metric | App Timestamps (ms) | RenderDoc (ms) | Deviation |")
    print("|--------|--------------------:|---------------:|-----------|")
    print(f"| Compute dispatch | {app_compute:.3f} | {rdoc_compute:.3f} "
          f"| {deviation(app_compute, rdoc_compute)} |")
    print(f"| Render pass | {app_render:.3f} | {rdoc_render:.3f} "
          f"| {deviation(app_render, rdoc_render)} |")
    print(f"| Barrier | — | {rdoc_barrier:.3f} | — |")
    print(f"| **Total GPU** | **{app_total:.3f}** | **{rdoc_total:.3f}** "
          f"| **{deviation(app_total, rdoc_total)}** |")
    print()

    if app_total > 0 and rdoc_total > 0:
        dev_abs = abs(rdoc_total / app_total - 1) * 100
        if dev_abs < 5:
            verdict = "Excellent — timestamps and RenderDoc agree within 5%."
        elif dev_abs < 15:
            verdict = ("Good — deviation likely due to RenderDoc interception "
                       "overhead or single-frame variance vs multi-frame average.")
        else:
            verdict = ("Significant deviation — possible causes: RenderDoc "
                       "serialisation, different frame, or timestamp calibration.")
        print(f"> **Verdict**: {verdict} ({dev_abs:.1f}% total deviation)")
    print()

    events = rdoc.get("events", [])
    if events:
        print("### Per-Event Breakdown")
        print()
        print("| Event ID | Name | Category | GPU (ms) |")
        print("|----------|------|----------|--------:|")
        for e in events:
            if e.get("gpuDurationMs", 0) > 0.0001:
                print(f"| {e['eventId']} | {e['name']} | {e['category']} "
                      f"| {e['gpuDurationMs']:.4f} |")
        print()


def main():
    parser = argparse.ArgumentParser(
        description="Cross-validate app timestamps vs RenderDoc GPU timing")
    parser.add_argument("rdoc_files", nargs="+",
                        help="RenderDoc timing JSON file(s) from rdoc_export_timing.py")
    parser.add_argument("--json", type=Path, default=None,
                        help="Path to results.json (default: ~/.gpu_bench/results.json)")
    args = parser.parse_args()

    results_path = args.json or default_results_path()
    results = load_results(results_path)
    print(f"Loaded {len(results)} app benchmark result(s)", file=sys.stderr)

    for rdoc_file in args.rdoc_files:
        rdoc = load_rdoc_timing(rdoc_file)
        app_match = find_matching_result(results, rdoc)
        print_comparison(rdoc, app_match)


if __name__ == "__main__":
    main()
