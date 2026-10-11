/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hardware test, core 1 side: draw a boundary/orientation test pattern on
 * the CO5300 in 16-row strips (no frame buffer), then answer core 0's pings
 * with our core ID and how the drawing went.
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

#define W     466
#define H     466
#define ROWS  16

static const struct device *const ipm = DEVICE_DT_GET(DT_NODELABEL(ipm0));
static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static struct amp_ping ping;
static K_SEM_DEFINE(ping_sem, 0, 1);
static int32_t disp_err = -ENODEV;
static uint32_t draw_ms;

/* one strip, internal RAM, sent straight to the SPI DMA */
static uint8_t strip[W * ROWS * 2] __aligned(4);

static void on_ipm(const struct device *dev, void *user_data, uint32_t id, volatile void *data)
{
	memcpy(&ping, (const void *)data, sizeof(ping));
	k_sem_give(&ping_sem);
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
	struct display_buffer_descriptor desc = {
		.buf_size = sizeof(strip), .width = W, .pitch = W,
	};

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
		}
	}

	while (1) {
		k_sem_take(&ping_sem, K_FOREVER);
		if (ping.magic != AMP_MAGIC) {
			continue;
		}
		struct amp_pong pong = {.seq = ping.seq, .core1_ms = k_uptime_get_32(),
					.core_id = core_id(), .disp_err = disp_err,
					.draw_ms = draw_ms};

		ipm_send(ipm, -1, AMP_MSG_PONG, &pong, sizeof(pong));
	}
	return 0;
}
