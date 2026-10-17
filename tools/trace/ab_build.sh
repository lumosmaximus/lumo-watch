#!/bin/sh
# Build the watch with each optimisation switched off, one build dir each,
# for before/after traces (docs/optimization-log.md). Run from the west
# workspace root (the directory that holds zephyr/ and amoled_175/).
#   sh amoled_175/tools/trace/ab_build.sh [variant ...]
set -e
APP=amoled_175/apps/watch
BOARD=esp32s3_touch_amoled_1_75/esp32s3/procpu
ROOT="-DBOARD_ROOT=$PWD/amoled_175"
build() {
	name=$1; shift
	echo "== build-ab-$name: $*"
	west build -p always --sysbuild -d build-ab-$name -b $BOARD $APP -- $ROOT "$@" \
		> build-ab-$name.log 2>&1 && echo "   ok" || { echo "   FAILED (see build-ab-$name.log)"; }
}
want="${*:-zephyr-spi no-overlap ring-flash no-gate free-run}"
for v in $want; do
	case $v in
	zephyr-spi) build $v -Dwatch_remote_CONFIG_WATCH_FAST_SPI=n ;;
	no-overlap) build $v -Dwatch_remote_CONFIG_WATCH_SPI_OVERLAP=n ;;
	ring-flash) build $v -Dwatch_remote_CONFIG_WATCH_RING_RAM=n ;;
	no-gate)    build $v -Dwatch_remote_CONFIG_WATCH_SWEEP_GATE=n ;;
	free-run)   build $v -Dwatch_remote_CONFIG_WATCH_TE_LOCK=n ;;
	esac
done
