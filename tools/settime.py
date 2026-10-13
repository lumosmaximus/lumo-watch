#!/usr/bin/env python3
# Copyright (c) 2026 Adi Nandanamudi
# SPDX-License-Identifier: Apache-2.0
"""Set the watch's clock from this computer over USB.

    uv run --with pyserial tools/settime.py [--wait] [port]

Sends "T YYYY-MM-DD HH:MM:SS" (local time) to the watch's USB serial console
right as a second starts, and prints the watch's reply. --wait keeps
retrying until the board is plugged in. The port is opened without touching
DTR/RTS, so the board is not reset.
"""

import datetime
import glob
import sys
import time

import serial


def find_port():
    ports = glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*")
    return ports[0] if ports else None


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    wait = "--wait" in sys.argv
    port = args[0] if args else None
    while True:
        port = port or find_port()
        if port:
            break
        if not wait:
            sys.exit("no watch found (plug it in, or pass the port)")
        time.sleep(0.5)

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = s.rts = True  # the defaults; changing them can reset the ESP32-S3
    s.open()
    # line up with the start of a second, the RTC counts whole seconds
    now = datetime.datetime.now()
    time.sleep(1 - now.microsecond / 1e6)
    t = datetime.datetime.now().replace(microsecond=0)
    s.write(f"T {t:%Y-%m-%d %H:%M:%S}\n".encode())
    end = time.time() + 3
    while time.time() < end:
        line = s.readline().decode(errors="replace").strip()
        if "time" in line:
            print(line)
    s.close()


if __name__ == "__main__":
    main()
