#!/usr/bin/env python3
"""
rdoc_export_timing.py — Export per-event GPU timing from a RenderDoc capture.

Two modes of operation:

  Mode A: Run inside RenderDoc's Python Shell (Window → Python Shell).
          Paste contents or use: exec(open('scripts/rdoc_export_timing.py').read())

  Mode B: Run standalone (requires RenderDoc's Python module on sys.path).
          python scripts/rdoc_export_timing.py capture.rdc -o timing.json

Output format (JSON):
  {
    "source": "capture.rdc",
    "gpu": "AMD Radeon RX 6900 XT",
    "events": [
      { "eventId": 5, "name": "vkCmdDispatch()", "gpuDurationMs": 0.082, "flags": "Dispatch" },
      ...
    ],
    "summary": {
      "computeMs": 0.082,
      "renderMs":  0.054,
      "totalMs":   0.136
    }
  }

This JSON can then be fed into compare_3dmark.py or plot_results.py for
cross-validation against the application's own timestamp queries.
"""

import json
import sys
from pathlib import Path

# ── Helpers ─────────────────────────────────────────────────────────────────

COMPUTE_KEYWORDS = ["dispatch", "compute", "particle compute"]
RENDER_KEYWORDS = ["draw", "render", "particle render"]
BARRIER_KEYWORDS = ["barrier", "pipeline barrier"]


def classify_event(name: str, flags) -> str:
    lower = name.lower()
    for kw in COMPUTE_KEYWORDS:
        if kw in lower:
            return "compute"
    for kw in RENDER_KEYWORDS:
        if kw in lower:
            return "render"
    for kw in BARRIER_KEYWORDS:
        if kw in lower:
            return "barrier"
    try:
        import renderdoc as rd
        if flags & rd.ActionFlags.Dispatch:
            return "compute"
        if flags & rd.ActionFlags.Drawcall:
            return "render"
    except Exception:
        pass
    return "other"


def flags_to_str(flags) -> str:
    labels = []
    try:
        import renderdoc as rd
        if flags & rd.ActionFlags.Drawcall:
            labels.append("Draw")
        if flags & rd.ActionFlags.Dispatch:
            labels.append("Dispatch")
        if flags & rd.ActionFlags.Clear:
            labels.append("Clear")
        if flags & rd.ActionFlags.PassBoundary:
            labels.append("PassBoundary")
    except Exception:
        pass
    return "|".join(labels) if labels else "Other"


# ── Core export function (works in both GUI and standalone) ─────────────────

def export_timing(controller, output_path: str = None, source_name: str = "capture.rdc"):
    try:
        import renderdoc as rd
    except ImportError:
        rd = __builtins__.__import__("renderdoc") if "renderdoc" in dir(__builtins__) else None
        if rd is None:
            print("ERROR: renderdoc module not available.", file=sys.stderr)
            return

    actions_map = {}

    def iter_actions(d):
        actions_map[d.eventId] = d
        for c in d.children:
            iter_actions(c)

    for d in controller.GetRootActions():
        iter_actions(d)

    counters = controller.EnumerateCounters()
    if rd.GPUCounter.EventGPUDuration not in counters:
        print("WARNING: GPU Duration counter not supported on this capture.",
              file=sys.stderr)
        return

    results = controller.FetchCounters([rd.GPUCounter.EventGPUDuration])
    desc = controller.DescribeCounter(rd.GPUCounter.EventGPUDuration)

    events = []
    compute_ms = 0.0
    render_ms = 0.0
    total_ms = 0.0

    for r in results:
        action = actions_map.get(r.eventId)
        if not action:
            continue
        name = action.GetName(controller.GetStructuredFile())

        if desc.resultByteWidth == 8:
            duration_sec = r.value.d
        else:
            duration_sec = r.value.f
        duration_ms = duration_sec * 1000.0

        cat = classify_event(name, action.flags)

        events.append({
            "eventId": r.eventId,
            "name": name,
            "gpuDurationMs": round(duration_ms, 6),
            "category": cat,
            "flags": flags_to_str(action.flags),
        })

        total_ms += duration_ms
        if cat == "compute":
            compute_ms += duration_ms
        elif cat == "render":
            render_ms += duration_ms

    gpu_name = ""
    try:
        api_props = controller.GetAPIProperties()
        gpu_name = controller.GetReplayOptions().apiValidation
    except Exception:
        pass

    output = {
        "source": source_name,
        "gpu": gpu_name,
        "events": events,
        "summary": {
            "computeMs": round(compute_ms, 6),
            "renderMs": round(render_ms, 6),
            "barrierMs": round(sum(e["gpuDurationMs"] for e in events
                                   if e["category"] == "barrier"), 6),
            "totalMs": round(total_ms, 6),
            "eventCount": len(events),
        }
    }

    if output_path:
        Path(output_path).parent.mkdir(parents=True, exist_ok=True)
        with open(output_path, "w", encoding="utf-8") as f:
            json.dump(output, f, indent=2)
        print(f"Exported {len(events)} events to {output_path}")
    else:
        print(json.dumps(output, indent=2))

    return output


# ── RenderDoc GUI mode ──────────────────────────────────────────────────────
# If running inside RenderDoc's Python Shell, pyrenderdoc is available.

if "pyrenderdoc" in dir() or "pyrenderdoc" in globals():
    _out = "rdoc_timing.json"
    print(f"RenderDoc GUI detected. Exporting timing to {_out}...")
    globals()["pyrenderdoc"].Replay().BlockInvoke(
        lambda ctrl: export_timing(ctrl, _out, "gui_capture"))

# ── Standalone mode ─────────────────────────────────────────────────────────

elif __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(
        description="Export per-event GPU timing from a RenderDoc .rdc capture")
    parser.add_argument("capture", type=str,
                        help="Path to .rdc capture file")
    parser.add_argument("-o", "--output", type=str, default=None,
                        help="Output JSON file (default: print to stdout)")
    args = parser.parse_args()

    try:
        import renderdoc as rd
    except ImportError:
        print("ERROR: renderdoc Python module not found.\n"
              "  Either:\n"
              "  a) Run this script inside RenderDoc's Python Shell, or\n"
              "  b) Add RenderDoc's Python module to your PYTHONPATH:\n"
              "     set PYTHONPATH=C:\\Program Files\\RenderDoc\n"
              "     python scripts/rdoc_export_timing.py capture.rdc\n",
              file=sys.stderr)
        sys.exit(1)

    rd.InitialiseReplay(rd.GlobalEnvironment(), [])

    cap = rd.OpenCaptureFile()
    result = cap.OpenFile(args.capture, "", None)
    if result != rd.ResultCode.Succeeded:
        print(f"ERROR: Could not open {args.capture}: {result}",
              file=sys.stderr)
        sys.exit(1)

    if not cap.LocalReplaySupport():
        print("ERROR: Capture cannot be replayed on this machine.",
              file=sys.stderr)
        sys.exit(1)

    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        print(f"ERROR: Could not replay capture: {result}",
              file=sys.stderr)
        sys.exit(1)

    export_timing(controller, args.output, args.capture)

    controller.Shutdown()
    cap.Shutdown()
    rd.ShutdownReplay()
