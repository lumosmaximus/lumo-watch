#!/usr/bin/env python3
# Copyright (c) 2026 Adi Nandanamudi
# SPDX-License-Identifier: Apache-2.0
"""The media remote's knob ring, in the same polar form as the GMT bezel (see
watch.py export): an unwrapped strip [ANG][RAD] of 8-bit palette indices plus
a 256-colour palette, so core 1 draws and turns it with the bezel's lookup.

A smooth pastel rainbow ring with a white dot
per click,
and a white marker that shows how far the knob has turned.

    uv run knob.py preview          -> knob_preview.png
    uv run knob.py export [dir]     -> knob_img.c / knob_img.h
"""

import math
import os
import sys

from PIL import Image

from watch import BEZEL_IN, BEZEL_OUT, C, SIZE, c_array, rgb565

ANG, R0 = 1440, BEZEL_IN - 2           # must match watch.py's ring lookup
RAD = BEZEL_OUT - R0 + 1
KNOB_CLICK_DEG = 3                     # the knob turns in clicks this size ...
KNOB_KEY_DEG = 15                      # ... and sends one key per this much (iPod-like:
                                       # a couple of turns from silent to full)
UNDER_IN = (30, 18, 40)                # the remote's background, inside the ring


def hsv(h, sat, val):
    """h in turns -> RGB 0..255"""
    import colorsys
    return [int(255 * c) for c in colorsys.hsv_to_rgb(h % 1.0, sat, val)]


def knob(theta, r):
    """Colour at clock angle theta (degrees) and radius r (pixels): a pastel
    rainbow knob with a white dot per click."""
    if r < 193:                                      # soft inner rim
        return (54, 34, 66)
    inner = 199                                      # a smooth inner edge
    if r < inner:
        return (UNDER_IN[0], UNDER_IN[1], UNDER_IN[2])
    base = hsv(theta / 360, 0.50, 1.0)               # pastel rainbow
    # gentle 3D: a soft highlight top-left, a little darker at the outer edge
    shade = 0.90 + 0.10 * math.cos(math.radians(theta - 315))
    shade *= 1.0 - 0.18 * max(0.0, (r - 222) / 11) ** 2
    # a soft lighter lip along the inner edge
    shade *= 1.0 + 0.12 * max(0.0, 1 - (r - inner) / 4)
    col = [c * shade for c in base]
    # a white dot per click, small, near the inner edge
    step = KNOB_CLICK_DEG
    d = abs(((theta + step / 2) % step) - step / 2) * math.pi / 180 * r
    if abs(r - 215) < 2.6:
        dot = max(0.0, min(1.0, 0.5 - (math.hypot(d, r - 215) - 1.3)))
        col = [c + (255 - c) * dot * 0.85 for c in col]
    # the marker: a white disc with a soft plum ring, at 12 o'clock
    dx = math.radians(((theta + 180) % 360) - 180) * r
    dist = math.hypot(dx, r - 222)
    ring = max(0.0, min(1.0, 0.5 - (dist - 10.0)))   # a bold plum ring ...
    core = max(0.0, min(1.0, 0.5 - (dist - 6.5)))    # ... round a white centre
    col = [c + (v - c) * ring for c, v in zip(col, (62, 28, 72))]
    col = [c + (255 - c) * core for c in col]
    return tuple(int(max(0, min(255, c))) for c in col)


def strip():
    """[ANG][RAD] RGB, each entry averaged over its pixel, edges pre-blended."""
    out = []
    for ai in range(ANG):
        row = []
        for ri in range(RAD):
            acc = [0.0, 0.0, 0.0]
            n = 0
            for da in (-0.25, 0.25):
                for dr in (-0.25, 0.25):
                    th = (ai + 0.5 + da) * 360 / ANG
                    r = R0 + ri + 0.5 + dr
                    c = knob(th, r) if r <= BEZEL_OUT else (0, 0, 0)
                    if r < BEZEL_IN - 1:
                        c = UNDER_IN
                    for k in range(3):
                        acc[k] += c[k]
                    n += 1
            row.append(tuple(int(v / n) for v in acc))
        out.append(row)
    return out


MUTE_CX, MUTE_CY, MUTE_R = 0, -104, 28   # mute button, relative to the middle
LABEL_W, LABEL_H = 120, 40               # bitmap above the button


def muted_label():
    """A8 bitmap of 'MUTED' curved over the mute button (left/top corner at
    (MUTE_CX - LABEL_W/2, MUTE_CY - MUTE_R - LABEL_H + 6) from the middle)."""
    from PIL import ImageDraw
    from watch import SS, font
    big = Image.new("L", (LABEL_W * SS, LABEL_H * SS), 0)
    f = font("Arial Bold.ttf", 11)
    cx, cy = LABEL_W / 2, LABEL_H - 6 + MUTE_R      # button centre in the bitmap
    txt, rr = "MUTED", MUTE_R + 9
    widths = [f.getlength(ch) / SS + 1.2 for ch in txt]
    a = -sum(widths) / rr / 2
    for ch, w in zip(txt, widths):
        mid = a + w / 2 / rr
        tmp = Image.new("L", (30 * SS, 30 * SS), 0)
        ImageDraw.Draw(tmp).text((15 * SS, 15 * SS), ch, font=f, fill=255, anchor="mm")
        tmp = tmp.rotate(-math.degrees(mid), resample=Image.BICUBIC)
        x = (cx + rr * math.sin(mid)) * SS - tmp.width / 2
        y = (cy - rr * math.cos(mid)) * SS - tmp.height / 2
        big.paste(255, (int(x), int(y)), tmp)
        a += w / rr
    return big.resize((LABEL_W, LABEL_H), Image.LANCZOS)


def preview():
    s = strip()
    img = Image.new("RGB", (SIZE, SIZE), UNDER_IN)
    px = img.load()
    for y in range(SIZE):
        for x in range(SIZE):
            dx, dy = x + 0.5 - C, y + 0.5 - C
            r = math.hypot(dx, dy)
            if r > BEZEL_OUT + 0.5:
                px[x, y] = (0, 0, 0)
            elif r >= R0:
                ai = int((math.degrees(math.atan2(dx, -dy)) % 360) * ANG / 360) % ANG
                px[x, y] = s[ai][min(RAD - 1, int(r - R0))]
    img.save("knob_preview.png")
    print("knob_preview.png")


def export(out):
    s = strip()
    img = Image.new("RGB", (RAD, ANG))
    img.putdata([s[ai][ri] for ai in range(ANG) for ri in range(RAD)])
    q = img.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = q.getpalette()[:768]
    pal += [0] * (768 - len(pal))
    pal_b = bytearray()
    for i in range(256):
        v = rgb565(*pal[i * 3:i * 3 + 3])
        pal_b += bytes((v & 0xFF, v >> 8))
    with open(os.path.join(out, "knob_img.c"), "w") as f:
        f.write("/* Generated by tools/watchface/knob.py - do not edit. */\n\n")
        f.write('#include "knob_img.h"\n\n')
        f.write(c_array("knob_pal_flash", pal_b))
        f.write(c_array("knob_q", bytes(q.get_flattened_data())))
        f.write(c_array("muted_a", bytes(muted_label().get_flattened_data())))
    with open(os.path.join(out, "knob_img.h"), "w") as f:
        f.write("/* Generated by tools/watchface/knob.py - do not edit. */\n\n")
        f.write("#ifndef KNOB_IMG_H_\n#define KNOB_IMG_H_\n\n#include <stdint.h>\n")
        f.write("#include <zephyr/toolchain.h>\n\n")
        f.write(f"#define KNOB_CLICK_DEG {KNOB_CLICK_DEG}\n#define KNOB_KEY_DEG {KNOB_KEY_DEG}\n")
        f.write("extern const uint8_t knob_pal_flash[]; /* 256 x RGB565 */\n")
        f.write("extern const uint8_t knob_q[];   /* u8 [ANG][RAD], same layout as watch_ring_q */\n")
        f.write(f"#define MUTED_W {LABEL_W}\n#define MUTED_H {LABEL_H}\n")
        f.write(f"#define MUTED_X0 ({MUTE_CX - LABEL_W // 2})  /* from the middle */\n")
        f.write(f"#define MUTED_Y0 ({MUTE_CY - MUTE_R - LABEL_H + 6})\n")
        f.write("extern const uint8_t muted_a[];  /* A8 'MUTED' label */\n")
        f.write("\n#endif\n")
    print("wrote knob_img.c/.h in", out)


if __name__ == "__main__":
    if sys.argv[1:2] == ["export"]:
        export(sys.argv[2] if len(sys.argv) > 2 else ".")
    else:
        preview()
