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
set js to "(()=>{const v=[...document.querySelectorAll('video')];if(!v.length)return '-1';return v.some(x=>!x.paused&&!x.ended)?'1':'0'})()"
repeat with browser_name in {"Google Chrome", "Microsoft Edge"}
	if p is not "1" then
		try
			if application browser_name is running then
				set q to "tell application \"" & browser_name & "\"
set r to \"-1\"
repeat with w in windows
	repeat with t in tabs of w
		if URL of t contains \"youtube.com\" then
			set x to execute t javascript \"" & js & "\"
			if x is \"1\" then return \"1\"
			if x is \"0\" then set r to \"0\"
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
				set ps to run script "tell application \"" & app_name & "\" to return (player state as string)"
				if ps is "playing" then
					set p to "1"
				else
					set p to "0"
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
    out = subprocess.run(["osascript", "-e", SCRIPT], capture_output=True, text=True,
                         timeout=3).stdout.strip()
    vol, muted, play = out.split(",")
    return int(vol), 1 if muted == "true" else 0, int(play)


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
    while True:
        try:
            v, m, p = media()
            line = f"S v={v} m={m} b={brightness()} p={p}\n"
            s.write(line.encode())
            if line != last:
                print(line.strip())
                last = line
        except (serial.SerialException, OSError):
            print("watch gone, waiting...")
            s = port()
            threading.Thread(target=drain, args=(s,), daemon=True).start()
        except Exception as e:  # osascript hiccup: try again
            print("read failed:", e, file=sys.stderr)
        time.sleep(0.4)


if __name__ == "__main__":
    main()
