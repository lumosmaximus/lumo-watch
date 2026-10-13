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
#define MSG_BUTTON 5 /* core 0 -> core 1: a button event */
#define MSG_HELLO  6 /* core 1 -> core 0: ready, please send the time */
#define MSG_KEY    7 /* core 1 -> core 0: send a media key to the computer (USB HID) */
#define MSG_STATE  8 /* core 0 -> core 1: the computer's media state (tools/agent) */

/* fields set to -1 are unknown (the helper could not read them) */
struct msg_state {
	int8_t volume;     /* 0..100 */
	int8_t muted;      /* 0/1 */
	int8_t brightness; /* 0..100 */
	int8_t playing;    /* 0/1 */
};

enum {
	KEY_VOL_UP = 1,
	KEY_VOL_DOWN,
	KEY_MUTE,
	KEY_PLAY_PAUSE,
	KEY_BRIGHT_UP,
	KEY_BRIGHT_DOWN,
	KEY_NEXT,
	KEY_PREV,
};

struct msg_key {
	uint8_t key;
};

enum {
	BTN_PWR = 1,       /* PWR pressed */
	BTN_BOOT = 2,      /* BOOT pressed and released within 3 s */
	BTN_BOOT_LONG = 3, /* BOOT held for 3 s (face picker) */
	BTN_TWO_HOLD = 4,  /* two fingers held on the screen for 1.5 s (remote) */
};

struct msg_button {
	uint8_t btn;
};

struct msg_bench {
	uint32_t seq_read, band_read, compute, internal_read, box_plain, box_hands;
	uint32_t ring_full_us; /* whole bezel ring repainted (one click), us */
	uint32_t ring_px;      /* pixels in that repaint */
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
	uint32_t underruns; /* SPI transfers whose DMA FIFO ran dry (last second) */
	uint32_t overlap;   /* 1: boxes sent while the next is composed */
};

struct msg_touch {
	uint16_t x, y;
	uint8_t down;
	uint8_t fingers;
};

/* The ESP32 IPM driver has one mailbox per direction and does not wait for
 * the other core to read a message: a second send right after the first
 * overwrites it (that lost the remote's open/close message, sent just before
 * a touch update). msg_send() sends one message at a time and gives the other
 * core's interrupt time to copy it out before the next.
 */
#include <zephyr/drivers/ipm.h>
#include <zephyr/kernel.h>

#define MSG_SEND_GAP_US 200

static inline int msg_send(const struct device *ipm, uint32_t id, const void *data, int size)
{
	static K_MUTEX_DEFINE(msg_lock);
	int ret;

	k_mutex_lock(&msg_lock, K_FOREVER);
	ret = ipm_send(ipm, -1, id, data, size);
	k_busy_wait(MSG_SEND_GAP_US);
	k_mutex_unlock(&msg_lock);
	return ret;
}

#endif
