#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "pyserial"]
# ///
# Copyright (c) 2026 Adi Nandanamudi
# SPDX-License-Identifier: Apache-2.0
"""Render the watch's screens on the computer: stills (PNG) and clips (GIF),
pixel-identical to the device, using the real compositor (host.c + shim/).

    sh tools/render/build.sh                 # once, and after main.c changes
    uv run tools/render/render.py            # all scenes -> docs/renders/
    uv run tools/render/render.py gmt remote # just these

Scenes are in SCENES below: a list of host `render` arguments, which frame to
keep as the still, and how many frames the GIF covers. Add one per screen you
want in the article.
"""

import os
import subprocess
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "docs", "renders")
TMP = os.path.join(HERE, "frames")
RENDER = os.path.join(HERE, "render")

# (args, still frame, gif frames, gif speed: 1 = real time (60 fps -> 20 fps gif, every 3rd))
SCENES = {
    # faces
    "gmt": (["--time", "10:09:30", "--at", "0:face=0"], 60, 180, 1),
    "gmt_bezel_turn": (["--time", "10:09:30", "--at", "0:face=0"] +
                       sum([["--at", f"{20 + 6 * i}:bezel=1"] for i in range(10)], []), 80, 120, 1),
    "chrono_black": (["--time", "10:09:30", "--at", "0:face=1", "--at", "1:button=1"], 90, 180, 1),
    "chrono_white": (["--time", "10:09:30", "--at", "0:face=2", "--at", "1:button=1"], 90, 180, 1),
    # the remote: paused, then playing with the computer's state and a title
    "remote": (["--at", "0:face=3", "--at", "2:state=63,0,85,0"], 30, 1, 1),
    "remote_playing": (["--at", "0:face=3", "--at", "2:state=63,0,85,1",
                        "--at", "3:text=" + os.path.join(HERE, "title.bin")] +
                       sum([["--at", f"{30 + 4 * i}:knob=5"] for i in range(12)], []), 100, 150, 1),
    "remote_muted": (["--at", "0:face=3", "--at", "2:state=63,1,85,1"], 20, 1, 1),
    "remote_brightness": (["--at", "0:face=3", "--at", "2:state=63,0,85,1",
                           "--at", "5:touch=233,381,1", "--at", "8:touch=233,381,0"], 30, 1, 1),
    # BOOT held: the picker; frames while it waits count too; a tap picks the remote
    "picker": (["--at", "0:face=0", "--at", "2:button=3", "--at", "40:touch=319,319,1",
                "--at", "42:touch=319,319,0"], 20, 1, 1),
}


def title_image():
    """The now-playing text as the helper would send it (2-bit, 2240 bytes)."""
    sys.path.insert(0, os.path.join(HERE, "..", "agent"))
    import agent_mac  # noqa: E402
    l1, l2 = agent_mac.Line("Cosmic Piano Waltzes", 16), agent_mac.Line("James Malikey", 13)
    with open(os.path.join(HERE, "title.bin"), "wb") as f:
        f.write(agent_mac.pack(l1.frame(0), l2.frame(0)))


def scene(name):
    args, still, nframes, speed = SCENES[name]
    total = max(still + 1, nframes)
    subprocess.run(["rm", "-rf", TMP], check=True)
    subprocess.run([RENDER, "--frames", str(total), "--out", TMP] + args, check=True,
                   capture_output=True)
    os.makedirs(OUT, exist_ok=True)
    img = Image.open(os.path.join(TMP, f"frame_{still:04d}.ppm"))
    img.save(os.path.join(OUT, name + ".png"))
    if nframes > 1:
        step = 3  # 60 fps -> 20 fps
        frames = [Image.open(os.path.join(TMP, f"frame_{i:04d}.ppm")).quantize(colors=128)
                  for i in range(0, nframes, step)]
        frames[0].save(os.path.join(OUT, name + ".gif"), save_all=True, append_images=frames[1:],
                       duration=int(1000 * step / 60 / speed), loop=0, optimize=True)
    print(name, "->", os.path.join(OUT, name + ".png"), "(+gif)" if nframes > 1 else "")


def main():
    if not os.path.exists(RENDER):
        sys.exit("build the renderer first: sh tools/render/build.sh")
    title_image()
    for name in (sys.argv[1:] or SCENES):
        scene(name)


if __name__ == "__main__":
    main()
