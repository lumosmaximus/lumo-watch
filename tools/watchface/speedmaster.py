#!/usr/bin/env python3
# Copyright (c) 2026 Adi Nandanamudi
# SPDX-License-Identifier: Apache-2.0
"""Chronograph face in the style of a 1960s moon-watch, for the 466x466 round
AMOLED: a static dial image (RGB565) with a fixed tachymeter bezel and three
subdials. All hands are vector shapes drawn on core 1 (see apps/watch).

    uv run speedmaster.py preview          -> speed_preview.png
    uv run speedmaster.py export [dir]     -> speed_img.c / speed_img.h

Layout (pixels from the centre, 233.0 in pixel-edge coordinates):
  subdials: running seconds at 9, 30-minute counter at 3, 12-hour counter
  at 6, each SUB_R in radius, centred SUB_OFF from the middle.
Generic chronograph styling only (no brand names or logos).
"""

import math
import os
import sys

from PIL import Image, ImageDraw

from watch import SS, SIZE, C, S, c_array, font, rgb565, text_on_ring  # noqa: F401

BEZEL_OUT, BEZEL_IN = 233, 196
TRACK_OUT, TRACK_IN = 194, 183      # 1/5 second track on the dial edge
DIAL = 195
SUB_OFF, SUB_R = 104, 50

WHITE = (236, 236, 236)
INK = (226, 228, 232)
LUME = (236, 240, 222)
BLACK = (8, 8, 9)
BEZEL_BLACK = (14, 14, 16)

# colour themes: the classic black dial, and white with a blue ceramic bezel
THEMES = {
    "black": dict(dial=BLACK, bezel=BEZEL_BLACK, bezel_ink=WHITE, ink=INK, index=WHITE,
                  double=True, lume=LUME, sub=(20, 20, 22), groove=(26, 26, 29),
                  sub_edge=(70, 70, 74), sub_ink=INK, text=WHITE),
    "white": dict(dial=(244, 244, 242), bezel=(30, 52, 120), bezel_ink=(240, 242, 246),
                  ink=(46, 56, 86), index=(26, 54, 128), double=False, lume=None,
                  sub=(232, 232, 233), groove=(222, 222, 224), sub_edge=(196, 198, 204),
                  sub_ink=(26, 54, 128), text=(26, 54, 128)),
}


def polar_at(cx, cy, r, deg):
    a = math.radians(deg - 90)
    return cx + r * math.cos(a), cy + r * math.sin(a)


def tick(d, cx, cy, r0, r1, deg, w, fill):
    x0, y0 = polar_at(cx, cy, r0, deg)
    x1, y1 = polar_at(cx, cy, r1, deg)
    d.line(S([(x0, y0), (x1, y1)]), fill=fill, width=max(1, int(w * SS)))


def text_at(d, txt, x, y, f, fill):
    d.text((x * SS, y * SS), txt, font=f, fill=fill, anchor="mm")


def ring_label(img, txt, r, deg, f, fill):
    """Text centred at (r, deg), tangent to the ring; the lower half is turned
    the other way so every label reads upright from outside."""
    rot = deg + 180 if 90 < deg < 270 else deg
    tmp = Image.new("RGBA", (120 * SS, 40 * SS), (0, 0, 0, 0))
    ImageDraw.Draw(tmp).text((60 * SS, 20 * SS), txt, font=f, fill=fill, anchor="mm")
    tmp = tmp.rotate(-rot, resample=Image.BICUBIC, expand=True)
    x, y = polar_at(C, C, r, deg)
    img.alpha_composite(tmp, (int(x * SS - tmp.width / 2), int(y * SS - tmp.height / 2)))


def arc_text(img, txt, r, deg, f, fill):
    """Text along the ring, centred at clock angle deg, each letter turned to
    follow the curve (reads clockwise, for the upper half)."""
    widths = [f.getlength(ch) / SS for ch in txt]
    a = math.radians(deg) - sum(widths) / r / 2
    for ch, w in zip(txt, widths):
        mid = a + w / 2 / r
        tmp = Image.new("RGBA", (40 * SS, 40 * SS), (0, 0, 0, 0))
        ImageDraw.Draw(tmp).text((20 * SS, 20 * SS), ch, font=f, fill=fill, anchor="mm")
        tmp = tmp.rotate(-math.degrees(mid), resample=Image.BICUBIC)
        x, y = polar_at(C, C, r, math.degrees(mid))
        img.alpha_composite(tmp, (int(x * SS - tmp.width / 2), int(y * SS - tmp.height / 2)))
        a += w / r


def draw_bezel(img, d, t):
    WHITE = t["bezel_ink"]
    d.ellipse(S([(C - BEZEL_OUT, C - BEZEL_OUT), (C + BEZEL_OUT, C + BEZEL_OUT)]),
              fill=t["bezel"])
    d.ellipse(S([(C - BEZEL_IN, C - BEZEL_IN), (C + BEZEL_IN, C + BEZEL_IN)]), fill=(0, 0, 0))
    # tachymeter: speed = 3600 / seconds; the scale starts at 7.2 s (500)
    f = font("Arial Bold.ttf", 13)
    labelled = [500, 400, 300, 250, 200, 180, 160, 150, 140, 130, 120, 110, 100, 90, 85,
                80, 75, 70, 65, 60]
    for v in labelled:
        deg = 3600 / v * 6
        tick(d, C, C, BEZEL_IN + 1, BEZEL_IN + 7, deg, 1.6, WHITE)
        ring_label(img, str(v), 219, deg, f, WHITE)
    # fine ticks between the labels
    for v in range(500, 60, -1):
        if v in labelled:
            continue
        if (v > 200 and v % 50) or (100 < v <= 200 and v % 10) or (v <= 100 and v % 5):
            continue
        tick(d, C, C, BEZEL_IN + 1, BEZEL_IN + 4, 3600 / v * 6, 1.0, WHITE)
    # dots beside each label, like the real scale
    for v in labelled:
        x, y = polar_at(C, C, BEZEL_IN + 10, 3600 / v * 6)
        d.ellipse(S([(x - 1.1, y - 1.1), (x + 1.1, y + 1.1)]), fill=WHITE)
    arc_text(img, "TACHYMETRE", 216, 21, font("Arial Bold.ttf", 12), WHITE)


def subdial(img, d, cx, cy, kind, t):
    INK = t["sub_ink"]
    # recessed, with concentric grooves
    d.ellipse(S([(cx - SUB_R, cy - SUB_R), (cx + SUB_R, cy + SUB_R)]), fill=t["sub"])
    for r in range(SUB_R - 2, 10, -3):
        d.ellipse(S([(cx - r, cy - r), (cx + r, cy + r)]), outline=t["groove"],
                  width=max(1, SS // 2))
    d.ellipse(S([(cx - SUB_R, cy - SUB_R), (cx + SUB_R, cy + SUB_R)]), outline=t["sub_edge"],
              width=SS)
    f = font("Arial.ttf", 11)
    if kind == "sec":          # running seconds: 60 ticks, 20 / 40 / 60
        for i in range(60):
            big = i % 5 == 0
            tick(d, cx, cy, SUB_R - (8 if big else 4), SUB_R - 1, i * 6, 1.4 if big else 0.8, INK)
        for v in (20, 40, 60):
            x, y = polar_at(cx, cy, SUB_R - 18, v * 6)
            text_at(d, str(v), x, y, f, INK)
    elif kind == "min":        # 30-minute counter, 3 / 6 / 9 marked long
        for i in range(30):
            big = i % 5 == 0
            mark = i in (3, 6, 9)
            tick(d, cx, cy, SUB_R - (8 if big or mark else 4), SUB_R - 1, i * 12,
                 1.4 if big else 0.8, INK)
        for v in (10, 20, 30):
            x, y = polar_at(cx, cy, SUB_R - 18, v * 12)
            text_at(d, str(v), x, y, f, INK)
    else:                      # 12-hour counter
        for i in range(60):
            big = i % 5 == 0
            tick(d, cx, cy, SUB_R - (8 if big else 4), SUB_R - 1, i * 6, 1.4 if big else 0.8, INK)
        for v in (3, 6, 9, 12):
            x, y = polar_at(cx, cy, SUB_R - 18, v * 30)
            text_at(d, str(v), x, y, f, INK)


def dial(theme="black"):
    t = THEMES[theme]
    INK, WHITE, LUME = t["ink"], t["index"], t["lume"]
    img = Image.new("RGBA", (SIZE * SS, SIZE * SS), (0, 0, 0, 255))
    d = ImageDraw.Draw(img)
    draw_bezel(img, d, t)
    d.ellipse(S([(C - DIAL, C - DIAL), (C + DIAL, C + DIAL)]), fill=t["dial"])

    # 1/5 second track: 300 ticks, longer every second, longest every 5 s
    for i in range(300):
        deg = i * 1.2
        if i % 25 == 0:
            r0, w = TRACK_IN - 3, 1.8
        elif i % 5 == 0:
            r0, w = TRACK_IN + 2, 1.2
        else:
            r0, w = TRACK_OUT - 4, 0.7
        tick(d, C, C, r0, TRACK_OUT, deg, w, INK)

    # hour batons with a lume dot outside; short where a subdial is close
    for h in range(12):
        deg = h * 30
        r0 = 150 if h not in (3, 6, 9) else 160
        r1 = 176
        for off in ((-2.2, 2.2) if t["double"] else (0,)):   # double-line or solid
            a = math.radians(deg - 90)
            ox, oy = -math.sin(a) * off, math.cos(a) * off
            x0, y0 = polar_at(C, C, r0, deg)
            x1, y1 = polar_at(C, C, r1, deg)
            d.line(S([(x0 + ox, y0 + oy), (x1 + ox, y1 + oy)]), fill=WHITE,
                   width=int((2.6 if t["double"] else 4.2) * SS))
        if LUME:
            x, y = polar_at(C, C, 180, deg)
            d.ellipse(S([(x - 2.4, y - 2.4), (x + 2.4, y + 2.4)]), fill=LUME)

    subdial(img, d, C - SUB_OFF, C, "sec", t)
    subdial(img, d, C + SUB_OFF, C, "min", t)
    subdial(img, d, C, C + SUB_OFF, "hour", t)

    text_at(d, "ZEPHYR", C, C - 92, font("Arial Bold.ttf", 17), t["text"])
    text_at(d, "CHRONOGRAPH", C, C - 58, font("Arial.ttf", 12), t["text"])
    text_at(d, "DUAL CORE · AMP", C, C - 42, font("Arial.ttf", 10), t["text"])
    return img.resize((SIZE, SIZE), Image.LANCZOS).convert("RGB")


def preview():
    for theme in THEMES:
        dial(theme).save(f"speed_preview_{theme}.png")
        print(f"speed_preview_{theme}.png")


def export(out):
    arrays = {"black": "speed_dial", "white": "speed_white_dial"}
    with open(os.path.join(out, "speed_img.c"), "w") as f:
        f.write("/* Generated by tools/watchface/speedmaster.py - do not edit. */\n\n")
        f.write('#include "speed_img.h"\n\n')
        for theme, name in arrays.items():
            px = dial(theme).load()
            data = bytearray()
            for y in range(SIZE):
                for x in range(SIZE):
                    v = rgb565(*px[x, y])
                    data += bytes((v & 0xff, v >> 8))
            f.write(c_array(name, data))
    with open(os.path.join(out, "speed_img.h"), "w") as f:
        f.write("/* Generated by tools/watchface/speedmaster.py - do not edit. */\n\n")
        f.write("#ifndef SPEED_IMG_H_\n#define SPEED_IMG_H_\n\n#include <stdint.h>\n#include <zephyr/toolchain.h>\n\n")
        f.write(f"#define SPEED_SUB_OFF {SUB_OFF}\n#define SPEED_SUB_R {SUB_R}\n\n")
        f.write("extern const uint8_t speed_dial[];       /* black, RGB565 LE, 466 x 466 */\n")
        f.write("extern const uint8_t speed_white_dial[]; /* white, RGB565 LE, 466 x 466 */\n\n")
        f.write("#endif\n")
    print("wrote speed_img.c/.h in", out)


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "preview"
    if cmd == "export":
        export(sys.argv[2] if len(sys.argv) > 2 else ".")
    else:
        preview()
