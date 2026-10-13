#!/bin/sh
# Copyright (c) 2026 Adi Nandanamudi
# SPDX-License-Identifier: Apache-2.0
#
# Flash both watch images (core 0 at 0x0, core 1 at 0xbc0000) in ONE esptool
# run, so the board is not reset between them. (west flash writes them one
# after the other with a reset in between; once core 0 runs our own USB
# device, the USB-Serial-JTAG port is gone and the second write fails.)
#
#   tools/flash.sh [build-dir]        default: ../build-watch
#
# If the watch firmware is running, the script first sends "B" on its USB
# serial port (reboot to ROM download mode). If no port answers, hold BOOT
# while plugging the board in, then run this again.
set -e
B=${1:-$(dirname "$0")/../../build-watch}
ESPTOOL=${ESPTOOL:-esptool}

port() { ls /dev/cu.usbmodem* 2>/dev/null | head -1; }

P=$(port)
if [ -n "$P" ] && ! system_profiler SPUSBDataType 2>/dev/null | grep -q "USB JTAG/serial debug unit"; then
	echo "watch firmware on $P: asking it to enter download mode"
	printf 'B\n' > "$P" || true
	sleep 3
fi
P=$(port)
[ -n "$P" ] || { echo "no USB-Serial-JTAG port: hold BOOT while plugging in, then retry"; exit 1; }
"$ESPTOOL" --port "$P" --baud 921600 --before default-reset --after hard-reset \
	write-flash -u --flash-mode dio --flash-freq 80m --flash-size 16MB \
	0x0 "$B/watch/zephyr/zephyr.bin" \
	0xbc0000 "$B/watch_remote/zephyr/zephyr.signed.bin"
