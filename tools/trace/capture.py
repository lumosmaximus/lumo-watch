#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# dependencies = ["pyserial"]
# ///
# Copyright (c) 2026 Adi Nandanamudi
# SPDX-License-Identifier: Apache-2.0
"""Record an event trace from the watch.

    uv run tools/trace/capture.py <name> [seconds]     (default 3 s)

Sends T1 (start), waits, sends T0 (stop + dump), and saves
docs/data/traces/<name>.trace (binary, 8 bytes per event; see tracelib.py)
plus <name>.log (the console lines around it). Do things on the watch while
it records: turn the bezel, open the remote, turn the knob.
"""

import base64
import os
import sys
import time

import serial
from serial.tools import list_ports

WATCH_VID, WATCH_PID = 0x1209, 0x0001


def port():
    for p in list_ports.comports():
        if p.vid == WATCH_VID and p.pid == WATCH_PID:
            return p.device
    sys.exit("watch not found (its USB device, 1209:0001)")


def main():
    name = sys.argv[1] if len(sys.argv) > 1 else "trace"
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0
    out = os.path.join(os.path.dirname(__file__), "..", "..", "docs", "data", "traces")
    os.makedirs(out, exist_ok=True)

    s = serial.Serial(port(), 115200, timeout=0.5)
    time.sleep(0.2)
    s.reset_input_buffer()
    s.write(b"T1\n")
    print(f"recording {secs:.1f} s ...")
    time.sleep(secs)
    s.write(b"T0\n")

    raw, log, header, bad = bytearray(), [], None, 0
    t_end = time.time() + 30
    while time.time() < t_end:
        line = s.readline().decode(errors="replace").strip()
        if not line:
            continue
        log.append(line)
        if line.startswith("TR begin"):
            _, _, n, dropped, hz = line.split()
            header = (int(n), int(dropped), int(hz))
        elif line == "TR end":
            break
        elif line.startswith("TR ") and header:
            body = line[3:]
            if len(body) % 4 or len(body) > 640:
                bad += 1  # another console line got mixed in: skip it
                continue
            try:
                raw += base64.b64decode(body, validate=True)
            except Exception:
                bad += 1
    s.close()
    if not header:
        sys.exit("no dump received; is the firmware built with CONFIG_WATCH_TRACE?")
    n, dropped, hz = header
    path = os.path.join(out, name + ".trace")
    with open(path, "wb") as f:
        f.write(b"LWTR" + hz.to_bytes(4, "little") + raw)
    with open(os.path.join(out, name + ".log"), "w") as f:
        f.write("\n".join(ln for ln in log if not ln.startswith("TR ")) + "\n")
    print(f"{len(raw) // 8} events ({dropped} dropped on core 0, {bad} bad lines) -> {path}")
    for ln in log:
        if ln.startswith("TR start"):
            print(ln)


if __name__ == "__main__":
    main()
