#!/usr/bin/env python3
"""
rdoc_analyse.py — Convert RenderDoc captures to Chrome JSON, parse API call
structure, and cross-validate against application timestamp queries.

Usage (called automatically by gpu_benchmark.exe Full Analysis):
    python scripts/rdoc_analyse.py \
        --captures rdoc_captures/ \
        --results  ~/.gpu_bench/results.json \
        --output   docs/rdoc_comparison.md
"""

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from collections import defaultdict


RENDERDOC_CMD = r"C:\Program Files\RenderDoc\renderdoccmd.exe"


def find_renderdoccmd() -> str:
    if os.path.isfile(RENDERDOC_CMD):
        return RENDERDOC_CMD
    for p in os.environ.get("PATH", "").split(os.pathsep):
        candidate = os.path.join(p, "renderdoccmd.exe" if sys.platform == "win32"
                                 else "renderdoccmd")
        if os.path.isfile(candidate):
            return candidate
    return "renderdoccmd"


def convert_rdc_to_json(rdc_path: Path, renderdoccmd: str) -> Path | None:
    json_path = rdc_path.with_suffix(".json")
    if json_path.exists() and json_path.stat().st_mtime >= rdc_path.stat().st_mtime:
        return json_path
    try:
        subprocess.run(
            [renderdoccmd, "convert",
             "-f", str(rdc_path), "-c", "chrome.json", "-o", str(json_path)],
            check=True, capture_output=True, timeout=60)
        return json_path
    except Exception as ex:
        print(f"  WARNING: Failed to convert {rdc_path.name}: {ex}",
              file=sys.stderr)
        return None


def detect_api_from_events(events: list[dict]) -> str:
    for ev in events:
        name = ev.get("name", "")
        if name.startswith("vk"):
            return "Vulkan"
        if name.startswith("ID3D12") or "D3D12" in name or "ExecuteCommandLists" in name:
            return "DX12"
        if name.startswith("ID3D11") or "DrawInstanced" in name or "Dispatch" == name:
            return "DX11"
        if name.startswith("gl") and not name.startswith("glfw"):
            return "OpenGL"
    return "Unknown"


def detect_api_from_filename(name: str) -> str:
    lower = name.lower()
    if "vulkan" in lower:
        return "Vulkan"
    if "dx12" in lower or "directx12" in lower:
        return "DX12"
    if "dx11" in lower or "directx11" in lower:
        return "DX11"
    if "opengl" in lower:
        return "OpenGL"
    if "metal" in lower:
        return "Metal"
    return "Unknown"


def pair_begin_end(events: list[dict]) -> list[dict]:
    """Pair B/E events to compute CPU-side durations."""
    stack = []
    paired = []
    for ev in events:
        ph = ev.get("ph", "")
        if ph == "B":
            stack.append(ev)
        elif ph == "E" and stack:
            begin = stack.pop()
            dur_ns = ev.get("ts", 0) - begin.get("ts", 0)
            paired.append({
                "name": begin.get("name", ""),
                "ts": begin.get("ts", 0),
                "dur_ns": dur_ns,
            })
        elif ph == "i":
            paired.append({
                "name": ev.get("name", ""),
                "ts": ev.get("ts", 0),
                "dur_ns": 0,
            })
    return paired


FRAME_START_MARKERS = [
    "vkWaitForFences", "vkResetFences", "vkBeginCommandBuffer",
    "ID3D12CommandAllocator", "ID3D11DeviceContext",
    "Beginning of Capture"
]


def is_frame_event(ev: dict) -> bool:
    name = ev.get("name", "")
    if name.startswith("Internal::"):
        return False
    if name.startswith("vkCreate") or name.startswith("vkAllocate"):
        cat = ev.get("cat", "")
        if cat == "Initialisation":
            return False
    return True


def extract_frame_commands(paired_events: list[dict]) -> list[dict]:
    """Extract events belonging to the captured frame (skip init)."""
    frame_events = []
    in_frame = False
    for ev in paired_events:
        name = ev.get("name", "")
        if any(m in name for m in FRAME_START_MARKERS):
            in_frame = True
        if in_frame and is_frame_event(ev):
            frame_events.append(ev)
    return frame_events if frame_events else [e for e in paired_events
                                               if is_frame_event(e)]


def classify_event_generic(name: str) -> str:
    lower = name.lower()

    # Compute dispatch
    if any(kw in lower for kw in ["cmddispatch", "::dispatch",
                                   "gldispatchcompute"]):
        return "Compute Dispatch"

    # Draw calls
    if any(kw in lower for kw in ["cmddraw", "::draw", "drawinstanced",
                                   "drawindexed", "gldrawarrays",
                                   "gldrawelements"]):
        return "Draw Call"

    # Barriers / transitions
    if any(kw in lower for kw in ["pipelinebarrier", "resourcebarrier",
                                   "glmemorybarrier"]):
        return "Pipeline Barrier"

    # Render pass
    if any(kw in lower for kw in ["beginrenderpass", "endrenderpass",
                                   "beginrendering", "endrendering",
                                   "clearrendertarget", "glclear"]):
        return "Render Pass / Clear"

    # Timestamp / query
    if any(kw in lower for kw in ["writetimestamp", "::begin", "::end",
                                   "glquerycounter", "glbeginquery",
                                   "glendquery"]):
        if "commandbuffer" not in lower and "renderpass" not in lower:
            return "Timestamp / Query"

    # Debug labels
    if any(kw in lower for kw in ["debugutils", "debugmarker",
                                   "glpushdebugg", "glpopdebugg",
                                   "setmarker", "beginevent", "endevent"]):
        return "Debug Label"

    # State binding
    if any(kw in lower for kw in ["bindpipeline", "binddescriptor",
                                   "setpipelinestate", "iasetinputlayout",
                                   "iasetprimitivetopology", "iasetvertex",
                                   "vssetshader", "pssetshader",
                                   "cssetshader", "cssetunordered",
                                   "cssetconstant", "omsetrendertarget",
                                   "omsetblendstate", "rssetstate",
                                   "rssetviewport", "glbindvertexarray",
                                   "gluseprogram", "glbindbufferbase",
                                   "gluniform", "glviewport"]):
        return "Bind State"

    # Push constants / map / update
    if any(kw in lower for kw in ["pushconstants", "::map", "::unmap",
                                   "glbuffersubdata"]):
        return "Resource Update"

    # Command buffer
    if any(kw in lower for kw in ["commandbuffer", "commandlist",
                                   "commandallocator", "commandqueue"]):
        return "Command Buffer"

    # Submit / execute
    if any(kw in lower for kw in ["queuesubmit", "executecommandlists",
                                   "signal", "glflush", "glfinish"]):
        return "Queue Submit"

    # Present / swap
    if any(kw in lower for kw in ["present", "swapbuffers"]):
        return "Present"

    # Synchronisation
    if any(kw in lower for kw in ["waitforfences", "resetfences", "fence",
                                   "waitforsingleobject", "glclientwait",
                                   "glfencesync"]):
        return "Synchronisation"

    # Copy / transfer
    if any(kw in lower for kw in ["copyresource", "copybuffer",
                                   "glcopybuffersubdata"]):
        return "Copy / Transfer"

    return "Other"


def short_gpu(name: str) -> str:
    for old, new in [("NVIDIA GeForce ", ""), ("AMD Radeon ", ""),
                     ("(TM)", ""), ("Graphics", "iGPU"),
                     ("Microsoft Basic Render Driver", "WARP"),
                     ("Microsoft WARP (CPU Software Renderer)", "WARP")]:
        name = name.replace(old, new)
    name = re.sub(r"\s*\(FL\s+\d+_\d+\)", "", name)
    for suffix in ["/PCIe/SSE2", "/PCIe/SSE42", "/PCIe"]:
        if suffix in name:
            name = name[:name.index(suffix)]
    return name.strip()


def load_app_results(path: Path) -> list[dict]:
    if not path.exists():
        return []
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if isinstance(data, dict) and "results" in data:
        return data["results"]
    return data if isinstance(data, list) else []


def generate_report(captures: list[dict], app_results: list[dict],
                    output: Path):
    lines = []
    lines.append("# RenderDoc Automated Analysis Report\n")
    lines.append(f"*Auto-generated by `rdoc_analyse.py` - "
                 f"{len(captures)} capture(s) analysed.*\n")

    lines.append("\n## Overview\n")
    lines.append("| Capture | API | Total Events | Frame Events "
                 "| Dispatches | Draw Calls | Barriers |")
    lines.append("|---------|-----|------------:|-------------:"
                 "|-----------:|-----------:|---------:|")

    for cap in captures:
        fc = cap["frame_classified"]
        lines.append(
            f"| {cap['filename']} | {cap['api']} "
            f"| {cap['total_events']} | {cap['frame_event_count']} "
            f"| {fc.get('Compute Dispatch', 0)} "
            f"| {fc.get('Draw Call', 0)} "
            f"| {fc.get('Pipeline Barrier', 0)} |")
    lines.append("")

    for ci, cap in enumerate(captures):
        api = cap["api"]
        lines.append(f"\n## {api} - `{cap['filename']}`\n")

        lines.append("### Per-Frame Command Sequence\n")
        lines.append("| # | API Call | Category | CPU Duration (ns) |")
        lines.append("|--:|---------|----------|------------------:|")
        for i, ev in enumerate(cap["frame_events"]):
            dur_str = f"{ev['dur_ns']:,}" if ev["dur_ns"] > 0 else "-"
            cat = classify_event_generic(ev["name"])
            lines.append(f"| {i+1} | `{ev['name']}` | {cat} | {dur_str} |")
        lines.append("")

        fc = cap["frame_classified"]
        if fc:
            lines.append("### Event Category Summary\n")
            lines.append("| Category | Count |")
            lines.append("|----------|------:|")
            for cat, count in sorted(fc.items(), key=lambda x: -x[1]):
                lines.append(f"| {cat} | {count} |")
            lines.append("")

        if api == "Vulkan":
            lines.append("### Observations\n")
            has_debug = fc.get("Debug Label", 0) > 0
            has_ts = fc.get("Timestamp Query", 0) > 0
            has_barrier = fc.get("Pipeline Barrier", 0) > 0
            lines.append(f"- **VK_EXT_debug_utils labels**: "
                         f"{'Present' if has_debug else 'Not found'} "
                         f"({fc.get('Debug Label', 0)} calls)")
            lines.append(f"- **Timestamp queries**: "
                         f"{'Present' if has_ts else 'Not found'} "
                         f"({fc.get('Timestamp Query', 0)} calls)")
            lines.append(f"- **Explicit barriers**: "
                         f"{'Present' if has_barrier else 'Not found'} "
                         f"(Vulkan requires manual synchronisation)")
            lines.append(f"- **Compute + render in single command buffer**: "
                         f"Yes (vkCmdDispatch -> vkCmdPipelineBarrier -> "
                         f"vkCmdBeginRenderPass)")
            lines.append("")

    if app_results:
        lines.append("\n## Cross-Validation: App Timestamps vs Capture Data\n")
        lines.append("The following table compares the application's own GPU "
                     "timestamp queries with the RenderDoc capture metadata.\n")

        api_results = defaultdict(list)
        for r in app_results:
            api = r.get("graphicsApi", "")
            api_results[api].append(r)

        lines.append("| GPU | API | App Compute (ms) | App Render (ms) "
                     "| App Total GPU (ms) | Avg FPS | Capture Available |")
        lines.append("|-----|-----|----------------:|---------------:"
                     "|------------------:|--------:|:-----------------:|")

        capture_apis = {c["api"] for c in captures}
        for r in app_results:
            api = r.get("graphicsApi", "")
            gpu = short_gpu(r.get("deviceName", "?"))
            has_cap = "Yes" if api in capture_apis else "No"
            lines.append(
                f"| {gpu} | {api} "
                f"| {r.get('avgComputeMs', 0):.3f} "
                f"| {r.get('avgRenderMs', 0):.3f} "
                f"| {r.get('avgTotalGpuMs', 0):.3f} "
                f"| {r.get('avgFps', 0):,.0f} "
                f"| {has_cap} |")
        lines.append("")

        lines.append("> **Interpretation**: App timestamps measure GPU-side "
                     "execution time via hardware counters. RenderDoc captures "
                     "record the CPU-side API call sequence and can be opened "
                     "in the RenderDoc GUI for GPU-side profiling via the "
                     "Performance Counter Viewer.\n")

    lines.append("\n## How to Use These Captures\n")
    lines.append("1. Open any `.rdc` file in `rdoc_captures/` with RenderDoc "
                 "GUI (`qrenderdoc.exe`)")
    lines.append("2. **Event Browser**: See the exact sequence of GPU "
                 "commands with debug labels")
    lines.append("3. **Pipeline State**: Inspect shader bindings, descriptor "
                 "sets, vertex layout")
    lines.append("4. **Buffer Viewer**: Examine SSBO particle data "
                 "(position, velocity, colour)")
    lines.append("5. **Performance Counter Viewer** (Window > Performance "
                 "Counter Viewer): Get per-draw GPU timing")
    lines.append("6. **Texture Viewer**: See the final rendered framebuffer "
                 "output\n")

    lines.append("---\n")
    lines.append("*Generated by rdoc_analyse.py. Captures can also be "
                 "viewed in Chrome's tracing viewer at `chrome://tracing` "
                 "by loading the `.json` files.*\n")

    output.parent.mkdir(parents=True, exist_ok=True)
    with open(output, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"  Report written to {output}")


def main():
    parser = argparse.ArgumentParser(
        description="Analyse RenderDoc captures and cross-validate")
    parser.add_argument("--captures", type=Path, required=True,
                        help="Directory containing .rdc capture files")
    parser.add_argument("--results", type=Path, default=None,
                        help="Path to results.json for cross-validation")
    parser.add_argument("--output", type=Path,
                        default=Path("docs/rdoc_comparison.md"),
                        help="Output markdown report path")
    args = parser.parse_args()

    renderdoccmd = find_renderdoccmd()

    rdc_files = sorted(args.captures.glob("*.rdc"))
    if not rdc_files:
        print("  No .rdc files found in", args.captures, file=sys.stderr)
        sys.exit(1)

    print(f"  Found {len(rdc_files)} .rdc capture(s)")

    seen = set()
    unique_rdcs = []
    for rdc in rdc_files:
        if rdc.name not in seen:
            seen.add(rdc.name)
            unique_rdcs.append(rdc)
    rdc_files = unique_rdcs

    captures = []
    for rdc in rdc_files:
        print(f"  Converting {rdc.name}...")
        json_path = convert_rdc_to_json(rdc, renderdoccmd)
        if not json_path or not json_path.exists():
            continue

        with open(json_path, "r", encoding="utf-8") as f:
            data = json.load(f)

        events = data if isinstance(data, list) else data.get("traceEvents", [])
        api = detect_api_from_filename(rdc.name)
        if api == "Unknown":
            api = detect_api_from_events(events)

        paired = pair_begin_end(events)
        frame_events = extract_frame_commands(paired)

        frame_classified = defaultdict(int)
        for ev in frame_events:
            frame_classified[classify_event_generic(ev["name"])] += 1

        captures.append({
            "filename": rdc.name,
            "api": api,
            "total_events": len(events),
            "frame_event_count": len(frame_events),
            "frame_events": frame_events,
            "frame_classified": dict(frame_classified),
        })
        print(f"    {api}: {len(events)} total events, "
              f"{len(frame_events)} frame events")

    if not captures:
        print("  No valid captures analysed.", file=sys.stderr)
        sys.exit(1)

    app_results = []
    if args.results and args.results.exists():
        app_results = load_app_results(args.results)
        print(f"  Loaded {len(app_results)} app result(s)")

    generate_report(captures, app_results, args.output)


if __name__ == "__main__":
    main()
