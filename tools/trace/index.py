#!/usr/bin/env python3
# Copyright (c) 2026 Adi Nandanamudi
# SPDX-License-Identifier: Apache-2.0
"""Rebuild every trace's HTML page and docs/data/traces/README.md (a table of
all traces with their numbers), so the index never goes stale.

    uv run tools/trace/index.py
"""

import glob
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
import tracelib  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
TRACES = os.path.join(HERE, "..", "..", "docs", "data", "traces")

SCENARIOS = {
    "gmt_idle": "GMT face, hands spread out (03:50), nothing touched",
    "gmt_bezel": "GMT face, bezel turned 4 clicks forward and 2 back by script",
    "gmt_hands_cross": "GMT face, clock set to 10:30:27 so the second hand crosses the minute hand",
    "chrono_black_idle": "black chronograph, chronograph stopped",
    "chrono_white_idle": "white chronograph, chronograph stopped",
    "remote_knob": "remote, knob turned 5 steps up and 3 down by script (no helper app)",
}
VARIANTS = {
    "zephyr-spi": "display writes through the Zephyr SPI driver (before direct SPI)",
    "no-overlap": "each box sent before the next is composed (no DMA overlap)",
    "ring-flash": "bezel ring strip read from flash (before copying it to RAM)",
    "no-gate": "second-hand stage run in every box (before bounding-box gating)",
    "free-run": "frames not locked to the panel's TE edge",
}


def describe(name):
    if name.startswith("ab_"):
        rest = name[3:]
        for v, d in VARIANTS.items():
            if rest.startswith(v + "_"):
                return f"A/B **{v}**: {d}; scenario: {SCENARIOS.get(rest[len(v) + 1:], '')}"
    return SCENARIOS.get(name, "")


def main():
    rows = []
    for path in sorted(glob.glob(os.path.join(TRACES, "*.trace"))):
        name = os.path.basename(path)[:-6]
        t = tracelib.load(path)
        fr = t.frames()
        subprocess.run([sys.executable, os.path.join(HERE, "animate.py"), path],
                       check=True, capture_output=True)
        if fr:
            periods = [b.t0 - a.t0 for a, b in zip(fr, fr[1:])] or [0]
            span = fr[-1].t1 - fr[0].t0
            fps = (len(fr) - 1) / span if span > 0 else 0
            work = sum(f.work_ms for f in fr) / len(fr)
            worst = max(f.work_ms for f in fr)
            stall = sum(f.stall_ms for f in fr) / len(fr)
            boxes = sum(len(f.boxes) for f in fr) / len(fr)
            rows.append(f"| [{name}]({name}.html) | {describe(name)} | {fps:.0f} | "
                        f"{1000 * max(periods):.1f} | {work:.1f} | {worst:.1f} | {boxes:.0f} | {stall:.2f} |")
    with open(os.path.join(TRACES, "README.md"), "w") as f:
        f.write("# Traces\n\nRecorded with `tools/trace/capture.py`; each `.trace` has an interactive "
                "`.html` (play, scrub) made by `tools/trace/animate.py`. Numbers from "
                "`tracelib.py`: fps over the capture, the longest gap between frames, mean and "
                "worst frame work, boxes per frame, and time the CPU stalled waiting for the "
                "DMA.\n\n| Trace | What | fps | worst gap ms | work ms | worst ms | boxes | stall ms |\n"
                "|---|---|---|---|---|---|---|---|\n" + "\n".join(rows) + "\n")
    print(f"{len(rows)} traces indexed")


if __name__ == "__main__":
    main()
