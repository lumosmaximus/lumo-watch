/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fast CO5300 window writes, driving the SPI2 registers directly.
 *
 * The Zephyr driver works, but costs ~100 us per window: three driver calls
 * (CASET, RASET, pixels), each reconfiguring the GDMA, with the 1-line header
 * bit-expanded into quad data. Here the SPI peripheral's own phases do the
 * protocol: an 8-bit command and a 24-bit address on one line, then the data
 * (1 line for parameters from the CPU buffer, 4 lines for pixels by DMA).
 * CASET and RASET take about a microsecond each, and a window's pixels go out
 * by DMA while the caller composes the next one.
 *
 * The Zephyr SPI and display drivers still do the setup (pins, clock, mode,
 * GDMA channel connected to SPI2): call this only after display_write() has
 * run at least once. From then on, this file owns SPI2.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <hal/spi_ll.h>
#include <soc/spi_struct.h>

#include "qspi_fast.h"

#define CO_OP_CMD    0x02
#define CO_OP_PIXELS 0x32
#define CO_CASET     0x2A
#define CO_RASET     0x2B
#define CO_RAMWR     0x2C
#define CO_X_OFF     6    /* panel column offset (devicetree column-offset) */
#define DMA_TX_CH    1    /* GDMA pair 0, TX (core 1's devicetree dmas) */

static spi_dev_t *const hw = &GPSPI2;
static const struct device *const dma = DEVICE_DT_GET(DT_NODELABEL(dma));
static bool busy;
uint32_t qf_underruns;

void qf_wait(void)
{
	if (busy) {
		while (!spi_ll_usr_is_done(hw)) {
		}
		busy = false;
		if (hw->dma_int_raw.outfifo_empty_err) {
			qf_underruns++;
			spi_ll_outfifo_empty_clr(hw);
		}
	}
}

/* Command + address phases on one line: op, then {0x00, reg, 0x00}. */
static void header(uint8_t op, uint8_t reg, int data_lines)
{
	spi_ll_clear_int_stat(hw);
	spi_ll_master_set_line_mode(hw, (spi_line_mode_t){.cmd_lines = 1, .addr_lines = 1,
							   .data_lines = data_lines});
	spi_ll_set_dummy(hw, 0);
	spi_ll_set_command_bitlen(hw, 8);
	spi_ll_set_command(hw, op, 8, false);
	spi_ll_set_addr_bitlen(hw, 24);
	spi_ll_set_address(hw, (uint32_t)reg << 8, 24, false);
	spi_ll_enable_miso(hw, 0);
	spi_ll_master_keep_cs(hw, 0);
}

static void cmd4(uint8_t reg, uint16_t a, uint16_t b)
{
	const uint8_t p[4] = {a >> 8, a & 0xff, b >> 8, b & 0xff};

	qf_wait();
	header(CO_OP_CMD, reg, 1);
	spi_ll_dma_tx_enable(hw, 0);
	spi_ll_set_mosi_bitlen(hw, 32);
	spi_ll_write_buffer(hw, p, 32);
	spi_ll_enable_mosi(hw, 1);
	spi_ll_apply_config(hw);
	spi_ll_user_start(hw);
	busy = true;
}

void qf_write(int x, int y, int w, int h, const void *buf)
{
	size_t len = (size_t)w * h * 2;

	cmd4(CO_CASET, x + CO_X_OFF, x + CO_X_OFF + w - 1);
	cmd4(CO_RASET, y, y + h - 1);
	qf_wait();

	header(CO_OP_PIXELS, CO_RAMWR, 4);
	spi_ll_set_mosi_bitlen(hw, len * 8);
	spi_ll_dma_tx_fifo_reset(hw);
	spi_ll_outfifo_empty_clr(hw);
	dma_reload(dma, DMA_TX_CH, (uint32_t)buf, 0, len);
	dma_start(dma, DMA_TX_CH);
	spi_ll_dma_tx_enable(hw, 1);
	spi_ll_enable_mosi(hw, 1);
	spi_ll_apply_config(hw);
	spi_ll_user_start(hw);
	busy = true;
}
