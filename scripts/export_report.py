#!/usr/bin/env python3
"""
export_report.py — Generate markdown tables and HTML reports from benchmark results.

Reads ~/.gpu_bench/results.json and outputs:
  1. A markdown comparison table (for pasting into docs)
  2. A standalone HTML report with sortable tables

Usage:
    python scripts/export_report.py                           # print markdown to stdout
    python scripts/export_report.py --md docs/results-table.md  # save markdown file
    python scripts/export_report.py --html docs/report.html      # save HTML report
    python scripts/export_report.py --json path.json             # custom results file
"""

import argparse
import json
import os
import sys
from datetime import datetime
from pathlib import Path


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


def short_gpu(name: str) -> str:
    for old, new in [("NVIDIA GeForce ", ""), ("AMD Radeon ", ""),
                     ("(TM)", ""), ("Graphics", "iGPU"),
                     ("Microsoft Basic Render Driver", "WARP")]:
        name = name.replace(old, new)
    return name.strip()


def normalise_api(api: str) -> str:
    mapping = {"vulkan": "Vulkan", "dx12": "DX12", "directx 12": "DX12",
               "dx11": "DX11", "directx 11": "DX11",
               "opengl": "OpenGL", "opengl 4.3": "OpenGL", "metal": "Metal"}
    return mapping.get(api.lower(), api)


def fmt_count(n) -> str:
    n = int(n)
    if n >= 1_000_000:
        return f"{n / 1_000_000:.1f}M"
    if n >= 1_000:
        return f"{n / 1_000:.0f}K"
    return str(n)


# ── Markdown ────────────────────────────────────────────────────────────────

def generate_markdown(results: list[dict]) -> str:
    results_sorted = sorted(results, key=lambda r: r.get("avgFps", 0),
                            reverse=True)

    lines = [
        "# Benchmark Results Comparison",
        "",
        f"> Generated: {datetime.now().strftime('%Y-%m-%d %H:%M')}  ",
        f"> Results: {len(results_sorted)} entries",
        "",
        "## Ranked by Average FPS",
        "",
        "| # | GPU | API | Particles | Avg FPS | Avg GPU (ms) | "
        "Compute (ms) | Render (ms) | Frame (ms) | CPU OH (ms) | Bottleneck |",
        "|---|-----|-----|-----------|---------|-------------|"
        "-------------|-------------|------------|-------------|------------|",
    ]

    best_fps = results_sorted[0].get("avgFps", 1) if results_sorted else 1

    for i, r in enumerate(results_sorted, 1):
        gpu = short_gpu(r.get("deviceName", "?"))
        api = normalise_api(r.get("graphicsApi", "?"))
        particles = fmt_count(r.get("particleCount", 0))
        fps = r.get("avgFps", 0)
        gpu_ms = r.get("avgTotalGpuMs", 0)
        compute = r.get("avgComputeMs", 0)
        render = r.get("avgRenderMs", 0)
        frame = r.get("avgFrameTimeMs", 0)
        cpu_oh = max(frame - gpu_ms, 0)
        bottleneck = r.get("bottleneck", "—")
        delta = ((fps / best_fps) - 1) * 100 if best_fps > 0 else 0

        fps_str = f"{fps:,.0f}" if i == 1 else f"{fps:,.0f} ({delta:+.0f}%)"

        lines.append(
            f"| {i} | {gpu} | {api} | {particles} | {fps_str} | "
            f"{gpu_ms:.3f} | {compute:.3f} | {render:.3f} | "
            f"{frame:.3f} | {cpu_oh:.3f} | {bottleneck} |"
        )

    lines.extend([
        "",
        "## GPU Time Breakdown",
        "",
        "| GPU | API | Particles | Compute (ms) | Render (ms) | "
        "Total GPU (ms) | GPU Util (%) |",
        "|-----|-----|-----------|-------------|-------------|"
        "---------------|-------------|",
    ])

    for r in results_sorted:
        gpu = short_gpu(r.get("deviceName", "?"))
        api = normalise_api(r.get("graphicsApi", "?"))
        particles = fmt_count(r.get("particleCount", 0))
        compute = r.get("avgComputeMs", 0)
        render = r.get("avgRenderMs", 0)
        total = r.get("avgTotalGpuMs", 0)
        util = r.get("gpuUtilisation", 0)

        lines.append(
            f"| {gpu} | {api} | {particles} | {compute:.3f} | "
            f"{render:.3f} | {total:.3f} | {util:.1f} |"
        )

    lines.append("")
    return "\n".join(lines)


# ── HTML ────────────────────────────────────────────────────────────────────

def generate_html(results: list[dict]) -> str:
    results_sorted = sorted(results, key=lambda r: r.get("avgFps", 0),
                            reverse=True)

    rows_html = ""
    for i, r in enumerate(results_sorted, 1):
        gpu = short_gpu(r.get("deviceName", "?"))
        api = normalise_api(r.get("graphicsApi", "?"))
        particles = r.get("particleCount", 0)
        fps = r.get("avgFps", 0)
        gpu_ms = r.get("avgTotalGpuMs", 0)
        compute = r.get("avgComputeMs", 0)
        render = r.get("avgRenderMs", 0)
        frame = r.get("avgFrameTimeMs", 0)
        cpu_oh = max(frame - gpu_ms, 0)
        bottleneck = r.get("bottleneck", "—")
        cpu_name = r.get("cpuName", "—")
        timestamp = r.get("timestamp", "—")
        memory = r.get("memory", "—")

        api_class = api.lower().replace(" ", "")
        rows_html += f"""
        <tr class="api-{api_class}">
          <td>{i}</td><td>{gpu}</td><td>{api}</td>
          <td>{particles:,}</td><td>{fps:,.0f}</td>
          <td>{gpu_ms:.3f}</td><td>{compute:.3f}</td><td>{render:.3f}</td>
          <td>{frame:.3f}</td><td>{cpu_oh:.3f}</td>
          <td>{bottleneck}</td><td>{memory}</td>
          <td>{cpu_name}</td><td>{timestamp}</td>
        </tr>"""

    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>GPU Benchmark Report</title>
<style>
  * {{ margin: 0; padding: 0; box-sizing: border-box; }}
  body {{
    font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
    background: #0f0f23; color: #e0e0e0;
    padding: 2rem; line-height: 1.6;
  }}
  h1 {{ color: #fff; margin-bottom: 0.5rem; }}
  .meta {{ color: #888; margin-bottom: 2rem; font-size: 0.9rem; }}
  table {{
    width: 100%; border-collapse: collapse; font-size: 0.85rem;
    margin-bottom: 2rem;
  }}
  th {{
    background: #1a1a3e; color: #aaa; text-transform: uppercase;
    font-size: 0.75rem; letter-spacing: 0.05em;
    padding: 0.7rem 0.5rem; text-align: left; cursor: pointer;
    border-bottom: 2px solid #333; white-space: nowrap;
  }}
  th:hover {{ color: #fff; }}
  td {{
    padding: 0.5rem; border-bottom: 1px solid #222; white-space: nowrap;
  }}
  tr:hover {{ background: #1a1a3e; }}
  .api-vulkan td:nth-child(3) {{ color: #E74C3C; font-weight: 600; }}
  .api-dx12 td:nth-child(3) {{ color: #3498DB; font-weight: 600; }}
  .api-dx11 td:nth-child(3) {{ color: #2ECC71; font-weight: 600; }}
  .api-opengl td:nth-child(3) {{ color: #F39C12; font-weight: 600; }}
  .api-metal td:nth-child(3) {{ color: #9B59B6; font-weight: 600; }}
  footer {{ color: #555; font-size: 0.8rem; margin-top: 2rem; }}
</style>
</head>
<body>
<h1>GPU Compute Microbenchmark Report</h1>
<p class="meta">
  Generated: {datetime.now().strftime('%Y-%m-%d %H:%M')} &bull;
  {len(results_sorted)} result(s) &bull;
  Ranked by Average FPS (descending)
</p>
<table id="results">
<thead><tr>
  <th>#</th><th>GPU</th><th>API</th><th>Particles</th><th>Avg FPS</th>
  <th>GPU (ms)</th><th>Compute (ms)</th><th>Render (ms)</th>
  <th>Frame (ms)</th><th>CPU OH (ms)</th><th>Bottleneck</th>
  <th>Memory</th><th>CPU</th><th>Timestamp</th>
</tr></thead>
<tbody>{rows_html}
</tbody>
</table>
<footer>
  GPU Compute Microbenchmark &mdash; github.com/your-repo
</footer>
<script>
document.querySelectorAll('#results th').forEach((th, col) => {{
  th.addEventListener('click', () => {{
    const tbody = document.querySelector('#results tbody');
    const rows = [...tbody.querySelectorAll('tr')];
    const dir = th.dataset.dir === 'asc' ? 'desc' : 'asc';
    th.dataset.dir = dir;
    rows.sort((a, b) => {{
      let av = a.children[col].textContent.replace(/,/g, '');
      let bv = b.children[col].textContent.replace(/,/g, '');
      let an = parseFloat(av), bn = parseFloat(bv);
      if (!isNaN(an) && !isNaN(bn))
        return dir === 'asc' ? an - bn : bn - an;
      return dir === 'asc' ? av.localeCompare(bv) : bv.localeCompare(av);
    }});
    rows.forEach((r, i) => {{ r.children[0].textContent = i + 1; tbody.appendChild(r); }});
  }});
}});
</script>
</body>
</html>"""


# ── Main ────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Export benchmark results as markdown tables or HTML report")
    parser.add_argument("--json", type=Path, default=None,
                        help="Path to results.json (default: ~/.gpu_bench/results.json)")
    parser.add_argument("--md", type=Path, default=None,
                        help="Save markdown output to file")
    parser.add_argument("--html", type=Path, default=None,
                        help="Save HTML report to file")
    args = parser.parse_args()

    results_path = args.json or default_results_path()
    results = load_results(results_path)
    print(f"Loaded {len(results)} result(s) from {results_path}", file=sys.stderr)

    if not results:
        print("No results to export.", file=sys.stderr)
        return

    md = generate_markdown(results)

    if args.md:
        args.md.parent.mkdir(parents=True, exist_ok=True)
        args.md.write_text(md, encoding="utf-8")
        print(f"Markdown saved to {args.md}", file=sys.stderr)
    elif not args.html:
        print(md)

    if args.html:
        html = generate_html(results)
        args.html.parent.mkdir(parents=True, exist_ok=True)
        args.html.write_text(html, encoding="utf-8")
        print(f"HTML report saved to {args.html}", file=sys.stderr)


if __name__ == "__main__":
    main()
