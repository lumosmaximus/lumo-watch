/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hardware test, core 1 side: draw a boundary/orientation test pattern on
 * the CO5300 in 16-row strips (no frame buffer), draw a dot wherever core 0
 * reports a touch, and answer core 0's pings with our core ID and how the
 * drawing went.
 *
 * Pattern: white 1 px ring on the very edge (r 232), cyan ring at r 228,
 * grey rings every 20 px, red crosshair, white centre dot, green ticks every
 * 30 degrees, magenta dot at 12 o'clock and cyan dot at 3 o'clock, faint
 * quadrant tints (TL red, TR green, BL blue, BR yellow), and magenta lines on
 * the 466x466 square (only their midpoints can show on a round panel).
 * If the panel offset or size is wrong, the outer ring is cut on one side.
 */

#include <math.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/kernel.h>

#include "../../common/amp_msg.h"

#define W    466
#define H    466
#define ROWS 16

static const struct device *const ipm = DEVICE_DT_GET(DT_NODELABEL(ipm0));
static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static int32_t disp_err = -ENODEV;
static uint32_t draw_ms;
static uint32_t bench_us;

/* one strip, internal RAM, sent straight to the SPI DMA */
static uint8_t strip[W * ROWS * 2] __aligned(4);

/* IPM callbacks run in an interrupt: copy the message, let main() act on it */
struct msg {
	uint32_t id;
	union {
		struct amp_ping ping;
		struct amp_touch touch;
	};
};
K_MSGQ_DEFINE(msgs, sizeof(struct msg), 16, 4);

static void on_ipm(const struct device *dev, void *user_data, uint32_t id, volatile void *data)
{
	struct msg m = {.id = id};

	if (id == AMP_MSG_PING) {
		memcpy(&m.ping, (const void *)data, sizeof(m.ping));
	} else if (id == AMP_MSG_TOUCH) {
		memcpy(&m.touch, (const void *)data, sizeof(m.touch));
	} else {
		return;
	}
	(void)k_msgq_put(&msgs, &m, K_NO_WAIT);
}

static uint32_t core_id(void)
{
	uint32_t prid;

	__asm__ volatile("rsr.prid %0" : "=a"(prid));
	return (prid & 0xffff) == 0xabab ? 1 : 0;
}

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
	return ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
}

static uint16_t pattern(int x, int y)
{
	const float c = (W - 1) / 2.0f; /* 232.5 */
	float dx = x - c, dy = y - c;
	float d = sqrtf(dx * dx + dy * dy);

	if (x == 0 || x == W - 1 || y == 0 || y == H - 1) {
		return rgb(255, 0, 255); /* square edge */
	}
	if (fabsf(d - 232.0f) < 1.0f) {
		return rgb(255, 255, 255); /* outermost ring */
	}
	if (fabsf(d - 228.0f) < 0.8f) {
		return rgb(0, 255, 255);
	}
	if ((x - 233) * (x - 233) + (y - 14) * (y - 14) < 36) {
		return rgb(255, 0, 255); /* 12 o'clock marker */
	}
	if ((x - 452) * (x - 452) + (y - 233) * (y - 233) < 16) {
		return rgb(0, 255, 255); /* 3 o'clock marker */
	}
	if (d < 4.0f) {
		return rgb(255, 255, 255);
	}
	if ((x == 232 || x == 233 || y == 232 || y == 233) && d < 220.0f) {
		return rgb(255, 0, 0);
	}
	if (d > 205.0f && d < 226.0f) {
		float a = atan2f(dy, dx) * 180.0f / 3.14159265f + 360.0f;

		if (fmodf(a + 0.75f, 30.0f) < 1.5f) {
			return rgb(0, 255, 0);
		}
	}
	for (int k = 1; k <= 11; k++) {
		if (fabsf(d - 20.0f * k) < 0.6f) {
			return rgb(70, 70, 70);
		}
	}
	if (dx < 0 && dy < 0) {
		return rgb(48, 0, 0);
	}
	if (dx >= 0 && dy < 0) {
		return rgb(0, 40, 0);
	}
	if (dx < 0) {
		return rgb(0, 0, 56);
	}
	return rgb(40, 40, 0);
}

static int draw_pattern(void)
{
	struct display_buffer_descriptor desc = {.width = W, .pitch = W};

	for (int y0 = 0; y0 < H; y0 += ROWS) {
		int rows = MIN(ROWS, H - y0);

		for (int r = 0; r < rows; r++) {
			for (int x = 0; x < W; x++) {
				uint16_t p = pattern(x, y0 + r);
				uint8_t *o = &strip[(r * W + x) * 2];

				o[0] = p >> 8; /* RGB565, high byte first */
				o[1] = p & 0xff;
			}
		}
		desc.height = rows;
		desc.buf_size = rows * W * 2;
		int ret = display_write(disp, 0, y0, &desc, strip);

		if (ret) {
			return ret;
		}
	}
	return 0;
}

/* Raw throughput: send BENCH_FRAMES full frames of a ready-made strip (no
 * per-pixel work), so the time is pure SPI/DMA. Colours cycle so the frames
 * are visibly different.
 */
#define BENCH_FRAMES 20
static uint32_t bench(void)
{
	static const uint16_t cols[] = {0xf800, 0x07e0, 0x001f, 0xffff, 0x0000};
	struct display_buffer_descriptor desc = {.width = W, .pitch = W};
	uint32_t t0 = k_cycle_get_32();

	for (int f = 0; f < BENCH_FRAMES; f++) {
		uint16_t c = cols[f % ARRAY_SIZE(cols)];

		for (int i = 0; i < W * ROWS; i++) {
			strip[2 * i] = c >> 8;
			strip[2 * i + 1] = c & 0xff;
		}
		for (int y0 = 0; y0 < H; y0 += ROWS) {
			desc.height = MIN(ROWS, H - y0);
			desc.buf_size = desc.height * W * 2;
			display_write(disp, 0, y0, &desc, strip);
		}
	}
	return k_cyc_to_us_floor32(k_cycle_get_32() - t0) / BENCH_FRAMES;
}

/* Corruption test (the panel can't be read back, so make errors visible):
 *  1. 300 full frames alternating 0x5555 / 0xAAAA: every data line flips on
 *     every clock, at full speed. Flaky timing shows as sparkles.
 *  2. Still screens, 4 s each: 1 px black/white checkerboard (should look an
 *     even grey; dropped/shifted bits give streaks or moire), flat mid grey
 *     (one wrong pixel is a visible speck), and R/G/B/grey ramps (a stuck
 *     bit shows as a band).
 */
typedef uint16_t (*pixel_fn)(int x, int y);

static void fill(pixel_fn fn)
{
	struct display_buffer_descriptor desc = {.width = W, .pitch = W};

	for (int y0 = 0; y0 < H; y0 += ROWS) {
		int rows = MIN(ROWS, H - y0);

		for (int r = 0; r < rows; r++) {
			for (int x = 0; x < W; x++) {
				uint16_t p = fn(x, y0 + r);

				strip[(r * W + x) * 2] = p >> 8;
				strip[(r * W + x) * 2 + 1] = p & 0xff;
			}
		}
		desc.height = rows;
		desc.buf_size = rows * W * 2;
		display_write(disp, 0, y0, &desc, strip);
	}
}

static uint16_t px_checker(int x, int y)
{
	return ((x ^ y) & 1) ? 0xffff : 0x0000;
}

static uint16_t px_grey(int x, int y)
{
	return rgb(128, 128, 128);
}

/* four horizontal bands, each ramping left to right through every level */
static uint16_t px_ramps(int x, int y)
{
	int band = y * 4 / H, v = x * 255 / (W - 1);

	switch (band) {
	case 0: return rgb(v, 0, 0);
	case 1: return rgb(0, v, 0);
	case 2: return rgb(0, 0, v);
	default: return rgb(v, v, v);
	}
}

static uint32_t stress_fps;

static void stress(void)
{
	struct display_buffer_descriptor desc = {.width = W, .pitch = W};
	uint32_t t0 = k_uptime_get_32();

	for (int f = 0; f < 300; f++) {
		uint16_t c = (f & 1) ? 0xaaaa : 0x5555;

		for (int i = 0; i < W * ROWS; i++) {
			strip[2 * i] = c >> 8;
			strip[2 * i + 1] = c & 0xff;
		}
		for (int y0 = 0; y0 < H; y0 += ROWS) {
			desc.height = MIN(ROWS, H - y0);
			desc.buf_size = desc.height * W * 2;
			display_write(disp, 0, y0, &desc, strip);
		}
	}
	stress_fps = 300000 / MAX(1, k_uptime_get_32() - t0);
	fill(px_checker);
	k_msleep(4000);
	fill(px_grey);
	k_msleep(4000);
	fill(px_ramps);
	k_msleep(4000);
}

/* A 13x13 yellow dot centred on the touch: a small window write. */
static void draw_dot(int cx, int cy)
{
	static uint8_t dot[13 * 13 * 2] __aligned(4);
	int x0 = CLAMP(cx - 6, 0, W - 13), y0 = CLAMP(cy - 6, 0, H - 13);
	struct display_buffer_descriptor d = {.buf_size = sizeof(dot), .width = 13,
					      .height = 13, .pitch = 13};

	for (int i = 0; i < 13 * 13; i++) {
		int px = x0 + i % 13, py = y0 + i / 13;
		int dx = px - cx, dy = py - cy;
		uint16_t p = dx * dx + dy * dy <= 36 ? rgb(255, 230, 0) : pattern(px, py);

		dot[2 * i] = p >> 8;
		dot[2 * i + 1] = p & 0xff;
	}
	display_write(disp, x0, y0, &d, dot);
}

int main(void)
{
	ipm_register_callback(ipm, on_ipm, NULL);
	ipm_set_enabled(ipm, 1);

	if (device_is_ready(disp)) {
		uint32_t t0 = k_uptime_get_32();

		disp_err = draw_pattern();
		draw_ms = k_uptime_get_32() - t0;
		if (disp_err == 0) {
			disp_err = display_blanking_off(disp);
			k_msleep(500);
			bench_us = bench();
			stress();
			draw_pattern(); /* back to the test pattern for the touch test */
		}
	}

	while (1) {
		struct msg m;

		k_msgq_get(&msgs, &m, K_FOREVER);
		if (m.id == AMP_MSG_TOUCH) {
			if (m.touch.down && disp_err == 0) {
				draw_dot(m.touch.x, m.touch.y);
			}
			continue;
		}
		if (m.ping.magic != AMP_MAGIC) {
			continue;
		}
		struct amp_pong pong = {.seq = m.ping.seq, .core1_ms = k_uptime_get_32(),
					.core_id = core_id(), .disp_err = disp_err,
					.draw_ms = draw_ms, .bench_us = bench_us};

		ipm_send(ipm, -1, AMP_MSG_PONG, &pong, sizeof(pong));
	}
	return 0;
}
