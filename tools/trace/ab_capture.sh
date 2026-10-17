#!/bin/sh
# Flash each A/B variant (tools/trace/ab_build.sh first) and record the same
# scripted GMT scenarios, then put the normal build back. Run from the west
# workspace root.
cd "$(dirname "$0")/../.." || exit 1
REPO=$PWD; cd ..
cap() { (cd "$REPO" && uv run tools/trace/capture.py "$@" 2>&1 | grep events); }
scen() {
	p=$1
	cap ${p}gmt_idle 1.8 --setup F0 --setup "T 2026-10-17 03:50:00"
	cap ${p}gmt_bezel 1.8 --setup F0 --at 0.3:Z1 --at 0.55:Z1 --at 0.8:Z1 --at 1.05:Z1 --at 1.3:Z-1
	cap ${p}gmt_hands_cross 1.8 --setup F0 --setup "T 2026-10-17 10:30:28"
}
for v in ${*:-zephyr-spi no-overlap ring-flash no-gate free-run}; do
	echo "== $v"
	sh "$REPO/tools/flash.sh" build-ab-$v 2>&1 | grep -E "Wrote.*bc0000|ERROR|no USB"
	sleep 16   # core 1 boots, draws, runs its benchmark
	scen ab_${v}_
done
echo "== main"
sh "$REPO/tools/flash.sh" build-watch 2>&1 | grep -E "Wrote.*bc0000|ERROR"
sleep 16
scen ""
echo done
