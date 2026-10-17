#!/usr/bin/env python3
# Copyright (c) 2026 Adi Nandanamudi
# SPDX-License-Identifier: Apache-2.0
"""Record the watch's console for N seconds, resetting the board first.

    uv run --with pyserial tools/capture/cap.py 30 > capture.txt

Used for the per-second stats captures in docs/data/*.txt. Resets the board
through the USB-Serial-JTAG DTR/RTS lines, so it only works on the chip's
flashing port (apps/amp_hello, apps/hw_test); the watch app owns the USB port
itself, so for it use tools/trace/capture.py instead.
"""
import serial, time, glob, re, sys
dur=float(sys.argv[1])
def op():
    for i in range(40):
        p=glob.glob('/dev/cu.usbmodem*')
        if p:
            try: return serial.Serial(p[0], 115200, timeout=0.3)
            except Exception: pass
        time.sleep(0.3)
s=op(); s.dtr=False; s.rts=True; time.sleep(0.1); s.rts=False
t=time.time()
while time.time()-t<dur:
    try: d=s.read(4096)
    except Exception:
        s.close(); time.sleep(0.5); s=op(); continue
    if d: sys.stdout.write(re.sub(r'\x1b\[[0-9;]*[A-Za-z]','',d.decode(errors='replace'))); sys.stdout.flush()
