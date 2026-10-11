/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Chipone CO5300 AMOLED controller on a 4-line QSPI bus.
 *
 * Wire format (same family as SH8601/AXS15231B):
 *   command:  opcode 0x02, address {0x00, cmd, 0x00}, parameters  - all on D0
 *   pixels:   opcode 0x32, address {0x00, 0x2C/0x3C, 0x00} on D0, then the
 *             RGB565 data (big-endian) on D0-D3
 *
 * Zephyr's ESP32 SPI driver runs a whole transaction in one line mode, so
 * the whole transaction is quad and the single-line part is "bit-expanded":
 * each bit is placed alone on D0 of a quad nibble (a 1-line byte becomes 4
 * quad bytes).
 *
 * Unlike the AXS15231B, the CO5300 honours both CASET and RASET, so any
 * rectangle can be written. That is what lets LVGL render in small strips
 * into internal RAM instead of keeping a 434 KB frame in PSRAM. Large
 * rectangles are split into stripes (RAMWR, then RAMWRC to continue), each one
 * SPI transaction with CS held, so a transfer never outgrows one DMA run.
 *
 * Pixel buffers are sent straight from the caller (no bounce copy), so they
 * must be DMA-capable: internal RAM, which is where core 1 keeps them.
 */

#define DT_DRV_COMPAT chipone_co5300_qspi

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(co5300_qspi, CONFIG_DISPLAY_LOG_LEVEL);

#define CO_OP_CMD    0x02
#define CO_OP_PIXELS 0x32

#define CO_SLPOUT  0x11
#define CO_DISPOFF 0x28
#define CO_DISPON  0x29
#define CO_CASET   0x2A
#define CO_RASET   0x2B
#define CO_RAMWR   0x2C
#define CO_RAMWRC  0x3C
#define CO_BRIGHT  0x51

/* header (4) + the longest parameter list sent (CASET/RASET, 4) */
#define CO_MAX_1LINE 8
#define CO_EXPAND    4
/* bytes per stripe: one SPI transaction, CS held */
#define CO_STRIPE_MAX (32 * 1024)

#define CO_SPI_OPERATION (SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB | \
			  SPI_LINES_QUAD)

struct co5300_config {
	struct spi_dt_spec spi;
	struct gpio_dt_spec reset;
	uint16_t width;
	uint16_t height;
	uint16_t x_off;
	uint16_t y_off;
};

struct co5300_data {
	uint8_t brightness;
};

/* Waveshare's init sequence for this panel (their ESP-IDF BSP), minus the
 * window, which every write sets itself.
 */
static const struct {
	uint8_t cmd;
	uint8_t len;
	uint8_t data[1];
} co5300_init_cmds[] = {
	{0xFE, 1, {0x20}}, /* page 0x20 */
	{0x19, 1, {0x10}},
	{0x1C, 1, {0xA0}},
	{0xFE, 1, {0x00}}, /* back to the user page */
	{0xC4, 1, {0x80}}, /* SPI mode control */
	{0x3A, 1, {0x55}}, /* COLMOD: RGB565 */
	{0x35, 1, {0x00}}, /* tearing-effect output on */
	{0x53, 1, {0x20}}, /* brightness control on */
	{0x51, 1, {0xFF}}, /* brightness */
	{0x63, 1, {0xFF}}, /* HBM brightness */
};

/* Put each bit of in[] alone on D0: quad clocks carry 4 bits each (one nibble,
 * high nibble first), D0 is the nibble's bit 0.
 */
static void co5300_expand(const uint8_t *in, size_t len, uint8_t *out)
{
	for (size_t i = 0; i < len; i++) {
		for (int k = 0; k < CO_EXPAND; k++) {
			uint8_t hi = (in[i] >> (7 - 2 * k)) & 1;
			uint8_t lo = (in[i] >> (6 - 2 * k)) & 1;

			*out++ = (hi << 4) | lo;
		}
	}
}

static int co5300_xfer(const struct device *dev, uint8_t op, uint8_t cmd, const uint8_t *params,
		       size_t nparams, const void *pixels, size_t pixel_len)
{
	const struct co5300_config *cfg = dev->config;
	uint8_t raw[CO_MAX_1LINE] = {op, 0x00, cmd, 0x00};
	uint8_t wire[CO_MAX_1LINE * CO_EXPAND] __aligned(4);
	size_t raw_len = 4 + nparams;

	if (raw_len > sizeof(raw)) {
		return -EINVAL;
	}
	memcpy(&raw[4], params, nparams);
	co5300_expand(raw, raw_len, wire);

	const struct spi_buf bufs[] = {
		{.buf = wire, .len = raw_len * CO_EXPAND},
		{.buf = (void *)pixels, .len = pixel_len},
	};
	const struct spi_buf_set tx = {.buffers = bufs, .count = pixel_len ? 2 : 1};

	return spi_write_dt(&cfg->spi, &tx);
}

static int co5300_cmd(const struct device *dev, uint8_t cmd, const uint8_t *p, size_t n)
{
	return co5300_xfer(dev, CO_OP_CMD, cmd, p, n, NULL, 0);
}

static int co5300_window(const struct device *dev, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
	const struct co5300_config *cfg = dev->config;
	uint16_t x0 = x + cfg->x_off, x1 = x0 + w - 1;
	uint16_t y0 = y + cfg->y_off, y1 = y0 + h - 1;
	const uint8_t ca[] = {x0 >> 8, x0 & 0xff, x1 >> 8, x1 & 0xff};
	const uint8_t ra[] = {y0 >> 8, y0 & 0xff, y1 >> 8, y1 & 0xff};
	int ret = co5300_cmd(dev, CO_CASET, ca, sizeof(ca));

	return ret ? ret : co5300_cmd(dev, CO_RASET, ra, sizeof(ra));
}

static int co5300_write(const struct device *dev, const uint16_t x, const uint16_t y,
			const struct display_buffer_descriptor *desc, const void *buf)
{
	const struct co5300_config *cfg = dev->config;
	const size_t row_bytes = desc->width * 2U;
	const uint16_t rows_per = MAX(1, CO_STRIPE_MAX / row_bytes);
	const uint8_t *src = buf;
	int ret;

	if (x + desc->width > cfg->width || y + desc->height > cfg->height) {
		return -EINVAL;
	}
	if (desc->pitch != desc->width) {
		return -ENOTSUP; /* LVGL and our callers send packed rectangles */
	}
	ret = co5300_window(dev, x, y, desc->width, desc->height);
	for (uint16_t row = 0; ret == 0 && row < desc->height; row += rows_per) {
		uint16_t n = MIN(rows_per, desc->height - row);

		ret = co5300_xfer(dev, CO_OP_PIXELS, row == 0 ? CO_RAMWR : CO_RAMWRC, NULL, 0,
				  src + row * row_bytes, n * row_bytes);
	}
	return ret;
}

static int co5300_blanking_on(const struct device *dev)
{
	return co5300_cmd(dev, CO_DISPOFF, NULL, 0);
}

static int co5300_blanking_off(const struct device *dev)
{
	return co5300_cmd(dev, CO_DISPON, NULL, 0);
}

static int co5300_set_brightness(const struct device *dev, const uint8_t brightness)
{
	struct co5300_data *data = dev->data;

	data->brightness = brightness;
	return co5300_cmd(dev, CO_BRIGHT, &brightness, 1);
}

static void co5300_get_capabilities(const struct device *dev, struct display_capabilities *caps)
{
	const struct co5300_config *cfg = dev->config;

	memset(caps, 0, sizeof(*caps));
	caps->x_resolution = cfg->width;
	caps->y_resolution = cfg->height;
	caps->supported_pixel_formats = PIXEL_FORMAT_RGB_565X;
	caps->current_pixel_format = PIXEL_FORMAT_RGB_565X;
	caps->current_orientation = DISPLAY_ORIENTATION_NORMAL;
}

static int co5300_set_pixel_format(const struct device *dev, const enum display_pixel_format pf)
{
	return pf == PIXEL_FORMAT_RGB_565X ? 0 : -ENOTSUP;
}

static int co5300_init(const struct device *dev)
{
	const struct co5300_config *cfg = dev->config;
	int ret;

	if (!spi_is_ready_dt(&cfg->spi) || !gpio_is_ready_dt(&cfg->reset)) {
		return -ENODEV;
	}
	/* reset pulse (active low in DT) */
	ret = gpio_pin_configure_dt(&cfg->reset, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		return ret;
	}
	k_msleep(10);
	gpio_pin_set_dt(&cfg->reset, 1);
	k_msleep(20);
	gpio_pin_set_dt(&cfg->reset, 0);
	k_msleep(120);

	for (size_t i = 0; i < ARRAY_SIZE(co5300_init_cmds); i++) {
		ret = co5300_cmd(dev, co5300_init_cmds[i].cmd, co5300_init_cmds[i].data,
				 co5300_init_cmds[i].len);
		if (ret) {
			LOG_ERR("init cmd 0x%02x failed (%d)", co5300_init_cmds[i].cmd, ret);
			return ret;
		}
	}
	ret = co5300_cmd(dev, CO_SLPOUT, NULL, 0);
	k_msleep(120);
	/* stay blanked until the app has drawn a frame and calls blanking_off */
	return ret;
}

static DEVICE_API(display, co5300_api) = {
	.blanking_on = co5300_blanking_on,
	.blanking_off = co5300_blanking_off,
	.write = co5300_write,
	.set_brightness = co5300_set_brightness,
	.get_capabilities = co5300_get_capabilities,
	.set_pixel_format = co5300_set_pixel_format,
};

#define CO5300_DEFINE(inst)                                                                  \
	static const struct co5300_config co5300_config_##inst = {                         \
		.spi = SPI_DT_SPEC_INST_GET(inst, CO_SPI_OPERATION),                       \
		.reset = GPIO_DT_SPEC_INST_GET(inst, reset_gpios),                         \
		.width = DT_INST_PROP(inst, width),                                        \
		.height = DT_INST_PROP(inst, height),                                      \
		.x_off = DT_INST_PROP(inst, column_offset),                                \
		.y_off = DT_INST_PROP(inst, row_offset),                                   \
	};                                                                                 \
	static struct co5300_data co5300_data_##inst = {.brightness = 0xff};               \
	DEVICE_DT_INST_DEFINE(inst, co5300_init, NULL, &co5300_data_##inst,                \
			      &co5300_config_##inst, POST_KERNEL,                          \
			      CONFIG_DISPLAY_INIT_PRIORITY, &co5300_api);

DT_INST_FOREACH_STATUS_OKAY(CO5300_DEFINE)
