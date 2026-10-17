#!/bin/sh
# Build the host renderer: core 1's compositor compiled for this computer.
#   sh tools/render/build.sh        -> tools/render/render
set -e
cd "$(dirname "$0")"
SRC=../../apps/watch/remote/src
cc -O2 -std=gnu11 -w \
	-I shim -I "$SRC" \
	-DCONFIG_WATCH_FAST_SPI=1 -DCONFIG_WATCH_SPI_OVERLAP=1 -DCONFIG_WATCH_RING_RAM=1 \
	-DCONFIG_WATCH_SWEEP_GATE=1 -DCONFIG_WATCH_TE_LOCK=1 \
	-Dmain=watch_main -include stdint.h \
	-o render.o -c "$SRC/main.c"
cc -O2 -std=gnu11 -w -I shim -I "$SRC" -c "$SRC/watch_img.c" -o watch_img.o
cc -O2 -std=gnu11 -w -I shim -I "$SRC" -c "$SRC/speed_img.c" -o speed_img.o
cc -O2 -std=gnu11 -w -I shim -I "$SRC" -c "$SRC/knob_img.c" -o knob_img.o
cc -O2 -std=gnu11 -w -I shim -I "$SRC" -c host.c -o host.o
cc -o render render.o watch_img.o speed_img.o knob_img.o host.o -lm
rm -f render.o watch_img.o speed_img.o knob_img.o host.o
echo "built tools/render/render"
