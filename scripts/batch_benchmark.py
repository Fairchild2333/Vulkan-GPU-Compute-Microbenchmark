#!/usr/bin/env python3
"""
batch_benchmark.py — Automated benchmark runner.

Iterates over all GPU × API × difficulty combinations and runs the benchmark
executable for each, collecting results into ~/.gpu_bench/results.json.

Usage:
    python scripts/batch_benchmark.py                          # auto-detect GPUs & APIs
    python scripts/batch_benchmark.py --exe build/Release/gpu_benchmark.exe
    python scripts/batch_benchmark.py --apis vulkan dx11       # only these APIs
    python scripts/batch_benchmark.py --gpus 0 1               # only these GPU indices
    python scripts/batch_benchmark.py --particles 1048576 16777216  # sweep particle counts
    python scripts/batch_benchmark.py --frames 500             # frames per run
    python scripts/batch_benchmark.py --dry-run                # print commands without running
"""

import argparse
import itertools
import json
import os
import subprocess
import sys
import time
from pathlib import Path


DEFAULT_EXE_WIN = Path("build/Release/gpu_benchmark.exe")
DEFAULT_EXE_UNIX = Path("build/gpu_benchmark")

DIFFICULTY_MAP = {
    "Low":     65536,
    "Medium":  1048576,
    "High":    4194304,
    "Extreme": 16777216,
}

ALL_APIS = ["vulkan", "dx12", "dx11", "opengl", "metal"]


def find_exe() -> Path:
    if sys.platform == "win32" and DEFAULT_EXE_WIN.exists():
        return DEFAULT_EXE_WIN
    if DEFAULT_EXE_UNIX.exists():
        return DEFAULT_EXE_UNIX
    print("ERROR: could not find gpu_benchmark executable.\n"
          "  Build the project first, or pass --exe <path>.", file=sys.stderr)
    sys.exit(1)


def probe_gpus(exe: Path) -> list[dict]:
    """Run --help and parse available APIs; run briefly to detect GPUs."""
    try:
        out = subprocess.run(
            [str(exe), "--help"],
            capture_output=True, text=True, timeout=10
        )
        lines = out.stdout + out.stderr
    except Exception:
        lines = ""

    apis = []
    for api in ALL_APIS:
        if api in lines.lower():
            apis.append(api)

    return apis


def run_single(exe: Path, api: str, gpu: int, particles: int,
               frames: int, dry_run: bool) -> bool:
    cmd = [
        str(exe),
        "--backend", api,
        "--gpu", str(gpu),
        "--benchmark", str(frames),
        "--particles", str(particles),
    ]

    label = f"[GPU {gpu}] {api.upper()} @ {particles:,} particles"

    if dry_run:
        print(f"  DRY-RUN: {' '.join(cmd)}")
        return True

    print(f"\n{'='*60}")
    print(f"  {label}")
    print(f"  Command: {' '.join(cmd)}")
    print(f"{'='*60}\n")

    start = time.time()
    try:
        result = subprocess.run(cmd, timeout=300)
        elapsed = time.time() - start
        success = result.returncode == 0
        status = "OK" if success else f"FAILED (exit {result.returncode})"
        print(f"\n  [{status}] {label} — {elapsed:.1f}s")
        return success
    except subprocess.TimeoutExpired:
        print(f"\n  [TIMEOUT] {label} — exceeded 300s limit")
        return False
    except Exception as e:
        print(f"\n  [ERROR] {label} — {e}")
        return False


def main():
    parser = argparse.ArgumentParser(
        description="Batch-run benchmarks across GPU × API × particle count")
    parser.add_argument("--exe", type=Path, default=None,
                        help="Path to gpu_benchmark executable")
    parser.add_argument("--apis", nargs="+", default=None,
                        help="APIs to test (default: auto-detect)")
    parser.add_argument("--gpus", nargs="+", type=int, default=[0],
                        help="GPU indices to test (default: 0)")
    parser.add_argument("--particles", nargs="+", type=int, default=None,
                        help="Particle counts to sweep (default: all difficulties)")
    parser.add_argument("--frames", type=int, default=500,
                        help="Benchmark frames per run (default: 500)")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print commands without executing")
    args = parser.parse_args()

    exe = args.exe or find_exe()
    if not exe.exists():
        print(f"ERROR: {exe} not found.", file=sys.stderr)
        sys.exit(1)

    apis = args.apis or probe_gpus(exe)
    if not apis:
        apis = ["vulkan"]
    print(f"Executable: {exe}")
    print(f"APIs:       {', '.join(apis)}")
    print(f"GPUs:       {args.gpus}")

    particle_counts = args.particles or list(DIFFICULTY_MAP.values())
    print(f"Particles:  {', '.join(f'{p:,}' for p in particle_counts)}")
    print(f"Frames:     {args.frames}")

    combos = list(itertools.product(args.gpus, apis, particle_counts))
    print(f"\nTotal runs: {len(combos)}")

    if args.dry_run:
        print("\n--- DRY RUN ---\n")

    passed = 0
    failed = 0
    skipped_combos = []

    for gpu, api, particles in combos:
        ok = run_single(exe, api, gpu, particles, args.frames, args.dry_run)
        if ok:
            passed += 1
        else:
            failed += 1
            skipped_combos.append(f"GPU {gpu} / {api} / {particles:,}")

    print(f"\n{'='*60}")
    print(f"  Batch complete: {passed} passed, {failed} failed "
          f"out of {len(combos)} total")
    if skipped_combos:
        print(f"  Failed runs:")
        for s in skipped_combos:
            print(f"    - {s}")
    print(f"{'='*60}")
    print(f"\nResults saved to {Path.home() / '.gpu_bench' / 'results.json'}")
    print(f"Run 'python scripts/plot_results.py' to generate charts.")


if __name__ == "__main__":
    main()
