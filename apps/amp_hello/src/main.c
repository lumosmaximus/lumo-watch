/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Core 0 side of the dual-core bring-up:
 *   1. scan the shared I2C bus and name what answers
 *   2. read the AXP2101's rail settings (left at power-on defaults for now)
 *   3. ping core 1 over shared memory every 500 ms and print its replies
 */

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "../common/amp_msg.h"

static const struct device *const i2c = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static const struct device *const ipm = DEVICE_DT_GET(DT_NODELABEL(ipm0));

static struct amp_pong pong;
static K_SEM_DEFINE(pong_sem, 0, 1);

static const char *who(uint8_t addr)
{
	switch (addr) {
	case 0x18: return "ES8311 codec";
	case 0x20: return "TCA9554 expander";
	case 0x34: return "AXP2101 PMIC";
	case 0x40: return "ES7210 mic ADC";
	case 0x51: return "PCF85063 RTC";
	case 0x5a: return "CST9217 touch";
	case 0x6b: return "QMI8658 IMU";
	default: return "?";
	}
}

static void i2c_scan(void)
{
	int found = 0;

	printk("i2c0 scan (GPIO15 SDA / GPIO14 SCL, 400 kHz):\n");
	for (uint8_t a = 0x08; a < 0x78; a++) {
		uint8_t b;

		if (i2c_read(i2c, &b, 1, a) == 0) {
			printk("  0x%02x  %s\n", a, who(a));
			found++;
		}
	}
	printk("  %d devices\n", found);
}

static int axp_rd(uint8_t reg, uint8_t *v)
{
	return i2c_reg_read_byte(i2c, 0x34, reg, v);
}

/* AXP2101 register map (datasheet): 0x80 DCDC on/off, 0x82 DCDC1 voltage
 * (1500 mV + 100 mV/step), 0x90 LDO on/off (bit0-3 ALDO1-4, bit4-5 BLDO1-2,
 * bit6 CPUSLDO, bit7 DLDO1), 0x92-0x97 ALDO1-4 / BLDO1-2 voltage
 * (500 mV + 100 mV/step). Raw values are printed too, to check against.
 */
static void axp_dump(void)
{
	uint8_t s0, s1, dc_en, dc1, ldo_en, v;
	static const char *const ldo[] = {"ALDO1", "ALDO2", "ALDO3", "ALDO4", "BLDO1", "BLDO2"};

	if (axp_rd(0x00, &s0) || axp_rd(0x01, &s1) || axp_rd(0x80, &dc_en) ||
	    axp_rd(0x82, &dc1) || axp_rd(0x90, &ldo_en)) {
		printk("AXP2101: read failed\n");
		return;
	}
	printk("AXP2101: status 0x%02x 0x%02x, DCDC enables 0x%02x, LDO enables 0x%02x\n", s0, s1,
	       dc_en, ldo_en);
	printk("  DCDC1 %s %u mV (raw 0x%02x)\n", (dc_en & 1) ? "on " : "off",
	       1500 + 100 * (dc1 & 0x1f), dc1);
	for (int i = 0; i < 6; i++) {
		if (axp_rd(0x92 + i, &v) == 0) {
			printk("  %s %s %u mV (raw 0x%02x)\n", ldo[i],
			       (ldo_en & BIT(i)) ? "on " : "off", 500 + 100 * (v & 0x1f), v);
		}
	}
}

static void on_ipm(const struct device *dev, void *user_data, uint32_t id, volatile void *data)
{
	memcpy(&pong, (const void *)data, sizeof(pong));
	k_sem_give(&pong_sem);
}

int main(void)
{
	uint32_t seq = 0;

	printk("\n*** amp_hello: core 0 (PROCPU) ***\n");
	if (!device_is_ready(i2c) || !device_is_ready(ipm)) {
		printk("i2c %d / ipm %d not ready\n", device_is_ready(i2c), device_is_ready(ipm));
		return 0;
	}
	i2c_scan();
	axp_dump();

	ipm_register_callback(ipm, on_ipm, NULL);
	ipm_set_enabled(ipm, 1);
	k_msleep(50); /* let core 1 finish booting */

	while (1) {
		struct amp_ping ping = {.magic = AMP_MAGIC, .seq = ++seq,
					.core0_ms = k_uptime_get_32()};
		uint32_t t0 = k_cycle_get_32();

		ipm_send(ipm, -1, AMP_MSG_PING, &ping, sizeof(ping));
		if (k_sem_take(&pong_sem, K_MSEC(1000)) == 0) {
			uint32_t rtt_us = k_cyc_to_us_floor32(k_cycle_get_32() - t0);

			printk("ping %u -> core %u replied: seq %u, its uptime %u ms, round trip %u us\n",
			       seq, pong.core_id, pong.seq, pong.core1_ms, rtt_us);
		} else {
			printk("ping %u: no reply from core 1\n", seq);
		}
		k_msleep(500);
	}
	return 0;
}
