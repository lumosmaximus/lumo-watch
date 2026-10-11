#!/usr/bin/env python3
# Copyright (c) 2026 Adi Nandanamudi
# SPDX-License-Identifier: Apache-2.0
"""GMT dive-watch face for the 466x466 round AMOLED: a static dial image
(RGB565) plus four hand images (ARGB8888) that LVGL rotates on core 1:
hour, minute, gliding seconds and a 24-hour GMT hand. Two-tone 24-hour
bezel (blue night half on top, red day half below).

    uv run watch.py preview          -> preview.png (dial + hands at 10:09:30)
    uv run watch.py export [dir]     -> watch_img.c / watch_img.h

Generic GMT-watch styling only (no brand names or logos).
"""

import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

SIZE = 466
C = SIZE / 2
SS = 3  # supersampling

BEZEL_OUT, BEZEL_IN = 233, 193
REHAUT = 187
DIAL = 186

INK = (226, 228, 232)        # platinum print
LUME = (236, 240, 222)       # luminous paint
GOLD = (205, 200, 190)       # white-gold surrounds
BLACK = (6, 6, 8)
BEZEL_BLUE = (24, 48, 128)   # night half (18 -> 24 -> 6)
BEZEL_RED = (150, 22, 34)    # day half (6 -> 12 -> 18)
GMT_RED = (214, 36, 44)

FONT_DIR = "/System/Library/Fonts/Supplemental/"


def font(name, px):
    try:
        return ImageFont.truetype(FONT_DIR + name, int(px * SS))
    except OSError:
        return ImageFont.load_default()


def polar(r, deg):
    """Point at radius r and clock angle deg (0 = 12 o'clock, clockwise)."""
    a = math.radians(deg - 90)
    return C + r * math.cos(a), C + r * math.sin(a)


def S(pts):
    return [(x * SS, y * SS) for x, y in pts]


def text_on_ring(img, txt, r, deg, f, fill):
    """Text centred at (r, deg), rotated so its baseline is tangent (upright at 12)."""
    tmp = Image.new("RGBA", (300 * SS, 80 * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(tmp)
    d.text((150 * SS, 40 * SS), txt, font=f, fill=fill, anchor="mm")
    tmp = tmp.rotate(-deg, resample=Image.BICUBIC, expand=True)
    x, y = polar(r, deg)
    img.alpha_composite(tmp, (int(x * SS - tmp.width / 2), int(y * SS - tmp.height / 2)))


def dial(with_bezel=True):
    """The face. with_bezel=False leaves the bezel ring black (it is drawn,
    rotatable, from bezel())."""
    img = Image.new("RGBA", (SIZE * SS, SIZE * SS), (0, 0, 0, 255))
    d = ImageDraw.Draw(img)
    if with_bezel:
        draw_bezel(img, d)

    # rehaut (inner ring) and dial
    d.ellipse([(C - BEZEL_IN) * SS, (C - BEZEL_IN) * SS, (C + BEZEL_IN) * SS, (C + BEZEL_IN) * SS],
              fill=(52, 52, 56))
    d.ellipse([(C - DIAL) * SS, (C - DIAL) * SS, (C + DIAL) * SS, (C + DIAL) * SS], fill=BLACK)
    # faint vignette towards the edge
    vig = Image.new("RGBA", img.size, (0, 0, 0, 0))
    vd = ImageDraw.Draw(vig)
    for r in range(DIAL * SS, int(DIAL * 0.55) * SS, -SS * 2):
        a = int(26 * (1 - (r / SS) / DIAL) ** 2)
        vd.ellipse([C * SS - r, C * SS - r, C * SS + r, C * SS + r], fill=(30, 30, 34, a))
    img.alpha_composite(vig)

    # minute track
    for m in range(60):
        r0 = DIAL - 4
        r1 = DIAL - (13 if m % 5 == 0 else 9)
        d.line(S([polar(r0, m * 6), polar(r1, m * 6)]), fill=INK,
               width=int((2.6 if m % 5 == 0 else 1.4) * SS))

    # hour markers: lume with white-gold surrounds
    R = 146
    for h in range(12):
        deg = h * 30
        if h == 0:  # inverted triangle
            tri = [polar(R + 26, -6.6), polar(R + 26, 6.6), polar(R - 8, 0)]
            d.polygon(S(tri), fill=LUME, outline=GOLD, width=int(2.4 * SS))
        elif h == 3:  # date window
            x, y = polar(R + 6, deg)
            d.rounded_rectangle(S([(x - 24, y - 17), (x + 24, y + 17)]), radius=4 * SS,
                                fill=(250, 250, 248), outline=GOLD, width=int(2.4 * SS))
        elif h in (6, 9):  # batons
            a = math.radians(deg - 90)
            ux, uy = math.cos(a), math.sin(a)
            vx, vy = -uy, ux
            r0, r1, hw = R - 18, R + 24, 7.5
            pts = [(C + ux * r0 + vx * hw, C + uy * r0 + vy * hw),
                   (C + ux * r1 + vx * hw, C + uy * r1 + vy * hw),
                   (C + ux * r1 - vx * hw, C + uy * r1 - vy * hw),
                   (C + ux * r0 - vx * hw, C + uy * r0 - vy * hw)]
            d.polygon(S(pts), fill=LUME, outline=GOLD, width=int(2.4 * SS))
        else:  # dots
            x, y = polar(R + 6, deg)
            rr = 13
            d.ellipse(S([(x - rr, y - rr), (x + rr, y + rr)]), fill=LUME, outline=GOLD,
                      width=int(2.4 * SS))

    # printing
    d.text((C * SS, (C - 82) * SS), "ZEPHYR", font=font("Times New Roman Bold.ttf", 25),
           fill=INK, anchor="mm")
    d.text((C * SS, (C - 60) * SS), "DUAL CORE  ·  AMP", font=font("Arial.ttf", 11.5),
           fill=INK, anchor="mm")
    d.text((C * SS, (C + 56) * SS), "GMT", font=font("Arial Bold.ttf", 17),
           fill=GMT_RED, anchor="mm")
    d.text((C * SS, (C + 75) * SS), "SUPERLATIVE", font=font("Arial.ttf", 11.5),
           fill=INK, anchor="mm")
    d.text((C * SS, (C + 92) * SS), "466 · AMOLED", font=font("Arial.ttf", 10),
           fill=(150, 150, 156), anchor="mm")

    return img.resize((SIZE, SIZE), Image.LANCZOS).convert("RGB")


def draw_bezel(img, d):
    # 24-hour bezel, two-tone ceramic with a soft sheen: blue on top
    # (18 -> 24 -> 6, night), red below (6 -> 12 -> 18, day)
    for r in range(BEZEL_OUT * SS, BEZEL_IN * SS, -1):
        t = (r / SS - BEZEL_IN) / (BEZEL_OUT - BEZEL_IN)
        k = 0.78 + 0.32 * math.sin(t * math.pi)
        box = [C * SS - r, C * SS - r, C * SS + r, C * SS + r]
        d.pieslice(box, 180, 360, fill=tuple(min(255, int(v * k)) for v in BEZEL_BLUE))
        d.pieslice(box, 0, 180, fill=tuple(min(255, int(v * k)) for v in BEZEL_RED))
    # even hours are numerals, odd hours small triangles
    fb = font("Arial Bold.ttf", 21)
    rm = (BEZEL_IN + BEZEL_OUT) / 2
    for hr in range(1, 24):
        deg = hr * 15
        if hr % 2 == 0:
            text_on_ring(img, str(hr), rm, deg, fb, INK)
        else:
            tri = [polar(BEZEL_OUT - 9, deg - 2.2), polar(BEZEL_OUT - 9, deg + 2.2),
                   polar(BEZEL_IN + 13, deg)]
            d.polygon(S(tri), fill=INK)
    # 24 marker: triangle with a lume pip
    tri = [polar(BEZEL_OUT - 6, -6.5), polar(BEZEL_OUT - 6, 6.5), polar(BEZEL_IN + 7, 0)]
    d.polygon(S(tri), fill=INK)
    px, py = polar(rm + 4, 0)
    d.ellipse(S([(px - 4.5, py - 4.5), (px + 4.5, py + 4.5)]), fill=LUME)



def bezel():
    """Just the bezel ring, transparent elsewhere (LVGL rotates it)."""
    img = Image.new("RGBA", (SIZE * SS, SIZE * SS), (0, 0, 0, 0))
    draw_bezel(img, ImageDraw.Draw(img))
    small = img.resize((SIZE, SIZE), Image.LANCZOS)
    # keep the ring only: everything inside the bezel's inner edge is clear
    mask = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).ellipse([C - BEZEL_OUT, C - BEZEL_OUT, C + BEZEL_OUT, C + BEZEL_OUT],
                                 fill=255)
    ImageDraw.Draw(mask).ellipse([C - BEZEL_IN + 0.5, C - BEZEL_IN + 0.5,
                                  C + BEZEL_IN - 0.5, C + BEZEL_IN - 0.5], fill=0)
    small.putalpha(mask)
    return small


# ---- hands (drawn pointing up; pivot = rotation centre) --------------------------

def hand_canvas(w, h):
    return Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))


def hour_hand():
    w, h, px, py = 44, 136, 22, 118
    img = hand_canvas(w, h)
    d = ImageDraw.Draw(img)
    shaft = [(px - 4, py + 14), (px + 4, py + 14), (px + 4, py - 54), (px - 4, py - 54)]
    d.polygon(S(shaft), fill=GOLD)
    # Mercedes circle
    cy, rr = py - 72, 17
    d.ellipse(S([(px - rr, cy - rr), (px + rr, cy + rr)]), fill=GOLD)
    d.ellipse(S([(px - rr + 4, cy - rr + 4), (px + rr - 4, cy + rr - 4)]), fill=LUME)
    for a in (90, 210, 330):
        ex = px + (rr - 2) * math.cos(math.radians(a))
        ey = cy + (rr - 2) * math.sin(math.radians(a))
        d.line(S([(px, cy), (ex, ey)]), fill=GOLD, width=int(3 * SS))
    tip = [(px - 9, cy - rr + 3), (px + 9, cy - rr + 3), (px, py - 108)]
    d.polygon(S(tip), fill=GOLD)
    d.polygon(S([(px - 5.5, cy - rr), (px + 5.5, cy - rr), (px, py - 101)]), fill=LUME)
    d.ellipse(S([(px - 9, py - 9), (px + 9, py + 9)]), fill=GOLD)
    return img.resize((w, h), Image.LANCZOS), (px, py)


def minute_hand():
    w, h, px, py = 30, 196, 15, 178
    img = hand_canvas(w, h)
    d = ImageDraw.Draw(img)
    sword = [(px - 7, py + 16), (px + 7, py + 16), (px + 7, py - 150), (px, py - 172),
             (px - 7, py - 150)]
    d.polygon(S(sword), fill=GOLD)
    d.polygon(S([(px - 3.5, py - 30), (px + 3.5, py - 30), (px + 3.5, py - 148), (px, py - 162),
                 (px - 3.5, py - 148)]), fill=LUME)
    d.ellipse(S([(px - 9, py - 9), (px + 9, py + 9)]), fill=GOLD)
    return img.resize((w, h), Image.LANCZOS), (px, py)


def second_hand():
    w, h, px, py = 24, 252, 12, 206
    img = hand_canvas(w, h)
    d = ImageDraw.Draw(img)
    d.line(S([(px, py + 40), (px, py - 196)]), fill=(240, 240, 242), width=int(2.6 * SS))
    d.ellipse(S([(px - 8, py - 140), (px + 8, py - 124)]), fill=GOLD)
    d.ellipse(S([(px - 5.5, py - 137.5), (px + 5.5, py - 126.5)]), fill=LUME)
    d.ellipse(S([(px - 6, py + 26), (px + 6, py + 38)]), fill=(240, 240, 242))  # counterweight
    d.ellipse(S([(px - 6.5, py - 6.5), (px + 6.5, py + 6.5)]), fill=(240, 240, 242))
    return img.resize((w, h), Image.LANCZOS), (px, py)


def gmt_hand():
    """24-hour hand: thin shaft, big red arrow reaching the minute track."""
    w, h, px, py = 36, 196, 18, 178
    img = hand_canvas(w, h)
    d = ImageDraw.Draw(img)
    d.line(S([(px, py + 10), (px, py - 150)]), fill=GOLD, width=int(3 * SS))
    arrow = [(px, py - 174), (px + 13, py - 146), (px - 13, py - 146)]
    d.polygon(S(arrow), fill=GOLD)
    d.polygon(S([(px, py - 167), (px + 9, py - 148.5), (px - 9, py - 148.5)]), fill=GMT_RED)
    d.ellipse(S([(px - 8, py - 8), (px + 8, py + 8)]), fill=GOLD)
    return img.resize((w, h), Image.LANCZOS), (px, py)


def shadow(hand):
    """Soft drop shadow so the hands lift off the dial."""
    img, pv = hand
    sh = Image.new("RGBA", (img.width + 8, img.height + 8), (0, 0, 0, 0))
    a = img.getchannel("A").point(lambda v: int(v * 0.55))
    blk = Image.new("RGBA", img.size, (0, 0, 0, 255))
    blk.putalpha(a)
    sh.alpha_composite(blk, (6, 6))
    sh = sh.filter(ImageFilter.GaussianBlur(2))
    sh.alpha_composite(img, (2, 2))
    return sh, (pv[0] + 2, pv[1] + 2)


def parts():
    return {
        "watch_gmt": shadow(gmt_hand()),
        "watch_hour": shadow(hour_hand()),
        "watch_minute": shadow(minute_hand()),
        "watch_second": shadow(second_hand()),
    }


def preview():
    face = dial().convert("RGBA")
    angles = {"watch_gmt": (16 + 9.5 / 60) * 15, "watch_hour": (10 + 9.5 / 60) * 30,
              "watch_minute": 9.5 * 6, "watch_second": 30 * 6}
    for name, (img, (px, py)) in parts().items():
        big = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
        big.alpha_composite(img, (int(C - px), int(C - py)))
        big = big.rotate(-angles[name], resample=Image.BICUBIC, center=(C, C))
        face.alpha_composite(big)
    face.save("preview.png")
    print("preview.png")


def c_array(name, data, per=24):
    rows = ["\t" + ", ".join(f"0x{v:02x}" for v in data[i:i + per]) + ","
            for i in range(0, len(data), per)]
    return f"const uint8_t {name}[] __aligned(4) = {{\n" + "\n".join(rows) + "\n};\n"


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


DATE_W, DATE_H = 44, 30   # date window interior, centred at (385, 233)


def date_images():
    """1..31 as black digits on the white date window."""
    f = font("Arial Bold.ttf", 21)
    out = []
    for day in range(1, 32):
        img = Image.new("RGB", (DATE_W * SS, DATE_H * SS), (250, 250, 248))
        ImageDraw.Draw(img).text((DATE_W * SS / 2, DATE_H * SS / 2 + SS), str(day), font=f,
                                 fill=(12, 12, 14), anchor="mm")
        out.append(img.resize((DATE_W, DATE_H), Image.LANCZOS))
    return out


def export(out):
    """Plain C arrays (no LVGL): core 1 composites these itself.
    RGB565 is little-endian; ARGB8888 is B,G,R,A; the bezel is an RGB565 plane
    followed by an A8 plane."""
    src = ["/* Generated by tools/watchface/watch.py export - do not edit. */",
           "#include <zephyr/toolchain.h>", '#include "watch_img.h"', ""]
    hdr = ["/* Generated by tools/watchface/watch.py export - do not edit. */",
           "#ifndef WATCH_IMG_H_", "#define WATCH_IMG_H_", "", "#include <stdint.h>", "",
           f"#define WATCH_SIZE {SIZE}", f"#define WATCH_BEZEL_IN {BEZEL_IN}",
           f"#define WATCH_BEZEL_OUT {BEZEL_OUT}", "",
           "extern const uint8_t watch_dial[];   /* RGB565 */",
           "extern const uint8_t watch_bezel[];  /* RGB565 + A8 */"]

    px = dial(with_bezel=False).tobytes()
    data = bytearray()
    for i in range(0, len(px), 3):
        v = rgb565(*px[i:i + 3])
        data += bytes((v & 0xFF, v >> 8))
    src.append(c_array("watch_dial", data))

    rgb, alpha = bytearray(), bytearray()
    for r, g, bl, a in bezel().get_flattened_data():
        v = rgb565(r, g, bl)
        rgb += bytes((v & 0xFF, v >> 8))
        alpha.append(a)
    src.append(c_array("watch_bezel", rgb + alpha))

    for name, (img, (pvx, pvy)) in parts().items():
        if name == "watch_second":
            continue  # drawn analytically on the device
        raw = img.tobytes()
        argb = bytearray()
        for i in range(0, len(raw), 4):
            r, g, b, a = raw[i:i + 4]
            if a == 0:
                r = g = b = 0
            argb += bytes((b, g, r, a))
        src.append(c_array(name, argb))
        N = name.upper()
        hdr += [f"extern const uint8_t {name}[];  /* ARGB8888 */",
                f"#define {N}_W {img.width}", f"#define {N}_H {img.height}",
                f"#define {N}_PIVOT_X {pvx}", f"#define {N}_PIVOT_Y {pvy}"]

    dates = bytearray()
    for img in date_images():
        for r, g, b in img.get_flattened_data():
            v = rgb565(r, g, b)
            dates += bytes((v & 0xFF, v >> 8))
    src.append(c_array("watch_dates", dates))
    hdr += ["", "extern const uint8_t watch_dates[]; /* 31 x RGB565, day 1 first */",
            f"#define WATCH_DATE_W {DATE_W}", f"#define WATCH_DATE_H {DATE_H}",
            "#define WATCH_DATE_CX 385", "#define WATCH_DATE_CY 233", "", "#endif", ""]
    open(os.path.join(out, "watch_img.c"), "w").write("\n".join(src))
    open(os.path.join(out, "watch_img.h"), "w").write("\n".join(hdr))
    print(f"wrote watch_img.c/.h to {out}")


if __name__ == "__main__":
    if sys.argv[1:2] == ["export"]:
        export(sys.argv[2] if len(sys.argv) > 2 else "../../apps/watch/remote/src")
    else:
        preview()
