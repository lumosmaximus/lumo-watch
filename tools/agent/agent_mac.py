#!/usr/bin/env python3
# Copyright (c) 2026 Adi Nandanamudi
# SPDX-License-Identifier: Apache-2.0
"""Tell the watch what the Mac is doing, so its remote stays in sync.

    uv run --with pyserial tools/agent/agent_mac.py

Every 0.4 s it sends one line over the watch's USB serial port:
    S v=<volume 0-100> m=<muted 0/1> b=<brightness 0-100 or -1> p=<playing 0/1 or -1>
The watch shows the level as an arc round its knob, and its mute and
play/pause buttons follow the Mac (pausing with the trackpad included).

Sources: volume and mute from macOS (AppleScript); brightness of the built-in
display (DisplayServices; -1 for an external main display); play state of
YouTube tabs in Chrome or Edge (turn on View > Developer > Allow JavaScript
from Apple Events in the browser), else Music or Spotify if running. (macOS
keeps the system-wide "now playing" from other apps.) The first run asks to
allow controlling the browser / Music / Spotify.
"""

import ctypes
import ctypes.util
import glob
import re
import subprocess
import sys
import threading
import time

import serial

SCRIPT = r'''
set s to get volume settings
set p to "-1"
-- YouTube (or any page with a video) in Chrome or Edge: needs
-- View > Developer > Allow JavaScript from Apple Events in that browser
-- returns "-1", or play state, title and channel separated by character 31
set js to "(()=>{const v=[...document.querySelectorAll('video')];if(!v.length)return '-1';const p=v.some(x=>!x.paused&&!x.ended)?'1':'0';const S=String.fromCharCode(31);const t=document.title.replace(/ - YouTube$/,'');const c=document.querySelector('ytd-video-owner-renderer #channel-name a')||document.querySelector('#owner #channel-name a');return p+S+t+S+(c?c.textContent.trim():'')})()"
repeat with browser_name in {"Google Chrome", "Microsoft Edge"}
	if p does not start with "1" then
		try
			if application browser_name is running then
				set q to "tell application \"" & browser_name & "\"
set r to \"-1\"
repeat with w in windows
	repeat with t in tabs of w
		if URL of t contains \"youtube.com\" then
			set x to execute t javascript \"" & js & "\"
			if x starts with \"1\" then return x
			if x starts with \"0\" and r is \"-1\" then set r to x
		end if
	end repeat
end repeat
return r
end tell"
				set b to run script q
				if b is not "-1" then set p to b
			end if
		end try
	end if
end repeat
repeat with app_name in {"Spotify", "Music"}
	if p is "-1" then
		try
			if application app_name is running then
				-- compiled only now, so a missing app is not a script error
				set sep to character id 31
				set ps to run script "tell application \"" & app_name & "\" to return (player state as string)"
				set info to run script "tell application \"" & app_name & "\" to return (name of current track) & (character id 31) & (artist of current track)"
				if ps is "playing" then
					set p to "1" & sep & info
				else
					set p to "0" & sep & info
				end if
			end if
		end try
	end if
end repeat
return ((output volume of s) as text) & "," & ((output muted of s) as text) & "," & p
'''

try:
    _ds = ctypes.CDLL("/System/Library/PrivateFrameworks/DisplayServices.framework/DisplayServices")
    _cg = ctypes.CDLL(ctypes.util.find_library("CoreGraphics"))
    _cg.CGMainDisplayID.restype = ctypes.c_uint32
except OSError:
    _ds = None


def brightness():
    if not _ds:
        return -1
    b = ctypes.c_float()
    if _ds.DisplayServicesGetBrightness(_cg.CGMainDisplayID(), ctypes.byref(b)) != 0:
        return -1
    return int(round(b.value * 100))


def media():
    """(volume, muted, playing or -1, title, artist)"""
    out = subprocess.run(["osascript", "-e", SCRIPT], capture_output=True, text=True,
                         timeout=4).stdout.strip()
    vol, muted, rest = out.split(",", 2)
    fields = rest.split("\x1f") + ["", ""]
    title = re.sub(r"^\(\d+\)\s+", "", fields[1])  # YouTube's "(31) " notification count
    return int(vol), 1 if muted == "true" else 0, int(fields[0]), title, fields[2]


# ---- now playing text: drawn here, sent as a small 2-bit alpha image ----------------
TEXT_W, TEXT_H = 224, 40        # must match the watch (RM_TEXT_W/H)
FONT = "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"


def strip_image(txt, size):
    """One line of text, 3x supersampled, as wide as it needs to be."""
    from PIL import Image, ImageDraw, ImageFont
    ss = 3
    f = ImageFont.truetype(FONT, size * ss)
    w = int(ImageDraw.Draw(Image.new("L", (1, 1))).textlength(txt, font=f) / ss) + 2
    img = Image.new("L", (w * ss, 20 * ss), 0)
    ImageDraw.Draw(img).text((ss, 10 * ss), txt, font=f, fill=255, anchor="lm")
    return img.resize((w, 20), Image.LANCZOS)


class Line:
    """A line of the now-playing text: still if it fits, else it scrolls."""
    GAP, SPEED, PAUSE = 48, 36.0, 1.5      # px between repeats, px/s, s at the start

    def __init__(self, txt, size):
        self.img = strip_image(txt, size)
        self.w = self.img.width
        self.t0 = time.time()

    def frame(self, now):
        from PIL import Image
        out = Image.new("L", (TEXT_W, 20), 0)
        if self.w <= TEXT_W:
            out.paste(self.img, ((TEXT_W - self.w) // 2, 0))
            return out
        t = max(0.0, now - self.t0 - self.PAUSE)
        x = int(t * self.SPEED) % (self.w + self.GAP)
        if x == 0 and t > 0:
            self.t0 = now          # pause again at the start of each pass
        out.paste(self.img, (-x, 0))
        out.paste(self.img, (self.w + self.GAP - x, 0))
        return out

    @property
    def moving(self):
        return self.w > TEXT_W


def pack(title_img, artist_img):
    """Title rows 0-19 (+2 blank), artist rows 22-39 (cropped from 20), 2 bits
    per pixel, 4 pixels per byte, first pixel in the top bits."""
    from PIL import Image
    img = Image.new("L", (TEXT_W, TEXT_H), 0)
    img.paste(title_img, (0, 1))
    img.paste(artist_img.crop((0, 1, TEXT_W, 19)), (0, 22))
    px = img.tobytes()
    out = bytearray()
    for i in range(0, len(px), 4):
        b = 0
        for k in range(4):
            b = (b << 2) | min(3, (px[i + k] + 42) // 85)
        out.append(b)
    return bytes(out)


def port():
    while True:
        p = glob.glob("/dev/cu.usbmodem*")
        if p:
            try:
                s = serial.Serial(p[0], 115200, timeout=0.2)
                print("watch on", p[0])
                return s
            except serial.SerialException:
                pass
        time.sleep(1)


def drain(s):
    """Read (and drop) the watch's console output so its buffer never fills."""
    while True:
        try:
            s.read(4096)
        except Exception:
            return


def main():
    s = port()
    threading.Thread(target=drain, args=(s,), daemon=True).start()
    last = None
    lines = [None]           # (Line title, Line artist), swapped by the poller
    lock = threading.Lock()

    def text_loop():
        """Send the now-playing text: ~12 frames/s while a line scrolls, else
        when it changes and every 5 s (the watch may have restarted)."""
        import base64
        sent, sent_t = None, 0
        while True:
            cur = lines[0]
            now = time.time()
            if cur and (cur[0].moving or cur[1].moving or cur is not sent or now - sent_t > 5):
                data = pack(cur[0].frame(now), cur[1].frame(now))
                try:
                    with lock:
                        s.write(b"N " + base64.b64encode(data) + b"\n")
                except Exception:
                    pass
                sent, sent_t = cur, now
            time.sleep(0.08)

    threading.Thread(target=text_loop, daemon=True).start()
    text = None
    while True:
        try:
            v, m, p, title, artist = media()
            line = f"S v={v} m={m} b={brightness()} p={p}\n"
            with lock:
                s.write(line.encode())
            if line != last:
                print(line.strip())
                last = line
            if (title, artist) != text:
                text = (title, artist)
                lines[0] = (Line(title, 16), Line(artist, 13))
                print("now playing:", title, "-", artist)
        except (serial.SerialException, OSError):
            print("watch gone, waiting...")
            s = port()
            threading.Thread(target=drain, args=(s,), daemon=True).start()
        except Exception as e:  # osascript hiccup: try again
            print("read failed:", e, file=sys.stderr)
        time.sleep(0.4)


if __name__ == "__main__":
    main()
