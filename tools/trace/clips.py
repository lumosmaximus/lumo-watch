#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "matplotlib"]
# ///
# Copyright (c) 2026 Adi Nandanamudi
# SPDX-License-Identifier: Apache-2.0
"""Short before/after GIF clips of traces for the write-up -> docs/data/clips/.

    uv run tools/trace/clips.py [name ...]

Each clip is ~3 s of GIF showing ~60 ms of trace (1/50 speed). For the bezel
clips the window starts just before the first ring repaint in the trace.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import animate    # noqa: E402
import tracelib   # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
TRACES = os.path.join(HERE, "..", "..", "docs", "data", "traces")
OUT = os.path.join(HERE, "..", "..", "docs", "data", "clips")

# name: (trace, start: seconds or "ring" (first bezel repaint) or "slow" (first frame > 17 ms), length s)
CLIPS = {
    "spi_before": ("ab_zephyr-spi_gmt_idle", 0.40, 0.06),
    "spi_after": ("gmt_idle", 0.40, 0.06),
    "overlap_before": ("ab_no-overlap_gmt_idle", 0.40, 0.06),
    "overlap_after": ("gmt_idle", 0.40, 0.06),
    "bezel_before": ("ab_ring-flash_gmt_bezel", "ring", 0.07),
    "bezel_after": ("gmt_bezel", "ring", 0.07),
    "te_before": ("ab_free-run_gmt_idle", 0.40, 0.06),
    "te_after": ("gmt_idle", 0.40, 0.06),
    "hands_cross": ("gmt_hands_cross", "slow", 0.08),
    "bands_small": ("chrono_white_idle", 0.40, 0.06),
    "bands_gmt": ("gmt_idle", 0.40, 0.06),
}


def start_for(t, how):
    fr = t.frames()
    if how == "ring":
        for f in fr:
            if f.ring and f.ring[1] > f.ring[0]:
                return max(0.0, f.t0 - 0.012)
    if how == "slow":
        for a, b in zip(fr, fr[1:]):
            if b.t0 - a.t0 > 0.0175:
                return max(0.0, a.t0 - 0.015)
    return 0.4 if not isinstance(how, (int, float)) else how


def main():
    os.makedirs(OUT, exist_ok=True)
    for name in (sys.argv[1:] or CLIPS):
        trace, start, length = CLIPS[name]
        t = tracelib.load(os.path.join(TRACES, trace + ".trace"))
        model = animate.build_model(t)
        s = start_for(t, start)
        path = os.path.join(OUT, name + ".gif")
        animate.write_gif(model, path, s, length, 15 * 50)  # 15 gif fps at 1/50 speed
        print(f"{name}: {trace} from {s:.3f} s -> {path} ({os.path.getsize(path) // 1024} KB)")


if __name__ == "__main__":
    main()
