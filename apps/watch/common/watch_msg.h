/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Messages between core 0 and core 1 for the watch app.
 */

#ifndef WATCH_MSG_H_
#define WATCH_MSG_H_

#include <stdint.h>

#define MSG_TIME  1 /* core 0 -> core 1: wall clock, sent right as a second starts */
#define MSG_STATS 2 /* core 1 -> core 0: frames drawn, for the fps readout */
#define MSG_TOUCH 3 /* core 0 -> core 1 */
#define MSG_BENCH 4 /* core 1 -> core 0: startup micro-benchmark, cycles per pixel x10 */

struct msg_bench {
	uint32_t seq_read, band_read, compute, internal_read, box_plain, box_hands;
};

struct msg_time {
	uint16_t year;
	uint8_t month, day, hour, min, sec, wday;
};

struct msg_stats {
	uint32_t frames;    /* frames drawn in the last second */
	uint32_t uptime_ms;
	uint32_t render_us; /* average work per frame over that second */
	uint32_t px;        /* average pixels composited per frame */
	uint32_t comp_us;   /* average compositing time per frame */
	uint32_t write_us;  /* average display_write time per frame */
	uint32_t boxes;     /* average dirty boxes per frame */
	uint32_t st_bg, st_ring, st_hands, st_sec, st_swap; /* us per frame, by stage */
	uint32_t sec_px;    /* pixels the second-hand stage visited per frame */
};

struct msg_touch {
	uint16_t x, y;
	uint8_t down;
};

#endif
