/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host render harness: runs core 1's compositor (apps/watch/remote/src/main.c,
 * compiled unchanged against the stub headers in shim/) on the computer,
 * with simulated time and a 466x466 frame buffer instead of the panel. Each
 * frame is written as a PPM; render.py turns them into PNGs and GIFs.
 *
 *   render [--frames N] [--time HH:MM:SS] [--out dir] [--every K]
 *          [--at FRAME:CMD ...]
 *   CMD: face=N | bezel=CLICKS | knob=DEG | touch=X,Y,DOWN[,FINGERS] |
 *        button=N (1 PWR, 2 BOOT, 3 BOOT long, 4 two-finger hold) |
 *        state=VOL,MUTED,BRIGHT,PLAYING | text=FILE (2-bit 224x40 image, 2240 B)
 *
 * The clock is the simulated one: frames are exactly 1/60 s apart, so the
 * second hand moves 0.1 degree per frame like on the watch.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/kernel.h>
#include "../../apps/watch/common/watch_msg.h"
#include "../../apps/watch/remote/src/qspi_fast.h"
#include <soc/systimer_struct.h>

#define N 466
#define FRAME_NS 16666667LL

int64_t host_time_ns = 2000000000LL; /* starts at 2 s of "uptime" */
const struct device host_dev = {"host"};
systimer_dev_t SYSTIMER;
uint32_t qf_underruns;

static uint16_t fb[N * N]; /* big-endian RGB565, as the panel receives it */
static ipm_callback_t on_msg;
static int frames_wanted = 120, every = 1, frame_no = -1, te_calls;
static const char *out_dir = "render_out";
static struct { int frame; char cmd[128]; } actions[64];
static int n_actions;
static int hh = 10, mm = 9, ss = 30;

int watch_main(void); /* main.c's main(), renamed with -Dmain=watch_main */

void host_advance_ns(int64_t ns)
{
	host_time_ns += ns;
}

static void deliver(uint32_t id, const void *data)
{
	if (on_msg) {
		on_msg(&host_dev, NULL, id, (volatile void *)data);
	}
}

static void send_time(void)
{
	int64_t t = host_time_ns / 1000000000LL;
	struct msg_time mt = {.year = 2026, .month = 10, .day = 17, .wday = 5};
	int s = ss + (int)t;

	mt.hour = (hh + (mm + s / 60) / 60) % 24;
	mt.min = (mm + s / 60) % 60;
	mt.sec = s % 60;
	deliver(MSG_TIME, &mt);
}

static void run_action(const char *cmd)
{
	int a, b, c, d;
	char path[100];

	if (sscanf(cmd, "face=%d", &a) == 1) {
		struct msg_cmd m = {CMD_FACE, (int16_t)a};
		deliver(MSG_CMD, &m);
	} else if (sscanf(cmd, "bezel=%d", &a) == 1) {
		struct msg_cmd m = {CMD_BEZEL, (int16_t)a};
		deliver(MSG_CMD, &m);
	} else if (sscanf(cmd, "knob=%d", &a) == 1) {
		struct msg_cmd m = {CMD_KNOB, (int16_t)a};
		deliver(MSG_CMD, &m);
	} else if (sscanf(cmd, "touch=%d,%d,%d,%d", &a, &b, &c, &d) >= 3) {
		struct msg_touch t = {.x = a, .y = b, .down = c, .fingers = d > 0 ? d : c};
		deliver(MSG_TOUCH, &t);
	} else if (sscanf(cmd, "button=%d", &a) == 1) {
		struct msg_button m = {.btn = a};
		deliver(MSG_BUTTON, &m);
	} else if (sscanf(cmd, "state=%d,%d,%d,%d", &a, &b, &c, &d) == 4) {
		struct msg_state st = {a, b, c, d};
		deliver(MSG_STATE, &st);
	} else if (sscanf(cmd, "text=%99s", path) == 1) {
		static uint8_t img[TEXT_BYTES];
		FILE *f = fopen(path, "rb");

		if (f && fread(img, 1, TEXT_BYTES, f) == TEXT_BYTES) {
			static struct msg_text mt;

			for (uint16_t off = 0; off < TEXT_BYTES; off += TEXT_CHUNK) {
				mt.off = off;
				mt.len = MIN(TEXT_CHUNK, TEXT_BYTES - off);
				memcpy(mt.data, img + off, mt.len);
				deliver(MSG_TEXT, &mt);
			}
		}
		if (f) {
			fclose(f);
		}
	} else {
		fprintf(stderr, "unknown action: %s\n", cmd);
	}
}

static void save_frame(void)
{
	char path[256];

	snprintf(path, sizeof(path), "%s/frame_%04d.ppm", out_dir, frame_no);
	FILE *f = fopen(path, "wb");

	if (!f) {
		perror(path);
		exit(1);
	}
	fprintf(f, "P6\n%d %d\n255\n", N, N);
	for (int i = 0; i < N * N; i++) {
		uint16_t v = (uint16_t)((fb[i] >> 8) | (fb[i] << 8)); /* back to native */
		uint8_t px[3] = {(v >> 8) & 0xf8, (v >> 3) & 0xfc, (v << 3) & 0xf8};

		fwrite(px, 1, 3, f);
	}
	fclose(f);
}

/* Each panel refresh (TE rising edge, the compositor polls for it): the
 * frame that was just drawn is complete, so save it, then run this frame's
 * scripted actions and advance time by one refresh.
 */
static void frame_edge(void)
{
	if (frame_no >= 0 && frame_no % every == 0) {
		save_frame();
	}
	frame_no++;
	if (frame_no >= frames_wanted) {
		fprintf(stderr, "%d frames -> %s\n", frames_wanted, out_dir);
		exit(0);
	}
	for (int i = 0; i < n_actions; i++) {
		if (actions[i].frame == frame_no) {
			run_action(actions[i].cmd);
		}
	}
	if (frame_no == 0) {
		send_time();
	}
	host_advance_ns(FRAME_NS);
}

/* wait_te() polls GPIO_IN_REG: high, low, high reads one TE cycle */
uint32_t host_reg_read(uint32_t reg)
{
	(void)reg;
	te_calls++;
	if (te_calls % 2 == 1) {
		if (frame_no >= -1 && te_calls > 2) {
			frame_edge();
		}
		return BIT(13);
	}
	return 0;
}

int display_write(const struct device *d, uint16_t x, uint16_t y,
		  const struct display_buffer_descriptor *desc, const void *buf)
{
	(void)d;
	const uint16_t *src = buf;

	for (int r = 0; r < desc->height; r++) {
		memcpy(&fb[(y + r) * N + x], &src[r * desc->width], desc->width * 2);
	}
	return 0;
}

void qf_write(int x, int y, int w, int h, const void *buf)
{
	struct display_buffer_descriptor desc = {.width = w, .height = h, .pitch = w};

	display_write(&host_dev, x, y, &desc, buf);
}

void qf_wait(void)
{
}

int ipm_send(const struct device *d, int wait, uint32_t id, const void *data, int size)
{
	(void)d;
	(void)wait;
	(void)size;
	if (id == MSG_HELLO) {
		send_time();
	} else if (id == MSG_KEY) {
		fprintf(stderr, "frame %d: key %u\n", frame_no, ((const struct msg_key *)data)->key);
	}
	return 0;
}

void ipm_register_callback(const struct device *d, ipm_callback_t cb, void *user)
{
	(void)d;
	(void)user;
	on_msg = cb;
}

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
			frames_wanted = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--every") && i + 1 < argc) {
			every = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
			out_dir = argv[++i];
		} else if (!strcmp(argv[i], "--time") && i + 1 < argc) {
			sscanf(argv[++i], "%d:%d:%d", &hh, &mm, &ss);
		} else if (!strcmp(argv[i], "--at") && i + 1 < argc && n_actions < 64) {
			const char *s = argv[++i];
			const char *colon = strchr(s, ':');

			if (colon) {
				actions[n_actions].frame = atoi(s);
				snprintf(actions[n_actions].cmd, sizeof(actions[0].cmd), "%s", colon + 1);
				n_actions++;
			}
		}
	}
	char mk[300];

	snprintf(mk, sizeof(mk), "mkdir -p '%s'", out_dir);
	if (system(mk) != 0) {
		return 1;
	}
	return watch_main();
}
